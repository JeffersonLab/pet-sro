#include "ReplayStats.hpp"

#include <iomanip>
#include <sstream>

namespace petsro {

namespace {

double megabytes(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace

std::uint64_t ReplayStats::totalEventsSkipped() const noexcept {
    std::uint64_t total = 0;
    for (const auto& s : streams) {
        total += s.eventsSkipped;
    }
    return total;
}

std::uint64_t ReplayStats::totalEventsRead() const noexcept {
    std::uint64_t total = 0;
    for (const auto& s : streams) {
        total += s.eventsRead;
    }
    return total;
}

std::string ReplayStats::progressLine(double elapsedSeconds) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "loop " << (loopsCompleted + 1) << " | groups " << groupsSent << " | events "
        << eventsSent << " | packets " << packetsSent << " | " << std::setprecision(2)
        << megabytes(payloadBytesSent) << " MiB";

    if (elapsedSeconds > 0.0) {
        const double mbps =
            (static_cast<double>(payloadBytesSent) * 8.0 / 1.0e6) / elapsedSeconds;
        oss << " | " << std::setprecision(1) << mbps << " Mbps";
    }
    if (sendErrors > 0 || readErrors > 0) {
        oss << " | errors: send " << sendErrors << ", read " << readErrors;
    }
    return oss.str();
}

void ReplayStats::printFinal(std::ostream& out, double elapsedSeconds) const {
    out << "\n========== Replay Summary ==========\n";
    out << std::fixed << std::setprecision(3);
    out << "  Elapsed                   : " << elapsedSeconds << " s\n";
    out << "  Replay loops completed    : " << loopsCompleted << '\n';
    out << "  Synchronized groups sent  : " << groupsSent << '\n';
    out << "  EVIO events sent          : " << eventsSent << '\n';
    out << "  EJFAT packets sent        : " << packetsSent << '\n';
    out << "  Payload bytes sent        : " << payloadBytesSent << " ("
        << std::setprecision(2) << megabytes(payloadBytesSent) << " MiB)\n";
    out << std::setprecision(3);

    if (elapsedSeconds > 0.0) {
        const double mbps =
            (static_cast<double>(payloadBytesSent) * 8.0 / 1.0e6) / elapsedSeconds;
        out << "  Average payload rate      : " << std::setprecision(2) << mbps << " Mbps\n";
    }

    out << "  Timestamp mismatches      : " << timestampMismatches << '\n';
    out << "  Timestamp regressions     : " << timestampRegressions << '\n';
    out << "  Incomplete groups dropped : " << incompleteGroups << '\n';
    out << "  Read errors               : " << readErrors << '\n';
    out << "  Send errors               : " << sendErrors << '\n';

    out << "\n  Per input stream:\n";
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const StreamStats& s = streams[i];
        out << "    [" << i << "] dataId=" << s.dataId << ' ' << s.path << '\n';
        out << "         read=" << s.eventsRead << " sent=" << s.eventsSent
            << " skipped=" << s.eventsSkipped << " bytes=" << s.bytesSent
            << " readErrors=" << s.readErrors << " sendErrors=" << s.sendErrors
            << " truncatedTails=" << s.truncatedTails << '\n';
    }
    out << std::endl;
}

}  // namespace petsro
