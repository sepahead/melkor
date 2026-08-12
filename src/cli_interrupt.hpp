#pragma once

#include "melkor/budget.hpp"

#include <csignal>

namespace melkor::cli {

// Convert SIGINT into cooperative cancellation at the next operation checkpoint.
class InterruptController final : public ProgressSink {
public:
    explicit InterruptController(CancellationToken cancellation) noexcept;
    ~InterruptController() override;

    InterruptController(const InterruptController&) = delete;
    InterruptController& operator=(const InterruptController&) = delete;

    bool installed() const noexcept { return installed_; }
    void on_progress(const ProgressEvent&) override;

private:
    using Handler = void (*)(int);

    static void request_cancel(int) noexcept;
    static volatile std::sig_atomic_t requested_;

    CancellationToken cancellation_;
    Handler previous_ = SIG_DFL;
    bool installed_ = false;
};

}  // namespace melkor::cli
