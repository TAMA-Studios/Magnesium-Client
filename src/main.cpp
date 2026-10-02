#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "../include/json.hpp"
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <future>
#include <cstring>
#include <vector>

using json = nlohmann::json;



namespace {
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
    std::future<void> syncFuture;
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
static void GetMessagesBackground() {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    std::string responseBuffer;
    curl_slist *headers = nullptr;
    const std::string authHeader = "Authorization: Bearer " + sessionToken;
    headers = curl_slist_append(headers, authHeader.c_str());

    const std::string url = connected_server + MESSAGES_ENDPOINT + "?after=" + std::to_string(lastFetchedId);

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

                    if (msg.id > lastFetchedId) {
                        chatMessages.push_back(msg);
                        lastFetchedId = msg.id;
                    }
                }
            }
        } catch (...) {
        }
    }
}

static void SendMessageBackground(const std::string& text) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    json outJson;
    outJson["text"] = text;
    const std::string jsonStr = outJson.dump();
    std::string responseBuffer;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string authHeader = "Authorization: Bearer " + sessionToken;
    headers = curl_slist_append(headers, authHeader.c_str());

    const std::string url = connected_server + MESSAGES_ENDPOINT;
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

static void drawOnboarding() {
    DrawText("Sodium Onboarding", 130, 30, 32, DARKGRAY);

    DrawText("Server Host:", 50, 105, 16, DARKGRAY);
    if (GuiTextBox(SERVER_EDIT, serverInputBuffer, 128, serverInputEditMode)) {
        serverInputEditMode = false;
        connected_server = std::string(serverInputBuffer); // Dynamically sync
    }

    DrawText("Username:", 50, 155, 16, DARKGRAY);
    GuiTextBox(USERNAME_EDIT, usernameBuffer, 64, usernameEditMode);

    DrawText("Password:", 50, 205, 16, DARKGRAY);
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);

    if (isNetworkRequestPending) {
        DrawText("Registering account...", 170, 255, 16, ORANGE);
    } else {
        DrawText(loginStatusMessage.c_str(), 150, 255, 14, RED);

        if (GuiButton((Rectangle){100, 300, 140, 40}, "Register")) {
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
        if (GuiButton((Rectangle){260, 300, 140, 40}, "Go to Sign In")) {
            currentScreen = SCREEN_SIGNIN;
            loginStatusMessage = "Please log in.";
        }
    }
}

static void drawSignin() {
    DrawText("Sodium Sign In", 160, 30, 32, DARKGRAY);
    DrawText("Server Host:", 50, 105, 16, DARKGRAY);
    if (GuiTextBox(SERVER_EDIT, serverInputBuffer, 128, serverInputEditMode)) {
        serverInputEditMode = false;
        connected_server = std::string(serverInputBuffer); // Dynamically sync
    }
    DrawText("Username:", 50, 155, 16, DARKGRAY);
    GuiTextBox(USERNAME_EDIT, usernameBuffer, 64, usernameEditMode);
    DrawText("Password:", 50, 205, 16, DARKGRAY);
    GuiTextBox(PASSPHRASE_EDIT, passwordBuffer, 64, passwordEditMode);
    if (isNetworkRequestPending) {
        DrawText("Signing in...", 190, 255, 16, ORANGE);
    } else {
        DrawText(loginStatusMessage.c_str(), 150, 255, 14, RED);
        if (GuiButton((Rectangle){100, 300, 140, 40}, "Login")) {
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
        if (GuiButton((Rectangle){260, 300, 140, 40}, "Register New")) {
            currentScreen = SCREEN_ONBOARD;
            loginStatusMessage = "Please fill out details.";
        }
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    InitWindow(600, 480, "Sodium Client");
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        // --- 1. Background Async Authentication Handler ---
        if (isNetworkRequestPending) {
            if (loginFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const bool success = loginFuture.get();
                isNetworkRequestPending = false;
                if (success) {
                    currentScreen = SCREEN_CHAT;
                    chatMessages.clear();
                    lastFetchedId = 0;
                }
            }
        }
        // --- 2. Periodic Message Loop Background Sync ---
        if (currentScreen == SCREEN_CHAT) {
            fetchTimer += dt;
            if (fetchTimer >= FETCH_INTERVAL && !isSyncRequestPending) {
                isSyncRequestPending = true;
                syncFuture = std::async(std::launch::async, GetMessagesBackground);
                fetchTimer = 0.0f;
            }
            if (isSyncRequestPending) {
                if (syncFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    syncFuture.get();
                    isSyncRequestPending = false;
                }
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
                chatInputEditMode = CheckCollisionPointRec(mousePos, (Rectangle){20, 430, 460, 30});
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
            DrawRectangle(15, 60, 570, 350, (Color){245, 245, 245, 255});
            DrawRectangleLines(15, 60, 570, 350, LIGHTGRAY);
            DrawText(TextFormat("Sodium Client - Logged in as: %s", loggedInUser.c_str()), 20, 20, 22, DARKGRAY);
            int rowsDrawn = 0;
            for (int i = static_cast<int>(chatMessages.size()) - 1; i >= 0 && rowsDrawn < 16; i--) {
                std::string logLine = chatMessages[i].sender + ": " + chatMessages[i].text;
                const Color textColor = (chatMessages[i].sender == loggedInUser) ? BLUE : DARKGRAY;
                DrawText(logLine.c_str(), 30, 380 - (rowsDrawn * 20), 16, textColor);
                rowsDrawn++;
            }
            if (GuiTextBox((Rectangle){20, 430, 460, 30}, chatInputBuffer, 256, chatInputEditMode)) {
                if (std::strlen(chatInputBuffer) > 0) {
                    std::async(std::launch::async, SendMessageBackground, std::string(chatInputBuffer));
                    std::memset(chatInputBuffer, 0, sizeof(chatInputBuffer));
                    fetchTimer = FETCH_INTERVAL;
                }
                chatInputEditMode = false;
            }
            if (GuiButton((Rectangle){490, 430, 90, 30}, "Send")) {
                if (std::strlen(chatInputBuffer) > 0) {
                    std::async(std::launch::async, SendMessageBackground, std::string(chatInputBuffer));
                    std::memset(chatInputBuffer, 0, sizeof(chatInputBuffer));
                    fetchTimer = FETCH_INTERVAL;
                }
                chatInputEditMode = false;
            }
            if (GuiButton((Rectangle){490, 15, 90, 30}, "Logout")) {
                sessionToken = "";
                std::memset(usernameBuffer, 0, sizeof(usernameBuffer));
                std::memset(passwordBuffer, 0, sizeof(passwordBuffer));
                loginStatusMessage = "Logged out.";
                currentScreen = SCREEN_SIGNIN;
            }
        }
        EndDrawing();
    }
    CloseWindow();
    curl_global_cleanup();
    return 0;
}
