#include "Logging.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace petsro {
namespace {

LogLevel g_level = LogLevel::Info;

// Only the statistics reporter and the main loop log, and both run on the main
// thread today. The mutex costs nothing measurable and keeps lines from
// interleaving if a sender thread ever logs.
std::mutex g_mutex;

const char* levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
    }
    return "?????";
}

/// Wall-clock stamp as HH:MM:SS.mmm -- enough to correlate with an LB log.
std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t secs = clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_r(&secs, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0')
        << std::setw(3) << millis.count();
    return oss.str();
}

}  // namespace

void setLogLevel(LogLevel level) noexcept { g_level = level; }

LogLevel logLevel() noexcept { return g_level; }

void logLine(LogLevel level, const std::string& message) {
    std::ostream& out =
        (level == LogLevel::Error || level == LogLevel::Warn) ? std::cerr : std::cout;

    std::lock_guard<std::mutex> lock(g_mutex);
    out << '[' << timestamp() << "] " << levelName(level) << ' ' << message << '\n';
    if (level == LogLevel::Error || level == LogLevel::Warn) {
        out.flush();
    }
}

}  // namespace petsro
