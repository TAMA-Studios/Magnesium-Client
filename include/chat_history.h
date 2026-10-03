#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct Message {
    int id{};
    double created{};
    std::string sender;
    std::string text;
};

// Retain a bounded movable window. The authoritative full history remains on Sodium.
class ChatHistory {
public:
    static constexpr std::size_t MaxMessages = 512;
    static constexpr std::size_t MaxAccountedBytes = 1024*1024;
    bool Append(Message message) {
        const auto cost = Cost(message);
        if (cost > MaxAccountedBytes) return false;
        while (!messages.empty() && (messages.size() >= MaxMessages || bytes > MaxAccountedBytes - cost)) {
            bytes -= Cost(messages.front());
            messages.erase(messages.begin());
            ++firstOrdinal;
        }
        bytes += cost; messages.push_back(std::move(message)); ++revision;
        return true;
    }
    bool Prepend(Message message) {
        const auto cost = Cost(message);
        if (cost > MaxAccountedBytes) return false;
        if (!messages.empty() && message.id >= messages.front().id) return false;
        while (!messages.empty() && (messages.size() >= MaxMessages || bytes > MaxAccountedBytes - cost)) {
            bytes -= Cost(messages.back()); messages.pop_back();
        }
        bytes += cost; messages.insert(messages.begin(), std::move(message));
        --firstOrdinal; ++revision;
        return true;
    }
    void TrimFront(std::size_t count) {
        while (count-- && !messages.empty()) {
            bytes -= Cost(messages.front()); messages.erase(messages.begin()); ++firstOrdinal; ++revision;
        }
    }
    void TrimBack(std::size_t count) {
        while (count-- && !messages.empty()) {
            bytes -= Cost(messages.back()); messages.pop_back(); ++revision;
        }
    }
    void Clear() {
        std::vector<Message>().swap(messages);
        bytes = 0; firstOrdinal = 0; ++revision;
    }
    std::size_t size() const { return messages.size(); }
    bool empty() const { return messages.empty(); }
    const Message& operator[](std::size_t index) const { return messages[index]; }
    std::size_t AccountedBytes() const { return bytes; }
    std::uint64_t Revision() const { return revision; }
    std::uint64_t FirstOrdinal() const { return firstOrdinal; }
private:
    static std::size_t Cost(const Message& m) {
        // Include a conservative allowance for explicit blank/wrapped-row metadata.
        return sizeof(Message) + m.sender.capacity() + m.text.capacity() +
               64*(1 + std::count(m.text.begin(), m.text.end(), '\n'));
    }
    std::vector<Message> messages;
    std::size_t bytes = 0;
    std::uint64_t revision = 0, firstOrdinal = 0;
};
