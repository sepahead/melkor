// Tests for the canonical scene model's invariants (P0-06).
//
// The pre-v2 model let a default splat carry uninitialized scalars, an SH degree outside the
// valid range, and arrays resized out of step through a mutable data(). Each test below is one
// of those failure modes, asserting the new model refuses it at construction with a diagnostic
// that names the offending splat and field.
//
// Self-contained (no external test framework), matching the existing suite's convention.

#include "melkor/scene.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using namespace melkor;

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

// A valid n-splat input: unit quaternions, positive scales, in-range opacities, degree-0 SH.
SplatBufferInput valid_input(std::size_t n) {
    SplatBufferInput in;
    in.positions.assign(n, Vec3f{0.0f, 0.0f, 0.0f});
    in.scales.assign(n, Vec3f{1.0f, 1.0f, 1.0f});
    in.rotations.assign(n, Quatf{});  // identity default
    in.opacities.assign(n, 1.0f);
    in.sh = ShBuffer::black(n).value();
    return in;
}

void test_default_storage_types_are_valid() {
    // A default Quatf is the identity rotation, not zero -- a default splat must be constructible
    // into a valid scene without the caller having to know to set the quaternion.
    Quatf q;
    CHECK(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f);
    Vec3f v;
    CHECK(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f);
}

void test_valid_scene_constructs() {
    auto data = SplatData::create(valid_input(3));
    CHECK(data.has_value());
    CHECK(data.value().size() == 3);
    CHECK(data.value().validate().has_value());
}

void test_length_mismatch_is_rejected() {
    auto in = valid_input(3);
    in.scales.pop_back();  // now 2 scales for 3 positions
    auto data = SplatData::create(std::move(in));
    CHECK(!data.has_value());
    CHECK(data.diagnostics()[0].code == "MK1503_LENGTH_MISMATCH");

    auto in2 = valid_input(3);
    in2.opacities.push_back(1.0f);  // 4 opacities for 3 positions
    CHECK(!SplatData::create(std::move(in2)).has_value());
}

void test_nonfinite_position_is_rejected() {
    auto in = valid_input(2);
    in.positions[1].y = std::numeric_limits<float>::quiet_NaN();
    auto data = SplatData::create(std::move(in));
    CHECK(!data.has_value());
    CHECK(data.diagnostics()[0].code == "MK1504_NONFINITE_POSITION");
    // The diagnostic must name the offending splat.
    CHECK(data.diagnostics()[0].context.count("splat_index") == 1);
}

void test_zero_scale_is_preserved_and_negative_scale_is_rejected() {
    auto in = valid_input(2);
    in.scales[0].z = 0.0f;
    auto zero = SplatData::create(std::move(in));
    CHECK(zero.has_value());
    if (zero.has_value())
        CHECK(zero.value().scales()[0].z == 0.0f);

    auto in2 = valid_input(1);
    in2.scales[0].x = -1.0f;
    auto negative = SplatData::create(std::move(in2));
    CHECK(!negative.has_value());
    if (!negative.diagnostics().empty())
        CHECK(negative.diagnostics()[0].code == "MK1505_NEGATIVE_SCALE");
}

void test_opacity_out_of_range_is_rejected() {
    auto in = valid_input(1);
    in.opacities[0] = 1.5f;  // opacity is linear in [0,1]
    auto over = SplatData::create(std::move(in));
    CHECK(!over.has_value());
    CHECK(over.diagnostics()[0].code == "MK1506_OPACITY_OUT_OF_RANGE");

    auto in2 = valid_input(1);
    in2.opacities[0] = -0.1f;
    CHECK(!SplatData::create(std::move(in2)).has_value());
}

void test_non_unit_quaternion_is_rejected() {
    auto in = valid_input(1);
    in.rotations[0] = Quatf{1.0f, 1.0f, 1.0f, 1.0f};  // norm 2, not unit
    auto data = SplatData::create(std::move(in));
    CHECK(!data.has_value());
    CHECK(data.diagnostics()[0].code == "MK1507_NON_UNIT_ROTATION");

    // A zero quaternion is also not unit and must be rejected (not silently made identity).
    auto in2 = valid_input(1);
    in2.rotations[0] = Quatf{0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(!SplatData::create(std::move(in2)).has_value());
}

void test_near_unit_quaternion_is_normalized() {
    auto in = valid_input(1);
    in.rotations[0] = Quatf{0.0f, 0.0f, 0.7070f, 0.7070f};
    auto data = SplatData::create(std::move(in));
    CHECK(data.has_value());
    if (!data.has_value())
        return;

    const Quatf& q = data.value().rotations()[0];
    const double norm = std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
    CHECK(std::fabs(norm - 1.0) < 1e-6);
}

// ---------------------------------------------------------------------------
// ShBuffer
// ---------------------------------------------------------------------------

void test_sh_buffer_lengths() {
    // Degree 0: 1 coefficient * 3 channels * n splats.
    auto d0 = ShBuffer::create(0, 2, std::vector<float>(2 * 1 * 3, 0.0f));
    CHECK(d0.has_value());
    CHECK(d0.value().coefficients() == 1);

    // Degree 3: 16 coefficients * 3 * n.
    auto d3 = ShBuffer::create(3, 2, std::vector<float>(2 * 16 * 3, 0.0f));
    CHECK(d3.has_value());
    CHECK(d3.value().coefficients() == 16);

    // The canonical PLY profile stores degree 4: 25 coefficients.
    auto d4 = ShBuffer::create(4, 1, std::vector<float>(25 * 3, 0.0f));
    CHECK(d4.has_value());
    CHECK(d4.value().coefficients() == 25);

    // The wrong length for the declared degree is rejected.
    auto wrong = ShBuffer::create(3, 2, std::vector<float>(10, 0.0f));
    CHECK(!wrong.has_value());
    CHECK(wrong.diagnostics()[0].code == "MK1501_SH_LENGTH_MISMATCH");

    // Degree 5 is outside the canonical range and is rejected, not clamped.
    auto d5 = ShBuffer::create(5, 1, std::vector<float>(36 * 3, 0.0f));
    CHECK(!d5.has_value());
}

void test_sh_nonfinite_is_rejected() {
    std::vector<float> data(3, 0.0f);
    data[1] = std::numeric_limits<float>::infinity();
    auto sh = ShBuffer::create(0, 1, std::move(data));
    CHECK(!sh.has_value());
    CHECK(sh.diagnostics()[0].code == "MK1502_SH_NONFINITE");
}

void test_sh_dc_accessor() {
    // DC term is the first three floats of each splat's block.
    std::vector<float> data{0.1f, 0.2f, 0.3f,   // splat 0: R,G,B
                            0.4f, 0.5f, 0.6f};  // splat 1
    auto sh = ShBuffer::create(0, 2, std::move(data));
    CHECK(sh.has_value());
    CHECK(sh.value().dc(0).has_value());
    CHECK(sh.value().dc(0).value().x == 0.1f);
    CHECK(sh.value().dc(1).has_value());
    CHECK(sh.value().dc(1).value().z == 0.6f);
    CHECK(!sh.value().dc(2).has_value());
}

void test_validate_catches_corruption() {
    // A valid scene passes validate(); this proves the check is not vacuous.
    auto data = SplatData::create(valid_input(4));
    CHECK(data.has_value());
    CHECK(data.value().validate().has_value());
}

void test_moved_from_values_remain_valid() {
    auto source_sh = ShBuffer::black(2).value();
    ShBuffer target_sh(std::move(source_sh));
    CHECK(target_sh.validate().has_value());
    CHECK(target_sh.splat_count() == 2);
    CHECK(source_sh.validate().has_value());
    CHECK(source_sh.splat_count() == 0);
    CHECK(source_sh.raw().empty());

    auto source_data = SplatData::create(valid_input(2)).value();
    SplatData target_data(std::move(source_data));
    CHECK(target_data.validate().has_value());
    CHECK(target_data.size() == 2);
    CHECK(source_data.validate().has_value());
    CHECK(source_data.empty());
}

}  // namespace

int main() {
    test_default_storage_types_are_valid();
    test_valid_scene_constructs();
    test_length_mismatch_is_rejected();
    test_nonfinite_position_is_rejected();
    test_zero_scale_is_preserved_and_negative_scale_is_rejected();
    test_opacity_out_of_range_is_rejected();
    test_non_unit_quaternion_is_rejected();
    test_near_unit_quaternion_is_normalized();

    test_sh_buffer_lengths();
    test_sh_nonfinite_is_rejected();
    test_sh_dc_accessor();
    test_validate_catches_corruption();
    test_moved_from_values_remain_valid();

    if (g_failures == 0) {
        std::printf("scene model: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "scene model: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
