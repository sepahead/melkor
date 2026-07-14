// Backend registration: the only translation unit in the project that is allowed to name a
// concrete backend.
//
// This file is the top of the dependency graph, and that is the whole point. Dependencies now
// flow in one direction:
//
//     melkor_core  <--  melkor_backend_{cpu,metal,cuda}  <--  melkor_runtime  <--  CLI / tests
//
// Previously they flowed both ways. `ComputeProvider::create()` was declared in a
// platform-neutral header in melkor_core but *defined* inside whichever backend was compiled,
// so core called a function only the backend could supply while the backend needed core's
// types. Each linked to the other PUBLIC. The resulting mutually dependent static archives
// linked only because the linker rescans them, and on Apple only after
// `-no_warn_duplicate_libraries` suppressed the diagnostic that was trying to warn about it.
// A suppressed linker warning was load-bearing. That is release blocker P0-05.
//
// Registration is an explicit call, not a static initializer. A self-registering backend in a
// static library is silently stripped by the linker when nothing else in its translation unit
// is referenced -- and the symptom is "my GPU is not detected", reported by a user, months
// later, on a build configuration nobody tested. An explicit call cannot be stripped.

#include "melkor/backend_registry.hpp"

#include "melkor/compute_provider.hpp"

namespace melkor {
namespace {

bool g_registered = false;

}  // namespace

void register_builtin_backends() {
    if (g_registered) {
        return;
    }
    g_registered = true;

    BackendRegistry& registry = BackendRegistry::instance();

    // CPU is always present. It is the semantic reference implementation: when an optimized
    // backend disagrees with it, the optimized backend is wrong. A build without CPU would
    // have no oracle to check the others against.
    registry.register_backend(ComputeBackend::CPU, []() -> std::unique_ptr<ComputeProvider> {
        return createCpuProvider();
    });

#if defined(MELKOR_HAS_METAL)
    // The factory returns nullptr when Metal is compiled in but no usable device exists.
    // "Compiled in" and "works on this machine" are different questions, and answering them
    // with one flag is how a binary ends up claiming a GPU it cannot use.
    registry.register_backend(ComputeBackend::Metal, []() -> std::unique_ptr<ComputeProvider> {
        return createMetalProvider();
    });
#endif

#if defined(MELKOR_HAS_CUDA)
    registry.register_backend(ComputeBackend::CUDA, []() -> std::unique_ptr<ComputeProvider> {
        return createCudaProvider();
    });
#endif
}

}  // namespace melkor
