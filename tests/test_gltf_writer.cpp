// Tests for the glTF KHR_gaussian_splatting writer, primarily by round-trip.
//
// The strongest correctness check for a codec pair is that write then read returns what went in.
// A degree-1 SplatData is written to a GLB and read back, and every field -- positions, scales,
// rotations, opacities, and the transposed spherical harmonics -- must match to float precision.
// A degree-4 source additionally pins the degree-3 truncation loss.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_writer.hpp"

#include "melkor/format/glb_container.hpp"
#include "melkor/format/gltf_document.hpp"
#include "melkor/format/gltf_reader.hpp"
#include "melkor/scene.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace melkor;
namespace gltf = melkor::format::gltf;
namespace khr = melkor::format::khr;

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

bool approx(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

bool has_loss(const LossReport& r, const std::string& code) {
    for (const auto& i : r.items())
        if (i.code == code)
            return true;
    return false;
}

bool has_limit_name(const Result<gltf::GlbWriteResult>& result, const std::string& name) {
    for (const Diagnostic& diagnostic : result.diagnostics()) {
        const auto found = diagnostic.context.find("limit_name");
        if (found == diagnostic.context.end())
            continue;
        const auto* value = std::get_if<std::string>(&found->second);
        if (value != nullptr && *value == name)
            return true;
    }
    return false;
}

class RecordingProgressSink final : public ProgressSink {
public:
    void on_progress(const ProgressEvent& event) override { events.push_back(event); }

    std::vector<ProgressEvent> events;
};

class CancelOnPhaseSink final : public ProgressSink {
public:
    CancelOnPhaseSink(CancellationToken token, std::string phase)
        : token_(std::move(token)), phase_(std::move(phase)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == phase_) {
            token_.cancel();
        }
    }

private:
    CancellationToken token_;
    std::string phase_;
};

// Builds a valid degree-`degree` SplatData with `n` splats and distinct, in-domain values.
SplatData make_splats(std::size_t n, std::uint32_t degree) {
    SplatBufferInput in;
    const std::size_t coeffs = (degree + 1) * (degree + 1);
    std::vector<float> sh;
    for (std::size_t s = 0; s < n; ++s) {
        in.positions.push_back(Vec3f{1.0f + s, 2.0f + s, 3.0f + s});
        in.scales.push_back(Vec3f{0.10f + 0.01f * s, 0.20f + 0.01f * s, 0.30f + 0.01f * s});
        // A distinct unit quaternion per splat: rotate about Z by a small angle.
        const float a = 0.2f * static_cast<float>(s);
        in.rotations.push_back(Quatf{0.0f, 0.0f, std::sin(a * 0.5f), std::cos(a * 0.5f)});
        in.opacities.push_back(0.3f + 0.05f * static_cast<float>(s % 10));
        for (std::size_t k = 0; k < coeffs; ++k) {
            for (int c = 0; c < 3; ++c) {
                sh.push_back(static_cast<float>(s) * 0.5f + static_cast<float>(k) +
                             static_cast<float>(c) * 0.1f);
            }
        }
    }
    in.sh = ShBuffer::create(degree, n, std::move(sh)).value();
    return SplatData::create(std::move(in)).value();
}

void test_roundtrip_degree1() {
    const std::size_t n = 4;
    auto original = make_splats(n, 1);

    auto written = gltf::write_glb(original, khr::ColorSpace::lin_rec709_display);
    CHECK(written.has_value());
    if (!written.has_value())
        return;
    CHECK(written.value().losses.empty());  // degree 1 fits the profile: no loss

    const auto framing =
        format::glb::parse_glb(written.value().bytes.data(), written.value().bytes.size());
    CHECK(framing.has_value());
    if (framing.has_value()) {
        const auto parsed =
            gltf::parse_gltf_json(written.value().bytes.data() + framing.value().json.offset(),
                                  static_cast<std::size_t>(framing.value().json.length()));
        CHECK(parsed.has_value());
        if (parsed.has_value()) {
            CHECK(!parsed.value().buffer_views.empty());
            for (const auto& view : parsed.value().buffer_views)
                CHECK(view.target == gltf::BufferViewTarget::array_buffer);
        }
    }

    auto read = gltf::read_glb(written.value().bytes.data(), written.value().bytes.size());
    CHECK(read.has_value());
    if (!read.has_value())
        return;
    const SplatData& back = read.value().data;

    CHECK(back.size() == n);
    CHECK(read.value().sh_degree == 1);
    CHECK(read.value().color_space == khr::ColorSpace::lin_rec709_display);

    for (std::size_t s = 0; s < n; ++s) {
        CHECK(approx(back.positions()[s].x, original.positions()[s].x));
        CHECK(approx(back.positions()[s].z, original.positions()[s].z));
        CHECK(approx(back.scales()[s].y, original.scales()[s].y));
        CHECK(approx(back.opacities()[s], original.opacities()[s]));
        // Identity node write/read keeps the rotation exactly (no eigendecomposition reshuffle).
        CHECK(approx(back.rotations()[s].z, original.rotations()[s].z));
        CHECK(approx(back.rotations()[s].w, original.rotations()[s].w));
    }
    // The spherical harmonics must survive the there-and-back transpose exactly.
    const auto& a = original.sh().raw();
    const auto& b = back.sh().raw();
    CHECK(a.size() == b.size());
    if (a.size() == b.size()) {
        bool all = true;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!approx(a[i], b[i]))
                all = false;
        CHECK(all);
    }
}

void test_empty_scene_is_rejected() {
    // Zero splats cannot be written as valid glTF: the base spec requires accessor.count >= 1, so a
    // zero-count POSITION accessor (and one with no min/max) would be invalid. write_glb refuses.
    auto original = make_splats(0, 0);
    auto written = gltf::write_glb(original, khr::ColorSpace::srgb_rec709_display);
    CHECK(!written.has_value());
}

void test_degree4_is_truncated_with_loss() {
    auto original = make_splats(2, 4);
    auto written = gltf::write_glb(original, khr::ColorSpace::srgb_rec709_display);
    CHECK(written.has_value());
    if (!written.has_value())
        return;
    // Degree 4 exceeds the KHR RC profile: a severe, approvable truncation loss must be reported.
    CHECK(has_loss(written.value().losses, "LOSS_SH_DEGREE_TRUNCATED"));
    CHECK(written.value().losses.has_blocking());
    CHECK(!written.value().losses.check_policy({}).has_value());
    CHECK(written.value().losses.check_policy({"LOSS_SH_DEGREE_TRUNCATED"}).has_value());

    auto read = gltf::read_glb(written.value().bytes.data(), written.value().bytes.size());
    CHECK(read.has_value());
    if (read.has_value()) {
        CHECK(read.value().sh_degree == 3);  // written at the profile ceiling
        // The degree 0-3 coefficients that survived must match the original's.
        const auto& a = original.sh().raw();           // 25 coeffs per splat
        const auto& b = read.value().data.sh().raw();  // 16 coeffs per splat
        // splat 0, DC (flat 0), all channels.
        for (int c = 0; c < 3; ++c) {
            CHECK(approx(a[static_cast<std::size_t>(0 * 25 * 3 + 0 * 3 + c)],
                         b[static_cast<std::size_t>(0 * 16 * 3 + 0 * 3 + c)]));
        }
        // splat 1, flat coefficient 15 (the last degree-3 coefficient).
        for (int c = 0; c < 3; ++c) {
            CHECK(approx(a[static_cast<std::size_t>(1 * 25 * 3 + 15 * 3 + c)],
                         b[static_cast<std::size_t>(1 * 16 * 3 + 15 * 3 + c)]));
        }
    }
}

void test_invalid_limits_are_rejected() {
    auto limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_memory_bytes = 0;
    auto written = gltf::write_glb(make_splats(1, 0), khr::ColorSpace::srgb_rec709_display, limits);
    CHECK(!written.has_value());
    CHECK(written.error_code() == ErrorCode::invalid_argument);
}

void test_invalid_color_space_is_rejected() {
    auto written = gltf::write_glb(make_splats(1, 0), static_cast<khr::ColorSpace>(255),
                                   Limits::for_profile(LimitsProfile::desktop));
    CHECK(!written.has_value());
    CHECK(written.error_code() == ErrorCode::invalid_argument);
}

void test_splat_and_accessor_limits_are_rejected() {
    auto splat_limits = Limits::for_profile(LimitsProfile::desktop);
    splat_limits.max_splats = 1;
    auto too_many_splats =
        gltf::write_glb(make_splats(2, 0), khr::ColorSpace::srgb_rec709_display, splat_limits);
    CHECK(!too_many_splats.has_value());
    CHECK(too_many_splats.error_code() == ErrorCode::resource_limit);
    CHECK(has_limit_name(too_many_splats, "splats"));

    // Degree 0 writes five accessors and five buffer views.
    auto accessor_limits = Limits::for_profile(LimitsProfile::desktop);
    accessor_limits.max_accessors = 9;
    auto too_many_accessors =
        gltf::write_glb(make_splats(1, 0), khr::ColorSpace::srgb_rec709_display, accessor_limits);
    CHECK(!too_many_accessors.has_value());
    CHECK(too_many_accessors.error_code() == ErrorCode::resource_limit);
    CHECK(has_limit_name(too_many_accessors, "accessors"));
}

void test_memory_limit_is_rejected_and_buffer_output_ignores_temp_limit() {
    auto original = make_splats(2, 1);
    auto baseline = gltf::write_glb(original, khr::ColorSpace::lin_rec709_display);
    CHECK(baseline.has_value());
    if (!baseline.has_value())
        return;

    auto output_limits = Limits::for_profile(LimitsProfile::desktop);
    output_limits.max_temp_bytes = 1;
    auto small_temp_output =
        gltf::write_glb(original, khr::ColorSpace::lin_rec709_display, output_limits);
    CHECK(small_temp_output.has_value());
    if (small_temp_output.has_value()) {
        CHECK(small_temp_output.value().bytes == baseline.value().bytes);
        CHECK(small_temp_output.value().retained_memory_bytes() ==
              small_temp_output.value().bytes.size());
    }

    auto memory_limits = Limits::for_profile(LimitsProfile::desktop);
    memory_limits.max_memory_bytes = baseline.value().bytes.size();
    auto memory_failure =
        gltf::write_glb(original, khr::ColorSpace::lin_rec709_display, memory_limits);
    CHECK(!memory_failure.has_value());
    CHECK(memory_failure.error_code() == ErrorCode::resource_limit);
    CHECK(has_limit_name(memory_failure, "memory_bytes"));
}

void test_context_cancellation_stops_a_bounded_loop() {
    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    CancelOnPhaseSink sink(context.cancellation, "positions");
    context.progress = &sink;

    auto written =
        gltf::write_glb(make_splats(5000, 0), khr::ColorSpace::srgb_rec709_display, context);
    CHECK(!written.has_value());
    CHECK(written.error_code() == ErrorCode::cancelled);
    CHECK(budget.used(BudgetKind::memory_bytes) == 0);
    CHECK(budget.used(BudgetKind::temp_bytes) == 0);
}

void test_shared_cardinalities_use_high_water_accounting() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_splats = 2;
    limits.max_accessors = 10;
    limits.max_gltf_nodes = 1;
    Budget budget(limits);
    CHECK(budget.consume(BudgetKind::splats, 2, "gltf.read.splats").has_value());
    CHECK(budget.consume(BudgetKind::accessors, 10, "gltf.read.structure").has_value());
    CHECK(budget.consume(BudgetKind::gltf_nodes, 1, "gltf.read.nodes").has_value());
    OperationContext context = make_default_context(budget);

    auto written =
        gltf::write_glb(make_splats(2, 0), khr::ColorSpace::srgb_rec709_display, context);
    CHECK(written.has_value());
    CHECK(budget.used(BudgetKind::splats) == 2);
    CHECK(budget.used(BudgetKind::accessors) == 10);
    CHECK(budget.used(BudgetKind::gltf_nodes) == 1);
    CHECK(budget.used(BudgetKind::temp_bytes) == 0);
    if (written.has_value()) {
        CHECK(written.value().retained_memory_bytes() == written.value().bytes.size());
        CHECK(budget.used(BudgetKind::memory_bytes) == written.value().retained_memory_bytes());
    }
}

void test_context_deadline_stops_before_allocation() {
    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    context.deadline = Deadline::at(Deadline::TimePoint::min());

    auto written =
        gltf::write_glb(make_splats(1, 0), khr::ColorSpace::srgb_rec709_display, context);
    CHECK(!written.has_value());
    CHECK(written.error_code() == ErrorCode::resource_limit);
    CHECK(written.diagnostics()[0].code == "MK0304_DEADLINE_EXCEEDED");
    CHECK(budget.used(BudgetKind::splats) == 0);
}

void test_context_reports_completion() {
    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    RecordingProgressSink sink;
    context.progress = &sink;

    auto written =
        gltf::write_glb(make_splats(2, 0), khr::ColorSpace::srgb_rec709_display, context);
    CHECK(written.has_value());
    CHECK(!sink.events.empty());
    CHECK(sink.events.front().phase == "prepare");
    CHECK(sink.events.back().phase == "complete");
    CHECK(sink.events.back().completed == 2);
    CHECK(sink.events.back().total == 2);
}

void test_result_releases_retained_memory() {
    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    {
        auto written =
            gltf::write_glb(make_splats(2, 0), khr::ColorSpace::srgb_rec709_display, context);
        CHECK(written.has_value());
        if (written.has_value()) {
            CHECK(written.value().retained_memory_bytes() == written.value().bytes.size());
            CHECK(budget.used(BudgetKind::memory_bytes) == written.value().bytes.size());
        }
    }
    CHECK(budget.used(BudgetKind::memory_bytes) == 0);
}

}  // namespace

int main() {
    test_roundtrip_degree1();
    test_empty_scene_is_rejected();
    test_degree4_is_truncated_with_loss();
    test_invalid_limits_are_rejected();
    test_invalid_color_space_is_rejected();
    test_splat_and_accessor_limits_are_rejected();
    test_memory_limit_is_rejected_and_buffer_output_ignores_temp_limit();
    test_context_cancellation_stops_a_bounded_loop();
    test_shared_cardinalities_use_high_water_accounting();
    test_context_deadline_stops_before_allocation();
    test_context_reports_completion();
    test_result_releases_retained_memory();

    if (g_failures == 0) {
        std::printf("gltf writer: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf writer: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
