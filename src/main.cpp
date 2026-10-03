#include "raylib.h"
#include "sodium_react_codepoints.h"
#include "chat_layout.h"
#include "chat_history.h"
#include "session_store.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "../include/json.hpp"
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <future>
#include <cstring>
#include <vector>
#include <cstdint>
#include <cmath>
#include <limits>

using json = nlohmann::json;



namespace {
    Font messageFont{};
    Font headingFont{};

    static Font LoadClientFont(const char *fileName, int rasterSize) {
        const std::string path = std::string(GetApplicationDirectory()) + "assets/fonts/" + fileName;
        if (!FileExists(path.c_str())) {
            TraceLog(LOG_WARNING, "Sodium React font missing: %s", path.c_str());
            return GetFontDefault();
        }
        const int count = static_cast<int>(sizeof(SODIUM_REACT_CODEPOINTS) / sizeof(SODIUM_REACT_CODEPOINTS[0]));
        Font font = LoadFontEx(path.c_str(), rasterSize, SODIUM_REACT_CODEPOINTS, count);
        if (font.texture.id != GetFontDefault().texture.id) {
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            // DrawTextEx/MeasureTextEx use the GPU atlas and metrics, never CPU
            // glyph pixels. Keep metrics/recs, release the redundant ImageDrawText copies.
            for (int i = 0; i < font.glyphCount; ++i) {
                UnloadImage(font.glyphs[i].image);
                font.glyphs[i].image = Image{};
            }
        }
        return font;
    }

    static void DrawClientText(const char *text, int x, int y, int size, Color color) {
        const Font font = (size >= 22) ? headingFont : messageFont;
        DrawTextEx(font, text, Vector2{static_cast<float>(x), static_cast<float>(y)},
                   static_cast<float>(size), 0.0f, color);
    }

    // --- Application States ---
    enum AppScreen { SCREEN_ONBOARD, SCREEN_SIGNIN, SCREEN_CHAT };

    AppScreen currentScreen = SCREEN_SIGNIN;
    std::string sessionToken, loggedInUser;
    std::string connected_server = "http://127.0.0.1:51211"; // Synced server config string
    std::string loginStatusMessage = "Please log in.";
    bool isNetworkRequestPending = false;

    // UI Layout Constraints
    auto SERVER_EDIT = (Rectangle){150, 100, 250, 30}; // Server config input boundary
    auto USERNAME_EDIT = (Rectangle){150, 150, 200, 30}; // Adjusted down to fit server config row
    auto PASSPHRASE_EDIT = (Rectangle){150, 200, 200, 30}; // Adjusted down to fit server config row

    // --- Input Field Buffers ---
    char serverInputBuffer[128] = "http://127.0.0.1:51211"; // Initialized with default target
    char usernameBuffer[64] = "\0";
    char passwordBuffer[64] = "\0";
    char chatInputBuffer[256] = "\0";

    bool serverInputEditMode = false;
    bool usernameEditMode = false;
    bool passwordEditMode = false;
    bool chatInputEditMode = false;

    // --- Chat Log Variables ---
    ChatHistory chatMessages;
    int lastFetchedId = 0;
    float fetchTimer = 0.0f;
    constexpr float FETCH_INTERVAL = 1.0f;

    // --- Background Worker Variables ---
    struct AuthResult { bool success = false; std::optional<SavedSession> session; std::string message; bool retrySaved = false; };
    struct SyncResult { std::vector<Message> messages; bool invalid = false; bool networkFailure = false; };
    std::future<AuthResult> loginFuture;
    std::future<StoreStatus> keyringFuture;
    bool memoryOnly = false;
    bool keyringPending = false, keyringDeleting = false, retrySaved = false, deleteFailed = false;
    std::string sessionStatus;
    enum class FetchMode { Live, Older, Newer, Latest };
    FetchMode syncMode = FetchMode::Live;
    bool browsingHistory = false, historyStart = false, preferOlderRows = false;
    std::string historyStatus;
    std::future<SyncResult> syncFuture;
    std::future<void> sendFuture;
    unsigned int sessionGeneration = 0, syncGeneration = 0;
    ChatScrollState chatScroll;
    bool isSyncRequestPending = false;
}


const std::string ONBOARDING_ENDPOINT = "/sodium/v1/users/onboarding";
const std::string LOGIN_ENDPOINT = "/sodium/v1/users/login";
const std::string MESSAGES_ENDPOINT = "/sodium/v1/messages";


// --- libcURL Callback to capture data ---
static size_t WriteCallback(void *contents, const size_t size, size_t nmemb, std::string *s) {
    constexpr std::size_t MaxResponseBytes = 1024*1024;
    if (size != 0 && nmemb > MaxResponseBytes/size) return 0;
    const size_t newLength = size * nmemb;
    if (s->size() > MaxResponseBytes || newLength > MaxResponseBytes - s->size()) return 0;
    try {
        s->append(static_cast<char *>(contents), newLength);
    } catch ([[maybe_unused]] std::bad_alloc &e) {
        return 0;
    }
    return newLength;
}

// Auth workers return results; all application state changes stay on the UI thread.
static AuthResult ExecuteAuth(std::string server, std::string username, std::string password, bool onboarding) {
    if (!ValidServer(server)) return {false, {}, "Enter a valid HTTP(S) server address."};
    CURL* curl = curl_easy_init();
    if (!curl) return {false, {}, "Network initialization failed."};
    json payload{{"username", username}, {"password", password}};
    if (onboarding) payload["email"] = "";
    std::fill(password.begin(), password.end(), '\0');
    const std::string body = payload.dump();
    std::string response;
    auto headers = curl_slist_append(nullptr, "Content-Type: application/json");
    const std::string url = server + (onboarding ? ONBOARDING_ENDPOINT : LOGIN_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    auto code = curl_easy_perform(curl); long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (code != CURLE_OK) return {false, {}, "Server connection failed. Please retry."};
    try {
        const auto data = json::parse(response);
        if (status == 200) {
            SavedSession saved{server, data.at("username").get<std::string>(), data.at("token").get<std::string>()};
            if (DecodeSession(EncodeSession(saved)).status == StoreStatus::Ok) return {true, saved, ""};
        }
        if (status == 409 && data.value("error", "").find("token_reset_required") == 0)
            return {false, {}, "Legacy account: explicitly reset the token through the API."};
        if (status == 401) return {false, {}, "Invalid username or password."};
        if (status == 409) return {false, {}, "Username already taken."};
        if (status == 429) return {false, {}, "Too many attempts. Please wait."};
    } catch (...) {}
    return {false, {}, "Sign in failed. Check the account and server."};
}
static AuthResult RestoreSession() {
    auto loaded = LoadSession();
    if (loaded.status == StoreStatus::Missing) return {false, {}, "Please log in."};
    if (loaded.status == StoreStatus::Unavailable) return {false, {}, "Keyring unavailable. You can sign in for this run.", true};
    if (loaded.status == StoreStatus::Invalid) {
        auto removed = DeleteSession();
        return {false, {}, removed == StoreStatus::Ok ? "Invalid saved session removed. Please log in." : "Invalid session; keyring deletion failed. Retry saved session.", removed != StoreStatus::Ok};
    }
    const auto saved = *loaded.session;
    CURL* curl = curl_easy_init();
    if (!curl) return {false, saved, "Network initialization failed. Retry saved session.", true};
    std::string response;
    const auto url = saved.server + "/sodium/v1/users/info";
    const auto auth = "Authorization: Bearer " + saved.token;
    auto headers = curl_slist_append(nullptr, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    auto code = curl_easy_perform(curl); long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (code != CURLE_OK || status >= 500 || status == 429) return {false, saved, "Server unavailable. Saved session kept; retry when online.", true};
    if (status == 401 || status == 403) {
        const auto removed = DeleteSession();
        return {false, {}, removed == StoreStatus::Ok ? "Saved token expired. Please log in." : "Token expired; keyring deletion failed. Retry saved session.", removed != StoreStatus::Ok};
    }
    try {
        if (status == 200 && json::parse(response).at("username").get<std::string>() == saved.username)
            return {true, saved, "Session restored from keyring."};
    } catch (...) {}
    return {false, saved, "Session validation failed. Saved session kept; verify the server.", true};
}
static void RemoveSavedSession() {
    keyringDeleting = true; keyringPending = true; deleteFailed = false;
    keyringFuture = std::async(std::launch::async, DeleteSession);
}

// --- Asynchronous Sync Mechanics ---
static SyncResult GetMessagesBackground(std::string server, std::string token, int after, int before = 0) {
    SyncResult result;
    auto& messages = result.messages;
    CURL *curl = curl_easy_init();
    if (!curl) { result.networkFailure = true; return result; }

    std::string responseBuffer;
    curl_slist *headers = nullptr;
    const std::string authHeader = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authHeader.c_str());

    const std::string url = server + MESSAGES_ENDPOINT + (before > 0 ? "?before=" + std::to_string(before) + "&limit=50" : after == 0 ? "?limit=50" : "?after=" + std::to_string(after) + "&limit=50");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.invalid = status == 401 || status == 403;
    result.networkFailure = res != CURLE_OK || status != 200;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK) {
        try {
            if (json responseJson = json::parse(responseBuffer); responseJson.contains("messages") && responseJson["messages"].is_array()) {
                for (auto &item: responseJson["messages"]) {
                    Message msg;
                    msg.id = item["id"].get<int>();
                    msg.created = item["created"].get<double>();
                    msg.sender = item["sender"].get<std::string>();
                    msg.text = item["text"].get<std::string>();

                    if (messages.size() == 50) break;
                    if (msg.sender.size() > 1024 || msg.text.size() > 8000) continue;
                    if (msg.id > after && (before == 0 || msg.id < before)) messages.push_back(std::move(msg));
                }
            } else result.networkFailure = true;
        } catch (...) {
            result.messages.clear(); result.networkFailure = true;
        }
    }
    std::sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) { return a.id < b.id; });
    messages.erase(std::unique(messages.begin(), messages.end(), [](const Message& a, const Message& b) { return a.id == b.id; }), messages.end());
    if (result.networkFailure) messages.clear();
    return result;
}

static void SendMessageBackground(std::string text, std::string server, std::string token) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    json outJson;
    outJson["text"] = text;
    const std::string jsonStr = outJson.dump();
    std::string responseBuffer;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string authHeader = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authHeader.c_str());

    const std::string url = server + MESSAGES_ENDPOINT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

static void SubmitChatMessage() {
    if (std::strlen(chatInputBuffer) > 0) {
        // One in-flight send bounds thread stacks and curl buffers. Busy input stays editable.
        if (sendFuture.valid()) { sessionStatus = "Still sending; your next message is kept in the box."; chatInputEditMode = true; return; }
        sendFuture = std::async(std::launch::async, SendMessageBackground,
                                std::string(chatInputBuffer), connected_server, sessionToken);
        std::memset(chatInputBuffer, 0, sizeof(chatInputBuffer));
        fetchTimer = FETCH_INTERVAL;
        chatScroll.followLatest = true;
    }
    chatInputEditMode = true;
}

static Rectangle ChatInputBounds() {
    return Rectangle{20, static_cast<float>(GetScreenHeight() - 50),
                     static_cast<float>(GetScreenWidth() - 140), 30};
}

static Rectangle ChatSendBounds() {
    return Rectangle{static_cast<float>(GetScreenWidth() - 110),
                     static_cast<float>(GetScreenHeight() - 50), 90, 30};
}

static Color UsernameColor(const std::string& username) {
    // Fixed hashing keeps nickname colors consistent between app launches.
    constexpr Color palette[] = {
        {35, 91, 170, 255}, {155, 48, 62, 255}, {38, 112, 65, 255},
        {116, 65, 160, 255}, {151, 82, 24, 255}, {19, 109, 119, 255},
        {156, 51, 121, 255}, {88, 98, 28, 255}, {66, 78, 151, 255},
        {137, 64, 41, 255}, {29, 115, 98, 255}, {122, 65, 128, 255}
    };
    std::uint32_t hash = 2166136261u;
    for (unsigned char byte : username) { hash ^= byte; hash *= 16777619u; }
    return palette[hash % (sizeof(palette)/sizeof(palette[0]))];
}

namespace {
    struct CachedChatRow { std::string text; std::size_t nicknameBytes; };
    struct CachedChatBlock { std::vector<CachedChatRow> rows; Color nicknameColor; bool shaded; float height; int messageId; };
    struct ChatLayoutCache {
        std::vector<CachedChatBlock> blocks;
        float width = -1, height = 0;
        std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t firstOrdinal = 0;
        std::size_t firstMessage = 0;
        void Clear() { std::vector<CachedChatBlock>().swap(blocks); width = -1; height = 0; firstMessage = 0; firstOrdinal = 0; revision = std::numeric_limits<std::uint64_t>::max(); }
    } chatLayout;
    int messageRasterSize = 0, headingRasterSize = 0;
}
static void ClearChatHistory() {
    chatMessages.Clear(); chatLayout.Clear(); chatScroll = ChatScrollState{};
    browsingHistory = false; historyStart = false; preferOlderRows = false; historyStatus.clear();
}
static void BeginMessageFetch(FetchMode mode) {
    if (isSyncRequestPending || keyringPending || currentScreen != SCREEN_CHAT) return;
    if ((mode == FetchMode::Older || mode == FetchMode::Newer) && chatMessages.empty()) return;
    syncMode = mode; syncGeneration = sessionGeneration; isSyncRequestPending = true;
    const int before = mode == FetchMode::Older ? chatMessages[0].id : 0;
    const int after = mode == FetchMode::Newer ? chatMessages[chatMessages.size()-1].id : mode == FetchMode::Live ? lastFetchedId : 0;
    syncFuture = std::async(std::launch::async, GetMessagesBackground, connected_server, sessionToken, after, before);
    fetchTimer = 0;
}
static void ApplyMessagePage(SyncResult& sync, FetchMode mode) {
    if (sync.networkFailure || sync.invalid) {
        if (mode != FetchMode::Live) historyStatus = "History unavailable; retry.";
        return;
    }
    historyStatus.clear();
    const auto received = sync.messages.size();
    if (mode == FetchMode::Older || mode == FetchMode::Newer) {
        // Apply only a contiguous portion that fits on screen under the row
        // budget. Leave the rest on the server for the next exclusive cursor.
        // Byte length bounds row count even if every character wraps separately.
        std::size_t kept = 0, rowBound = 0, byteBound = 0;
        for (std::size_t i = 0; i < received; ++i) {
            const auto& message = sync.messages[mode == FetchMode::Older ? received-1-i : i];
            const auto rows = message.sender.size() + message.text.size() + 3;
            const auto bytes = sizeof(Message) + message.sender.capacity() + message.text.capacity() +
                64*(1 + std::count(message.text.begin(), message.text.end(), '\n'));
            if (kept && (rowBound + rows > 4096 || byteBound + bytes > ChatHistory::MaxAccountedBytes/2)) break;
            rowBound += rows; byteBound += bytes; ++kept;
        }
        if (mode == FetchMode::Older) sync.messages.erase(sync.messages.begin(), sync.messages.end()-kept);
        else sync.messages.erase(sync.messages.begin()+kept, sync.messages.end());
    }
    const bool exhausted = received < 50 && sync.messages.size() == received;
    if (mode == FetchMode::Latest) {
        ClearChatHistory(); lastFetchedId = 0;
    }
    if (mode == FetchMode::Older) {
        browsingHistory = true; preferOlderRows = true;
        // Width must stay the same to retain the current message's screen position.
        // Force a revision change below; cache direction only changes on rebuild.
        chatScroll.followLatest = false;
        historyStart = exhausted;
        for (auto it = sync.messages.rbegin(); it != sync.messages.rend(); ++it)
            chatMessages.Prepend(std::move(*it));
        if (sync.messages.empty()) historyStatus = "Beginning of history.";
    } else {
        if (mode == FetchMode::Newer) preferOlderRows = false;
        for (auto& message : sync.messages) {
            if (chatMessages.empty() || message.id > chatMessages[chatMessages.size()-1].id) {
                lastFetchedId = std::max(lastFetchedId, message.id);
                chatMessages.Append(std::move(message));
            }
        }
        if (mode == FetchMode::Newer && exhausted) {
            browsingHistory = false; chatLayout.Clear(); chatScroll.followLatest = true;
        }
        if (mode == FetchMode::Newer) historyStart = false;
    }
}
static void RefreshClientFonts() {
    const auto dpi = GetWindowScaleDPI();
    const float scale = std::max(1.0f, std::max(dpi.x, dpi.y));
    // Rasterize at the actual physical text size, including Retina/monitor changes.
    const int regularSize = std::max(32, static_cast<int>(std::ceil(16*scale)));
    const int titleSize = std::max(32, static_cast<int>(std::ceil(32*scale)));
    if (regularSize == messageRasterSize && titleSize == headingRasterSize) return;
    GuiSetFont(GetFontDefault());
    if (messageFont.texture.id && messageFont.texture.id != GetFontDefault().texture.id) UnloadFont(messageFont);
    if (headingFont.texture.id && headingFont.texture.id != GetFontDefault().texture.id) UnloadFont(headingFont);
    messageFont = LoadClientFont("SodiumReact-Regular.ttf", regularSize);
    headingFont = LoadClientFont("SodiumReact-Semibold.ttf", titleSize);
    messageRasterSize = regularSize; headingRasterSize = titleSize;
    GuiSetFont(messageFont); chatLayout.Clear();
}
static void DrawChatHistory(Rectangle bounds) {
    constexpr float fontSize = 16.0f, lineHeight = 23.0f, padding = 12.0f;
    const float contentWidth = bounds.width - 2*GuiGetStyle(DEFAULT, BORDER_WIDTH)
                               - GuiGetStyle(LISTVIEW, SCROLLBAR_WIDTH);
    const float wrapWidth = std::max(1.0f, contentWidth - padding*2);
    if (chatLayout.width != wrapWidth || chatLayout.revision != chatMessages.Revision()) {
        const bool preservePosition = !chatScroll.followLatest && chatLayout.width == wrapWidth;
        int anchorId = 0;
        float anchorY = 0;
        if (preservePosition) {
            float y = padding + chatScroll.offset;
            for (const auto& block : chatLayout.blocks) {
                if (y + block.height > 0) { anchorId = block.messageId; anchorY = y; break; }
                y += block.height;
            }
        }
        chatLayout.blocks.clear();
        chatLayout.height = padding*2;
        std::size_t rowCount = 0;
        constexpr std::size_t MaxCachedRows = 8192;
        // Build newest-first so unusually newline-heavy history cannot inflate the cache.
        for (std::size_t index = chatMessages.size(); index > 0; --index) {
            const std::size_t messageIndex = (browsingHistory && preferOlderRows) ? chatMessages.size() - index : index - 1;
            const auto& message = chatMessages[messageIndex];
            auto lines = WrapMessageText(message.sender + ": " + message.text, wrapWidth,
                [](const std::string& text) { return MeasureTextEx(messageFont, text.c_str(), fontSize, 0).x; });
            if (rowCount + lines.size() > MaxCachedRows) break;
            rowCount += lines.size();
            CachedChatBlock block{{}, UsernameColor(message.sender), (chatMessages.FirstOrdinal() + messageIndex) % 2 == 1,
                                  static_cast<float>(lines.size() + 1)*lineHeight, message.id};
            std::string remainingNickname = message.sender + ":";
            block.rows.reserve(lines.size());
            for (auto& line : lines) {
                const std::size_t nicknameBytes = std::min(line.size(), remainingNickname.size());
                block.rows.push_back({std::move(line), nicknameBytes});
                remainingNickname.erase(0, nicknameBytes);
                if (!remainingNickname.empty()) {
                    const auto next = remainingNickname.find_first_not_of(" \t");
                    remainingNickname.erase(0, next == std::string::npos ? remainingNickname.size() : next);
                }
            }
            chatLayout.height += block.height;
            chatLayout.blocks.push_back(std::move(block));
        }
        if (!(browsingHistory && preferOlderRows)) std::reverse(chatLayout.blocks.begin(), chatLayout.blocks.end());
        // Retire messages omitted by the row budget too, so pagination cursors
        // always describe the visible window and cannot skip hidden messages.
        const auto omitted = chatMessages.size() - chatLayout.blocks.size();
        if (browsingHistory && preferOlderRows) chatMessages.TrimBack(omitted);
        else chatMessages.TrimFront(omitted);
        chatLayout.firstMessage = 0;
        chatLayout.firstOrdinal = chatMessages.FirstOrdinal();
        if (preservePosition && anchorId != 0) {
            float y = padding;
            for (const auto& block : chatLayout.blocks) {
                if (block.messageId == anchorId) { chatScroll.offset = anchorY - y; break; }
                y += block.height;
            }
        }
        chatLayout.width = wrapWidth; chatLayout.revision = chatMessages.Revision();
    }
    const auto& blocks = chatLayout.blocks;
    const float contentHeight = chatLayout.height;
    const float viewportHeight = bounds.height - 2*GuiGetStyle(DEFAULT, BORDER_WIDTH);
    chatScroll.Fit(contentHeight, viewportHeight);
    Vector2 offset{0, chatScroll.offset};
    Rectangle view{};
    GuiScrollPanel(bounds, nullptr, Rectangle{0, 0, contentWidth, contentHeight}, &offset, &view);
    chatScroll.Record(offset.y, contentHeight, view.height);
    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y),
                     static_cast<int>(view.width), static_cast<int>(view.height));
    float y = view.y + offset.y + padding;
    for (const auto& block : blocks) {
        if (y + block.height > view.y && y - 6 < view.y + view.height) {
            if (block.shaded) {
                // Shade the entire message, including all wrapped lines.
                DrawRectangleRec(Rectangle{view.x, y - 6, view.width, block.height}, Color{232, 232, 232, 255});
            }
            float lineY = y;
            for (const auto& row : block.rows) {
                if (lineY + lineHeight > view.y && lineY < view.y + view.height) {
                    const Vector2 position{view.x + padding, lineY};
                    const std::string nickname = row.text.substr(0, row.nicknameBytes);
                    DrawTextEx(messageFont, nickname.c_str(), position, fontSize, 0, block.nicknameColor);
                    const float nicknameWidth = MeasureTextEx(messageFont, nickname.c_str(), fontSize, 0).x;
                    const char* body = row.text.c_str() + row.nicknameBytes;
                    DrawTextEx(messageFont, body, Vector2{position.x + nicknameWidth, position.y},
                               fontSize, 0, DARKGRAY);
                }
                lineY += lineHeight;
            }
        }
        y += block.height;
    }
    if (chatMessages.empty()) DrawClientText("No messages yet.", static_cast<int>(view.x + padding),
                                           static_cast<int>(view.y + padding), 16, GRAY);
    EndScissorMode();
}

static void DrawAuthStatus() {
    auto lines = WrapMessageText(loginStatusMessage, 420, [](const std::string& text) { return MeasureTextEx(messageFont, text.c_str(), 14, 0).x; });
    float y = SERVER_EDIT.y + 143;
    for (const auto& line : lines) {
        DrawClientText(line.c_str(), static_cast<int>(SERVER_EDIT.x - 100), static_cast<int>(y), 14, RED); y += 18;
    }
}
static void drawOnboarding() {
    DrawClientText("Sodium Onboarding", 130 + static_cast<int>(SERVER_EDIT.x - 150), 30 + static_cast<int>(SERVER_EDIT.y - 100), 32, DARKGRAY);

    DrawClientText("Server Host:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 105 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    if (GuiTextBox(SERVER_EDIT, serverInputBuffer, 128, serverInputEditMode)) {
        serverInputEditMode = false;
        connected_server = std::string(serverInputBuffer); // Dynamically sync
    }

    DrawClientText("Username:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 155 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    GuiTextBox(USERNAME_EDIT, usernameBuffer, 64, usernameEditMode);

    DrawClientText("Password:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 205 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    const int oldText = GuiGetStyle(TEXTBOX, TEXT_COLOR_NORMAL);
    const int oldFocus = GuiGetStyle(TEXTBOX, TEXT_COLOR_FOCUSED);
    const int oldPressed = GuiGetStyle(TEXTBOX, TEXT_COLOR_PRESSED);
    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, 0); GuiSetStyle(TEXTBOX, TEXT_COLOR_FOCUSED, 0); GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, 0);
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);
    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, oldText); GuiSetStyle(TEXTBOX, TEXT_COLOR_FOCUSED, oldFocus); GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, oldPressed);
    std::string mask(std::strlen(passwordBuffer), '*');
    BeginScissorMode(static_cast<int>(PASSPHRASE_EDIT.x + 8), static_cast<int>(PASSPHRASE_EDIT.y), static_cast<int>(PASSPHRASE_EDIT.width - 16), static_cast<int>(PASSPHRASE_EDIT.height));
    DrawClientText(mask.c_str(), static_cast<int>(PASSPHRASE_EDIT.x + 8), static_cast<int>(PASSPHRASE_EDIT.y + 7), 14, DARKGRAY);
    EndScissorMode();

    if (isNetworkRequestPending || keyringPending) {
        DrawClientText(keyringPending ? "Updating keyring..." : "Connecting...", 170 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 16, ORANGE);
    } else {
        DrawAuthStatus();

        if (GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 215, 140, 40}, "Register")) {
            if (std::strlen(usernameBuffer) > 0 && std::strlen(passwordBuffer) > 0 && std::strlen(serverInputBuffer) >
                0) {
                isNetworkRequestPending = true;
                connected_server = std::string(serverInputBuffer); // Ensure sync before fetching
                while (!connected_server.empty() && connected_server.back() == '/') connected_server.pop_back();
                loginFuture = std::async(std::launch::async, ExecuteAuth, connected_server,
                                         std::string(usernameBuffer), std::string(passwordBuffer), true);
                std::memset(passwordBuffer, 0, sizeof(passwordBuffer));
            } else {
                loginStatusMessage = "Fields cannot be empty!";
            }
        }
        if (GuiButton(Rectangle{SERVER_EDIT.x + 110, SERVER_EDIT.y + 215, 140, 40}, "Go to Sign In")) {
            currentScreen = SCREEN_SIGNIN;
            loginStatusMessage = "Please log in.";
        }
    }
}

static void drawSignin() {
    DrawClientText("Sodium Sign In", 160 + static_cast<int>(SERVER_EDIT.x - 150), 30 + static_cast<int>(SERVER_EDIT.y - 100), 32, DARKGRAY);
    DrawClientText("Server Host:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 105 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    if (GuiTextBox(SERVER_EDIT, serverInputBuffer, 128, serverInputEditMode)) {
        serverInputEditMode = false;
        connected_server = std::string(serverInputBuffer); // Dynamically sync
    }
    DrawClientText("Username:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 155 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    GuiTextBox(USERNAME_EDIT, usernameBuffer, 64, usernameEditMode);
    DrawClientText("Password:", 50 + static_cast<int>(SERVER_EDIT.x - 150), 205 + static_cast<int>(SERVER_EDIT.y - 100), 16, DARKGRAY);
    const int oldText = GuiGetStyle(TEXTBOX, TEXT_COLOR_NORMAL);
    const int oldFocus = GuiGetStyle(TEXTBOX, TEXT_COLOR_FOCUSED);
    const int oldPressed = GuiGetStyle(TEXTBOX, TEXT_COLOR_PRESSED);
    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, 0); GuiSetStyle(TEXTBOX, TEXT_COLOR_FOCUSED, 0); GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, 0);
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);
    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, oldText); GuiSetStyle(TEXTBOX, TEXT_COLOR_FOCUSED, oldFocus); GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, oldPressed);
    std::string mask(std::strlen(passwordBuffer), '*');
    BeginScissorMode(static_cast<int>(PASSPHRASE_EDIT.x + 8), static_cast<int>(PASSPHRASE_EDIT.y), static_cast<int>(PASSPHRASE_EDIT.width - 16), static_cast<int>(PASSPHRASE_EDIT.height));
    DrawClientText(mask.c_str(), static_cast<int>(PASSPHRASE_EDIT.x + 8), static_cast<int>(PASSPHRASE_EDIT.y + 7), 14, DARKGRAY);
    EndScissorMode();
    if (isNetworkRequestPending || keyringPending) {
        DrawClientText(keyringPending ? "Updating keyring..." : "Connecting...", 190 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 16, ORANGE);
    } else {
        DrawAuthStatus();
        if (GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 215, 140, 40}, "Login")) {
            if (std::strlen(usernameBuffer) > 0 && std::strlen(passwordBuffer) > 0 && std::strlen(serverInputBuffer) >
                0) {
                isNetworkRequestPending = true;
                connected_server = std::string(serverInputBuffer); // Ensure sync before fetching
                while (!connected_server.empty() && connected_server.back() == '/') connected_server.pop_back();
                loginFuture = std::async(std::launch::async, ExecuteAuth, connected_server,
                                         std::string(usernameBuffer), std::string(passwordBuffer), false);
                std::memset(passwordBuffer, 0, sizeof(passwordBuffer));
            } else {
                loginStatusMessage = "Fields cannot be empty!";
            }
        }
        if (GuiButton(Rectangle{SERVER_EDIT.x + 110, SERVER_EDIT.y + 215, 140, 40}, "Register New")) {
            currentScreen = SCREEN_ONBOARD;
            loginStatusMessage = "Please fill out details.";
        }
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(600, 480, "Sodium Client");
    SetWindowMinSize(600, 480);
    RefreshClientFonts();
    GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, 0xBCC4CCFF);
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, 0xE8EDF2FF);
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, 0xDCE8F4FF);
    GuiSetStyle(DEFAULT, TEXT_PADDING, 8);
    SetTargetFPS(30);
    isNetworkRequestPending = true;
    loginStatusMessage = "Restoring saved session...";
    loginFuture = std::async(std::launch::async, RestoreSession);
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        RefreshClientFonts();
        if (keyringPending && keyringFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto status = keyringFuture.get(); keyringPending = false;
            if (keyringDeleting) {
                deleteFailed = status != StoreStatus::Ok;
                loginStatusMessage = deleteFailed ? "Signed out; keyring deletion failed. Retry removal." : "Saved session removed. Please log in.";
            } else {
                memoryOnly = status != StoreStatus::Ok;
                sessionStatus = memoryOnly ? "Keyring unavailable; session is remembered only for this run." : "Session saved in OS keyring.";
            }
        }
        // Center the authentication form as the window grows.
        const float formX = (GetScreenWidth() - 600)*0.5f;
        const float formY = (GetScreenHeight() - 480)*0.5f;
        SERVER_EDIT = Rectangle{150 + formX, 100 + formY, 250, 30};
        USERNAME_EDIT = Rectangle{150 + formX, 150 + formY, 200, 30};
        PASSPHRASE_EDIT = Rectangle{150 + formX, 200 + formY, 200, 30};
        if (sendFuture.valid() && sendFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            sendFuture.get(); fetchTimer = FETCH_INTERVAL;
        }
        // Apply fetched messages only on the UI thread, and only to their session.
        if (isSyncRequestPending && syncFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto sync = syncFuture.get();
            isSyncRequestPending = false;
            if (syncGeneration == sessionGeneration && currentScreen == SCREEN_CHAT) {
                sessionStatus = sync.networkFailure ? "Connection interrupted; retrying..." : memoryOnly ? "Connected; keyring unavailable, session kept only for this run." : "Connected; session remembered in OS keyring.";
                if (sync.invalid) {
                    ++sessionGeneration; sessionToken.clear(); loggedInUser.clear();
                    currentScreen = SCREEN_SIGNIN; loginStatusMessage = "Token expired. Please log in.";
                    ClearChatHistory();
                    if (!keyringPending) RemoveSavedSession();
                }
                if (!sync.invalid) ApplyMessagePage(sync, syncMode);
            }
        }
        // --- 1. Background Async Authentication Handler ---
        if (isNetworkRequestPending) {
            if (loginFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const auto result = loginFuture.get();
                loginStatusMessage = result.message; retrySaved = result.retrySaved;
                if (result.session) {
                    connected_server = result.session->server;
                    std::snprintf(serverInputBuffer, sizeof(serverInputBuffer), "%s", connected_server.c_str());
                    std::snprintf(usernameBuffer, sizeof(usernameBuffer), "%s", result.session->username.c_str());
                }
                isNetworkRequestPending = false;
                if (result.success) {
                    memoryOnly = false; deleteFailed = false;
                    sessionToken = result.session->token; loggedInUser = result.session->username;
                    if (result.message.empty()) {
                        keyringPending = true; keyringDeleting = false;
                        keyringFuture = std::async(std::launch::async, SaveSession, *result.session);
                        sessionStatus = "Saving session in keyring...";
                    } else sessionStatus = result.message;
                    currentScreen = SCREEN_CHAT;
                    ClearChatHistory();
                    lastFetchedId = 0;
                    ++sessionGeneration;
                    chatScroll = ChatScrollState{};
                    chatInputEditMode = true;
                    fetchTimer = FETCH_INTERVAL;
                }
            }
        }
        // --- 2. Periodic Message Loop Background Sync ---
        if (currentScreen == SCREEN_CHAT) {
            fetchTimer += dt;
            if (fetchTimer >= FETCH_INTERVAL && !browsingHistory) BeginMessageFetch(FetchMode::Live);
        }
        // --- 3. Click Calculation Handlers (Focus Engine) ---
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !isNetworkRequestPending && !keyringPending) {
            const Vector2 mousePos = GetMousePosition();
            if (currentScreen == SCREEN_ONBOARD || currentScreen == SCREEN_SIGNIN) {
                serverInputEditMode = CheckCollisionPointRec(mousePos, SERVER_EDIT); // Added focus node
                usernameEditMode = CheckCollisionPointRec(mousePos, USERNAME_EDIT);
                passwordEditMode = CheckCollisionPointRec(mousePos, PASSPHRASE_EDIT);
            } else if (currentScreen == SCREEN_CHAT) {
                if (!CheckCollisionPointRec(mousePos, ChatSendBounds())) {
                    chatInputEditMode = CheckCollisionPointRec(mousePos, ChatInputBounds());
                }
            }
        }
        // --- 4. Presentation / Drawing Pipeline ---
        BeginDrawing();
        ClearBackground(Color{247, 249, 251, 255});
        if (currentScreen == SCREEN_ONBOARD) {
            drawOnboarding();
        } else if (currentScreen == SCREEN_SIGNIN) {
            drawSignin();
            if (!isNetworkRequestPending && !keyringPending && retrySaved && GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 263, 300, 30}, "Retry saved session")) {
                isNetworkRequestPending = true; loginFuture = std::async(std::launch::async, RestoreSession);
            }
            if (!isNetworkRequestPending && !keyringPending && deleteFailed && GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 263, 300, 30}, "Retry keyring removal")) RemoveSavedSession();
        } else if (currentScreen == SCREEN_CHAT) {
            const float width = static_cast<float>(GetScreenWidth());
            const float height = static_cast<float>(GetScreenHeight());
            DrawChatHistory(Rectangle{20, 106, width - 40, height - 176});
            const std::string connectionLabel = browsingHistory ? "Browsing history; live updates paused. " + sessionStatus : sessionStatus;
            DrawClientText(connectionLabel.c_str(), 20, 49, 13, GRAY);
            if (isSyncRequestPending || keyringPending || historyStart || chatMessages.empty()) GuiDisable();
            const bool older = GuiButton(Rectangle{20,73,95,26}, "Older"); GuiEnable();
            if (isSyncRequestPending || keyringPending || !browsingHistory) GuiDisable();
            const bool newer = GuiButton(Rectangle{123,73,95,26}, "Newer");
            const bool latest = GuiButton(Rectangle{226,73,95,26}, "Latest"); GuiEnable();
            if (older) BeginMessageFetch(FetchMode::Older);
            if (newer) BeginMessageFetch(FetchMode::Newer);
            if (latest) BeginMessageFetch(FetchMode::Latest);
            DrawClientText((isSyncRequestPending && syncMode != FetchMode::Live ? "Loading..." : historyStatus.c_str()), 331,80,13,GRAY);
            // A deliberate upward wheel gesture at the top loads the preceding page.
            if (chatScroll.offset >= -2 && GetMouseWheelMove() > 0 &&
                CheckCollisionPointRec(GetMousePosition(), Rectangle{20,106,width-40,height-176}) && !historyStart)
                BeginMessageFetch(FetchMode::Older);
            if (browsingHistory && chatScroll.followLatest && GetMouseWheelMove() < 0 &&
                CheckCollisionPointRec(GetMousePosition(), Rectangle{20,106,width-40,height-176}))
                BeginMessageFetch(FetchMode::Newer);
            // Clip long account names before the Logout button.
            BeginScissorMode(20, 15, GetScreenWidth() - 140, 35);
            DrawClientText(TextFormat("Sodium Client - Logged in as: %s", loggedInUser.c_str()), 20, 20, 22, DARKGRAY);
            EndScissorMode();
            const bool submitKey = chatInputEditMode &&
                                   (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER));
            if (GuiTextBox(ChatInputBounds(), chatInputBuffer, sizeof(chatInputBuffer), chatInputEditMode)) {
                if (!submitKey) chatInputEditMode = !chatInputEditMode;
            }
            if (sendFuture.valid()) GuiDisable();
            const bool submitButton = GuiButton(ChatSendBounds(), sendFuture.valid() ? "Sending..." : "Send");
            GuiEnable();
            if (submitKey || submitButton) SubmitChatMessage();
            if (keyringPending) GuiDisable();
            const bool logout = GuiButton(Rectangle{width - 110, 15, 90, 30}, "Logout");
            GuiEnable();
            if (logout) {
                RemoveSavedSession();
                retrySaved = false;
                ClearChatHistory(); loggedInUser.clear();
                ++sessionGeneration;
                chatInputEditMode = false;
                std::memset(chatInputBuffer, 0, sizeof(chatInputBuffer));
                sessionToken = "";
                std::memset(usernameBuffer, 0, sizeof(usernameBuffer));
                std::memset(passwordBuffer, 0, sizeof(passwordBuffer));
                loginStatusMessage = "Logged out.";
                currentScreen = SCREEN_SIGNIN;
            }
        }
        EndDrawing();
    }
    if (keyringFuture.valid()) keyringFuture.wait();
    if (loginFuture.valid()) loginFuture.wait();
    if (syncFuture.valid()) syncFuture.wait();
    if (sendFuture.valid()) sendFuture.wait();
    GuiSetFont(GetFontDefault());
    if (headingFont.texture.id != GetFontDefault().texture.id) UnloadFont(headingFont);
    if (messageFont.texture.id != GetFontDefault().texture.id) UnloadFont(messageFont);
    CloseWindow();
    curl_global_cleanup();
    return 0;
}
