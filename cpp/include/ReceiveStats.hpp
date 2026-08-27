// ReceiveStats.hpp -- the counters the EJFAT receiver keeps and prints.
//
// Split out of recv_main.cpp so the executable and the ERSAP actor report the
// same numbers from the same code. Everything here is transport-agnostic: it
// accounts for EVIO events, not for packets. The packet-level view belongs to
// the E2SAR Reassembler and is printed by EjfatReceiver::reportTransport().

#ifndef PETSRO_RECEIVESTATS_HPP
#define PETSRO_RECEIVESTATS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>

namespace petsro {

/// What one EJFAT source (one dataId, i.e. one input file on the sender) sent.
struct SourceStats {
    std::uint64_t events = 0;
    std::uint64_t bytes = 0;
    std::uint64_t malformed = 0;

    std::uint32_t rocid = 0;
    bool rocidStable = true;

    std::uint64_t firstTimestamp = 0;
    std::uint64_t lastTimestamp = 0;
    bool haveTimestamp = false;

    std::uint64_t timestampRegressions = 0;
    std::uint64_t frameCounterGaps = 0;
    std::uint64_t lastFrameCounter = 0;
    bool haveFrameCounter = false;

    std::size_t smallestEvent = 0;
    std::size_t largestEvent = 0;
};

/// How much EVIO validation accumulate() performs on each event.
///
/// The receiver always needs the block length and magic to say anything at all
/// about a buffer, so None is the floor rather than "no checks": it still
/// reports a decode failure, it just does not follow the timestamp and frame
/// counter across events.
enum class ValidationLevel {
    None = 0,       ///< decode only; do not cross-check against previous events
    Structural = 1, ///< the default: magic, length agreement, per-source rocid
    Strict = 2      ///< also treat a regression or a counter gap as malformed
};

const char* toString(ValidationLevel level) noexcept;

/// Parses "none", "structural" or "strict". Returns false on anything else.
bool parseValidationLevel(const std::string& text, ValidationLevel& out) noexcept;

struct ReceiveStats {
    std::uint64_t events = 0;
    std::uint64_t bytes = 0;
    std::uint64_t malformed = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t recvErrors = 0;
    std::map<std::uint16_t, SourceStats> perSource;

    /// Folds one reassembled event into the counters and decodes its EVIO
    /// header. Returns true when the event is usable at `level`; when it
    /// returns false the caller has a malformed event and `problem` says why.
    ///
    /// Borrows `buffer` for the duration of the call and never copies it.
    bool accumulate(std::uint16_t dataId, const std::uint8_t* buffer, std::size_t size,
                    ValidationLevel level, std::string& problem);

    /// One line, for the periodic progress report.
    std::string progressLine(double elapsedSeconds) const;

    /// The per-event breakdown, printed once at shutdown. The transport's own
    /// counters are appended separately by EjfatReceiver::reportTransport().
    void printFinal(std::ostream& out, double elapsedSeconds) const;
};

}  // namespace petsro

#endif  // PETSRO_RECEIVESTATS_HPP
