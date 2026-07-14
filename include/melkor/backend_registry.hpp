// Backend registration.
//
// This header exists to break a circular dependency, and the shape of the cycle is worth
// stating because it is a common one:
//
//   `ComputeProvider::create()` was *declared* in a platform-neutral header but *defined*
//   inside each backend library -- Metal defined it in `metal_compute_provider.mm`, CUDA in
//   `cuda_compute_provider.cpp`, and a CPU-only build in `gpu_stub.cpp`. So `melkor_core`
//   called a function that only the backend could supply, while the backend needed core's
//   types. Each linked to the other `PUBLIC`, producing mutually dependent static archives
//   that only linked at all because the linker rescans them -- and on Apple, only after
//   suppressing the duplicate-library diagnostic with `-no_warn_duplicate_libraries`.
//
// That is release blocker P0-05, and a suppressed linker warning was the load-bearing part of
// the build.
//
// The fix is to invert the dependency. Core owns a registry and knows nothing about any
// concrete backend. Each backend *registers itself* into that registry. A thin runtime layer,
// which sits above both, calls the registration entry points for whichever backends were
// compiled in. Dependencies then point one way only:
//
//     melkor_core  <--  melkor_backend_{cpu,metal,cuda}  <--  melkor_runtime  <--  CLI/tests
//
// Registration is an explicit function call rather than a static initializer. Static
// initializers in a static library are stripped by the linker when nothing in that
// translation unit is referenced, so a self-registering backend silently vanishes from a
// static build -- a failure mode that is invisible until a user reports that their GPU is not
// detected. An explicit call cannot be stripped.

#ifndef MELKOR_BACKEND_REGISTRY_HPP
#define MELKOR_BACKEND_REGISTRY_HPP

#include "melkor/compute_provider.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace melkor {

// Creates an instance of one backend. Returns nullptr when the backend is compiled in but
// unusable on this machine -- no Metal device, no CUDA driver, an unsupported GPU.
//
// "Compiled in" and "available at runtime" are different questions, and conflating them is
// how a binary ends up claiming a GPU it cannot actually use.
using BackendFactory = std::function<std::unique_ptr<ComputeProvider>()>;

// The set of backends this binary can construct.
//
// Immutable after startup. `melkor_runtime` populates it once, before anything asks for a
// provider; nothing mutates it afterwards. A registry that can change under a running
// operation would make backend selection nondeterministic.
class BackendRegistry {
  public:
    // The process-wide registry.
    static BackendRegistry& instance();

    // Adds a backend. Called only by the runtime layer's registration entry point.
    //
    // Registering the same kind twice replaces the earlier factory, so a test can substitute a
    // fake without the real one having to be absent.
    void register_backend(ComputeBackend kind, BackendFactory factory);

    // Constructs the named backend, or nullptr if it was never registered or is unavailable
    // on this machine.
    std::unique_ptr<ComputeProvider> create(ComputeBackend kind) const;

    // Constructs the best available backend, preferring GPU over CPU.
    //
    // CPU is always available and is the semantic reference implementation, so this never
    // returns nullptr in a correctly linked binary.
    std::unique_ptr<ComputeProvider> create_best() const;

    // Which backends were compiled into this binary. Says nothing about whether they work on
    // the current machine -- `create()` answers that.
    std::vector<ComputeBackend> registered() const;

    bool has(ComputeBackend kind) const;

  private:
    BackendRegistry() = default;

    struct Entry {
        ComputeBackend kind;
        BackendFactory factory;
    };
    std::vector<Entry> entries_;
};

// Registers every backend compiled into this binary.
//
// Defined in `melkor_runtime`, which is the only layer that may name a concrete backend. It
// is idempotent: calling it twice is harmless.
//
// The CLI, the tests, the Python bindings, and the WASM facade each call this once at startup.
// Core never does -- core must not know that Metal exists.
void register_builtin_backends();

}  // namespace melkor

#endif  // MELKOR_BACKEND_REGISTRY_HPP
