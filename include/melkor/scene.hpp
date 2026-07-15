// The canonical scene model.
//
// The pre-v2 model was a `std::vector<GaussianSplat>` of plain structs. Its default constructor
// left scalar fields uninitialised, its SH degree could be set outside the valid range, and its
// mutable `data()` let any caller violate the length invariants behind the type's back. That is
// release blocker P0-06: a data model that cannot defend its own invariants.
//
// This replaces it with a structure-of-arrays buffer whose only construction path validates
// every domain and every array length up front, and which exposes no mutable reference that
// could break those invariants afterwards. Every value in a constructed `SplatData` is finite
// and in its canonical domain, or the construction failed with a diagnostic saying which splat
// and which field.
//
// Storage is float32, matching the on-disk formats and the GPU. The validation and any transform
// use the float64 math oracle (melkor/math/*) and convert back, so the numerically sensitive
// work is done in double even though the result is stored in single.
//
// This model is additive: it coexists with the legacy `GaussianCloud` while adapters migrate
// onto it. New code should target this.

#ifndef MELKOR_SCENE_HPP
#define MELKOR_SCENE_HPP

#include "melkor/error.hpp"

#include <cstdint>
#include <vector>

namespace melkor {

// float32 storage types. Deliberately small and trivial; all validation lives in the factories.
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quatf {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;  // identity default: a default Quatf is already a valid rotation
};

// Spherical-harmonic storage.
//
// Layout is coefficient-major: for each of the `(degree+1)^2` coefficients, three channels
// (R, G, B), for each splat. The DC term (coefficient 0) is always present; higher coefficients
// exist only when degree > 0, and a degree always stores its lower degrees completely. Degree is
// 0..4 (SPZ v4 needs 4; the glTF RC profile stops at 3, and converting down is a reported loss,
// never a silent truncation).
//
// There is no per-splat heap allocation: everything is one contiguous vector, sized and checked
// once at construction.
class ShBuffer {
  public:
    // A valid empty buffer: degree 0, zero splats. This is the only publicly default-constructible
    // state, so an ShBuffer never exists in an invalid shape; a populated one is built through
    // create() or black(), which validate.
    ShBuffer() = default;

    // Builds an SH buffer from a flat coefficient array. `data` must be exactly
    // splat_count * (degree+1)^2 * 3 floats, laid out coefficient-major. Any other length, or a
    // degree above 4, fails with a diagnostic.
    static Result<ShBuffer> create(std::uint32_t degree, std::size_t splat_count,
                                   std::vector<float> data);

    // A degree-0 (DC only) buffer initialised to black. Used as the safe default appearance.
    static Result<ShBuffer> black(std::size_t splat_count);

    std::uint32_t degree() const noexcept { return degree_; }
    std::size_t splat_count() const noexcept { return splat_count_; }

    // Coefficients per channel: (degree+1)^2.
    std::size_t coefficients() const noexcept;

    // The DC (degree-0) coefficient for one splat and channel (0=R,1=G,2=B).
    float dc(std::size_t splat, int channel) const;

    const std::vector<float>& raw() const noexcept { return data_; }

  private:
    std::uint32_t degree_ = 0;
    std::size_t splat_count_ = 0;
    std::vector<float> data_;
};

// The inputs a SplatData is built from. Every array must have the same length as `positions`, or
// construction fails; opacities/scales/rotations must be in their canonical domains.
struct SplatBufferInput {
    std::vector<Vec3f> positions;    // finite, metres, canonical frame
    std::vector<Vec3f> scales;       // finite, strictly positive, linear
    std::vector<Quatf> rotations;    // finite, unit within tolerance, xyzw
    std::vector<float> opacities;    // finite, [0, 1], linear
    ShBuffer sh;                     // its splat_count must equal positions.size()
};

// A validated structure-of-arrays Gaussian buffer.
//
// The ONLY way to make one is `create`, which validates every field of every splat. After that,
// there is no mutable accessor that can violate an invariant: bulk read is by const reference,
// and a modified scene is produced by building a new SplatData through `create` again, which
// re-validates atomically. (A future in-place edit transaction would follow the same rule: it
// would validate the whole change before committing, leaving the original untouched on failure.)
class SplatData {
  public:
    // Validates all lengths and domains and takes ownership. On failure, returns a diagnostic
    // identifying the first offending splat index and field; the moved-from input is left in a
    // valid empty state by the caller's move.
    static Result<SplatData> create(SplatBufferInput input);

    std::size_t size() const noexcept { return positions_.size(); }
    bool empty() const noexcept { return positions_.empty(); }

    // Const bulk access. No mutable overload exists: a mutable span is exactly how the old model
    // let a caller inject a NaN or resize one array out of step with the others.
    const std::vector<Vec3f>& positions() const noexcept { return positions_; }
    const std::vector<Vec3f>& scales() const noexcept { return scales_; }
    const std::vector<Quatf>& rotations() const noexcept { return rotations_; }
    const std::vector<float>& opacities() const noexcept { return opacities_; }
    const ShBuffer& sh() const noexcept { return sh_; }

    // Re-checks every invariant. Cheap defence for a value that has crossed an ABI or been
    // deserialised; a well-formed SplatData created through create() always passes.
    Result<void> validate() const;

  private:
    SplatData() = default;
    std::vector<Vec3f> positions_;
    std::vector<Vec3f> scales_;
    std::vector<Quatf> rotations_;
    std::vector<float> opacities_;
    ShBuffer sh_;
};

}  // namespace melkor

#endif  // MELKOR_SCENE_HPP
