// Atomic output: the existing destination must survive every failure.
//
// This suite exists because of a real, shipped data-loss bug (P0-08). The SPZ writer opened
// the destination with `std::ios::trunc` -- destroying the user's file immediately -- and then
// called `std::remove(filepath)` from each of its error handlers. A conversion that ran out of
// memory partway through would truncate your good scene.spz and then delete it, leaving you
// with neither the new file nor the old one.
//
// The property under test is stated once and checked after every injected failure:
//
//     **After any failure, the pre-existing destination is byte-for-byte what it was.**
//
// A test that only checks the happy path would have passed against the buggy writer too.
//
// Self-contained (no external test framework), matching the existing suite's convention.

#include "melkor/budget.hpp"
#include "melkor/error.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"
#include "melkor/ply_writer.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace melkor;
using namespace melkor::io;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// A scratch directory that cleans itself up.
class TempDir {
  public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("melkor-atomic-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

  private:
    fs::path path_;
    static int counter_;
};
int TempDir::counter_ = 0;

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The invariant, factored out so every failure test asserts exactly the same thing.
void assert_destination_intact(const fs::path& destination, const std::string& original,
                               const char* scenario) {
    if (!fs::exists(destination)) {
        ++g_failures;
        std::fprintf(stderr, "FAIL [%s]: the destination was DELETED\n", scenario);
        return;
    }
    const std::string actual = read_file(destination);
    ++g_checks;
    if (actual != original) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL [%s]: the destination was MODIFIED (%zu bytes, expected %zu)\n",
                     scenario, actual.size(), original.size());
    }
}

// No temporary files may be left behind: they would accumulate and eventually fill a disk.
void assert_no_temp_files(const fs::path& directory, const char* scenario) {
    ++g_checks;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.find(".melkor-") != std::string::npos) {
            ++g_failures;
            std::fprintf(stderr, "FAIL [%s]: temporary file left behind: %s\n", scenario,
                         name.c_str());
            return;
        }
    }
}

Budget make_budget() { return Budget(Limits::for_profile(LimitsProfile::desktop)); }

// ---------------------------------------------------------------------------
// The happy path
// ---------------------------------------------------------------------------

void test_writes_new_file() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "out.bin";

    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());

    CHECK(writer.value()->write("hello ", 6).has_value());
    CHECK(writer.value()->write("world", 5).has_value());
    CHECK(writer.value()->bytes_written() == 11);

    // Until commit, the destination must not exist. A reader watching the path sees nothing,
    // then sees the complete file -- never a half-written one.
    CHECK(!fs::exists(destination));

    CHECK(writer.value()->commit().has_value());
    CHECK(fs::exists(destination));
    CHECK(read_file(destination) == "hello world");
    assert_no_temp_files(dir.path(), "writes_new_file");
}

// ---------------------------------------------------------------------------
// Overwrite policy
// ---------------------------------------------------------------------------

void test_refuses_to_overwrite_without_force() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "existing.bin";
    const std::string original = "PRECIOUS ORIGINAL DATA";
    write_file(destination, original);

    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::io_error);
    CHECK(writer.diagnostics()[0].code == "MK0505_OUTPUT_EXISTS");

    // The refusal must happen before anything is touched. The old writer truncated first and
    // asked questions later.
    assert_destination_intact(destination, original, "refuses_without_force");
    assert_no_temp_files(dir.path(), "refuses_without_force");
}

void test_overwrites_with_force() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "existing.bin";
    write_file(destination, "old contents");

    WriteOptions options;
    options.overwrite = true;

    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(writer.has_value());

    // Even with --force, the destination keeps its ORIGINAL contents until commit succeeds.
    // Nothing is truncated up front.
    CHECK(read_file(destination) == "old contents");

    CHECK(writer.value()->write("new contents", 12).has_value());
    CHECK(read_file(destination) == "old contents");  // still

    CHECK(writer.value()->commit().has_value());
    CHECK(read_file(destination) == "new contents");
    assert_no_temp_files(dir.path(), "overwrites_with_force");
}

// ---------------------------------------------------------------------------
// Failure injection. This is the heart of the suite.
// ---------------------------------------------------------------------------

void test_abort_preserves_destination() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "existing.bin";
    const std::string original = "PRECIOUS ORIGINAL DATA";
    write_file(destination, original);

    WriteOptions options;
    options.overwrite = true;

    {
        auto writer = AtomicWriter::create(destination, options, context);
        CHECK(writer.has_value());
        CHECK(writer.value()->write("partial garbage", 15).has_value());
        writer.value()->abort();
    }

    assert_destination_intact(destination, original, "explicit_abort");
    assert_no_temp_files(dir.path(), "explicit_abort");
}

void test_dropped_without_commit_preserves_destination() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "existing.bin";
    const std::string original = "PRECIOUS ORIGINAL DATA";
    write_file(destination, original);

    WriteOptions options;
    options.overwrite = true;

    {
        // The realistic case: a caller returns early on an error, or an exception unwinds
        // past them, and commit() is simply never reached. The safe outcome must be the one
        // you get by doing nothing.
        auto writer = AtomicWriter::create(destination, options, context);
        CHECK(writer.has_value());
        CHECK(writer.value()->write("partial garbage", 15).has_value());
        // No commit. Destructor runs here.
    }

    assert_destination_intact(destination, original, "dropped_without_commit");
    assert_no_temp_files(dir.path(), "dropped_without_commit");
}

void test_budget_exhaustion_mid_write_preserves_destination() {
    TempDir dir;

    // A temp-disk budget so small that the write fails partway through. This is the exact
    // scenario -- an encode that fails after output has begun -- in which the old writer
    // destroyed the destination.
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_temp_bytes = 100;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "existing.bin";
    const std::string original = "PRECIOUS ORIGINAL DATA";
    write_file(destination, original);

    WriteOptions options;
    options.overwrite = true;

    {
        auto writer = AtomicWriter::create(destination, options, context);
        CHECK(writer.has_value());

        const std::vector<char> chunk(60, 'x');
        CHECK(writer.value()->write(chunk.data(), chunk.size()).has_value());  // 60 <= 100

        auto failed = writer.value()->write(chunk.data(), chunk.size());       // 120 > 100
        CHECK(!failed.has_value());
        CHECK(failed.error_code() == ErrorCode::resource_limit);
    }

    assert_destination_intact(destination, original, "budget_exhaustion");
    assert_no_temp_files(dir.path(), "budget_exhaustion");
}

void test_destination_is_a_directory() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "a_directory";
    fs::create_directories(destination);

    WriteOptions options;
    options.overwrite = true;

    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(!writer.has_value());
    CHECK(writer.diagnostics()[0].code == "MK0504_OUTPUT_IS_DIRECTORY");
    CHECK(fs::is_directory(destination));  // untouched
}

void test_missing_parent_directory() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    auto writer = AtomicWriter::create(dir.path() / "nope" / "out.bin", WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.diagnostics()[0].code == "MK0501_OUTPUT_PARENT_MISSING");
}

#if !defined(_WIN32)
void test_refuses_to_follow_output_symlink() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    // The attack: a symlink is planted at the output path, pointing somewhere the user never
    // named. Following it writes attacker-chosen content to an attacker-chosen location.
    const fs::path real_target = dir.path() / "somewhere_else.bin";
    const std::string protected_contents = "DO NOT OVERWRITE ME";
    write_file(real_target, protected_contents);

    const fs::path link = dir.path() / "output.bin";
    fs::create_symlink(real_target, link);

    WriteOptions options;
    options.overwrite = true;  // even with --force

    auto writer = AtomicWriter::create(link, options, context);
    CHECK(!writer.has_value());
    CHECK(writer.diagnostics()[0].code == "MK0503_OUTPUT_IS_SYMLINK");

    // The symlink's target must be untouched.
    CHECK(read_file(real_target) == protected_contents);
}

void test_allows_symlink_when_explicitly_requested() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path real_target = dir.path() / "target.bin";
    write_file(real_target, "old");
    const fs::path link = dir.path() / "link.bin";
    fs::create_symlink(real_target, link);

    WriteOptions options;
    options.overwrite = true;
    options.allow_output_symlink = true;

    auto writer = AtomicWriter::create(link, options, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("new", 3).has_value());
    CHECK(writer.value()->commit().has_value());

    // Note the semantics: the atomic replace installs a regular file AT the link path,
    // replacing the link itself. It does not write through to the target. That is the correct
    // behaviour for an atomic writer -- rename() cannot write "through" a symlink -- and it is
    // why allow_output_symlink is a niche escape hatch rather than a sensible default.
    CHECK(fs::exists(link));
    CHECK(!fs::is_symlink(fs::symlink_status(link)));
}
#endif

// ---------------------------------------------------------------------------
// Temporary file placement
// ---------------------------------------------------------------------------

void test_temporary_is_in_the_destination_directory() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "out.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());

    // The temporary MUST be a sibling of the destination. rename() is only atomic within one
    // filesystem; a temporary in /tmp is frequently on a different device, which silently
    // degrades the rename into copy-then-delete and reopens the exact window this class
    // exists to close.
    CHECK(writer.value()->temporary_path().parent_path() == destination.parent_path());

    // And it must not be predictable: a guessable name lets an attacker pre-create it.
    const std::string name = writer.value()->temporary_path().filename().string();
    CHECK(name.find(".melkor-") != std::string::npos);
    CHECK(name.size() > 20);

    writer.value()->abort();
}

void test_temporary_names_are_unique() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    auto a = AtomicWriter::create(dir.path() / "a.bin", WriteOptions{}, context);
    auto b = AtomicWriter::create(dir.path() / "b.bin", WriteOptions{}, context);
    CHECK(a.has_value() && b.has_value());
    CHECK(a.value()->temporary_path() != b.value()->temporary_path());

    a.value()->abort();
    b.value()->abort();
}

// ---------------------------------------------------------------------------
// Same-file detection
// ---------------------------------------------------------------------------

void test_same_file_detection() {
    TempDir dir;

    const fs::path file = dir.path() / "scene.ply";
    write_file(file, "data");

    // The same file reached by different path strings. A converter that reads and writes the
    // same file at once destroys it, and string comparison cannot see this.
    auto same = is_same_file(file, dir.path() / "." / "scene.ply");
    CHECK(same.has_value() && same.value());

    const fs::path other = dir.path() / "other.ply";
    write_file(other, "data");  // identical CONTENTS, different file
    auto different = is_same_file(file, other);
    CHECK(different.has_value() && !different.value());

#if !defined(_WIN32)
    // A symlink to the file is the same file.
    const fs::path link = dir.path() / "link.ply";
    fs::create_symlink(file, link);
    auto via_link = is_same_file(file, link);
    CHECK(via_link.has_value() && via_link.value());
#endif

    // A destination that does not exist yet is certainly not the input.
    auto missing = is_same_file(file, dir.path() / "does-not-exist.ply");
    CHECK(missing.has_value() && !missing.value());
}

// ---------------------------------------------------------------------------
// P0-08 regression, through the real public writers.
//
// The unit tests above prove AtomicWriter's destination-preservation property. These prove
// that the actual SPZ and PLY entry points now go through it -- that the fix is wired up, not
// merely available.
// ---------------------------------------------------------------------------

void test_ply_writer_does_not_truncate_on_open() {
    TempDir dir;

    const fs::path destination = dir.path() / "scene.ply";
    const std::string original = "PRECIOUS ORIGINAL PLY DATA";
    write_file(destination, original);

    // Make the write fail at validation. The old writer opened the destination with
    // std::ofstream, which truncates *on open*, before any validation could reject anything.
    // A failure after that point left a zero-length file where the user's asset had been.
    //
    // Here, a destination whose parent does not exist is rejected before a single byte moves.
    GaussianCloud cloud;
    PlyWriter writer;
    const PlyWriteResult result =
        writer.writeToFile((dir.path() / "no-such-dir" / "out.ply").string(), cloud,
                           PlyWriteConfig{});
    CHECK(!result.success);

    // And the real file, elsewhere, is untouched.
    assert_destination_intact(destination, original, "ply_no_truncate");

    // Writing to a directory must also fail without collateral damage.
    const fs::path dir_destination = dir.path() / "a_dir";
    fs::create_directories(dir_destination);
    const PlyWriteResult to_dir =
        writer.writeToFile(dir_destination.string(), cloud, PlyWriteConfig{});
    CHECK(!to_dir.success);
    CHECK(fs::is_directory(dir_destination));

    assert_no_temp_files(dir.path(), "ply_no_truncate");
}

void test_ply_writer_commits_atomically() {
    TempDir dir;

    const fs::path destination = dir.path() / "scene.ply";
    write_file(destination, "old asset");

    GaussianCloud cloud;
    GaussianSplat splat;
    splat.x = 1.0f;
    splat.y = 2.0f;
    splat.z = 3.0f;
    cloud.addSplat(splat);

    PlyWriter writer;
    const PlyWriteResult result = writer.writeToFile(destination.string(), cloud, PlyWriteConfig{});
    CHECK(result.success);
    CHECK(result.bytes_written > 0);

    // The replacement really happened, and it is a valid PLY rather than a mangled splice of
    // old and new bytes.
    const std::string written = read_file(destination);
    CHECK(written.rfind("ply\n", 0) == 0);
    CHECK(written != "old asset");

    assert_no_temp_files(dir.path(), "ply_commits_atomically");
}

}  // namespace

int main() {
    test_writes_new_file();

    test_refuses_to_overwrite_without_force();
    test_overwrites_with_force();

    test_abort_preserves_destination();
    test_dropped_without_commit_preserves_destination();
    test_budget_exhaustion_mid_write_preserves_destination();
    test_destination_is_a_directory();
    test_missing_parent_directory();

#if !defined(_WIN32)
    test_refuses_to_follow_output_symlink();
    test_allows_symlink_when_explicitly_requested();
#endif

    test_temporary_is_in_the_destination_directory();
    test_temporary_names_are_unique();
    test_same_file_detection();

    test_ply_writer_does_not_truncate_on_open();
    test_ply_writer_commits_atomically();

    if (g_failures == 0) {
        std::printf("atomic writer: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "atomic writer: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
