// Activation-domain conversions.
//
// Training pipelines store opacity as a logit and scale as a natural log, because those are the
// unconstrained domains an optimiser works in. Melkor's canonical scene stores linear opacity
// in [0,1] and positive linear scale. The conversion between them happens exactly once, at the
// format-profile boundary, through these named functions -- never inline in an adapter, and
// never twice.
//
// Applying a sigmoid to an already-linear opacity, or an exp to an already-linear scale, is the
// "double activation" P0-17 warns about: it produces a plausible-looking but wrong value that no
// range check catches, because small log-scales and small linear scales overlap numerically.
// Keeping the conversions in one module with explicit names is what prevents that.

#ifndef MELKOR_MATH_ACTIVATION_HPP
#define MELKOR_MATH_ACTIVATION_HPP

#include "melkor/error.hpp"

namespace melkor::math {

// logit (unbounded) -> probability in (0,1), numerically stable for large-magnitude input.
Result<float> sigmoid_from_logit(float logit);

// probability in the open interval (0,1) -> logit. A probability of exactly 0 or 1 has an
// infinite logit; the input must be strictly inside the interval (a decoder that produced an
// endpoint should clamp with an explicit, recorded epsilon first).
Result<float> logit_from_probability(float probability);

// natural-log scale (unbounded) -> positive linear scale, via a bounded exp. The exponent range
// is limited so that a hostile or corrupt log-scale cannot produce an overflowing or infinite
// linear scale; an out-of-range input fails rather than returning inf.
Result<float> linear_scale_from_log(float log_scale);

// positive linear scale -> natural-log scale. The scale must be finite and strictly positive.
Result<float> log_scale_from_linear(float linear_scale);

}  // namespace melkor::math

#endif  // MELKOR_MATH_ACTIVATION_HPP
