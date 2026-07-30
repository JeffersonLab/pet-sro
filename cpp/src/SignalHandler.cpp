#include "SignalHandler.hpp"

#include <csignal>
#include <cstdlib>

namespace petsro {
namespace {

std::atomic<bool> g_shutdown{false};

// Written from the handler, so it must be the one type the C++ standard
// guarantees is safe there. The atomic above is additionally required to be
// lock-free; see the static_assert.
volatile std::sig_atomic_t g_signal = 0;

static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
              "std::atomic<bool> must be always-lock-free to be set from a signal handler");

/// Async-signal-safe: one relaxed store, one sig_atomic_t store, or _Exit.
/// Nothing that allocates, locks, or calls into stdio.
extern "C" void handleSignal(int signal) {
    if (g_shutdown.load(std::memory_order_relaxed)) {
        // Already shutting down and the operator asked again. Leave now rather
        // than wait for a blocked send to return.
        std::_Exit(128 + signal);
    }
    g_signal = signal;
    g_shutdown.store(true, std::memory_order_relaxed);
}

bool install(int signal) noexcept {
    struct sigaction sa {};
    sa.sa_handler = &handleSignal;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: a blocking read or sendmsg should return EINTR so the loop
    // reaches its next shutdown check promptly.
    sa.sa_flags = 0;
    return sigaction(signal, &sa, nullptr) == 0;
}

}  // namespace

bool installSignalHandlers() noexcept {
    const bool okInt = install(SIGINT);
    const bool okTerm = install(SIGTERM);
    return okInt && okTerm;
}

const std::atomic<bool>& shutdownFlag() noexcept { return g_shutdown; }

void requestShutdown() noexcept { g_shutdown.store(true, std::memory_order_relaxed); }

void resetShutdownForTesting() noexcept {
    g_signal = 0;
    g_shutdown.store(false, std::memory_order_relaxed);
}

int shutdownSignal() noexcept { return static_cast<int>(g_signal); }

}  // namespace petsro
