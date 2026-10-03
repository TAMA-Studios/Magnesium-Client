#pragma once
#include <string>
#include <optional>
#include "json.hpp"
struct SavedSession { std::string server, username, token; };
enum class StoreStatus { Ok, Missing, Unavailable, Invalid };
struct StoreResult { StoreStatus status; std::optional<SavedSession> session; };
// One encrypted native credential holds the entire record, including host and username.
std::string EncodeSession(const SavedSession& session);
StoreResult DecodeSession(const std::string& value);
StoreResult LoadSession();
StoreStatus SaveSession(const SavedSession& session);
StoreStatus DeleteSession();
bool ValidServer(const std::string& server);
