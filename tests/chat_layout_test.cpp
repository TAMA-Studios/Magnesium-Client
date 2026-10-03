#include "chat_layout.h"
#include <cstdlib>
#include <iostream>

static void Check(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

static float Codepoints(const std::string& text) {
    float length = 0;
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++length;
    return length;
}

int main() {
    Check(WrapMessageText("", 5, Codepoints) == std::vector<std::string>{""}, "Empty history");
    Check(WrapMessageText("hello world", 6, Codepoints) == std::vector<std::string>{"hello", "world"}, "Word wrapping");
    Check(WrapMessageText("ab\n\ncd\n", 6, Codepoints) == std::vector<std::string>{"ab", "", "cd", ""}, "Explicit blank lines");
    Check(WrapMessageText("ab\r\ncd", 6, Codepoints) == std::vector<std::string>{"ab", "cd"}, "CRLF");
    const std::string url = "https://example.org/averylongpath";
    auto lines = WrapMessageText(url, 5, Codepoints);
    std::string joined;
    for (const auto& line : lines) { Check(Codepoints(line) <= 5, "URL exceeds width"); joined += line; }
    Check(joined == url, "Long URL lost text");
    lines = WrapMessageText(u8"cafénaïve😀", 3, Codepoints);
    joined.clear();
    for (const auto& line : lines) {
        Check(Codepoints(line) <= 3, "Unicode exceeds width");
        Check((static_cast<unsigned char>(line.front()) & 0xc0) != 0x80, "Split a UTF-8 character");
        joined += line;
    }
    Check(joined == u8"cafénaïve😀", "Unicode text changed");
    Check(WrapMessageText("abcd", 2, Codepoints) == std::vector<std::string>{"ab", "cd"}, "Exact-width wrapping");
    Check(WrapMessageText("x", 0.1f, Codepoints) == std::vector<std::string>{"x"}, "Single wide glyph");

    ChatScrollState scroll;
    scroll.Fit(1000, 300); Check(scroll.offset == -700, "Initial scroll to latest");
    scroll.Record(-200, 1000, 300); Check(!scroll.followLatest, "Manual scroll disengages following");
    scroll.Fit(1200, 300); Check(scroll.offset == -200, "Incoming text moved reader");
    scroll.Fit(1200, 1100); Check(scroll.offset == -100, "Resize clamps old offset");
    scroll.Record(-100, 1200, 1100); Check(scroll.followLatest, "Bottom re-engages following");
    scroll.Fit(1400, 1100); Check(scroll.offset == -300, "Follow incoming messages");
    scroll.Fit(100, 1100); Check(scroll.offset == 0, "Short history must not scroll");
    std::cout << "Wrapping and scroll behavior passed.\n";
}
