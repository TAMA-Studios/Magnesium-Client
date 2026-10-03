#pragma once

#include <algorithm>
#include <string>
#include <vector>

// Wrap at spaces when possible, splitting long URLs/words only between UTF-8
// codepoints. The caller measures using the same font and size as the drawing.
template <typename Measure>
std::vector<std::string> WrapMessageText(const std::string& text, float width, Measure measure) {
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') { ++i; continue; }
        if (text[i] == '\n') { lines.push_back(line); line.clear(); ++i; continue; }
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        bytes = std::min(bytes, text.size() - i);
        const std::string glyph = text.substr(i, bytes);
        i += bytes;
        if (!line.empty() && measure(line + glyph) > width) {
            const auto space = line.find_last_of(" \t");
            if (space != std::string::npos && space > 0) {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else { lines.push_back(line); line.clear(); }
            // A remaining long word can still exceed the available width.
            if (!line.empty() && measure(line + glyph) > width) {
                lines.push_back(line); line.clear();
            }
            if (line.empty() && (glyph == " " || glyph == "\t")) continue;
        }
        line += glyph;
    }
    lines.push_back(line);
    return lines;
}

struct ChatScrollState {
    bool followLatest = true;
    float offset = 0.0f;  // raygui uses negative offsets to move down the log.

    void Fit(float contentHeight, float viewportHeight) {
        const float bottom = -std::max(0.0f, contentHeight - viewportHeight);
        offset = followLatest ? bottom : std::clamp(offset, bottom, 0.0f);
    }
    void Record(float newOffset, float contentHeight, float viewportHeight) {
        offset = newOffset;
        const float bottom = -std::max(0.0f, contentHeight - viewportHeight);
        followLatest = offset <= bottom + 2.0f;
    }
};
