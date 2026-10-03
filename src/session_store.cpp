#include "session_store.h"
#ifndef MAGNESIUM_SESSION_SERVICE
#define MAGNESIUM_SESSION_SERVICE "us.tamastudios.magnesium.session"
#endif
#if defined(__APPLE__)
#include <Security/Security.h>
namespace {
CFMutableDictionaryRef Query() {
    auto q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR(MAGNESIUM_SESSION_SERVICE));
    CFDictionarySetValue(q, kSecAttrAccount, CFSTR("current-session"));
    return q;
}
StoreStatus Status(OSStatus code) { return code == errSecSuccess ? StoreStatus::Ok : code == errSecItemNotFound ? StoreStatus::Missing : StoreStatus::Unavailable; }
}
StoreResult LoadSession() {
    auto q = Query(); CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFTypeRef data = nullptr; const auto code = SecItemCopyMatching(q, &data); CFRelease(q);
    if (code != errSecSuccess) return {Status(code), {}};
    if (!data || CFGetTypeID(data) != CFDataGetTypeID()) { if (data) CFRelease(data); return {StoreStatus::Invalid, {}}; }
    auto bytes = static_cast<CFDataRef>(data);
    std::string value(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), CFDataGetLength(bytes));
    CFRelease(data); return DecodeSession(value);
}
StoreStatus SaveSession(const SavedSession& session) {
    const auto value = EncodeSession(session);
    auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(value.data()), value.size());
    auto q = Query(); auto update = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    auto code = SecItemUpdate(q, update);
    if (code == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        code = SecItemAdd(q, nullptr);
    }
    CFRelease(update); CFRelease(q); CFRelease(data); return Status(code);
}
StoreStatus DeleteSession() { auto q = Query(); auto code = SecItemDelete(q); CFRelease(q); return code == errSecItemNotFound ? StoreStatus::Ok : Status(code); }
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#define MAGNESIUM_WIDEN_IMPL(value) L##value
#define MAGNESIUM_WIDEN(value) MAGNESIUM_WIDEN_IMPL(value)
static constexpr wchar_t target[] = MAGNESIUM_WIDEN(MAGNESIUM_SESSION_SERVICE);
StoreResult LoadSession() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &credential)) return {GetLastError() == ERROR_NOT_FOUND ? StoreStatus::Missing : StoreStatus::Unavailable, {}};
    std::string value(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential); return DecodeSession(value);
}
StoreStatus SaveSession(const SavedSession& session) {
    const auto value = EncodeSession(session);
    if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return StoreStatus::Unavailable;
    CREDENTIALW credential{}; credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target);
    credential.UserName = const_cast<wchar_t*>(L"current-session");
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) ? StoreStatus::Ok : StoreStatus::Unavailable;
}
StoreStatus DeleteSession() { return CredDeleteW(target, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND ? StoreStatus::Ok : StoreStatus::Unavailable; }
#else
#include <libsecret/secret.h>
#include <thread>
#include <mutex>
#include <condition_variable>
namespace {
const SecretSchema schema = {MAGNESIUM_SESSION_SERVICE, SECRET_SCHEMA_NONE, {{"record", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
// A missing/locked service cannot hold a background operation forever.
struct Deadline {
    GCancellable* cancel = g_cancellable_new();
    std::mutex mutex; std::condition_variable ready; bool done = false; std::thread timer;
    Deadline() : timer([this] { std::unique_lock<std::mutex> lock(mutex); if (!ready.wait_for(lock, std::chrono::seconds(15), [this] { return done; })) g_cancellable_cancel(cancel); }) {}
    ~Deadline() { { std::lock_guard<std::mutex> lock(mutex); done = true; } ready.notify_one(); timer.join(); g_object_unref(cancel); }
};
}
StoreResult LoadSession() {
    Deadline deadline; GError* error = nullptr;
    gchar* value = secret_password_lookup_sync(&schema, deadline.cancel, &error, "record", "current-session", nullptr);
    if (error) { g_error_free(error); if (value) secret_password_free(value); return {StoreStatus::Unavailable, {}}; }
    if (!value) return {StoreStatus::Missing, {}};
    auto result = DecodeSession(value); secret_password_free(value); return result;
}
StoreStatus SaveSession(const SavedSession& session) {
    Deadline deadline; GError* error = nullptr; const auto value = EncodeSession(session);
    const auto ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "Magnesium saved session", value.c_str(), deadline.cancel, &error, "record", "current-session", nullptr);
    if (error) g_error_free(error);
    return ok && !error ? StoreStatus::Ok : StoreStatus::Unavailable;
}
StoreStatus DeleteSession() {
    Deadline deadline; GError* error = nullptr;
    secret_password_clear_sync(&schema, deadline.cancel, &error, "record", "current-session", nullptr);
    if (error) { g_error_free(error); return StoreStatus::Unavailable; }
    return StoreStatus::Ok;
}
#endif
