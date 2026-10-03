#include "raylib.h"
#include "sodium_react_codepoints.h"
#include "chat_layout.h"
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

using json = nlohmann::json;



namespace {
    Font messageFont{};
    Font headingFont{};

    static Font LoadClientFont(const char *fileName) {
        const std::string path = std::string(GetApplicationDirectory()) + "assets/fonts/" + fileName;
        if (!FileExists(path.c_str())) {
            TraceLog(LOG_WARNING, "Sodium React font missing: %s", path.c_str());
            return GetFontDefault();
        }
        const int count = static_cast<int>(sizeof(SODIUM_REACT_CODEPOINTS) / sizeof(SODIUM_REACT_CODEPOINTS[0]));
        Font font = LoadFontEx(path.c_str(), 64, SODIUM_REACT_CODEPOINTS, count);
        if (font.texture.id != GetFontDefault().texture.id) {
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        }
        return font;
    }

    static void DrawClientText(const char *text, int x, int y, int size, Color color) {
        const Font font = (size >= 22) ? headingFont : messageFont;
        DrawTextEx(font, text, Vector2{static_cast<float>(x), static_cast<float>(y)},
                   static_cast<float>(size), 0.0f, color);
    }

    struct Message {
        int id{};
        double created{};
        std::string sender;
        std::string text;
    };
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
    std::vector<Message> chatMessages;
    int lastFetchedId = 0;
    float fetchTimer = 0.0f;
    constexpr float FETCH_INTERVAL = 1.0f;

    // --- Background Worker Variables ---
    std::future<bool> loginFuture;
    std::future<std::vector<Message>> syncFuture;
    std::vector<std::future<void>> sendFutures;
    unsigned int sessionGeneration = 0, syncGeneration = 0;
    ChatScrollState chatScroll;
    bool isSyncRequestPending = false;
}


const std::string ONBOARDING_ENDPOINT = "/sodium/v1/users/onboarding";
const std::string LOGIN_ENDPOINT = "/sodium/v1/users/login";
const std::string MESSAGES_ENDPOINT = "/sodium/v1/messages";


// --- libcURL Callback to capture data ---
static size_t WriteCallback(void *contents, const size_t size, size_t nmemb, std::string *s) {
    const size_t newLength = size * nmemb;
    try {
        s->append(static_cast<char *>(contents), newLength);
    } catch ([[maybe_unused]] std::bad_alloc &e) {
        return 0;
    }
    return newLength;
}

// --- The Core Auth cURL Requests ---
static bool ExecuteOnboarding(const std::string& username, const std::string& password) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        loginStatusMessage = "cURL Initialization Failed.";
        return false;
    }

    json loginJson;
    loginJson["username"] = username;
    loginJson["password"] = password;
    loginJson["email"] = "";
    const std::string jsonStr = loginJson.dump();

    std::string responseBuffer;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    const std::string url = connected_server + ONBOARDING_ENDPOINT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        loginStatusMessage = "Server Connection Failed.";
        return false;
    }

    try {
        if (json responseJson = json::parse(responseBuffer); responseJson.contains("ok") && responseJson["ok"].get<bool>() && responseJson.contains("token")) {
            sessionToken = responseJson["token"].get<std::string>();
            loggedInUser = responseJson["username"].get<std::string>();
            return true;
        } else if (responseJson.contains("error")) {
            loginStatusMessage = responseJson["error"].get<std::string>();
        } else {
            loginStatusMessage = "Invalid Server Response.";
        }
    } catch (json::parse_error &e) {
        loginStatusMessage = "JSON Parsing Error.";
    }
    return false;
}

static bool ExecuteSignin(const std::string& username, const std::string& password) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        loginStatusMessage = "cURL Initialization Failed.";
        return false;
    }

    json loginJson;
    loginJson["username"] = username;
    loginJson["password"] = password;
    const std::string jsonStr = loginJson.dump();

    std::string responseBuffer;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    const std::string url = connected_server + LOGIN_ENDPOINT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        loginStatusMessage = "Server Connection Failed.";
        return false;
    }

    try {
        if (json responseJson = json::parse(responseBuffer); responseJson.contains("ok") && responseJson["ok"].get<bool>() && responseJson.contains("token")) {
            sessionToken = responseJson["token"].get<std::string>();
            loggedInUser = responseJson["username"].get<std::string>();
            return true;
        } else if (responseJson.contains("error")) {
            loginStatusMessage = responseJson["error"].get<std::string>();
        } else {
            loginStatusMessage = "Invalid Server Response.";
        }
    } catch (json::parse_error &e) {
        loginStatusMessage = "JSON Parsing Error.";
    }
    return false;
}

// --- Asynchronous Sync Mechanics ---
static std::vector<Message> GetMessagesBackground(std::string server, std::string token, int after) {
    std::vector<Message> messages;
    CURL *curl = curl_easy_init();
    if (!curl) return messages;

    std::string responseBuffer;
    curl_slist *headers = nullptr;
    const std::string authHeader = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authHeader.c_str());

    const std::string url = server + MESSAGES_ENDPOINT + "?after=" + std::to_string(after);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);

    CURLcode res = curl_easy_perform(curl);
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

                    if (msg.id > after) messages.push_back(std::move(msg));
                }
            }
        } catch (...) {
        }
    }
    return messages;
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

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

static void SubmitChatMessage() {
    if (std::strlen(chatInputBuffer) > 0) {
        // Keep the future alive so sending does not block the drawing loop.
        sendFutures.push_back(std::async(std::launch::async, SendMessageBackground,
                                        std::string(chatInputBuffer), connected_server, sessionToken));
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

static void DrawChatHistory(Rectangle bounds) {
    constexpr float fontSize = 16.0f, lineHeight = 23.0f, padding = 12.0f;
    // Reserve room for a vertical scrollbar even when it is not visible yet.
    const float contentWidth = bounds.width - 2*GuiGetStyle(DEFAULT, BORDER_WIDTH)
                               - GuiGetStyle(LISTVIEW, SCROLLBAR_WIDTH);
    const float wrapWidth = std::max(1.0f, contentWidth - padding*2);
    struct Row { std::string text; std::string nickname; };
    struct Block { std::vector<Row> rows; Color nicknameColor; bool shaded; float height; };
    std::vector<Block> blocks;
    float contentHeight = padding*2;
    for (std::size_t index = 0; index < chatMessages.size(); ++index) {
        const auto& message = chatMessages[index];
        const auto lines = WrapMessageText(message.sender + ": " + message.text, wrapWidth,
            [](const std::string& text) { return MeasureTextEx(messageFont, text.c_str(), fontSize, 0).x; });
        Block block{{}, UsernameColor(message.sender), index % 2 == 1,
                    static_cast<float>(lines.size() + 1)*lineHeight};
        // A long nickname can itself wrap. Color only its portion of each line.
        std::string remainingNickname = message.sender + ":";
        for (const auto& line : lines) {
            const std::size_t nicknameBytes = std::min(line.size(), remainingNickname.size());
            block.rows.push_back({line, line.substr(0, nicknameBytes)});
            remainingNickname.erase(0, nicknameBytes);
            if (!remainingNickname.empty()) {
                const auto next = remainingNickname.find_first_not_of(" \t");
                remainingNickname.erase(0, next == std::string::npos ? remainingNickname.size() : next);
            }
        }
        contentHeight += block.height;
        blocks.push_back(std::move(block));
    }
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
                    DrawTextEx(messageFont, row.nickname.c_str(), position, fontSize, 0, block.nicknameColor);
                    const float nicknameWidth = MeasureTextEx(messageFont, row.nickname.c_str(), fontSize, 0).x;
                    const std::string body = row.text.substr(row.nickname.size());
                    DrawTextEx(messageFont, body.c_str(), Vector2{position.x + nicknameWidth, position.y},
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
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);

    if (isNetworkRequestPending) {
        DrawClientText("Registering account...", 170 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 16, ORANGE);
    } else {
        DrawClientText(loginStatusMessage.c_str(), 150 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 14, RED);

        if (GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 200, 140, 40}, "Register")) {
            if (std::strlen(usernameBuffer) > 0 && std::strlen(passwordBuffer) > 0 && std::strlen(serverInputBuffer) >
                0) {
                isNetworkRequestPending = true;
                connected_server = std::string(serverInputBuffer); // Ensure sync before fetching
                loginFuture = std::async(std::launch::async, ExecuteOnboarding,
                                         std::string(usernameBuffer),
                                         std::string(passwordBuffer));
            } else {
                loginStatusMessage = "Fields cannot be empty!";
            }
        }
        if (GuiButton(Rectangle{SERVER_EDIT.x + 110, SERVER_EDIT.y + 200, 140, 40}, "Go to Sign In")) {
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
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);
    if (isNetworkRequestPending) {
        DrawClientText("Signing in...", 190 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 16, ORANGE);
    } else {
        DrawClientText(loginStatusMessage.c_str(), 150 + static_cast<int>(SERVER_EDIT.x - 150), 255 + static_cast<int>(SERVER_EDIT.y - 100), 14, RED);
        if (GuiButton(Rectangle{SERVER_EDIT.x - 50, SERVER_EDIT.y + 200, 140, 40}, "Login")) {
            if (std::strlen(usernameBuffer) > 0 && std::strlen(passwordBuffer) > 0 && std::strlen(serverInputBuffer) >
                0) {
                isNetworkRequestPending = true;
                connected_server = std::string(serverInputBuffer); // Ensure sync before fetching
                loginFuture = std::async(std::launch::async, ExecuteSignin,
                                         std::string(usernameBuffer),
                                         std::string(passwordBuffer));
            } else {
                loginStatusMessage = "Fields cannot be empty!";
            }
        }
        if (GuiButton(Rectangle{SERVER_EDIT.x + 110, SERVER_EDIT.y + 200, 140, 40}, "Register New")) {
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
    messageFont = LoadClientFont("SodiumReact-Regular.ttf");
    headingFont = LoadClientFont("SodiumReact-Semibold.ttf");
    GuiSetFont(messageFont);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        // Center the authentication form as the window grows.
        const float formX = (GetScreenWidth() - 600)*0.5f;
        const float formY = (GetScreenHeight() - 480)*0.5f;
        SERVER_EDIT = Rectangle{150 + formX, 100 + formY, 250, 30};
        USERNAME_EDIT = Rectangle{150 + formX, 150 + formY, 200, 30};
        PASSPHRASE_EDIT = Rectangle{150 + formX, 200 + formY, 200, 30};
        for (auto it = sendFutures.begin(); it != sendFutures.end();) {
            if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                it->get(); it = sendFutures.erase(it); fetchTimer = FETCH_INTERVAL;
            } else ++it;
        }
        // Apply fetched messages only on the UI thread, and only to their session.
        if (isSyncRequestPending && syncFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto messages = syncFuture.get();
            isSyncRequestPending = false;
            if (syncGeneration == sessionGeneration && currentScreen == SCREEN_CHAT) {
                for (auto& message : messages) {
                    if (message.id > lastFetchedId) {
                        lastFetchedId = message.id;
                        chatMessages.push_back(std::move(message));
                    }
                }
            }
        }
        // --- 1. Background Async Authentication Handler ---
        if (isNetworkRequestPending) {
            if (loginFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const bool success = loginFuture.get();
                isNetworkRequestPending = false;
                if (success) {
                    currentScreen = SCREEN_CHAT;
                    chatMessages.clear();
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
            if (fetchTimer >= FETCH_INTERVAL && !isSyncRequestPending) {
                isSyncRequestPending = true;
                syncGeneration = sessionGeneration;
                syncFuture = std::async(std::launch::async, GetMessagesBackground,
                                        connected_server, sessionToken, lastFetchedId);
                fetchTimer = 0.0f;
            }
        }
        // --- 3. Click Calculation Handlers (Focus Engine) ---
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !isNetworkRequestPending) {
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
        ClearBackground(RAYWHITE);
        if (currentScreen == SCREEN_ONBOARD) {
            drawOnboarding();
        } else if (currentScreen == SCREEN_SIGNIN) {
            drawSignin();
        } else if (currentScreen == SCREEN_CHAT) {
            const float width = static_cast<float>(GetScreenWidth());
            const float height = static_cast<float>(GetScreenHeight());
            DrawChatHistory(Rectangle{15, 60, width - 30, height - 130});
            // Clip long account names before the Logout button.
            BeginScissorMode(20, 15, GetScreenWidth() - 140, 35);
            DrawClientText(TextFormat("Sodium Client - Logged in as: %s", loggedInUser.c_str()), 20, 20, 22, DARKGRAY);
            EndScissorMode();
            const bool submitKey = chatInputEditMode &&
                                   (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER));
            if (GuiTextBox(ChatInputBounds(), chatInputBuffer, sizeof(chatInputBuffer), chatInputEditMode)) {
                if (!submitKey) chatInputEditMode = !chatInputEditMode;
            }
            const bool submitButton = GuiButton(ChatSendBounds(), "Send");
            if (submitKey || submitButton) SubmitChatMessage();
            if (GuiButton(Rectangle{width - 110, 15, 90, 30}, "Logout")) {
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
    if (loginFuture.valid()) loginFuture.wait();
    if (syncFuture.valid()) syncFuture.wait();
    for (auto& send : sendFutures) send.wait();
    GuiSetFont(GetFontDefault());
    if (headingFont.texture.id != GetFontDefault().texture.id) UnloadFont(headingFont);
    if (messageFont.texture.id != GetFontDefault().texture.id) UnloadFont(messageFont);
    CloseWindow();
    curl_global_cleanup();
    return 0;
}
