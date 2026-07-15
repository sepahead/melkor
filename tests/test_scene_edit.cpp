#include "melkor/budget.hpp"
#include "melkor/scene.hpp"

#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace melkor;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

SplatData scene(std::size_t count = 2, std::uint32_t degree = 0) {
    SplatBufferInput input;
    input.positions.assign(count, Vec3f{1.0f, 2.0f, 3.0f});
    input.scales.assign(count, Vec3f{1.0f, 1.0f, 1.0f});
    input.rotations.assign(count, Quatf{});
    input.opacities.assign(count, 0.5f);
    const std::size_t coefficients = (degree + 1) * (degree + 1);
    input.sh = ShBuffer::create(degree, count,
                                std::vector<float>(count * coefficients * 3, 0.25f))
                   .value();
    return SplatData::create(std::move(input)).value();
}

Budget desktop_budget() { return Budget(Limits::for_profile(LimitsProfile::desktop)); }

void test_valid_commit_replaces_only_the_new_value() {
    SplatData original = scene();
    auto edit = original.edit();
    auto scales = original.scales();
    scales[0] = Vec3f{2.0f, 3.0f, 4.0f};
    edit.set_scales(std::move(scales));
    auto changed = edit.commit();

    CHECK(changed.has_value());
    CHECK(changed.value().scales()[0].x == 2.0f);
    CHECK(original.scales()[0].x == 1.0f);
    CHECK(original.validate().has_value());
}

void test_failed_commit_is_atomic() {
    SplatData original = scene();
    const Vec3f position_before = original.positions()[0];
    const Vec3f scale_before = original.scales()[0];
    const float opacity_before = original.opacities()[0];
    const std::vector<float> sh_before = original.sh().raw();

    auto edit = original.edit();
    auto scales = original.scales();
    scales[0].x = -1.0f;
    edit.set_scales(std::move(scales));
    auto failed = edit.commit();

    CHECK(!failed.has_value());
    CHECK(failed.diagnostics()[0].code == "MK1505_NONPOSITIVE_SCALE");
    CHECK(original.positions()[0].x == position_before.x);
    CHECK(original.positions()[0].y == position_before.y);
    CHECK(original.positions()[0].z == position_before.z);
    CHECK(original.scales()[0].x == scale_before.x);
    CHECK(original.opacities()[0] == opacity_before);
    CHECK(original.sh().raw() == sh_before);
    CHECK(original.validate().has_value());
}

void test_length_mismatch_is_rejected_at_commit() {
    SplatData original = scene();
    auto edit = original.edit();
    edit.set_opacities(std::vector<float>{0.25f});
    auto failed = edit.commit();
    CHECK(!failed.has_value());
    CHECK(failed.diagnostics()[0].code == "MK1503_LENGTH_MISMATCH");
    CHECK(original.size() == 2);
}

void test_budget_failure_leaves_logical_content_unchanged() {
    SplatData original = scene();
    auto edit = original.edit();
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_memory_bytes = 1;  // Nonzero: zero means unlimited in Budget::consume.
    Budget tiny(limits);

    auto reserved = edit.reserve(100, tiny);
    CHECK(!reserved.has_value());
    CHECK(reserved.error_code() == ErrorCode::resource_limit);

    // A failed reserve did not touch values; the transaction can still commit the original copy.
    auto unchanged = edit.commit();
    CHECK(unchanged.has_value());
    CHECK(unchanged.value().size() == original.size());
    CHECK(unchanged.value().positions()[0].x == original.positions()[0].x);
    CHECK(unchanged.value().sh().raw() == original.sh().raw());
}

void test_append_is_validated_budgeted_and_atomic() {
    SplatData original = scene(1, 1);
    auto edit = original.edit();
    Budget budget = desktop_budget();
    SplatRecord record;
    record.position = Vec3f{4.0f, 5.0f, 6.0f};
    record.scale = Vec3f{0.5f, 0.75f, 1.25f};
    record.opacity = 0.25f;
    record.sh.assign(12, 0.75f);  // degree 1: four coefficients * RGB

    auto appended = edit.append(record, budget);
    CHECK(appended.has_value());
    CHECK(budget.used(BudgetKind::splats) == 1);
    CHECK(budget.used(BudgetKind::memory_bytes) > 0);
    auto result = edit.commit();
    CHECK(result.has_value());
    CHECK(result.value().size() == 2);
    CHECK(result.value().positions()[1].x == 4.0f);
    CHECK(result.value().sh().degree() == 1);
    CHECK(result.value().sh().raw().back() == 0.75f);

    auto invalid_edit = original.edit();
    SplatRecord invalid = record;
    invalid.opacity = std::numeric_limits<float>::quiet_NaN();
    Budget invalid_budget = desktop_budget();
    auto rejected = invalid_edit.append(invalid, invalid_budget);
    CHECK(!rejected.has_value());
    CHECK(invalid_budget.used(BudgetKind::memory_bytes) == 0);
    CHECK(invalid_budget.used(BudgetKind::splats) == 0);
    auto still_original = invalid_edit.commit();
    CHECK(still_original.has_value());
    CHECK(still_original.value().size() == original.size());
}

void test_append_splat_limit_failure_does_not_append() {
    SplatData original = scene(1);
    auto edit = original.edit();
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_splats = 1;
    Budget budget(limits);
    CHECK(budget.consume(BudgetKind::splats, 1, "test.preconsume").has_value());

    SplatRecord record;
    record.sh = {0.0f, 0.0f, 0.0f};
    auto failed = edit.append(record, budget);
    CHECK(!failed.has_value());
    CHECK(failed.error_code() == ErrorCode::resource_limit);
    auto unchanged = edit.commit();
    CHECK(unchanged.has_value());
    CHECK(unchanged.value().size() == 1);
}

void test_reserve_reaccounts_after_sh_degree_changes() {
    SplatData original = scene(1, 4);
    auto edit = original.edit();
    Budget budget = desktop_budget();
    CHECK(edit.reserve(100, budget).has_value());

    edit.set_sh(ShBuffer::create(0, 1, std::vector<float>(3, 0.0f)).value());
    CHECK(edit.reserve(1000, budget).has_value());
    edit.set_sh(ShBuffer::create(4, 1, std::vector<float>(75, 0.0f)).value());
    CHECK(edit.reserve(1000, budget).has_value());

    const std::uint64_t expected_per_splat = sizeof(Vec3f) * 2 + sizeof(Quatf) + sizeof(float) +
                                             75 * sizeof(float);
    CHECK(budget.used(BudgetKind::memory_bytes) == 1000 * expected_per_splat);
}

void test_commit_is_one_shot() {
    auto edit = scene().edit();
    CHECK(edit.commit().has_value());
    auto second = edit.commit();
    CHECK(!second.has_value());
    CHECK(second.diagnostics()[0].code == "MK1511_EDIT_ALREADY_COMMITTED");
}

}  // namespace

int main() {
    test_valid_commit_replaces_only_the_new_value();
    test_failed_commit_is_atomic();
    test_length_mismatch_is_rejected_at_commit();
    test_budget_failure_leaves_logical_content_unchanged();
    test_append_is_validated_budgeted_and_atomic();
    test_append_splat_limit_failure_does_not_append();
    test_reserve_reaccounts_after_sh_degree_changes();
    test_commit_is_one_shot();

    if (g_failures == 0) {
        std::printf("scene edit transaction: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "scene edit transaction: %d of %d checks FAILED\n", g_failures,
                 g_checks);
    return 1;
}
