#include "melkor/format/gltf_reader.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/glb_container.hpp"
#include "melkor/format/gltf_document.hpp"

#include "gltf_usage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace melkor::format::gltf {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kReadChunkBytes = std::size_t{1024} * 1024;

Result<SceneRead> fail_scene(ErrorCode error, const char* code, std::string message,
                             const fs::path* path = nullptr,
                             const OperationContext* context = nullptr) {
    Diagnostic diagnostic(code, Severity::error, std::move(message));
    if (path != nullptr && context != nullptr) {
        diagnostic.with_path(
            redact_path(path->u8string(), context->path_policy, context->path_root));
    }
    return Result<SceneRead>::failure(error, std::move(diagnostic));
}

template <class T>
Result<T> fail_value(ErrorCode error, const char* code, std::string message,
                     const fs::path* path = nullptr, const OperationContext* context = nullptr) {
    Diagnostic diagnostic(code, Severity::error, std::move(message));
    if (path != nullptr && context != nullptr) {
        diagnostic.with_path(
            redact_path(path->u8string(), context->path_policy, context->path_root));
    }
    return Result<T>::failure(error, std::move(diagnostic));
}

class ScopedMemoryRelease {
public:
    ScopedMemoryRelease() = default;
    ScopedMemoryRelease(Budget* budget, std::uint64_t amount) noexcept
        : budget_(budget), amount_(amount) {}

    ~ScopedMemoryRelease() {
        if (budget_ != nullptr && amount_ != 0)
            budget_->release(BudgetKind::memory_bytes, amount_);
    }

    ScopedMemoryRelease(const ScopedMemoryRelease&) = delete;
    ScopedMemoryRelease& operator=(const ScopedMemoryRelease&) = delete;

    ScopedMemoryRelease(ScopedMemoryRelease&& other) noexcept
        : budget_(std::exchange(other.budget_, nullptr)), amount_(std::exchange(other.amount_, 0)) {
    }

    ScopedMemoryRelease& operator=(ScopedMemoryRelease&& other) noexcept {
        if (this == &other)
            return *this;
        if (budget_ != nullptr && amount_ != 0)
            budget_->release(BudgetKind::memory_bytes, amount_);
        budget_ = std::exchange(other.budget_, nullptr);
        amount_ = std::exchange(other.amount_, 0);
        return *this;
    }

    void dismiss() noexcept {
        budget_ = nullptr;
        amount_ = 0;
    }

private:
    Budget* budget_ = nullptr;
    std::uint64_t amount_ = 0;
};

class AccountedBytes {
public:
    AccountedBytes(std::vector<std::uint8_t> bytes, Budget::Charge memory_charge) noexcept
        : memory_(std::move(memory_charge)), bytes_(std::move(bytes)) {}

    AccountedBytes(AccountedBytes&& other) noexcept
        : memory_(std::move(other.memory_)), bytes_(std::move(other.bytes_)) {}
    AccountedBytes& operator=(AccountedBytes&& other) noexcept {
        if (this == &other)
            return *this;
        bytes_ = std::move(other.bytes_);
        memory_ = std::move(other.memory_);
        return *this;
    }
    AccountedBytes(const AccountedBytes&) = delete;
    AccountedBytes& operator=(const AccountedBytes&) = delete;

    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return bytes_.size(); }

private:
    // Later members are destroyed first. Release the charge after the bytes are destroyed.
    Budget::Charge memory_;
    std::vector<std::uint8_t> bytes_;
};

#if defined(__unix__) || defined(__APPLE__)
class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0)
            ::close(value_);
    }

    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this == &other)
            return *this;
        if (value_ >= 0)
            ::close(value_);
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return value_; }

private:
    int value_ = -1;
};

ssize_t read_nointr(int file, void* output, std::size_t size) noexcept {
    ssize_t result = 0;
    do {
        result = ::read(file, output, size);
    } while (result < 0 && errno == EINTR);
    return result;
}

int open_nointr(const char* path, int flags) noexcept {
    int result = -1;
    do {
        result = ::open(path, flags);
    } while (result < 0 && errno == EINTR);
    return result;
}

int openat_nointr(int directory, const char* path, int flags) noexcept {
    int result = -1;
    do {
        result = ::openat(directory, path, flags);
    } while (result < 0 && errno == EINTR);
    return result;
}

int fstat_nointr(int file, struct stat* output) noexcept {
    int result = -1;
    do {
        result = ::fstat(file, output);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool same_file_state(const struct stat& before, const struct stat& after) noexcept {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size) {
        return false;
    }
#if defined(__APPLE__)
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}
#elif defined(_WIN32)
class WindowsHandle {
public:
    explicit WindowsHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~WindowsHandle() {
        if (valid())
            CloseHandle(value_);
    }

    WindowsHandle(WindowsHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    WindowsHandle& operator=(WindowsHandle&& other) noexcept {
        if (this == &other)
            return *this;
        if (valid())
            CloseHandle(value_);
        value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        return *this;
    }
    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;

    bool valid() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool same_file_state(const BY_HANDLE_FILE_INFORMATION& before,
                     const BY_HANDLE_FILE_INFORMATION& after) noexcept {
    return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
           before.nFileIndexHigh == after.nFileIndexHigh &&
           before.nFileIndexLow == after.nFileIndexLow &&
           before.nFileSizeHigh == after.nFileSizeHigh &&
           before.nFileSizeLow == after.nFileSizeLow &&
           CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
}
#endif

struct ResourceRoot {
    fs::path path;
#if defined(__unix__) || defined(__APPLE__)
    int descriptor = -1;
#elif defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring final_path;
#endif
};

enum class FileRole : std::uint8_t { primary, external };

Result<void> charge_file(const OperationContext& context, FileRole role, std::uint64_t bytes) {
    if (role == FileRole::primary) {
        auto input = context.consume(BudgetKind::input_bytes, bytes, "gltf.primary_input");
        if (!input.has_value())
            return input;
    } else {
        auto resource =
            context.consume(BudgetKind::resource_bytes, bytes, "gltf.external_resource");
        if (!resource.has_value())
            return resource;
    }
    return Result<void>::success();
}

#if defined(__unix__) || defined(__APPLE__)
Result<AccountedBytes> read_descriptor(FileDescriptor file, const fs::path& display_path,
                                       FileRole role, const OperationContext& context) {
    struct stat info{};
    if (fstat_nointr(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2180_GLTF_FILE_TYPE",
                                          "the glTF input is not a regular file", &display_path,
                                          &context);
    }
    const auto file_size = static_cast<std::uintmax_t>(info.st_size);
    if (file_size > std::numeric_limits<std::size_t>::max() ||
        file_size > std::numeric_limits<std::uint64_t>::max()) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2181_GLTF_FILE_SIZE",
                                          "the glTF file size is not representable", &display_path,
                                          &context);
    }
    const std::uint64_t size64 = static_cast<std::uint64_t>(file_size);
    auto charged = charge_file(context, role, size64);
    if (!charged.has_value()) {
        return Result<AccountedBytes>::failure(charged.error_code(), charged.diagnostics());
    }
    auto memory = context.budget->reserve(BudgetKind::memory_bytes, size64, "gltf.source_bytes");
    if (!memory.has_value()) {
        return Result<AccountedBytes>::failure(memory.error_code(), memory.diagnostics());
    }
    Budget::Charge charge = std::move(memory).value();

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto control = context.checkpoint(
                {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file", offset,
                 bytes.size(), "bytes"});
            if (!control.has_value()) {
                return Result<AccountedBytes>::failure(control.error_code(), control.diagnostics());
            }
            const std::size_t request = std::min(kReadChunkBytes, bytes.size() - offset);
            const ssize_t received = read_nointr(file.get(), bytes.data() + offset, request);
            if (received <= 0) {
                return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                                  "the glTF file changed or could not be read",
                                                  &display_path, &context);
            }
            offset += static_cast<std::size_t>(received);
        }
        std::uint8_t extra = 0;
        if (read_nointr(file.get(), &extra, 1) != 0) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read",
                                              &display_path, &context);
        }
        struct stat current{};
        if (fstat_nointr(file.get(), &current) != 0 || !same_file_state(info, current)) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read",
                                              &display_path, &context);
        }
        auto completed = context.checkpoint(
            {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file",
             bytes.size(), bytes.size(), "bytes"});
        if (!completed.has_value()) {
            return Result<AccountedBytes>::failure(completed.error_code(), completed.diagnostics());
        }
        AccountedBytes result(std::move(bytes), std::move(charge));
        return Result<AccountedBytes>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                                          "the glTF file allocation failed", &display_path,
                                          &context);
    } catch (const std::length_error&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                                          "the glTF file exceeds the host container limit",
                                          &display_path, &context);
    }
}
#elif defined(_WIN32)
Result<std::wstring> windows_final_path(HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (needed == 0) {
        return fail_value<std::wstring>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                        "the final glTF resource path is unavailable");
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (written == 0 || written >= buffer.size()) {
        return fail_value<std::wstring>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                        "the final glTF resource path is unavailable");
    }
    return Result<std::wstring>::success(std::wstring(buffer.data(), written));
}

void trim_windows_path(std::wstring& path) {
    while (path.size() > 4 && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
}

bool windows_path_is_within(std::wstring root, std::wstring candidate) {
    trim_windows_path(root);
    trim_windows_path(candidate);
    if (candidate.size() <= root.size() ||
        root.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (CompareStringOrdinal(root.data(), static_cast<int>(root.size()), candidate.data(),
                             static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return candidate[root.size()] == L'\\' || candidate[root.size()] == L'/';
}

Result<WindowsHandle> open_windows_file(const fs::path& path, const char* code,
                                        const OperationContext& context) {
    WindowsHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return fail_value<WindowsHandle>(ErrorCode::io_error, code,
                                         "the glTF file could not be opened", &path, &context);
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information) ||
        GetFileType(file.get()) != FILE_TYPE_DISK ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return fail_value<WindowsHandle>(ErrorCode::io_error, "MK2180_GLTF_FILE_TYPE",
                                         "the glTF input is not a regular file", &path, &context);
    }
    return Result<WindowsHandle>::success(std::move(file));
}

Result<AccountedBytes> read_windows_handle(WindowsHandle file, const fs::path& display_path,
                                           FileRole role, const OperationContext& context) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                          "the glTF file state could not be read", &display_path,
                                          &context);
    }
    const std::uint64_t size64 =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    if (size64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2181_GLTF_FILE_SIZE",
                                          "the glTF file size is not representable", &display_path,
                                          &context);
    }
    auto charged = charge_file(context, role, size64);
    if (!charged.has_value())
        return Result<AccountedBytes>::failure(charged.error_code(), charged.diagnostics());
    auto memory = context.budget->reserve(BudgetKind::memory_bytes, size64, "gltf.source_bytes");
    if (!memory.has_value())
        return Result<AccountedBytes>::failure(memory.error_code(), memory.diagnostics());
    Budget::Charge charge = std::move(memory).value();

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size64));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto control = context.checkpoint(
                {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file", offset,
                 bytes.size(), "bytes"});
            if (!control.has_value()) {
                return Result<AccountedBytes>::failure(control.error_code(), control.diagnostics());
            }
            const DWORD request =
                static_cast<DWORD>(std::min<std::size_t>(kReadChunkBytes, bytes.size() - offset));
            DWORD received = 0;
            if (!ReadFile(file.get(), bytes.data() + offset, request, &received, nullptr) ||
                received != request) {
                return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                                  "the glTF file changed or could not be read",
                                                  &display_path, &context);
            }
            offset += received;
        }
        std::uint8_t extra = 0;
        DWORD received = 0;
        if (!ReadFile(file.get(), &extra, 1, &received, nullptr) || received != 0) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read",
                                              &display_path, &context);
        }
        BY_HANDLE_FILE_INFORMATION current{};
        if (!GetFileInformationByHandle(file.get(), &current) || !same_file_state(info, current)) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read",
                                              &display_path, &context);
        }
        auto completed = context.checkpoint(
            {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file",
             bytes.size(), bytes.size(), "bytes"});
        if (!completed.has_value()) {
            return Result<AccountedBytes>::failure(completed.error_code(), completed.diagnostics());
        }
        return Result<AccountedBytes>::success(AccountedBytes(std::move(bytes), std::move(charge)));
    } catch (const std::bad_alloc&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                                          "the glTF file allocation failed", &display_path,
                                          &context);
    } catch (const std::length_error&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                                          "the glTF file exceeds the host container limit",
                                          &display_path, &context);
    }
}
#else
Result<AccountedBytes> read_stream_file(const fs::path& path, FileRole role,
                                        const OperationContext& context) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2180_GLTF_FILE_TYPE",
                                          "the glTF input is not a regular file", &path, &context);
    }
    const std::uintmax_t file_size = fs::file_size(path, error);
    if (error || file_size > std::numeric_limits<std::size_t>::max() ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2181_GLTF_FILE_SIZE",
                                          "the glTF file size is not representable", &path,
                                          &context);
    }
    const fs::file_time_type modified = fs::last_write_time(path, error);
    if (error) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                          "the glTF file state could not be read", &path, &context);
    }
    const std::uint64_t size64 = static_cast<std::uint64_t>(file_size);
    auto charged = charge_file(context, role, size64);
    if (!charged.has_value())
        return Result<AccountedBytes>::failure(charged.error_code(), charged.diagnostics());
    auto memory = context.budget->reserve(BudgetKind::memory_bytes, size64, "gltf.source_bytes");
    if (!memory.has_value()) {
        return Result<AccountedBytes>::failure(memory.error_code(), memory.diagnostics());
    }
    Budget::Charge charge = std::move(memory).value();
    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed or could not be read", &path,
                                              &context);
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto control = context.checkpoint(
                {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file", offset,
                 bytes.size(), "bytes"});
            if (!control.has_value()) {
                return Result<AccountedBytes>::failure(control.error_code(), control.diagnostics());
            }
            const std::size_t request = std::min(kReadChunkBytes, bytes.size() - offset);
            stream.read(reinterpret_cast<char*>(bytes.data() + offset),
                        static_cast<std::streamsize>(request));
            if (stream.gcount() != static_cast<std::streamsize>(request)) {
                return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                                  "the glTF file changed or could not be read",
                                                  &path, &context);
            }
            offset += request;
        }
        char extra = 0;
        stream.get(extra);
        if (stream.gcount() != 0 || stream.bad()) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read", &path,
                                              &context);
        }
        const fs::file_status current_status = fs::symlink_status(path, error);
        if (error || fs::is_symlink(current_status) || !fs::is_regular_file(current_status)) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read", &path,
                                              &context);
        }
        const std::uintmax_t current_size = fs::file_size(path, error);
        if (error) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read", &path,
                                              &context);
        }
        const fs::file_time_type current_modified = fs::last_write_time(path, error);
        if (error || current_size != file_size || current_modified != modified) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2182_GLTF_FILE_READ",
                                              "the glTF file changed while it was read", &path,
                                              &context);
        }
        auto completed = context.checkpoint(
            {"gltf.read", role == FileRole::primary ? "primary_file" : "external_file",
             bytes.size(), bytes.size(), "bytes"});
        if (!completed.has_value()) {
            return Result<AccountedBytes>::failure(completed.error_code(), completed.diagnostics());
        }
        AccountedBytes result(std::move(bytes), std::move(charge));
        return Result<AccountedBytes>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                                          "the glTF file allocation failed", &path, &context);
    }
}
#endif

Result<AccountedBytes> read_primary_file(const fs::path& path, const ResourceRoot& root,
                                         const OperationContext& context) {
#if defined(__unix__) || defined(__APPLE__)
    const std::string name = path.filename().string();
    if (root.descriptor < 0 || name.empty()) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2184_GLTF_FILE_OPEN",
                                          "the glTF input path is invalid", &path, &context);
    }
    FileDescriptor file(openat_nointr(root.descriptor, name.c_str(),
                                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2184_GLTF_FILE_OPEN",
                                          "the glTF input could not be opened", &path, &context);
    }
    return read_descriptor(std::move(file), path, FileRole::primary, context);
#elif defined(_WIN32)
    auto opened = open_windows_file(path, "MK2184_GLTF_FILE_OPEN", context);
    if (!opened.has_value())
        return Result<AccountedBytes>::failure(opened.error_code(), opened.diagnostics());
    auto final_path = windows_final_path(opened.value().get());
    if (!final_path.has_value())
        return Result<AccountedBytes>::failure(final_path.error_code(), final_path.diagnostics());
    if (root.handle == INVALID_HANDLE_VALUE || root.final_path.empty() ||
        !windows_path_is_within(root.final_path, final_path.value())) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2184_GLTF_FILE_OPEN",
                                          "the glTF input is outside its resource directory", &path,
                                          &context);
    }
    return read_windows_handle(std::move(opened).value(), path, FileRole::primary, context);
#else
    return read_stream_file(path, FileRole::primary, context);
#endif
}

bool valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + count >= value.size())
            return false;
        for (std::size_t offset = 1; offset <= count; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((count == 2 && codepoint < 0x800U) || (count == 3 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
            return false;
        }
        index += count + 1;
    }
    return true;
}

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_byte = static_cast<unsigned char>(left[index]);
        auto right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte >= 'A' && left_byte <= 'Z')
            left_byte = static_cast<unsigned char>(left_byte + ('a' - 'A'));
        if (right_byte >= 'A' && right_byte <= 'Z')
            right_byte = static_cast<unsigned char>(right_byte + ('a' - 'A'));
        if (left_byte != right_byte)
            return false;
    }
    return true;
}

bool is_data_uri(std::string_view uri) noexcept {
    if (uri.size() < 5)
        return false;
    uri.remove_suffix(uri.size() - 5);
    return ascii_iequals(uri, "data:");
}

bool is_windows_device_name(std::string_view segment) noexcept {
    const std::size_t dot = segment.find('.');
    std::string_view stem = segment;
    if (dot != std::string_view::npos)
        stem.remove_suffix(stem.size() - dot);
    if (ascii_iequals(stem, "con") || ascii_iequals(stem, "prn") || ascii_iequals(stem, "aux") ||
        ascii_iequals(stem, "nul") || ascii_iequals(stem, "conin$") ||
        ascii_iequals(stem, "conout$")) {
        return true;
    }
    if (stem.size() < 4)
        return false;
    std::string_view prefix = stem;
    prefix.remove_suffix(prefix.size() - 3);
    if (!ascii_iequals(prefix, "com") && !ascii_iequals(prefix, "lpt"))
        return false;
    if (stem.size() == 4)
        return stem[3] >= '1' && stem[3] <= '9';
    if (stem.size() != 5 || static_cast<unsigned char>(stem[3]) != 0xc2U)
        return false;
    const auto digit = static_cast<unsigned char>(stem[4]);
    return digit == 0xb9U || digit == 0xb2U || digit == 0xb3U;
}

bool is_portable_path_segment(std::string_view segment) noexcept {
    if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
        segment.back() == ' ' || is_windows_device_name(segment)) {
        return false;
    }
    return segment.find_first_of("<>\"|?*") == std::string_view::npos;
}

Result<std::vector<std::string>> decode_relative_uri(const std::string& uri) {
    if (uri.empty() || uri.find_first_of("?#\\") != std::string::npos || uri.rfind("//", 0) == 0) {
        return fail_value<std::vector<std::string>>(
            ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
            "the glTF buffer URI is not a portable relative path");
    }

    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] != '%') {
            const auto byte = static_cast<unsigned char>(uri[index]);
            if (byte <= 0x20U || byte == 0x7fU) {
                return fail_value<std::vector<std::string>>(
                    ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                    "the glTF buffer URI contains a control or space byte");
            }
            decoded.push_back(uri[index]);
            continue;
        }
        if (index + 2 >= uri.size()) {
            return fail_value<std::vector<std::string>>(
                ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                "the glTF buffer URI contains an incomplete percent escape");
        }
        const int high = hex_value(uri[index + 1]);
        const int low = hex_value(uri[index + 2]);
        if (high < 0 || low < 0) {
            return fail_value<std::vector<std::string>>(
                ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                "the glTF buffer URI contains an invalid percent escape");
        }
        const char byte = static_cast<char>((high << 4) | low);
        if (byte == '\0' || byte == '/' || byte == '\\') {
            return fail_value<std::vector<std::string>>(
                ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                "the glTF buffer URI contains an unsafe escaped byte");
        }
        decoded.push_back(byte);
        index += 2;
    }
    if (!valid_utf8(decoded) || decoded.empty() || decoded.front() == '/' ||
        decoded.find('\\') != std::string::npos || decoded.find(':') != std::string::npos ||
        std::any_of(decoded.begin(), decoded.end(),
                    [](unsigned char value) { return value < 0x20U || value == 0x7fU; })) {
        return fail_value<std::vector<std::string>>(
            ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
            "the glTF buffer URI is not a valid portable relative path");
    }

    std::vector<std::string> segments;
    std::size_t begin = 0;
    while (begin <= decoded.size()) {
        const std::size_t separator = decoded.find('/', begin);
        const std::size_t end = separator == std::string::npos ? decoded.size() : separator;
        const std::string segment = decoded.substr(begin, end - begin);
        if (!is_portable_path_segment(segment)) {
            return fail_value<std::vector<std::string>>(
                ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                "the glTF buffer URI contains a nonportable path segment");
        }
        segments.push_back(segment);
        if (separator == std::string::npos)
            break;
        begin = separator + 1;
    }
    return Result<std::vector<std::string>>::success(std::move(segments));
}

Result<AccountedBytes> read_external_file(const ResourceRoot& root, const std::string& uri,
                                          const OperationContext& context) {
    auto decoded = decode_relative_uri(uri);
    if (!decoded.has_value()) {
        return Result<AccountedBytes>::failure(decoded.error_code(), decoded.diagnostics());
    }
#if defined(__unix__) || defined(__APPLE__)
    if (root.descriptor < 0) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                                          "the glTF resource directory could not be opened",
                                          &root.path, &context);
    }
    int current_directory = root.descriptor;
    FileDescriptor directory;
    for (std::size_t index = 0; index + 1 < decoded.value().size(); ++index) {
        FileDescriptor child(openat_nointr(current_directory, decoded.value()[index].c_str(),
                                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (child.get() < 0) {
            return fail_value<AccountedBytes>(
                ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                "a glTF resource path component is unavailable or is a symbolic link");
        }
        directory = std::move(child);
        current_directory = directory.get();
    }
    const std::string& name = decoded.value().back();
    FileDescriptor file(openat_nointr(current_directory, name.c_str(),
                                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                          "the glTF resource is unavailable or is a symbolic link");
    }
    return read_descriptor(std::move(file), fs::path(name), FileRole::external, context);
#elif defined(_WIN32)
    if (root.handle == INVALID_HANDLE_VALUE || root.final_path.empty()) {
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                                          "the glTF resource directory could not be opened",
                                          &root.path, &context);
    }
    fs::path candidate = root.path;
    for (const std::string& segment : decoded.value()) {
        candidate /= fs::u8path(segment);
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                              "a glTF resource path is unavailable or is a link");
        }
    }
    auto opened = open_windows_file(candidate, "MK2187_GLTF_RESOURCE_OPEN", context);
    if (!opened.has_value())
        return Result<AccountedBytes>::failure(opened.error_code(), opened.diagnostics());
    auto final_path = windows_final_path(opened.value().get());
    if (!final_path.has_value())
        return Result<AccountedBytes>::failure(final_path.error_code(), final_path.diagnostics());
    if (!windows_path_is_within(root.final_path, final_path.value())) {
        return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                                          "the glTF buffer URI leaves the asset directory");
    }
    return read_windows_handle(std::move(opened).value(), candidate, FileRole::external, context);
#else
    fs::path candidate = root.path;
    std::error_code error;
    for (const std::string& segment : decoded.value()) {
        candidate /= fs::u8path(segment);
        const fs::file_status status = fs::symlink_status(candidate, error);
        if (error || fs::is_symlink(status)) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                              "a glTF resource path is unavailable or is a link");
        }
    }
    const fs::path canonical_root = fs::weakly_canonical(root.path, error);
    if (error)
        return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                                          "the glTF resource directory is unavailable");
    const fs::path canonical_candidate = fs::weakly_canonical(candidate, error);
    const fs::path relative = canonical_candidate.lexically_relative(canonical_root);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2185_GLTF_BUFFER_URI",
                                          "the glTF buffer URI leaves the asset directory");
    }
    fs::path cursor = canonical_root;
    for (const fs::path& segment : relative) {
        cursor /= segment;
        if (fs::is_symlink(fs::symlink_status(cursor, error)) || error) {
            return fail_value<AccountedBytes>(ErrorCode::io_error, "MK2187_GLTF_RESOURCE_OPEN",
                                              "a glTF resource path is a symbolic link");
        }
    }
    return read_stream_file(canonical_candidate, FileRole::external, context);
#endif
}

int base64_value(char value) noexcept {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

Result<AccountedBytes> decode_data_uri(const std::string& uri, const OperationContext& context) {
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos || !is_data_uri(uri)) {
        return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
                                          "the glTF buffer data URI is malformed");
    }
    const std::string_view metadata(uri.data() + 5, comma - 5);
    if (!ascii_iequals(metadata, "application/octet-stream;base64") &&
        !ascii_iequals(metadata, "application/gltf-buffer;base64")) {
        return fail_value<AccountedBytes>(
            ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
            "a glTF buffer data URI needs a permitted media type and base64 encoding");
    }
    const std::string_view encoded(uri.data() + comma + 1, uri.size() - comma - 1);
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
                                          "the glTF buffer base64 length is invalid");
    }
    std::size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=')
        ++padding;
    if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=')
        ++padding;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (index % kReadChunkBytes == 0) {
            auto control = context.checkpoint(
                {"gltf.read", "data_uri_validate", index, encoded.size(), "base64_bytes"});
            if (!control.has_value()) {
                return Result<AccountedBytes>::failure(control.error_code(), control.diagnostics());
            }
        }
        const bool in_padding = index >= encoded.size() - padding;
        if ((in_padding && encoded[index] != '=') ||
            (!in_padding && base64_value(encoded[index]) < 0)) {
            return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
                                              "the glTF buffer contains invalid base64 data");
        }
    }
    if ((padding == 1 && (base64_value(encoded[encoded.size() - 2]) & 0x03) != 0) ||
        (padding == 2 && (base64_value(encoded[encoded.size() - 3]) & 0x0f) != 0)) {
        return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
                                          "the glTF buffer base64 padding bits are not zero");
    }
    auto triples = checked_mul(encoded.size() / 4, 3, "glTF data URI decoded bytes");
    if (!triples.has_value() || triples.value() < padding) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2189_GLTF_DATA_URI_SIZE",
                                          "the glTF buffer data URI size overflows");
    }
    const std::uint64_t decoded_size = triples.value() - padding;
    auto resource_charge =
        context.consume(BudgetKind::resource_bytes, decoded_size, "gltf.data_uri");
    if (!resource_charge.has_value()) {
        return Result<AccountedBytes>::failure(resource_charge.error_code(),
                                               resource_charge.diagnostics());
    }
    auto decoded_charge = context.consume(BudgetKind::decoded_bytes, decoded_size, "gltf.data_uri");
    if (!decoded_charge.has_value()) {
        return Result<AccountedBytes>::failure(decoded_charge.error_code(),
                                               decoded_charge.diagnostics());
    }
    auto memory_charge =
        context.budget->reserve(BudgetKind::memory_bytes, decoded_size, "gltf.data_uri");
    if (!memory_charge.has_value()) {
        return Result<AccountedBytes>::failure(memory_charge.error_code(),
                                               memory_charge.diagnostics());
    }
    Budget::Charge charge = std::move(memory_charge).value();
    auto output_size = checked_size_cast(decoded_size, "glTF data URI decoded bytes");
    if (!output_size.has_value()) {
        return Result<AccountedBytes>::failure(output_size.error_code(), output_size.diagnostics());
    }
    try {
        std::vector<std::uint8_t> output(output_size.value());
        std::size_t write = 0;
        for (std::size_t index = 0; index < encoded.size(); index += 4) {
            if (index % kReadChunkBytes == 0) {
                auto control = context.checkpoint(
                    {"gltf.read", "data_uri", index, encoded.size(), "base64_bytes"});
                if (!control.has_value()) {
                    return Result<AccountedBytes>::failure(control.error_code(),
                                                           control.diagnostics());
                }
            }
            const int a = base64_value(encoded[index]);
            const int b = base64_value(encoded[index + 1]);
            const int c = encoded[index + 2] == '=' ? 0 : base64_value(encoded[index + 2]);
            const int d = encoded[index + 3] == '=' ? 0 : base64_value(encoded[index + 3]);
            const std::uint32_t bits =
                static_cast<std::uint32_t>(a << 18) | static_cast<std::uint32_t>(b << 12) |
                static_cast<std::uint32_t>(c << 6) | static_cast<std::uint32_t>(d);
            if (write < output.size())
                output[write++] = static_cast<std::uint8_t>((bits >> 16) & 0xffU);
            if (write < output.size())
                output[write++] = static_cast<std::uint8_t>((bits >> 8) & 0xffU);
            if (write < output.size())
                output[write++] = static_cast<std::uint8_t>(bits & 0xffU);
        }
        if (write != output.size()) {
            return fail_value<AccountedBytes>(ErrorCode::invalid_data, "MK2188_GLTF_DATA_URI",
                                              "the glTF buffer base64 data has an invalid length");
        }
        AccountedBytes result(std::move(output), std::move(charge));
        return Result<AccountedBytes>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail_value<AccountedBytes>(ErrorCode::resource_limit, "MK2189_GLTF_DATA_URI_SIZE",
                                          "the glTF data URI allocation failed");
    }
}

struct BufferSet {
    BufferSet() = default;
    BufferSet(const BufferSet&) = delete;
    BufferSet& operator=(const BufferSet&) = delete;
    BufferSet(BufferSet&&) noexcept = default;
    BufferSet& operator=(BufferSet&& other) noexcept {
        if (this == &other)
            return *this;

        // Release the old tables before their budget charge.
        owned = std::move(other.owned);
        spans = std::move(other.spans);
        used_bin = other.used_bin;
        table_memory = std::move(other.table_memory);
        return *this;
    }

    // Later members are destroyed first. Release the charge after both tables are destroyed.
    Budget::Charge table_memory;
    std::vector<AccountedBytes> owned;
    std::vector<BufferSpan> spans;
    bool used_bin = false;
};

Result<BufferSet> resolve_buffers(const Document& document, const ResourceRoot* resource_root,
                                  const std::uint8_t* container_data,
                                  const std::optional<ByteRange>& bin,
                                  const OperationContext& context) {
    BufferSet result;
    auto usage = collect_default_scene_usage(document, context);
    if (!usage.has_value()) {
        return Result<BufferSet>::failure(usage.error_code(), usage.diagnostics());
    }
    auto table_items =
        checked_mul(document.buffers.size(), sizeof(AccountedBytes) + sizeof(BufferSpan),
                    "glTF buffer table bytes");
    if (!table_items.has_value()) {
        return Result<BufferSet>::failure(table_items.error_code(), table_items.diagnostics());
    }
    auto table_charge =
        context.budget->reserve(BudgetKind::memory_bytes, table_items.value(), "gltf.buffer_table");
    if (!table_charge.has_value()) {
        return Result<BufferSet>::failure(table_charge.error_code(), table_charge.diagnostics());
    }
    result.table_memory = std::move(table_charge).value();
    try {
        result.owned.reserve(document.buffers.size());
        result.spans.resize(document.buffers.size());
    } catch (const std::bad_alloc&) {
        return fail_value<BufferSet>(ErrorCode::resource_limit, "MK2190_GLTF_BUFFER_TABLE_MEMORY",
                                     "the glTF buffer table allocation failed");
    }

    for (std::size_t index = 0; index < document.buffers.size(); ++index) {
        if (usage.value().buffers[index] == 0)
            continue;
        const BufferDesc& descriptor = document.buffers[index];
        auto size = checked_size_cast(descriptor.byte_length, "glTF buffer length");
        if (!size.has_value())
            return Result<BufferSet>::failure(size.error_code(), size.diagnostics());

        if (!descriptor.uri.has_value()) {
            if (index != 0 || !bin.has_value() || container_data == nullptr) {
                return fail_value<BufferSet>(ErrorCode::invalid_data, "MK2191_GLTF_BUFFER_SOURCE",
                                             "a glTF buffer without a URI has no GLB binary chunk");
            }
            if (descriptor.byte_length > bin->length() ||
                bin->length() - descriptor.byte_length > 3) {
                return fail_value<BufferSet>(ErrorCode::invalid_data, "MK2172_GLB_BUFFER_LENGTH",
                                             "the GLB binary chunk length is invalid");
            }
            auto offset = checked_size_cast(bin->offset(), "GLB binary offset");
            if (!offset.has_value())
                return Result<BufferSet>::failure(offset.error_code(), offset.diagnostics());
            for (std::uint64_t padding_index = descriptor.byte_length;
                 padding_index < bin->length(); ++padding_index) {
                if (container_data[offset.value() + static_cast<std::size_t>(padding_index)] != 0) {
                    return fail_value<BufferSet>(ErrorCode::invalid_data, "MK2173_GLB_BIN_PADDING",
                                                 "the GLB binary padding contains a nonzero byte");
                }
            }
            result.spans[index] = BufferSpan{container_data + offset.value(), size.value()};
            result.used_bin = true;
            continue;
        }

        auto resource_count = context.consume(BudgetKind::external_resources, 1, "gltf.buffer_uri");
        if (!resource_count.has_value()) {
            return Result<BufferSet>::failure(resource_count.error_code(),
                                              resource_count.diagnostics());
        }

        Result<AccountedBytes> loaded =
            is_data_uri(*descriptor.uri) ? decode_data_uri(*descriptor.uri, context)
            : resource_root != nullptr
                ? read_external_file(*resource_root, *descriptor.uri, context)
                : fail_value<AccountedBytes>(ErrorCode::unsupported_feature,
                                             "MK2192_GLTF_EXTERNAL_UNAVAILABLE",
                                             "an in-memory GLB cannot resolve a local "
                                             "buffer URI");
        if (!loaded.has_value()) {
            return Result<BufferSet>::failure(loaded.error_code(), loaded.diagnostics());
        }
        result.owned.push_back(std::move(loaded.value()));
        AccountedBytes& owned = result.owned.back();
        if (owned.size() != size.value()) {
            return fail_value<BufferSet>(
                ErrorCode::invalid_data, "MK2193_GLTF_RESOURCE_LENGTH",
                owned.size() < size.value()
                    ? "a glTF buffer resource is shorter than buffer.byteLength"
                    : "a glTF buffer resource is longer than buffer.byteLength");
        }
        result.spans[index] = BufferSpan{owned.data(), size.value()};
    }
    return Result<BufferSet>::success(std::move(result));
}

Result<SceneRead> read_loaded_asset(const AccountedBytes& primary, const ResourceRoot& root,
                                    FileEncoding encoding, const OperationContext& context) {
    const bool has_glb_magic = primary.size() >= 4 && primary.data()[0] == 'g' &&
                               primary.data()[1] == 'l' && primary.data()[2] == 'T' &&
                               primary.data()[3] == 'F';
    if (encoding == FileEncoding::json && has_glb_magic) {
        return fail_scene(ErrorCode::invalid_data, "MK2196_GLTF_CONTAINER_MISMATCH",
                          "the selected JSON glTF input contains a GLB container");
    }
    if (encoding != FileEncoding::automatic && encoding != FileEncoding::json &&
        encoding != FileEncoding::binary_glb) {
        return fail_scene(ErrorCode::invalid_argument, "MK2197_GLTF_FILE_ENCODING",
                          "the glTF file encoding is invalid");
    }
    const bool is_glb = encoding == FileEncoding::binary_glb ||
                        (encoding == FileEncoding::automatic && has_glb_magic);
    const std::uint8_t* json_data = primary.data();
    std::size_t json_size = primary.size();
    std::optional<ByteRange> bin;
    std::uint64_t unknown_chunk_count = 0;
    if (is_glb) {
        auto framing = glb::parse_glb(primary.data(), primary.size());
        if (!framing.has_value()) {
            return Result<SceneRead>::failure(framing.error_code(), framing.diagnostics());
        }
        auto offset = checked_size_cast(framing.value().json.offset(), "GLB JSON offset");
        auto size = checked_size_cast(framing.value().json.length(), "GLB JSON length");
        if (!offset.has_value() || !size.has_value()) {
            return fail_scene(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                              "the GLB JSON range is not representable");
        }
        json_data = primary.data() + offset.value();
        json_size = size.value();
        bin = framing.value().bin;
        unknown_chunk_count = framing.value().unknown_chunk_count;
    }

    auto document = parse_gltf_json(json_data, json_size, context);
    if (!document.has_value()) {
        return Result<SceneRead>::failure(document.error_code(), document.diagnostics());
    }
    document.value().source_features.unknown_container_chunk_count = unknown_chunk_count;
    const ResourceRoot* resource_root = &root;
    auto buffers = resolve_buffers(document.value(), resource_root, primary.data(), bin, context);
    if (!buffers.has_value()) {
        return Result<SceneRead>::failure(buffers.error_code(), buffers.diagnostics());
    }
    if (bin.has_value() && !buffers.value().used_bin) {
        document.value().source_features.unused_container_bin_count = 1;
    }
    auto result = read_gaussian_scene(document.value(), buffers.value().spans, context);
    if (result.has_value())
        result.value().source_bytes = primary.size();
    return result;
}

}  // namespace

Result<SceneRead> read_file(const fs::path& path, const Limits& limits) try {
    return read_file(path, FileEncoding::automatic, limits);
} catch (const std::bad_alloc&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exhausted available memory");
} catch (const std::length_error&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exceeded a container limit");
}

Result<SceneRead> read_file(const fs::path& path, FileEncoding encoding, const Limits& limits) try {
    auto valid = limits.validate();
    if (!valid.has_value())
        return Result<SceneRead>::failure(valid.error_code(), valid.diagnostics());
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return read_file(path, encoding, context);
} catch (const std::bad_alloc&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exhausted available memory");
} catch (const std::length_error&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exceeded a container limit");
}

Result<SceneRead> read_file(const fs::path& path, const OperationContext& context) try {
    return read_file(path, FileEncoding::automatic, context);
} catch (const std::bad_alloc&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exhausted available memory", &path, &context);
} catch (const std::length_error&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exceeded a container limit", &path, &context);
} catch (const fs::filesystem_error&) {
    return fail_scene(ErrorCode::io_error, "MK2195_GLTF_FILE_EXCEPTION",
                      "the glTF file operation failed", &path, &context);
} catch (const std::exception&) {
    return fail_scene(ErrorCode::internal_error, "MK2195_GLTF_FILE_EXCEPTION",
                      "the glTF file operation failed safely", &path, &context);
}

Result<SceneRead> read_file(const fs::path& path, FileEncoding encoding,
                            const OperationContext& context) try {
    if (context.budget == nullptr) {
        return fail_scene(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                          "the glTF file reader requires a resource budget");
    }
    auto valid = context.budget->limits().validate();
    if (!valid.has_value())
        return Result<SceneRead>::failure(valid.error_code(), valid.diagnostics());
    if (encoding != FileEncoding::automatic && encoding != FileEncoding::json &&
        encoding != FileEncoding::binary_glb) {
        return fail_scene(ErrorCode::invalid_argument, "MK2197_GLTF_FILE_ENCODING",
                          "the glTF file encoding is invalid", &path, &context);
    }
    auto started = context.checkpoint({"gltf.read", "file", 0, 1, "files"});
    if (!started.has_value())
        return Result<SceneRead>::failure(started.error_code(), started.diagnostics());

    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    if (error) {
        return fail_scene(ErrorCode::io_error, "MK2184_GLTF_FILE_OPEN",
                          "the glTF input path could not be resolved", &path, &context);
    }
    const fs::path root = fs::weakly_canonical(absolute.parent_path(), error);
    if (error || root.empty()) {
        return fail_scene(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                          "the glTF resource directory could not be resolved", &path, &context);
    }
    ResourceRoot resource_root;
    resource_root.path = root;
#if defined(__unix__) || defined(__APPLE__)
    FileDescriptor root_directory(
        open_nointr(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root_directory.get() < 0) {
        return fail_scene(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                          "the glTF resource directory could not be opened", &path, &context);
    }
    resource_root.descriptor = root_directory.get();
#elif defined(_WIN32)
    WindowsHandle root_directory(CreateFileW(
        root.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION root_information{};
    if (!root_directory.valid() ||
        !GetFileInformationByHandle(root_directory.get(), &root_information) ||
        (root_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (root_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return fail_scene(ErrorCode::io_error, "MK2186_GLTF_RESOURCE_ROOT",
                          "the glTF resource directory could not be opened", &path, &context);
    }
    auto root_path = windows_final_path(root_directory.get());
    if (!root_path.has_value()) {
        return Result<SceneRead>::failure(root_path.error_code(), root_path.diagnostics());
    }
    resource_root.handle = root_directory.get();
    resource_root.final_path = std::move(root_path).value();
#endif
    auto primary = read_primary_file(absolute, resource_root, context);
    if (!primary.has_value()) {
        return Result<SceneRead>::failure(primary.error_code(), primary.diagnostics());
    }
    auto result = read_loaded_asset(primary.value(), resource_root, encoding, context);
    if (!result.has_value())
        return result;
    auto completed = context.checkpoint({"gltf.read", "file", 1, 1, "files"});
    if (!completed.has_value()) {
        return Result<SceneRead>::failure(completed.error_code(), completed.diagnostics());
    }
    return result;
} catch (const std::bad_alloc&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exhausted available memory", &path, &context);
} catch (const std::length_error&) {
    return fail_scene(ErrorCode::resource_limit, "MK2183_GLTF_FILE_MEMORY",
                      "the glTF file operation exceeded a container limit", &path, &context);
} catch (const fs::filesystem_error&) {
    return fail_scene(ErrorCode::io_error, "MK2195_GLTF_FILE_EXCEPTION",
                      "the glTF file operation failed", &path, &context);
} catch (const std::exception&) {
    return fail_scene(ErrorCode::internal_error, "MK2195_GLTF_FILE_EXCEPTION",
                      "the glTF file operation failed safely", &path, &context);
}

}  // namespace melkor::format::gltf
