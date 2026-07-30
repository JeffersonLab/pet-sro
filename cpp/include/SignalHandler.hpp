// SignalHandler.hpp -- Ctrl-C handling, and nothing else.
//
// The handler does exactly one thing: store true into a lock-free atomic flag.
// No logging, no allocation, no cleanup -- all of that happens in ordinary
// control flow, which polls shutdownRequested().
//
// A second signal exits immediately via _Exit(). That is async-signal-safe, and
// it gives an operator a way out if a send is wedged in the kernel.

#ifndef PETSRO_SIGNALHANDLER_HPP
#define PETSRO_SIGNALHANDLER_HPP

#include <atomic>

namespace petsro {

/// Installs handlers for SIGINT and SIGTERM. Returns false if either could not
/// be installed, in which case the program still runs but is not interruptible
/// cleanly.
bool installSignalHandlers() noexcept;

/// The flag the handler sets. Polled by the replay loop and the synchronizer.
const std::atomic<bool>& shutdownFlag() noexcept;

inline bool shutdownRequested() noexcept {
    return shutdownFlag().load(std::memory_order_relaxed);
}

/// Sets the flag from ordinary code. Used by the tests, and on a fatal error.
void requestShutdown() noexcept;

/// Clears the flag. Tests only; the program never resumes after a signal.
void resetShutdownForTesting() noexcept;

/// The signal that caused the shutdown, or 0 if it was requested in code.
int shutdownSignal() noexcept;

}  // namespace petsro

#endif  // PETSRO_SIGNALHANDLER_HPP
