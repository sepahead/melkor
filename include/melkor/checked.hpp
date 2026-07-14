// Checked arithmetic for values that come from untrusted files.
//
// Every parser in this project multiplies a count by a stride, adds an offset to a length, or
// converts a 64-bit file-declared number into a `size_t` before allocating. Each of those is
// an integer overflow waiting to happen, and an overflow in that position is not a wrong
// number -- it is a heap overflow, because the allocation ends up smaller than the loop that
// fills it.
//
// The rule this header exists to enforce: **a number that came from a file is never used in
// arithmetic that reaches an allocation without passing through one of these functions.**
//
// All intermediate arithmetic is done in `std::uint64_t` and converted down explicitly at the
// end, so that a 32-bit platform cannot silently truncate a value that a 64-bit one accepts.

#ifndef MELKOR_CHECKED_HPP
#define MELKOR_CHECKED_HPP

#include "melkor/error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace melkor {

// A validated byte range within a known-size buffer. Constructing one is the only sanctioned
// way to turn a file-declared (offset, length) pair into something you may index with.
struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;

    std::uint64_t end() const noexcept { return offset + length; }  // safe: checked_range
};

Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b, const char* what = "value");
Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b, const char* what = "value");
Result<std::uint64_t> checked_sub(std::uint64_t a, std::uint64_t b, const char* what = "value");

// Narrows a 64-bit file-declared value to `size_t`.
//
// On a 64-bit host this is nearly always a no-op, which is exactly why it is dangerous to
// leave implicit: the truncation bug then only appears on 32-bit builds, where nobody tests,
// and it appears as a heap overflow.
Result<std::size_t> checked_size_cast(std::uint64_t value, const char* what = "value");

// Narrows to a 32-bit count, for formats whose fields are declared 32-bit.
Result<std::uint32_t> checked_u32_cast(std::uint64_t value, const char* what = "value");

// Validates that [offset, offset + length) lies entirely within a buffer of `total` bytes.
//
// This is the single most important function in the header. It catches the classic
// `offset + length` wraparound, where a huge offset plus a huge length wraps to a small
// number that passes a naive `<= total` check.
Result<ByteRange> checked_range(std::uint64_t offset, std::uint64_t length, std::uint64_t total,
                                const char* what = "range");

// `count * stride`, the canonical shape of an element-array size computation.
Result<std::uint64_t> checked_array_bytes(std::uint64_t count, std::uint64_t stride,
                                          const char* what = "array");

// Coefficient count for spherical harmonics of a given degree: (degree + 1)^2 per channel.
//
// Separate function rather than an inline `(d+1)*(d+1)` because the SH degree arrives from a
// file, the count feeds an allocation, and the relationship is easy to get subtly wrong (it is
// per-channel, and lower degrees must be complete).
Result<std::uint64_t> checked_sh_coefficient_count(std::uint32_t degree);

// Total SH storage for `splat_count` splats at `degree`, across three colour channels.
Result<std::uint64_t> checked_sh_total_floats(std::uint64_t splat_count, std::uint32_t degree);

}  // namespace melkor

#endif  // MELKOR_CHECKED_HPP
