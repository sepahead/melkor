#pragma once

#include <cstddef>
#include <ostream>
#include <string>

namespace melkor::text {

inline size_t utf8SequenceLength(const std::string& value, size_t index) {
    const auto byte = [&](size_t offset) {
        return static_cast<unsigned char>(value[index + offset]);
    };
    const size_t remaining = value.size() - index;
    const unsigned char first = byte(0);
    if (first < 0x80) return 1;
    if (first >= 0xc2 && first <= 0xdf && remaining >= 2 &&
        byte(1) >= 0x80 && byte(1) <= 0xbf) {
        return 2;
    }
    if (first >= 0xe0 && first <= 0xef && remaining >= 3 &&
        byte(2) >= 0x80 && byte(2) <= 0xbf) {
        const unsigned char second = byte(1);
        if ((first == 0xe0 && second >= 0xa0 && second <= 0xbf) ||
            (first >= 0xe1 && first <= 0xec && second >= 0x80 && second <= 0xbf) ||
            (first == 0xed && second >= 0x80 && second <= 0x9f) ||
            (first >= 0xee && first <= 0xef && second >= 0x80 && second <= 0xbf)) {
            return 3;
        }
    }
    if (first >= 0xf0 && first <= 0xf4 && remaining >= 4 &&
        byte(2) >= 0x80 && byte(2) <= 0xbf && byte(3) >= 0x80 && byte(3) <= 0xbf) {
        const unsigned char second = byte(1);
        if ((first == 0xf0 && second >= 0x90 && second <= 0xbf) ||
            (first >= 0xf1 && first <= 0xf3 && second >= 0x80 && second <= 0xbf) ||
            (first == 0xf4 && second >= 0x80 && second <= 0x8f)) {
            return 4;
        }
    }
    return 0;
}

inline void writeDisplayString(std::ostream& stream, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (ch >= 0x80) {
            const size_t sequence_length = utf8SequenceLength(value, index);
            // C1 control characters remain terminal-active when encoded as
            // UTF-8 (for example U+009B is the single-character CSI form).
            if (sequence_length == 2 && ch == 0xc2) {
                const unsigned char second = static_cast<unsigned char>(value[index + 1]);
                if (second >= 0x80 && second <= 0x9f) {
                    stream << "\\u00" << hex[second >> 4] << hex[second & 0x0f];
                    index += 2;
                    continue;
                }
            }
            if (sequence_length > 0) {
                stream.write(value.data() + index,
                             static_cast<std::streamsize>(sequence_length));
                index += sequence_length;
                continue;
            }
            stream << "\\x" << hex[ch >> 4] << hex[ch & 0x0f];
        } else if (ch == '\n') {
            stream << "\\n";
        } else if (ch == '\r') {
            stream << "\\r";
        } else if (ch == '\t') {
            stream << "\\t";
        } else if (ch < 0x20 || ch == 0x7f) {
            stream << "\\x" << hex[ch >> 4] << hex[ch & 0x0f];
        } else {
            stream.put(static_cast<char>(ch));
        }
        ++index;
    }
}

} // namespace melkor::text
