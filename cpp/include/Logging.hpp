// Logging.hpp -- a deliberately small level-filtered logger.
//
// Nothing here justifies a logging framework: the program emits a handful of
// lines per replay loop plus a periodic statistics line. Per-packet logging
// exists but is gated behind Debug so the hot path stays quiet by default.

#ifndef PETSRO_LOGGING_HPP
#define PETSRO_LOGGING_HPP

#include <ostream>
#include <sstream>
#include <string>

namespace petsro {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

/// Sets the global threshold. Messages above it are discarded.
void setLogLevel(LogLevel level) noexcept;
LogLevel logLevel() noexcept;

inline bool logEnabled(LogLevel level) noexcept {
    return static_cast<int>(level) <= static_cast<int>(logLevel());
}

/// Writes one already-formatted line. Errors and warnings go to stderr, the
/// rest to stdout, so a shell redirect separates diagnostics from progress.
void logLine(LogLevel level, const std::string& message);

namespace detail {
/// Streams into a temporary and emits on destruction, so LOG_* is one statement.
class LogStream {
  public:
    explicit LogStream(LogLevel level) : level_(level) {}
    ~LogStream() { logLine(level_, buf_.str()); }

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value) {
        buf_ << value;
        return *this;
    }

  private:
    LogLevel level_;
    std::ostringstream buf_;
};
}  // namespace detail

// The level test happens before the stream is built, so a suppressed Debug
// line costs one comparison rather than a string format.
#define PETSRO_LOG(lvl)          \
    if (!::petsro::logEnabled(lvl)) \
        ;                        \
    else                         \
        ::petsro::detail::LogStream(lvl)

#define LOG_ERROR PETSRO_LOG(::petsro::LogLevel::Error)
#define LOG_WARN PETSRO_LOG(::petsro::LogLevel::Warn)
#define LOG_INFO PETSRO_LOG(::petsro::LogLevel::Info)
#define LOG_DEBUG PETSRO_LOG(::petsro::LogLevel::Debug)

}  // namespace petsro

#endif  // PETSRO_LOGGING_HPP
