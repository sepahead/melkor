// Shared deterministic corpus replay support for the standalone fuzz harnesses.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace melkor::fuzzing {

// Replays every explicitly requested file or directory. A missing/unreadable path is an error:
// otherwise a source checkout that accidentally omitted an ignored corpus would still make the
// regression test green by running only the harness's built-in malformed inputs.
template <typename Exercise>
bool replay_requested_inputs(int argc, char** argv, Exercise exercise, int& cases) {
    namespace fs = std::filesystem;

    for (int i = 1; i < argc; ++i) {
        const fs::path root = argv[i];
        std::vector<fs::path> files;
        try {
            if (!fs::exists(root)) {
                std::fprintf(stderr, "fuzz replay: requested corpus path does not exist: %s\n",
                             root.string().c_str());
                return false;
            }
            if (fs::is_directory(root)) {
                for (const auto& entry : fs::recursive_directory_iterator(root)) {
                    if (entry.is_regular_file()) files.push_back(entry.path());
                }
            } else if (fs::is_regular_file(root)) {
                files.push_back(root);
            } else {
                std::fprintf(stderr,
                             "fuzz replay: requested corpus path is not a regular file or "
                             "directory: %s\n",
                             root.string().c_str());
                return false;
            }
        } catch (const fs::filesystem_error& error) {
            std::fprintf(stderr, "fuzz replay: cannot enumerate %s: %s\n",
                         root.string().c_str(), error.what());
            return false;
        }

        // Filesystem iteration order is unspecified. Sorting makes diagnostics and replay order
        // stable across platforms and filesystems.
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "fuzz replay: cannot open corpus input: %s\n",
                             file.string().c_str());
                return false;
            }
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            if (in.bad()) {
                std::fprintf(stderr, "fuzz replay: failed while reading corpus input: %s\n",
                             file.string().c_str());
                return false;
            }
            exercise(bytes.data(), bytes.size());
            ++cases;
        }
    }
    return true;
}

}  // namespace melkor::fuzzing
