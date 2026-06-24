// Gradient-check test for the differentiable Gaussian rasterizer.
//
// A backward pass is only trustworthy if its analytic gradients match finite
// differences. This test renders a tiny scene (a few Gaussians at the image
// center), computes the analytic backward gradient of an L1 loss w.r.t. each
// Gaussian's color and opacity, and compares against central finite
// differences. The color gradient in particular exercises the full
// alpha-blend chain rule (dL/dcolor = alpha * T * dL/dC plus the
// transmittance propagation).
//
// This is a macOS-only test (the DifferentiableRenderer lives in the Metal
// translation unit), so it's compiled when MELKOR_HAS_METAL is set.

#include "melkor/gaussian_data.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/metal_compute.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

// Build a simple camera looking down -Z at the origin, where the Gaussians sit.
melkor::Camera make_camera(int w, int h) {
    melkor::Camera cam{};
    cam.position[0] = 0; cam.position[1] = 0; cam.position[2] = 3.0f;
    cam.target[0] = 0; cam.target[1] = 0; cam.target[2] = 0;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.fov_y = 0.8f;
    cam.aspect = static_cast<float>(w) / static_cast<float>(h);  // REQUIRED or projection divides by zero
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

    // L1 loss against the constant target image, averaged over all channels.
    int n = cam.width * cam.height;
    float norm = 1.0f / static_cast<float>(n * 3);  // match the loss normalization
    std::vector<float> grad_image(n * 3, 0.0f);
    double loss = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            float diff = fwd.image[i * 3 + c] - target[c];
            loss += std::abs(diff);
            // dL/dC = sign(diff) * (1 / (n*3))  -- the chain rule through the mean.
            float s = (diff > 0.0f) ? 1.0f : ((diff < 0.0f) ? -1.0f : 0.0f);
            grad_image[i * 3 + c] = s * norm;
        }
    }
    if (grad_out) *grad_out = rdr.backward(fwd, grad_image);
    return static_cast<float>(loss * norm);
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

    // Two Gaussians near the image center so they overlap (exercises the
    // transmittance chain in the backward). Small image for speed.
    const int W = 16, H = 16;
    auto cam = make_camera(W, H);

    auto make_gaussian = [](float dx, float sh0, float sh1, float sh2, float op_logit) {
        PackedGaussian g{};
        g.position[0] = dx; g.position[1] = 0.0f; g.position[2] = 0.0f;
        g.position[3] = op_logit;  // opacity (logit) lives in position[3]
        g.color[0] = sh0; g.color[1] = sh1; g.color[2] = sh2; g.color[3] = 0.0f;
        g.scale[0] = -2.5f; g.scale[1] = -2.5f; g.scale[2] = -2.5f; g.scale[3] = 0.0f;
        g.rotation[0] = 1.0f; g.rotation[1] = 0.0f; g.rotation[2] = 0.0f; g.rotation[3] = 0.0f;
        return g;
    };

    std::vector<PackedGaussian> gs = {
        make_gaussian(-0.1f, 0.5f, 0.2f, -0.1f, 1.0f),
        make_gaussian(0.1f, -0.3f, 0.4f, 0.1f, 0.5f),
    };

    float target[3] = {0.7f, 0.3f, 0.2f};

    // Analytic gradients.
    DifferentiableRenderer::BackwardResult analytic;
    render_loss_and_grad(rdr, gs, cam, target, &analytic);

    int failures = 0;
    const float eps = 1e-3f;
    // Relative tolerance: finite differences have O(eps^2) truncation error
    // plus the backward's own approximations; 15% is a sane bar that still
    // catches sign-flips and order-of-magnitude errors.
    const float rel_tol = 0.15f;

    auto check = [&](const char* name, int gid, int channel,
                     float analytic_val, float base_loss,
                     std::function<void(std::vector<PackedGaussian>&, float)> perturb) {
        auto gp = gs, gm = gs;
        perturb(gp, +eps);
        perturb(gm, -eps);
        float lp = render_loss_and_grad(rdr, gp, cam, target, nullptr);
        float lm = render_loss_and_grad(rdr, gm, cam, target, nullptr);
        float fd = (lp - lm) / (2 * eps);
        float denom = std::max({1e-6f, std::abs(fd), std::abs(analytic_val)});
        float rel = std::abs(fd - analytic_val) / denom;
        bool ok = rel < rel_tol || (std::abs(fd) < 1e-4f && std::abs(analytic_val) < 1e-4f);
        printf("  %s[g=%d,c=%d] analytic=%+.6f finite_diff=%+.6f rel=%.4f %s\n",
               name, gid, channel, analytic_val, fd, rel, ok ? "OK" : "MISMATCH");
        if (!ok) ++failures;
    };

    printf("[gradient-check] differentiable rasterizer vs finite differences\n");

    // Color (SH-DC) gradients for both Gaussians, all 3 channels.
    for (size_t i = 0; i < gs.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            int idx = static_cast<int>(i);
            float av = analytic.grad_colors[idx * 3 + c];
            float bl = render_loss_and_grad(rdr, gs, cam, target, nullptr);
            auto perturb = [i, c](std::vector<PackedGaussian>& g, float e) {
                g[i].color[c] += e;
            };
            check("color", idx, c, av, bl, perturb);
        }
    }

    // Opacity (logit) gradient for both Gaussians.
    for (size_t i = 0; i < gs.size(); ++i) {
        int idx = static_cast<int>(i);
        float av = analytic.grad_opacities[idx];
        float bl = render_loss_and_grad(rdr, gs, cam, target, nullptr);
        auto perturb = [i](std::vector<PackedGaussian>& g, float e) {
            g[i].position[3] += e;  // opacity logit
        };
        check("opacity", idx, 0, av, bl, perturb);
    }

    printf(failures == 0 ? "\n  GRADIENT CHECK PASSED\n" : "\n  %d GRADIENT MISMATCHES\n",
           failures);
    return failures == 0 ? 0 : 1;
}
