#include "session_store.h"
#include <iostream>
// Read-only by default. Roundtrip requires an explicitly isolated, empty test keyring.
int main(int argc, char** argv) {
    auto existing = LoadSession();
    if (argc != 2 || std::string(argv[1]) != "--isolated-roundtrip") {
        std::cout << "Native keyring read status: " << static_cast<int>(existing.status) << '\n';
        return existing.status == StoreStatus::Unavailable ? 2 : 0;
    }
    if (existing.status != StoreStatus::Missing) { std::cerr << "Requires empty isolated keyring\n"; return 3; }
    SavedSession sample{"http://127.0.0.1:51211", "test-user", "non-production-test-token"};
    if (SaveSession(sample) != StoreStatus::Ok) return 4;
    auto loaded = LoadSession();
    bool ok = loaded.status == StoreStatus::Ok && loaded.session->server == sample.server && loaded.session->username == sample.username && loaded.session->token == sample.token;
    if (DeleteSession() != StoreStatus::Ok) return 5;
    if (LoadSession().status != StoreStatus::Missing) return 6;
    std::cout << "Native keyring save/load/delete: " << (ok ? "passed" : "failed") << '\n';
    return ok ? 0 : 7;
}
