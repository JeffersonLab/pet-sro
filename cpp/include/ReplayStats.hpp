// ReplayStats.hpp -- the counters printed periodically and at shutdown.

#ifndef PETSRO_REPLAYSTATS_HPP
#define PETSRO_REPLAYSTATS_HPP

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace petsro {

/// Per-input-file accounting. One entry per --file, in command-line order.
struct StreamStats {
    std::string path;
    std::uint16_t dataId = 0;
    std::uint64_t eventsRead = 0;
    std::uint64_t eventsSkipped = 0;
    std::uint64_t eventsSent = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t truncatedTails = 0;
};

struct ReplayStats {
    std::vector<StreamStats> streams;

    std::uint64_t loopsCompleted = 0;
    std::uint64_t groupsSent = 0;
    std::uint64_t eventsSent = 0;
    std::uint64_t packetsSent = 0;
    std::uint64_t payloadBytesSent = 0;
    std::uint64_t timestampMismatches = 0;
    std::uint64_t timestampRegressions = 0;
    std::uint64_t incompleteGroups = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t readErrors = 0;

    std::uint64_t totalEventsSkipped() const noexcept;
    std::uint64_t totalEventsRead() const noexcept;

    /// One line, for the periodic progress report.
    std::string progressLine(double elapsedSeconds) const;

    /// The full breakdown, printed once at shutdown.
    void printFinal(std::ostream& out, double elapsedSeconds) const;
};

}  // namespace petsro

#endif  // PETSRO_REPLAYSTATS_HPP
