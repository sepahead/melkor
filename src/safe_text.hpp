#pragma once

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <locale.h>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace melkor::text {

namespace detail {

#if defined(_WIN32)
using NumericLocale = _locale_t;
#else
using NumericLocale = locale_t;
#endif

inline NumericLocale numericLocale() noexcept {
#if defined(_WIN32)
    static NumericLocale locale = _create_locale(LC_NUMERIC, "C");
#else
    static NumericLocale locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
#endif
    return locale;
}

inline bool isAsciiSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

template <typename Value>
inline Value parseWithLocale(const char* input, char** end, NumericLocale locale) noexcept {
#if defined(_WIN32)
    if constexpr (std::is_same_v<Value, float>)
        return _strtof_l(input, end, locale);
    return _strtod_l(input, end, locale);
#else
    if constexpr (std::is_same_v<Value, float>)
        return strtof_l(input, end, locale);
    return strtod_l(input, end, locale);
#endif
}

template <typename Value>
inline bool parseClassicNumber(std::string_view input, Value& output) noexcept {
    static_assert(std::is_same_v<Value, float> || std::is_same_v<Value, double>);
    if (input.empty() || input.front() == '+' || isAsciiSpace(input.front()))
        return false;

    const std::size_t prefix = input.front() == '-' ? 1 : 0;
    if (input.size() >= prefix + 2 && input[prefix] == '0' &&
        (input[prefix + 1] == 'x' || input[prefix + 1] == 'X')) {
        return false;
    }

    std::array<char, 128> local{};
    std::string allocated;
    const char* begin = nullptr;
    if (input.size() < local.size()) {
        std::memcpy(local.data(), input.data(), input.size());
        begin = local.data();
    } else {
        try {
            allocated.assign(input.data(), input.size());
        } catch (...) {
            return false;
        }
        begin = allocated.c_str();
    }

    NumericLocale locale = numericLocale();
    if (locale == nullptr)
        return false;
    errno = 0;
    char* parsed_end = nullptr;
    const Value parsed = parseWithLocale<Value>(begin, &parsed_end, locale);
    if (parsed_end != begin + input.size())
        return false;
    if (errno == ERANGE && (parsed == Value{0} || !std::isfinite(parsed)))
        return false;
    output = parsed;
    return true;
}

}  // namespace detail

inline bool parseClassicFloat(std::string_view input, float& output) noexcept {
    return detail::parseClassicNumber(input, output);
}

inline bool parseClassicDouble(std::string_view input, double& output) noexcept {
    return detail::parseClassicNumber(input, output);
}

inline bool formatClassicFloat(float value, char* output, std::size_t capacity,
                               std::size_t& length) noexcept {
    if (output == nullptr || capacity == 0)
        return false;
    detail::NumericLocale locale = detail::numericLocale();
    if (locale == nullptr)
        return false;

    int written = -1;
#if defined(_WIN32)
    written = _snprintf_l(output, capacity, "%.*g", locale,
                          std::numeric_limits<float>::max_digits10, static_cast<double>(value));
#else
    const locale_t previous = uselocale(locale);
    if (previous == nullptr)
        return false;
    written = std::snprintf(output, capacity, "%.*g", std::numeric_limits<float>::max_digits10,
                            static_cast<double>(value));
    if (uselocale(previous) == nullptr)
        return false;
#endif
    if (written < 0 || static_cast<std::size_t>(written) >= capacity)
        return false;
    length = static_cast<std::size_t>(written);
    return true;
}

inline std::size_t utf8SequenceLength(const std::string& value, std::size_t index) {
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(value[index + offset]);
    };
    const std::size_t remaining = value.size() - index;
    const unsigned char first = byte(0);
    if (first < 0x80)
        return 1;
    if (first >= 0xc2 && first <= 0xdf && remaining >= 2 && byte(1) >= 0x80 && byte(1) <= 0xbf) {
        return 2;
    }
    if (first >= 0xe0 && first <= 0xef && remaining >= 3 && byte(2) >= 0x80 && byte(2) <= 0xbf) {
        const unsigned char second = byte(1);
        if ((first == 0xe0 && second >= 0xa0 && second <= 0xbf) ||
            (first >= 0xe1 && first <= 0xec && second >= 0x80 && second <= 0xbf) ||
            (first == 0xed && second >= 0x80 && second <= 0x9f) ||
            (first >= 0xee && first <= 0xef && second >= 0x80 && second <= 0xbf)) {
            return 3;
        }
    }
    if (first >= 0xf0 && first <= 0xf4 && remaining >= 4 && byte(2) >= 0x80 && byte(2) <= 0xbf &&
        byte(3) >= 0x80 && byte(3) <= 0xbf) {
        const unsigned char second = byte(1);
        if ((first == 0xf0 && second >= 0x90 && second <= 0xbf) ||
            (first >= 0xf1 && first <= 0xf3 && second >= 0x80 && second <= 0xbf) ||
            (first == 0xf4 && second >= 0x80 && second <= 0x8f)) {
            return 4;
        }
    }
    return 0;
}

inline std::uint32_t utf8CodePoint(const std::string& value, std::size_t index,
                                   std::size_t length) noexcept {
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(value[index + offset]);
    };
    if (length == 1)
        return byte(0);
    std::uint32_t code_point = byte(0) & (0x7fU >> length);
    for (std::size_t offset = 1; offset < length; ++offset)
        code_point = (code_point << 6U) | (byte(offset) & 0x3fU);
    return code_point;
}

inline bool isDangerousFormatControl(std::uint32_t code_point) noexcept {
    return code_point == 0x00adU || code_point == 0x061cU ||
           (code_point >= 0x200bU && code_point <= 0x200fU) ||
           (code_point >= 0x2028U && code_point <= 0x2029U) ||
           (code_point >= 0x202aU && code_point <= 0x202eU) ||
           (code_point >= 0x2060U && code_point <= 0x2064U) ||
           (code_point >= 0x2066U && code_point <= 0x2069U) || code_point == 0xfeffU ||
           (code_point >= 0xfff9U && code_point <= 0xfffbU) || code_point == 0xe0001U ||
           (code_point >= 0xe0020U && code_point <= 0xe007fU);
}

inline void writeUnicodeEscape(std::ostream& stream, std::uint32_t code_point) {
    static constexpr char hex[] = "0123456789abcdef";
    if (code_point <= 0xffffU) {
        stream << "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
            stream.put(hex[(code_point >> shift) & 0x0fU]);
        return;
    }
    stream << "\\U";
    for (int shift = 28; shift >= 0; shift -= 4)
        stream.put(hex[(code_point >> shift) & 0x0fU]);
}

inline void writeDisplayString(std::ostream& stream, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (ch >= 0x80) {
            const std::size_t sequence_length = utf8SequenceLength(value, index);
            // Escape UTF-8 C1 control characters because terminals can interpret them.
            if (sequence_length == 2 && ch == 0xc2) {
                const unsigned char second = static_cast<unsigned char>(value[index + 1]);
                if (second >= 0x80 && second <= 0x9f) {
                    stream << "\\u00" << hex[second >> 4] << hex[second & 0x0f];
                    index += 2;
                    continue;
                }
            }
            if (sequence_length > 0) {
                const std::uint32_t code_point = utf8CodePoint(value, index, sequence_length);
                if (isDangerousFormatControl(code_point)) {
                    writeUnicodeEscape(stream, code_point);
                    index += sequence_length;
                    continue;
                }
                stream.write(value.data() + index, static_cast<std::streamsize>(sequence_length));
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

}  // namespace melkor::text
