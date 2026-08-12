#include "cli_interrupt.hpp"

#include <utility>

namespace melkor::cli {

volatile std::sig_atomic_t InterruptController::requested_ = 0;

void InterruptController::request_cancel(int) noexcept {
    requested_ = 1;
}

InterruptController::InterruptController(CancellationToken cancellation) noexcept
    : cancellation_(std::move(cancellation)) {
    requested_ = 0;
    previous_ = std::signal(SIGINT, request_cancel);
    installed_ = previous_ != SIG_ERR;
}

InterruptController::~InterruptController() {
    if (installed_)
        std::signal(SIGINT, previous_);
}

void InterruptController::on_progress(const ProgressEvent&) {
    if (requested_ != 0)
        cancellation_.cancel();
}

}  // namespace melkor::cli
