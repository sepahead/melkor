#pragma once

#include "melkor/budget.hpp"
#include "melkor/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace melkor::io {

// Keeps one verified regular-file handle open for all reads.
class InputFile {
public:
    static Result<InputFile> open(const std::filesystem::path& path,
                                  const OperationContext& context);

    ~InputFile();
    InputFile(InputFile&&) noexcept;
    InputFile& operator=(InputFile&&) noexcept;
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;

    std::uint64_t size() const noexcept;

    Result<void> read_exact(std::uint64_t offset, std::uint8_t* output, std::size_t amount,
                            const OperationContext& context, const char* operation,
                            const char* phase);

private:
    struct Impl;
    explicit InputFile(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace melkor::io
