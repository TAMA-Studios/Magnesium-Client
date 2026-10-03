#include "session_store.h"
#include <stdexcept>
#include <curl/curl.h>
static void check(bool b) { if (!b) throw std::runtime_error("session validation failed"); }
int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    SavedSession session{"https://example.org:51211", "alice café", "valid-token"};
    auto encoded = EncodeSession(session); auto decoded = DecodeSession(encoded);
    check(decoded.status == StoreStatus::Ok);
    check(decoded.session->server == session.server && decoded.session->username == session.username && decoded.session->token == session.token);
    check(encoded.find("password") == std::string::npos);
    for (auto url : {"file:///tmp/token", "https://user:password@example.org", "https://example.org?q=x", "https://example.org/#x", "http://example.org\r\nX: y", ""}) check(!ValidServer(url));
    check(DecodeSession("{}").status == StoreStatus::Invalid);
    check(DecodeSession("not JSON").status == StoreStatus::Invalid);
    session.token = "token\r\nHeader: injection";
    check(DecodeSession(EncodeSession(session)).status == StoreStatus::Invalid);
    session.token = ""; check(DecodeSession(EncodeSession(session)).status == StoreStatus::Invalid);
    curl_global_cleanup();
}
