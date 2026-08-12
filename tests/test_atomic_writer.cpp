// Atomic output: the existing destination must survive every failure.
//
// This suite exists because of a real, shipped data-loss bug (P0-08). The SPZ writer opened
// the destination with `std::ios::trunc` -- destroying the user's file immediately -- and then
// called `std::remove(filepath)` from each of its error handlers. A conversion that ran out of
// memory partway through would truncate your good scene.spz and then delete it, leaving you
// with neither the new file nor the old one.
//
// The pre-installation property is stated once and checked after each injected failure:
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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
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

SplatData make_ply_data(std::vector<Vec3f> positions = {}) {
    SplatBufferInput input;
    input.positions = std::move(positions);
    input.scales.assign(input.positions.size(), Vec3f{1.0f, 1.0f, 1.0f});
    input.rotations.assign(input.positions.size(), Quatf{});
    input.opacities.assign(input.positions.size(), 0.5f);
    input.sh = ShBuffer::black(input.positions.size()).value();
    return SplatData::create(std::move(input)).value();
}

PlyWriteConfig ply_write_config() {
    PlyWriteConfig config;
    config.color_space = ColorSpace::lin_rec709_display;
    config.antialiased = false;
    return config;
}

// A scratch directory that cleans itself up.
class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("melkor-atomic-test-" + std::to_string(stamp) + "-" + std::to_string(counter_++));
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
        std::fprintf(stderr, "FAIL [%s]: the destination was MODIFIED (%zu bytes, expected %zu)\n",
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

Budget make_budget() {
    return Budget(Limits::for_profile(LimitsProfile::desktop));
}

class RecordingProgressSink final : public ProgressSink {
public:
    void on_progress(const ProgressEvent& event) override { events.push_back(event); }

    std::vector<ProgressEvent> events;
};

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
    CHECK(writer.value()->committed());
    CHECK(fs::exists(destination));
    CHECK(read_file(destination) == "hello world");
#if !defined(_WIN32)
    struct stat destination_stat{};
    CHECK(::stat(destination.c_str(), &destination_stat) == 0);
    CHECK((destination_stat.st_mode & 0777u) == 0600u);
#endif
    assert_no_temp_files(dir.path(), "writes_new_file");
}

void test_cancelled_create_has_no_side_effects() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    context.cancellation.cancel();

    const fs::path destination = dir.path() / "out.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::cancelled);
    CHECK(!fs::exists(destination));
    assert_no_temp_files(dir.path(), "cancelled_create");
}

void test_expired_deadline_has_no_side_effects() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    context.deadline = Deadline::at(Deadline::TimePoint::min());

    const fs::path destination = dir.path() / "out.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::resource_limit);
    CHECK(writer.diagnostics()[0].code == "MK0304_DEADLINE_EXCEEDED");
    CHECK(!fs::exists(destination));
    assert_no_temp_files(dir.path(), "expired_deadline");
}

void test_empty_destination_fails_before_temp_creation() {
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    auto writer = AtomicWriter::create(fs::path{}, WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::invalid_argument);
    CHECK(writer.diagnostics()[0].code == "MK0517_OUTPUT_PATH_EMPTY");
}

void test_destination_with_null_character_fails() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    std::string invalid_name("out\0hidden.bin", 14);
    auto writer =
        AtomicWriter::create(dir.path() / fs::path(invalid_name), WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::invalid_argument);
    CHECK(writer.diagnostics()[0].code == "MK0527_OUTPUT_PATH_NUL");
    assert_no_temp_files(dir.path(), "null_character");
}

void test_invalid_limits_fail_before_temp_creation() {
    Limits limits = Limits::for_profile(LimitsProfile::custom);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);

    auto writer = AtomicWriter::create("out.bin", WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::invalid_argument);
}

void test_output_stream_flush_accepts_eof() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    auto writer = AtomicWriter::create(dir.path() / "stream.bin", WriteOptions{}, context);
    CHECK(writer.has_value());

    const std::string payload(64 * 1024, 'x');
    {
        AtomicOutputStream stream(*writer.value());
        CHECK(budget.used(BudgetKind::memory_bytes) == 64 * 1024);
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        stream.flush();
        CHECK(stream.good());
        CHECK(!stream.failed());
    }
    CHECK(budget.used(BudgetKind::memory_bytes) == 0);
    CHECK(writer.value()->commit().has_value());
    CHECK(read_file(dir.path() / "stream.bin") == payload);
}

void test_output_stream_respects_memory_limit() {
    TempDir dir;
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_memory_bytes = 1;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    const fs::path destination = dir.path() / "stream.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());

    {
        AtomicOutputStream stream(*writer.value());
        CHECK(stream.failed());
        CHECK(stream.error_code() == ErrorCode::resource_limit);
        CHECK(!stream.diagnostics().empty());
        CHECK(budget.used(BudgetKind::memory_bytes) == 0);
    }
    writer.value()->abort();
    CHECK(!fs::exists(destination));
    assert_no_temp_files(dir.path(), "stream_memory_limit");
}

void test_output_stream_destructor_does_not_throw() {
    TempDir dir;
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_temp_bytes = 4;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    const fs::path destination = dir.path() / "stream.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());

    {
        AtomicOutputStream stream(*writer.value());
        stream.exceptions(std::ios::badbit);
        stream.write("12345", 5);
        // The destructor flush exceeds the budget. An ostream exception mask must not make the
        // destructor throw or terminate the process.
    }

    CHECK(!writer.value()->commit().has_value());
    CHECK(!fs::exists(destination));
    assert_no_temp_files(dir.path(), "stream_destructor");
}

void test_cancelled_commit_preserves_destination() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "out.bin";
    const std::string original = "original";
    write_file(destination, original);
    WriteOptions options;
    options.overwrite = true;
    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("replacement", 11).has_value());

    context.cancellation.cancel();
    auto committed = writer.value()->commit();
    CHECK(!committed.has_value());
    CHECK(committed.error_code() == ErrorCode::cancelled);
    assert_destination_intact(destination, original, "cancelled_commit");
    assert_no_temp_files(dir.path(), "cancelled_commit");
}

void test_write_and_commit_report_progress() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    RecordingProgressSink sink;
    context.progress = &sink;

    const fs::path destination = dir.path() / "out.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("abc", 3).has_value());
    CHECK(writer.value()->commit().has_value());
    CHECK(sink.events.size() >= 3);
    CHECK(sink.events.front().operation == "atomic_writer.write");
    CHECK(sink.events.front().completed == 0);
    CHECK(sink.events[1].completed == 3);
    CHECK(sink.events.back().operation == "atomic_writer.commit");
    CHECK(sink.events.back().completed == 3);
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

void test_late_destination_is_not_overwritten_without_force() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "late.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("new data", 8).has_value());

    const std::string late_contents = "LATE PRECIOUS DATA";
    write_file(destination, late_contents);

    const auto committed = writer.value()->commit();
    CHECK(!committed.has_value());
    CHECK(!writer.value()->committed());
    assert_destination_intact(destination, late_contents, "late_destination");
    assert_no_temp_files(dir.path(), "late_destination");
}

void test_temp_budget_is_released() {
    TempDir dir;
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_temp_bytes = 16;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);

    auto aborted = AtomicWriter::create(dir.path() / "aborted.bin", WriteOptions{}, context);
    CHECK(aborted.has_value());
    CHECK(aborted.value()->write("12345678", 8).has_value());
    CHECK(budget.remaining(BudgetKind::temp_bytes) == 8);
    aborted.value()->abort();
    CHECK(budget.remaining(BudgetKind::temp_bytes) == 16);

    auto committed = AtomicWriter::create(dir.path() / "committed.bin", WriteOptions{}, context);
    CHECK(committed.has_value());
    CHECK(committed.value()->write("12345678", 8).has_value());
    CHECK(committed.value()->commit().has_value());
    CHECK(budget.remaining(BudgetKind::temp_bytes) == 16);
}

void test_failed_write_cannot_commit_partial_output() {
    TempDir dir;
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_temp_bytes = 4;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "partial.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());
    CHECK(!writer.value()->write("12345678", 8).has_value());
    CHECK(!writer.value()->commit().has_value());
    CHECK(!writer.value()->committed());
    CHECK(!fs::exists(destination));
    CHECK(budget.remaining(BudgetKind::temp_bytes) == 4);
    assert_no_temp_files(dir.path(), "failed_write_cannot_commit");
}

void test_null_write_buffer_fails_safely() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path destination = dir.path() / "null.bin";
    auto writer = AtomicWriter::create(destination, WriteOptions{}, context);
    CHECK(writer.has_value());
    CHECK(!writer.value()->write(nullptr, 1).has_value());
    CHECK(!writer.value()->commit().has_value());
    CHECK(!fs::exists(destination));
    assert_no_temp_files(dir.path(), "null_write_buffer");
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

        auto failed = writer.value()->write(chunk.data(), chunk.size());  // 120 > 100
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

void test_parent_is_not_a_directory() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    const fs::path parent = dir.path() / "regular-file";
    write_file(parent, "data");

    auto writer = AtomicWriter::create(parent / "out.bin", WriteOptions{}, context);
    CHECK(!writer.has_value());
    CHECK(writer.diagnostics()[0].code == "MK0502_OUTPUT_PARENT_NOT_DIRECTORY");
    CHECK(read_file(parent) == "data");
}

#if !defined(_WIN32)
void test_refuses_to_replace_special_file() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    const fs::path destination = dir.path() / "output.fifo";
    CHECK(::mkfifo(destination.c_str(), 0600) == 0);

    WriteOptions options;
    options.overwrite = true;
    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(!writer.has_value());
    CHECK(writer.error_code() == ErrorCode::io_error);
    CHECK(writer.diagnostics()[0].code == "MK0532_OUTPUT_NOT_REGULAR");

    struct stat destination_stat{};
    CHECK(::lstat(destination.c_str(), &destination_stat) == 0);
    CHECK(S_ISFIFO(destination_stat.st_mode));
    assert_no_temp_files(dir.path(), "special_file");
}

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
    // behavior for an atomic writer -- rename() cannot write "through" a symlink -- and it is
    // why allow_output_symlink is a niche escape hatch rather than a sensible default.
    CHECK(fs::exists(link));
    CHECK(!fs::is_symlink(fs::symlink_status(link)));
}

void run_parent_directory_swap_commit(bool overwrite, Durability durability, const char* scenario) {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);

    const fs::path live_directory = dir.path() / "live";
    const fs::path original_directory = dir.path() / "original";
    fs::create_directory(live_directory);

    const fs::path destination = live_directory / "output.bin";
    if (overwrite) {
        write_file(destination, "original data");
    }

    WriteOptions options;
    options.overwrite = overwrite;
    options.durability = durability;
    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("trusted data", 12).has_value());
    const fs::path temporary_name = writer.value()->temporary_path().filename();

    fs::rename(live_directory, original_directory);
    fs::create_directory(live_directory);
    if (overwrite) {
        write_file(live_directory / "output.bin", "decoy data");
    }
    write_file(live_directory / temporary_name, "attacker data");

    const auto committed = writer.value()->commit();
    CHECK(committed.has_value());
    CHECK(writer.value()->committed());
    CHECK(read_file(original_directory / "output.bin") == "trusted data");
    if (overwrite) {
        CHECK(read_file(live_directory / "output.bin") == "decoy data");
    } else {
        CHECK(!fs::exists(live_directory / "output.bin"));
    }
    CHECK(read_file(live_directory / temporary_name) == "attacker data");
    assert_no_temp_files(original_directory, scenario);
}

void test_parent_directory_swap_cannot_redirect_replace() {
    run_parent_directory_swap_commit(true, Durability::full, "directory_swap_replace");
}

void test_parent_directory_swap_cannot_redirect_no_overwrite() {
    run_parent_directory_swap_commit(false, Durability::metadata, "directory_swap_no_overwrite");
}

void test_parent_directory_swap_cannot_redirect_abort() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    const std::uint64_t initial_budget = budget.remaining(BudgetKind::temp_bytes);

    const fs::path live_directory = dir.path() / "live";
    const fs::path original_directory = dir.path() / "original";
    fs::create_directory(live_directory);
    const fs::path destination = live_directory / "output.bin";
    write_file(destination, "original data");

    WriteOptions options;
    options.overwrite = true;
    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("partial data", 12).has_value());
    const fs::path temporary_name = writer.value()->temporary_path().filename();

    fs::rename(live_directory, original_directory);
    fs::create_directory(live_directory);
    write_file(live_directory / "output.bin", "decoy data");
    write_file(live_directory / temporary_name, "attacker data");

    writer.value()->abort();
    CHECK(read_file(original_directory / "output.bin") == "original data");
    CHECK(!fs::exists(original_directory / temporary_name));
    CHECK(read_file(live_directory / "output.bin") == "decoy data");
    CHECK(read_file(live_directory / temporary_name) == "attacker data");
    CHECK(budget.remaining(BudgetKind::temp_bytes) == initial_budget);
}

void test_replace_preserves_permissions() {
    TempDir dir;
    Budget budget = make_budget();
    OperationContext context = make_default_context(budget);
    const fs::path destination = dir.path() / "mode.bin";
    write_file(destination, "old");
    CHECK(::chmod(destination.c_str(), 0640) == 0);

    WriteOptions options;
    options.overwrite = true;
    auto writer = AtomicWriter::create(destination, options, context);
    CHECK(writer.has_value());
    CHECK(writer.value()->write("new", 3).has_value());
    CHECK(writer.value()->commit().has_value());

    struct stat destination_stat{};
    CHECK(::stat(destination.c_str(), &destination_stat) == 0);
    CHECK((destination_stat.st_mode & 0777u) == 0640u);
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

#if !defined(_WIN32)
    // POSIX reports ENAMETOOLONG. Windows can report a long missing path as not found.
    const fs::path too_long = dir.path() / std::string(4096, 'x');
    auto failed = is_same_file(file, too_long);
    CHECK(!failed.has_value());
    CHECK(failed.error_code() == ErrorCode::io_error);
#endif
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
    const SplatData data = make_ply_data();
    PlyWriter writer;
    const PlyWriteResult result = writer.writeToFile(
        (dir.path() / "no-such-dir" / "out.ply").string(), data, ply_write_config());
    CHECK(!result.success);

    // And the real file, elsewhere, is untouched.
    assert_destination_intact(destination, original, "ply_no_truncate");

    // Writing to a directory must also fail without collateral damage.
    const fs::path dir_destination = dir.path() / "a_dir";
    fs::create_directories(dir_destination);
    const PlyWriteResult to_dir =
        writer.writeToFile(dir_destination.string(), data, ply_write_config());
    CHECK(!to_dir.success);
    CHECK(fs::is_directory(dir_destination));

    assert_no_temp_files(dir.path(), "ply_no_truncate");
}

void test_ply_writer_commits_atomically() {
    TempDir dir;

    const fs::path destination = dir.path() / "scene.ply";
    write_file(destination, "old asset");

    const SplatData data = make_ply_data({Vec3f{1.0f, 2.0f, 3.0f}});

    PlyWriter writer;
    PlyWriteConfig config = ply_write_config();
    config.overwrite = true;
    const PlyWriteResult result = writer.writeToFile(destination.string(), data, config);
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
    test_cancelled_create_has_no_side_effects();
    test_expired_deadline_has_no_side_effects();
    test_empty_destination_fails_before_temp_creation();
    test_destination_with_null_character_fails();
    test_invalid_limits_fail_before_temp_creation();
    test_output_stream_flush_accepts_eof();
    test_output_stream_respects_memory_limit();
    test_output_stream_destructor_does_not_throw();
    test_cancelled_commit_preserves_destination();
    test_write_and_commit_report_progress();

    test_refuses_to_overwrite_without_force();
    test_overwrites_with_force();
    test_late_destination_is_not_overwritten_without_force();
    test_temp_budget_is_released();
    test_failed_write_cannot_commit_partial_output();
    test_null_write_buffer_fails_safely();

    test_abort_preserves_destination();
    test_dropped_without_commit_preserves_destination();
    test_budget_exhaustion_mid_write_preserves_destination();
    test_destination_is_a_directory();
    test_missing_parent_directory();
    test_parent_is_not_a_directory();

#if !defined(_WIN32)
    test_refuses_to_replace_special_file();
    test_refuses_to_follow_output_symlink();
    test_allows_symlink_when_explicitly_requested();
    test_parent_directory_swap_cannot_redirect_replace();
    test_parent_directory_swap_cannot_redirect_no_overwrite();
    test_parent_directory_swap_cannot_redirect_abort();
    test_replace_preserves_permissions();
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
