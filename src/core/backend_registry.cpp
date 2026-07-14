#include "melkor/backend_registry.hpp"

#include <algorithm>

namespace melkor {

BackendRegistry& BackendRegistry::instance() {
    // Function-local static: constructed on first use, thread-safe initialization since C++11.
    // A namespace-scope global would be subject to static initialization order across
    // translation units, which is precisely the kind of subtlety that makes backend
    // availability depend on link order.
    static BackendRegistry registry;
    return registry;
}

void BackendRegistry::register_backend(ComputeBackend kind, BackendFactory factory) {
    auto existing = std::find_if(entries_.begin(), entries_.end(),
                                 [kind](const Entry& entry) { return entry.kind == kind; });
    if (existing != entries_.end()) {
        existing->factory = std::move(factory);
        return;
    }
    entries_.push_back(Entry{kind, std::move(factory)});
}

std::unique_ptr<ComputeProvider> BackendRegistry::create(ComputeBackend kind) const {
    auto entry = std::find_if(entries_.begin(), entries_.end(),
                              [kind](const Entry& e) { return e.kind == kind; });
    if (entry == entries_.end()) {
        // Not compiled into this binary.
        return nullptr;
    }

    // The factory returns nullptr when the backend exists in the binary but cannot run on this
    // machine -- no Metal device, no CUDA driver. That distinction matters: the caller may want
    // to report "CUDA was not built in" differently from "CUDA is built in but your driver is
    // too old", and a single nullptr from a single code path could not tell them apart.
    return entry->factory();
}

std::unique_ptr<ComputeProvider> BackendRegistry::create_best() const {
    // GPU first, then CPU. CPU is always available and is the semantic reference
    // implementation, so a correctly linked binary always gets something.
    //
    // Note what this deliberately does NOT do: it does not fall back from a *requested* GPU
    // backend to CPU. That is the caller's decision, because a silent fallback changes
    // performance characteristics -- and potentially numerical results -- without telling
    // anyone. `create(kind)` returns nullptr and lets the caller decide.
    for (ComputeBackend kind : {ComputeBackend::Metal, ComputeBackend::CUDA}) {
        if (auto provider = create(kind)) {
            return provider;
        }
    }
    return create(ComputeBackend::CPU);
}

std::vector<ComputeBackend> BackendRegistry::registered() const {
    std::vector<ComputeBackend> kinds;
    kinds.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        kinds.push_back(entry.kind);
    }
    return kinds;
}

bool BackendRegistry::has(ComputeBackend kind) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [kind](const Entry& entry) { return entry.kind == kind; });
}

// ---------------------------------------------------------------------------
// ComputeProvider's factories now consult the registry.
//
// Previously these were defined inside whichever backend library was compiled, which is what
// forced melkor_core to link back to the backend and created the cycle (P0-05). They now live
// in core and know nothing about any concrete backend.
// ---------------------------------------------------------------------------

std::unique_ptr<ComputeProvider> ComputeProvider::create() {
    return BackendRegistry::instance().create_best();
}

std::unique_ptr<ComputeProvider> ComputeProvider::create(ComputeBackend backend) {
    return BackendRegistry::instance().create(backend);
}

}  // namespace melkor
