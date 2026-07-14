#include "melkor/checked.hpp"

#include <string>

namespace melkor {
namespace {

Diagnostic overflow_diagnostic(const char* code, const char* what, std::uint64_t a,
                               std::uint64_t b, const char* op) {
    Diagnostic diagnostic(code, Severity::error,
                          std::string("integer overflow computing ") + what);
    diagnostic.with_context("operand_a", a);
    diagnostic.with_context("operand_b", b);
    diagnostic.with_context("operation", std::string(op));
    return diagnostic;
}

}  // namespace

Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b, const char* what) {
    // Rearranged to avoid computing the overflowing sum at all: `a + b < a` would already be
    // the wrapped value, and relying on the wrap is only defined for unsigned types by luck
    // of the standard rather than by intent.
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return Result<std::uint64_t>::failure(
            ErrorCode::invalid_data,
            overflow_diagnostic("MK0101_INTEGER_OVERFLOW", what, a, b, "add"));
    }
    return Result<std::uint64_t>::success(a + b);
}

Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b, const char* what) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return Result<std::uint64_t>::failure(
            ErrorCode::invalid_data,
            overflow_diagnostic("MK0101_INTEGER_OVERFLOW", what, a, b, "multiply"));
    }
    return Result<std::uint64_t>::success(a * b);
}

Result<std::uint64_t> checked_sub(std::uint64_t a, std::uint64_t b, const char* what) {
    if (b > a) {
        return Result<std::uint64_t>::failure(
            ErrorCode::invalid_data,
            overflow_diagnostic("MK0102_INTEGER_UNDERFLOW", what, a, b, "subtract"));
    }
    return Result<std::uint64_t>::success(a - b);
}

Result<std::size_t> checked_size_cast(std::uint64_t value, const char* what) {
    // On a 64-bit host this is a no-op and the branch is never taken. That is precisely why it
    // must exist: without it, a 32-bit build truncates silently, allocates a buffer smaller
    // than the loop that fills it, and the bug shows up as memory corruption on the platform
    // nobody tested.
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            Diagnostic diagnostic("MK0103_SIZE_NOT_REPRESENTABLE", Severity::error,
                                  std::string("value does not fit in size_t on this platform: ") +
                                      what);
            diagnostic.with_context("value", value);
            diagnostic.with_context("size_t_max",
                                    static_cast<std::uint64_t>(
                                        std::numeric_limits<std::size_t>::max()));
            return Result<std::size_t>::failure(ErrorCode::resource_limit, std::move(diagnostic));
        }
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

Result<std::uint32_t> checked_u32_cast(std::uint64_t value, const char* what) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        Diagnostic diagnostic("MK0104_VALUE_NOT_REPRESENTABLE", Severity::error,
                              std::string("value exceeds the 32-bit field it must occupy: ") +
                                  what);
        diagnostic.with_context("value", value);
        diagnostic.with_context(
            "max", static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
        return Result<std::uint32_t>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(value));
}

Result<ByteRange> checked_range(std::uint64_t offset, std::uint64_t length, std::uint64_t total,
                                const char* what) {
    // The classic bug this prevents:
    //
    //     if (offset + length <= total) { read(buffer + offset, length); }
    //
    // With offset = 2^64 - 8 and length = 16, `offset + length` wraps to 8, the check passes,
    // and the read runs wildly out of bounds. Doing the addition through checked_add means the
    // wrap is a diagnostic rather than a memory-safety failure.
    auto end = checked_add(offset, length, what);
    if (!end.has_value()) {
        return Result<ByteRange>::failure(end.error_code(), end.diagnostics());
    }

    if (end.value() > total) {
        Diagnostic diagnostic("MK0105_RANGE_OUT_OF_BOUNDS", Severity::error,
                              std::string("range extends past the end of the buffer: ") + what);
        diagnostic.with_context("offset", offset);
        diagnostic.with_context("length", length);
        diagnostic.with_context("end", end.value());
        diagnostic.with_context("buffer_size", total);
        return Result<ByteRange>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }

    ByteRange range;
    range.offset = offset;
    range.length = length;
    return Result<ByteRange>::success(range);
}

Result<std::uint64_t> checked_array_bytes(std::uint64_t count, std::uint64_t stride,
                                          const char* what) {
    return checked_mul(count, stride, what);
}

Result<std::uint64_t> checked_sh_coefficient_count(std::uint32_t degree) {
    // Melkor's canonical scene stores SH degrees 0..4. Degree 4 is needed because SPZ v4
    // carries it; the pinned glTF profile stops at 3, and converting between them is a
    // *reported loss*, not a silent truncation.
    constexpr std::uint32_t kMaxDegree = 4;
    if (degree > kMaxDegree) {
        Diagnostic diagnostic("MK0106_SH_DEGREE_OUT_OF_RANGE", Severity::error,
                              "spherical-harmonic degree is outside the supported range");
        diagnostic.with_context("degree", static_cast<std::uint64_t>(degree));
        diagnostic.with_context("max_degree", static_cast<std::uint64_t>(kMaxDegree));
        return Result<std::uint64_t>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }

    // (degree + 1)^2 coefficients per colour channel. Degree 0 is 1 (the DC term), degree 3 is
    // 16, degree 4 is 25.
    const std::uint64_t n = static_cast<std::uint64_t>(degree) + 1;
    return checked_mul(n, n, "spherical-harmonic coefficient count");
}

Result<std::uint64_t> checked_sh_total_floats(std::uint64_t splat_count, std::uint32_t degree) {
    auto per_channel = checked_sh_coefficient_count(degree);
    if (!per_channel.has_value()) {
        return per_channel;
    }

    // 3 channels (RGB) per coefficient.
    auto per_splat = checked_mul(per_channel.value(), 3, "spherical-harmonic floats per splat");
    if (!per_splat.has_value()) {
        return per_splat;
    }

    return checked_mul(splat_count, per_splat.value(), "total spherical-harmonic floats");
}

}  // namespace melkor
