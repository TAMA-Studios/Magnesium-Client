#include "session_store.h"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
bool ValidServer(const std::string& server) {
    if (server.empty() || server.size() > 127 || std::any_of(server.begin(), server.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return false;
    CURLU* url = curl_url();
    if (!url) return false;
    bool ok = curl_url_set(url, CURLUPART_URL, server.c_str(), 0) == CURLUE_OK;
    char *scheme = nullptr, *host = nullptr, *extra = nullptr;
    ok = ok && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
         (std::string(scheme) == "http" || std::string(scheme) == "https") &&
         curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK;
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT}) {
        if (curl_url_get(url, part, &extra, 0) == CURLUE_OK) { ok = false; curl_free(extra); extra = nullptr; }
    }
    curl_free(scheme); curl_free(host); curl_url_cleanup(url);
    return ok;
}
std::string EncodeSession(const SavedSession& s) {
    return nlohmann::json{{"version", 1}, {"server", s.server}, {"username", s.username}, {"token", s.token}}.dump();
}
StoreResult DecodeSession(const std::string& value) {
    try {
        if (value.size() > 4096) return {StoreStatus::Invalid, {}};
        auto j = nlohmann::json::parse(value);
        if (j.at("version").get<int>() != 1) return {StoreStatus::Invalid, {}};
        SavedSession s{j.at("server").get<std::string>(), j.at("username").get<std::string>(), j.at("token").get<std::string>()};
        if (!ValidServer(s.server) || s.username.empty() || s.username.size() > 256 || s.token.empty() || s.token.size() > 256 ||
            std::any_of(s.token.begin(), s.token.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) return {StoreStatus::Invalid, {}};
        return {StoreStatus::Ok, s};
    } catch (...) { return {StoreStatus::Invalid, {}}; }
}
