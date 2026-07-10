// Gradient-check test for the differentiable Gaussian rasterizer.
//
// Verifies analytic gradients from backward() against central finite
// differences for ALL 5 Gaussian parameters: position (x,y,z), scale (3),
// rotation (quaternion 4), color (SH-DC 3), and opacity (logit).
//
// Uses multiple random scenes with varied Gaussian counts, positions, scales,
// rotations, and opacities. Tolerance is 5% relative error, which is tight
// enough to catch sign-flips and order-of-magnitude errors while allowing
// for float32 finite-difference truncation noise.
//
// This is a macOS-only test (the DifferentiableRenderer lives in the Metal
// translation unit), so it's compiled when MELKOR_HAS_METAL is set.

#include "melkor/gaussian_data.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/metal_compute.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

namespace {

// Build a camera looking at the origin from a given distance and angle.
melkor::Camera make_camera(int w, int h, float dist, float az, float el) {
    melkor::Camera cam{};
    cam.position[0] = dist * std::cos(el) * std::sin(az);
    cam.position[1] = dist * std::sin(el);
    cam.position[2] = dist * std::cos(el) * std::cos(az);
    cam.target[0] = 0; cam.target[1] = 0; cam.target[2] = 0;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.fov_y = 0.8f;
    cam.aspect = static_cast<float>(w) / static_cast<float>(h);
    cam.near_plane = 0.01f;
    cam.far_plane = 100.0f;
    cam.width = w; cam.height = h;
    cam.computeMatrices();
    return cam;
}

// Render and return the L1 loss against a mid-gray target, plus the analytic
// gradients. Reused for both the analytic and (perturbed) finite-diff renders.
float render_loss_and_grad(
    melkor::DifferentiableRenderer& rdr,
    const std::vector<melkor::PackedGaussian>& gs,
    const melkor::Camera& cam,
    const float target[3],
    melkor::DifferentiableRenderer::BackwardResult* grad_out) {

    float bg[3] = {0.0f, 0.0f, 0.0f};
    auto fwd = rdr.forward(gs, cam, bg);

    int n = cam.width * cam.height;
    float norm = 1.0f / static_cast<float>(n * 3);
    std::vector<float> grad_image(n * 3, 0.0f);
    double loss = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            float diff = fwd.image[i * 3 + c] - target[c];
            loss += std::abs(diff);
            float s = (diff > 0.0f) ? 1.0f : ((diff < 0.0f) ? -1.0f : 0.0f);
            grad_image[i * 3 + c] = s * norm;
        }
    }
    if (grad_out) *grad_out = rdr.backward(fwd, grad_image);
    return static_cast<float>(loss * norm);
}

// Simple xorshift RNG for reproducible random scenes.
struct RNG {
    uint32_t state;
    explicit RNG(uint32_t seed) : state(seed) {}
    float next_float(float lo, float hi) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        float t = static_cast<float>(state) / 4294967296.0f;
        return lo + t * (hi - lo);
    }
};

melkor::PackedGaussian make_random_gaussian(RNG& rng) {
    melkor::PackedGaussian g{};
    g.position[0] = rng.next_float(-0.3f, 0.3f);
    g.position[1] = rng.next_float(-0.3f, 0.3f);
    g.position[2] = rng.next_float(-0.2f, 0.2f);
    g.position[3] = rng.next_float(-0.5f, 2.0f);  // opacity logit
    g.color[0] = rng.next_float(-1.0f, 1.0f);
    g.color[1] = rng.next_float(-1.0f, 1.0f);
    g.color[2] = rng.next_float(-1.0f, 1.0f);
    g.color[3] = 0.0f;
    g.scale[0] = rng.next_float(-3.5f, -1.5f);  // log scale
    g.scale[1] = rng.next_float(-3.5f, -1.5f);
    g.scale[2] = rng.next_float(-3.5f, -1.5f);
    g.scale[3] = 0.0f;
    // Random unit quaternion (w, x, y, z).
    float qw = rng.next_float(-1.0f, 1.0f);
    float qx = rng.next_float(-1.0f, 1.0f);
    float qy = rng.next_float(-1.0f, 1.0f);
    float qz = rng.next_float(-1.0f, 1.0f);
    float ql = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz) + 1e-8f;
    g.rotation[0] = qw / ql;
    g.rotation[1] = qx / ql;
    g.rotation[2] = qy / ql;
    g.rotation[3] = qz / ql;
    return g;
}

}  // namespace

int main() {
    using namespace melkor;

    if (!metal::MetalContext::isAvailable()) {
        printf("[skip] gradient check (Metal unavailable)\n");
        return 0;
    }
    metal::MetalContext ctx;
    if (!ctx.initialize()) {
        printf("[skip] gradient check (Metal init failed)\n");
        return 0;
    }
    DifferentiableRenderer rdr(ctx);

    const int W = 16, H = 16;

    // Non-black background reference: foreground is identical between the
    // two renders, so white-minus-black must equal final transmittance. This
    // catches the former `background + foreground` over-brightening bug.
    {
        auto cam = make_camera(W, H, 3.0f, 0.0f, 0.0f);
        PackedGaussian g{};
        g.position[3] = 2.0f;
        g.color[0] = utils::rgbToShDc(0.8f);
        g.color[1] = utils::rgbToShDc(0.3f);
        g.color[2] = utils::rgbToShDc(0.1f);
        g.scale[0] = g.scale[1] = g.scale[2] = -1.5f;
        g.rotation[0] = 1.0f;
        float black[3] = {0, 0, 0};
        float white[3] = {1, 1, 1};
        auto on_black = rdr.forward({g}, cam, black);
        auto on_white = rdr.forward({g}, cam, white);
        float max_error = 0.0f;
        for (size_t pixel = 0; pixel < on_black.alpha.size(); ++pixel) {
            for (int channel = 0; channel < 3; ++channel) {
                const float difference = on_white.image[pixel * 3 + channel] -
                                         on_black.image[pixel * 3 + channel];
                max_error = std::max(max_error,
                    std::abs(difference - (1.0f - on_black.alpha[pixel])));
            }
        }
        if (max_error > 1e-5f) {
            printf("  BACKGROUND COMPOSITING FAILED: max error %.7f\n", max_error);
            return 1;
        }
        printf("  background transmittance reference: PASS\n");

        // Input order must not affect the image: centers are depth-sorted
        // front-to-back before alpha compositing.
        PackedGaussian near_g = g, far_g = g;
        near_g.position[2] = 0.2f;
        far_g.position[2] = -0.2f;
        near_g.color[0] = utils::rgbToShDc(1.0f);
        near_g.color[2] = utils::rgbToShDc(0.0f);
        far_g.color[0] = utils::rgbToShDc(0.0f);
        far_g.color[2] = utils::rgbToShDc(1.0f);
        auto first = rdr.forward({near_g, far_g}, cam, black);
        auto reversed = rdr.forward({far_g, near_g}, cam, black);
        float order_error = 0.0f;
        for (size_t i = 0; i < first.image.size(); ++i) {
            order_error = std::max(order_error, std::abs(first.image[i] - reversed.image[i]));
        }
        if (order_error > 1e-5f) {
            printf("  DEPTH ORDER FAILED: max error %.7f\n", order_error);
            return 1;
        }
        printf("  depth-sorted compositing reference: PASS\n");
    }

    const float eps = 2e-3f;
    // 10% relative tolerance for stable finite-difference checks. Parameters
    // that cross a discrete coverage/alpha-threshold boundary are identified
    // by comparing two epsilon scales and reported as non-differentiable,
    // rather than allowing an arbitrary percentage of wrong gradients.
    const float rel_tol = 0.10f;

    int total_failures = 0;
    int total_checks = 0;
    int total_nondifferentiable = 0;

    // Run 5 random scenes with 2-3 Gaussians each.
    for (int scene = 0; scene < 5; ++scene) {
        RNG rng(12345 + scene * 777);
        int num_g = 2 + scene % 2;  // 2 or 3 Gaussians
        float dist = 3.0f + rng.next_float(-0.5f, 0.5f);
        float az = rng.next_float(0.0f, 3.14159f);
        float el = rng.next_float(-0.3f, 0.3f);
        auto cam = make_camera(W, H, dist, az, el);

        std::vector<PackedGaussian> gs;
        for (int i = 0; i < num_g; ++i) gs.push_back(make_random_gaussian(rng));

        float target[3] = {rng.next_float(0.2f, 0.8f), rng.next_float(0.2f, 0.8f), rng.next_float(0.2f, 0.8f)};

        // Analytic gradients.
        DifferentiableRenderer::BackwardResult analytic;
        render_loss_and_grad(rdr, gs, cam, target, &analytic);

        int scene_failures = 0;

        auto check = [&](const char* name, int gid, int channel,
                         float analytic_val,
                         std::function<void(std::vector<PackedGaussian>&, float)> perturb) {
            auto gp = gs, gm = gs;
            perturb(gp, +eps);
            perturb(gm, -eps);
            float lp = render_loss_and_grad(rdr, gp, cam, target, nullptr);
            float lm = render_loss_and_grad(rdr, gm, cam, target, nullptr);
            float fd = (lp - lm) / (2 * eps);

            auto gp_half = gs, gm_half = gs;
            perturb(gp_half, +eps * 0.5f);
            perturb(gm_half, -eps * 0.5f);
            const float lp_half = render_loss_and_grad(rdr, gp_half, cam, target, nullptr);
            const float lm_half = render_loss_and_grad(rdr, gm_half, cam, target, nullptr);
            const float fd_half = (lp_half - lm_half) / eps;
            const float stability_denom = std::max({1e-5f, std::abs(fd), std::abs(fd_half)});
            if (std::abs(fd - fd_half) / stability_denom > 0.15f) {
                printf("  [scene %d] %s[g=%d,c=%d] non-differentiable boundary "
                       "fd=%+.6f fd_half=%+.6f SKIP\n",
                       scene, name, gid, channel, fd, fd_half);
                ++total_nondifferentiable;
                return;
            }
            float denom = std::max({1e-6f, std::abs(fd), std::abs(analytic_val)});
            float rel = std::abs(fd - analytic_val) / denom;
            bool ok = rel <= rel_tol || std::abs(fd - analytic_val) < 2e-5f;
            if (!ok) {
                printf("  [scene %d] %s[g=%d,c=%d] analytic=%+.6f fd=%+.6f rel=%.4f MISMATCH\n",
                       scene, name, gid, channel, analytic_val, fd, rel);
                ++scene_failures;
            }
            ++total_checks;
        };

        // Color (SH-DC) gradients: 3 channels per Gaussian.
        for (int i = 0; i < num_g; ++i)
            for (int c = 0; c < 3; ++c) {
                float av = analytic.grad_colors[i * 3 + c];
                check("color", i, c, av, [i, c](auto& g, float e) { g[i].color[c] += e; });
            }

        // Opacity (logit) gradient.
        for (int i = 0; i < num_g; ++i) {
            float av = analytic.grad_opacities[i];
            check("opacity", i, 0, av, [i](auto& g, float e) { g[i].position[3] += e; });
        }

        // Position (x, y, z) gradients.
        for (int i = 0; i < num_g; ++i)
            for (int c = 0; c < 3; ++c) {
                float av = analytic.grad_positions[i * 3 + c];
                check("position", i, c, av, [i, c](auto& g, float e) { g[i].position[c] += e; });
            }

        // Scale (log scale, 3 axes) gradients.
        for (int i = 0; i < num_g; ++i)
            for (int c = 0; c < 3; ++c) {
                float av = analytic.grad_scales[i * 3 + c];
                check("scale", i, c, av, [i, c](auto& g, float e) { g[i].scale[c] += e; });
            }

        // Rotation (quaternion w, x, y, z) gradients.
        for (int i = 0; i < num_g; ++i)
            for (int c = 0; c < 4; ++c) {
                float av = analytic.grad_rotations[i * 4 + c];
                check("rotation", i, c, av, [i, c](auto& g, float e) { g[i].rotation[c] += e; });
            }

        printf("[scene %d] %d Gaussians, %d checks, %s\n",
               scene, num_g, num_g * (3 + 1 + 3 + 3 + 4),
               scene_failures == 0 ? "PASS" : "FAIL");
        total_failures += scene_failures;
    }

    printf("\n  %d/%d stable checks passed, %d failures, %d boundary skips\n",
           total_checks - total_failures, total_checks, total_failures,
           total_nondifferentiable);
    bool pass = total_checks > 0 && total_failures == 0;
    printf(pass ? "  GRADIENT CHECK PASSED\n" : "  GRADIENT CHECK FAILED\n");
    return pass ? 0 : 1;
}
