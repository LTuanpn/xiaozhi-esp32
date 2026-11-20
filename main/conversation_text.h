#pragma once

#include <algorithm>
#include <deque>
#include <string>
#include <string_view>

inline std::string_view TrimConversationText(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return {};
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

inline std::string CleanAssistantText(std::string_view text) {
    std::string cleaned;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        for (size_t pos = 0; (pos = line.find('%', pos)) != std::string_view::npos; ++pos) {
            auto marker = line.substr(pos, 8);
            std::string lower(marker);
            for (auto& c : lower) {
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            }
            if (lower.starts_with("%emotion") || lower.starts_with("%tool") ||
                lower.starts_with("%mcp")) {
                line = line.substr(0, pos);
                break;
            }
        }
        line = TrimConversationText(line);
        if (!line.empty()) {
            if (!cleaned.empty())
                cleaned += '\n';
            cleaned += line;
        }
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return cleaned;
}

inline bool RememberAssistantSentence(std::deque<std::string>& recent, const std::string& text) {
    if (text.empty() || std::find(recent.begin(), recent.end(), text) != recent.end())
        return false;
    if (text.size() <= 512) {
        if (recent.size() == 8)
            recent.pop_front();
        recent.push_back(text);
    }
    return true;
}

inline bool IsMusicRequest(std::string_view text) {
    text = TrimConversationText(text);
    std::string command(text.substr(0, 256));
    for (auto& c : command) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    text = command;
    if (text.starts_with("jarvis")) {
        text.remove_prefix(6);
        const auto start = text.find_first_not_of(" ,:");
        text = start == std::string_view::npos ? std::string_view() : text.substr(start);
    }
    for (auto verb : {"mở ", "phát ", "bật ", "nghe "}) {
        if (text.starts_with(verb)) {
            text.remove_prefix(std::string_view(verb).length());
            return text == "nhạc" || text.starts_with("nhạc ") || text.starts_with("nhạc.") ||
                   text.starts_with("bài ") || text.starts_with("bản nhạc ") ||
                   (text.find(" của ") != std::string_view::npos && !text.starts_with("qr ") &&
                    !text.starts_with("trang ") && !text.starts_with("file ") &&
                    !text.starts_with("tệp "));
        }
    }
    return false;
}
