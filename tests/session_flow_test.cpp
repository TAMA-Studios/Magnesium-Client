#define main magnesium_ui_main
#include "../src/main.cpp"
#undef main
#include <stdexcept>
namespace { StoreResult fakeStore{StoreStatus::Missing, {}}; StoreStatus deleteStatus = StoreStatus::Ok; int deletes = 0; }
StoreResult LoadSession() { return fakeStore; }
StoreStatus SaveSession(const SavedSession&) { return StoreStatus::Ok; }
StoreStatus DeleteSession() { ++deletes; return deleteStatus; }
static void check(bool ok) { if (!ok) throw std::runtime_error("Session flow assertion failed"); }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    curl_global_init(CURL_GLOBAL_ALL);
    std::string base = argv[1];
    check(!RestoreSession().success && deletes == 0);
    fakeStore = {StoreStatus::Unavailable, {}};
    check(RestoreSession().retrySaved && deletes == 0);
    fakeStore = {StoreStatus::Invalid, {}};
    check(!RestoreSession().success && deletes == 1);
    deleteStatus = StoreStatus::Unavailable;
    check(RestoreSession().retrySaved);
    deleteStatus = StoreStatus::Ok;
    auto saved = [&](const char* path) { fakeStore = {StoreStatus::Ok, SavedSession{base + path, "alice", "test-token"}}; };
    saved("/valid"); auto restored = RestoreSession();
    check(restored.success && restored.session->username == "alice");
    const int before = deletes;
    saved("/offline"); check(RestoreSession().retrySaved && deletes == before);
    saved("/malformed"); check(RestoreSession().retrySaved && deletes == before);
    saved("/mismatch"); check(RestoreSession().retrySaved && deletes == before);
    saved("/invalid"); check(!RestoreSession().success && deletes == before + 1);
    fakeStore = {StoreStatus::Ok, SavedSession{"http://127.0.0.1:1", "alice", "test-token"}};
    check(RestoreSession().retrySaved && deletes == before + 1);
    check(ExecuteAuth(base + "/valid", "alice", "secret", false).success);
    check(ExecuteAuth(base + "/valid", "alice", "secret", true).success);
    check(!ExecuteAuth(base + "/valid", "alice", "wrong", false).success);
    check(!ExecuteAuth(base + "/legacy", "alice", "secret", false).success);
    auto recent = GetMessagesBackground(base + "/recent", "test-token", 0);
    check(recent.messages.size() == 1 && recent.messages[0].id == 900);
    recent = GetMessagesBackground(base + "/recent", "test-token", 900);
    check(recent.messages.size() == 1 && recent.messages[0].id == 901);
    auto sync = GetMessagesBackground(base + "/invalid", "test-token", 0);
    check(sync.invalid);
    sync = GetMessagesBackground(base + "/offline", "test-token", 0);
    check(sync.networkFailure && !sync.invalid);
    // Browse beyond the retained window, recover every older row, then return live.
    ClearChatHistory(); lastFetchedId = 0;
    auto page = GetMessagesBackground(base + "/pages", "test-token", 0);
    ApplyMessagePage(page, FetchMode::Latest);
    check(chatMessages[0].id == 1251 && lastFetchedId == 1300 && !browsingHistory);
    int expected = 1251;
    while (expected > 1) {
        page = GetMessagesBackground(base + "/pages", "test-token", 0, chatMessages[0].id);
        ApplyMessagePage(page, FetchMode::Older);
        expected = std::max(1, expected - 50);
        check(chatMessages[0].id == expected && browsingHistory);
        check(chatMessages.size() <= 512 && chatMessages.AccountedBytes() <= ChatHistory::MaxAccountedBytes);
    }
    page = GetMessagesBackground(base + "/pages", "test-token", 0, 1);
    ApplyMessagePage(page, FetchMode::Older);
    check(historyStart && chatMessages[0].id == 1);
    const auto revision = chatMessages.Revision();
    page = GetMessagesBackground(base + "/offline", "test-token", 0, 1);
    ApplyMessagePage(page, FetchMode::Older);
    check(chatMessages.Revision() == revision && browsingHistory && !historyStatus.empty());
    page = GetMessagesBackground(base + "/broken-page", "test-token", 0, 1);
    check(page.networkFailure);
    while (browsingHistory) {
        page = GetMessagesBackground(base + "/pages", "test-token", chatMessages[chatMessages.size()-1].id);
        ApplyMessagePage(page, FetchMode::Newer);
        check(chatMessages.size() <= 512 && chatMessages.AccountedBytes() <= ChatHistory::MaxAccountedBytes);
    }
    check(chatMessages[chatMessages.size()-1].id == 1300 && !historyStart);
    page = GetMessagesBackground(base + "/pages", "test-token", 0, chatMessages[0].id);
    ApplyMessagePage(page, FetchMode::Older); check(browsingHistory);
    page = GetMessagesBackground(base + "/pages", "test-token", 0);
    ApplyMessagePage(page, FetchMode::Latest);
    check(!browsingHistory && chatMessages.size() == 50 && chatMessages[0].id == 1251 && chatScroll.followLatest);
    // Partial application of a newline-heavy page must retain the nearest
    // contiguous range and keep paging enabled for the unconsumed messages.
    ClearChatHistory(); chatMessages.Append({101,0,"alice","tail"});
    SyncResult heavy;
    for (int i = 51; i <= 100; ++i) heavy.messages.push_back({i,0,"alice",std::string(2000,'\n')});
    ApplyMessagePage(heavy, FetchMode::Older);
    check(heavy.messages.size() == 2 && chatMessages[0].id == 99 && !historyStart);
    ClearChatHistory(); browsingHistory = true; chatMessages.Append({1,0,"alice","start"});
    heavy.messages.clear();
    for (int i = 2; i <= 20; ++i) heavy.messages.push_back({i,0,"alice",std::string(2000,'\n')});
    ApplyMessagePage(heavy, FetchMode::Newer);
    check(heavy.messages.size() == 2 && chatMessages[chatMessages.size()-1].id == 3 && browsingHistory);
    curl_global_cleanup();
}
