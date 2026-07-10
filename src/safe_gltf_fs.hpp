#pragma once

// Include tiny_gltf.h before this internal header.

#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace melkor::gltf_fs {

namespace fs = std::filesystem;

struct Context {
    fs::path root;
    uintmax_t max_file_bytes = static_cast<uintmax_t>(std::numeric_limits<int32_t>::max());
};

inline bool setRootForInput(Context& context, const std::string& filepath,
                            std::string& error) {
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(filepath), ec);
    if (ec) {
        error = "Could not resolve glTF input path";
        return false;
    }
    context.root = fs::weakly_canonical(absolute.parent_path(), ec);
    if (ec || context.root.empty()) {
        error = "Could not establish a safe glTF asset directory";
        return false;
    }
    return true;
}

inline std::optional<fs::path> resolve(const std::string& raw, const Context& context) {
    if (raw.empty() || context.root.empty() || raw.find('\0') != std::string::npos ||
        raw.find("://") != std::string::npos || raw.rfind("file:", 0) == 0) {
        return std::nullopt;
    }
    std::error_code ec;
    fs::path candidate = fs::path(raw);
    if (candidate.is_relative()) candidate = fs::absolute(candidate, ec);
    if (ec) return std::nullopt;
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec) return std::nullopt;

    const fs::path relative = candidate.lexically_relative(context.root);
    if (relative.empty() || relative.is_absolute()) return std::nullopt;
    const auto first = relative.begin();
    if (first != relative.end() && *first == "..") return std::nullopt;
    return candidate;
}

inline tinygltf::FsCallbacks callbacks(Context& context) {
    tinygltf::FsCallbacks result;
    result.user_data = &context;
    result.ExpandFilePath = [](const std::string& path, void* userdata) {
        const auto resolved = resolve(path, *static_cast<Context*>(userdata));
        return resolved ? resolved->string() : std::string{};
    };
    result.FileExists = [](const std::string& path, void* userdata) {
        const auto resolved = resolve(path, *static_cast<Context*>(userdata));
        if (!resolved) return false;
        std::error_code ec;
        return fs::is_regular_file(*resolved, ec) && !ec;
    };
    result.GetFileSizeInBytes = [](size_t* output, std::string* error,
                                   const std::string& path, void* userdata) {
        const auto& context = *static_cast<Context*>(userdata);
        const auto resolved = resolve(path, context);
        if (!resolved) {
            if (error) *error = "path is outside the glTF asset directory";
            return false;
        }
        std::error_code ec;
        const uintmax_t bytes = fs::file_size(*resolved, ec);
        if (ec || bytes > context.max_file_bytes || bytes > std::numeric_limits<size_t>::max()) {
            if (error) *error = "file is unavailable or exceeds the glTF resource limit";
            return false;
        }
        *output = static_cast<size_t>(bytes);
        return true;
    };
    result.ReadWholeFile = [](std::vector<unsigned char>* output, std::string* error,
                              const std::string& path, void* userdata) {
        size_t bytes = 0;
        auto& context = *static_cast<Context*>(userdata);
        const auto resolved = resolve(path, context);
        if (!resolved) {
            if (error) *error = "path is outside the glTF asset directory";
            return false;
        }
        std::error_code ec;
        const uintmax_t file_bytes = fs::file_size(*resolved, ec);
        if (ec || file_bytes > context.max_file_bytes ||
            file_bytes > std::numeric_limits<size_t>::max()) {
            if (error) *error = "file is unavailable or exceeds the glTF resource limit";
            return false;
        }
        bytes = static_cast<size_t>(file_bytes);
        try {
            output->assign(bytes, 0);
        } catch (const std::bad_alloc&) {
            if (error) *error = "not enough memory for glTF resource";
            return false;
        }
        std::ifstream stream(*resolved, std::ios::binary);
        if (!stream || (bytes > 0 &&
            !stream.read(reinterpret_cast<char*>(output->data()),
                         static_cast<std::streamsize>(bytes)))) {
            output->clear();
            if (error) *error = "could not read glTF resource";
            return false;
        }
        return true;
    };
    result.WriteWholeFile = [](std::string* error, const std::string&,
                               const std::vector<unsigned char>&, void*) {
        if (error) *error = "glTF filesystem writes are disabled";
        return false;
    };
    return result;
}

}  // namespace melkor::gltf_fs
