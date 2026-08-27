#include "ReceiveStats.hpp"

#include "EvioEventView.hpp"
#include "SroWireFormat.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace petsro {

const char* toString(ValidationLevel level) noexcept {
    switch (level) {
        case ValidationLevel::None:       return "none";
        case ValidationLevel::Structural: return "structural";
        case ValidationLevel::Strict:     return "strict";
    }
    return "unknown";
}

bool parseValidationLevel(const std::string& text, ValidationLevel& out) noexcept {
    if (text == "none") {
        out = ValidationLevel::None;
        return true;
    }
    if (text == "structural") {
        out = ValidationLevel::Structural;
        return true;
    }
    if (text == "strict") {
        out = ValidationLevel::Strict;
        return true;
    }
    return false;
}

bool ReceiveStats::accumulate(std::uint16_t dataId, const std::uint8_t* buffer, std::size_t size,
                              ValidationLevel level, std::string& problem) {
    problem.clear();

    events++;
    bytes += size;

    SourceStats& src = perSource[dataId];
    src.events++;
    src.bytes += size;
    src.smallestEvent = (src.smallestEvent == 0) ? size : std::min(src.smallestEvent, size);
    src.largestEvent = std::max(src.largestEvent, size);

    const EvioEventView view = inspectEvioEvent(buffer, size);
    if (!view.valid) {
        malformed++;
        src.malformed++;
        problem = view.problem;
        return false;
    }

    if (level == ValidationLevel::Strict) {
        // Two header fields inspectEvioEvent() deliberately leaves alone,
        // because the file reader guarantees them and re-checking them on every
        // event of a healthy stream is wasted work. At Strict the point is to
        // catch a stream that is not what it claims to be.
        const std::uint32_t headerWords =
            sro::readBe32(buffer + sro::wordOffset(sro::WORD_HEADER_LENGTH));
        if (headerWords != sro::MIN_BLOCK_WORDS) {
            std::ostringstream oss;
            oss << "block header claims " << headerWords << " words, expected "
                << sro::MIN_BLOCK_WORDS;
            problem = oss.str();
            malformed++;
            src.malformed++;
            return false;
        }

        const std::uint32_t version =
            sro::versionFromWord(sro::readBe32(buffer + sro::wordOffset(sro::WORD_VERSION)));
        if (version != sro::EVIO_VERSION) {
            std::ostringstream oss;
            oss << "EVIO version " << version << ", expected " << sro::EVIO_VERSION;
            problem = oss.str();
            malformed++;
            src.malformed++;
            return false;
        }
    }

    bool regressed = false;
    if (!src.haveTimestamp) {
        src.firstTimestamp = view.timestamp;
        src.rocid = view.rocid;
        src.haveTimestamp = true;
    } else if (view.timestamp < src.lastTimestamp) {
        // With rebasing enabled on the sender this should never happen, not
        // even across a replay-loop seam.
        src.timestampRegressions++;
        regressed = true;
        std::ostringstream oss;
        oss << "timestamp went backwards, " << src.lastTimestamp << " -> " << view.timestamp;
        problem = oss.str();
    }
    src.lastTimestamp = view.timestamp;

    if (view.rocid != src.rocid) {
        src.rocidStable = false;
    }

    bool gapped = false;
    if (src.haveFrameCounter && view.frameCounter != src.lastFrameCounter + 1) {
        // Reassembly drops show up here as well as in the E2SAR loss counters,
        // and this view is the one that matches what a physics consumer sees.
        src.frameCounterGaps++;
        gapped = true;
        if (problem.empty()) {
            std::ostringstream oss;
            oss << "frame counter jumped, " << src.lastFrameCounter << " -> "
                << view.frameCounter;
            problem = oss.str();
        }
    }
    src.lastFrameCounter = view.frameCounter;
    src.haveFrameCounter = true;

    if (level == ValidationLevel::Strict && (regressed || gapped)) {
        malformed++;
        src.malformed++;
        return false;
    }

    // At None and Structural a regression or a gap is counted and reported but
    // does not condemn the event: its bytes are still a well-formed EVIO block.
    problem.clear();
    return true;
}

std::string ReceiveStats::progressLine(double elapsedSeconds) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "events " << events << " | sources " << perSource.size() << " | "
        << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
    if (elapsedSeconds > 0.0) {
        oss << " | " << std::setprecision(1)
            << (static_cast<double>(bytes) * 8.0 / 1.0e6) / elapsedSeconds << " Mbps"
            << " | " << (static_cast<double>(events) / elapsedSeconds) << " evt/s";
    }
    if (malformed > 0) {
        oss << " | malformed " << malformed;
    }
    return oss.str();
}

void ReceiveStats::printFinal(std::ostream& out, double elapsedSeconds) const {
    out << "\n========== Reception Summary ==========\n";
    out << std::fixed << std::setprecision(3);
    out << "  Elapsed                   : " << elapsedSeconds << " s\n";
    out << "  EVIO events received      : " << events << '\n';
    out << "  Payload bytes received    : " << bytes << " (" << std::setprecision(2)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB)\n";
    out << std::setprecision(3);
    out << "  Malformed EVIO events     : " << malformed << '\n';
    out << "  Receive timeouts          : " << timeouts << '\n';
    out << "  Receive errors            : " << recvErrors << '\n';
    if (elapsedSeconds > 0.0) {
        out << "  Average rate              : " << std::setprecision(2)
            << (static_cast<double>(bytes) * 8.0 / 1.0e6) / elapsedSeconds << " Mbps, "
            << (static_cast<double>(events) / elapsedSeconds) << " events/s\n";
        out << std::setprecision(3);
    }

    out << "\n  Per EJFAT source (dataId):\n";
    for (const auto& entry : perSource) {
        const SourceStats& s = entry.second;
        out << "    dataId=" << entry.first << " rocid=" << s.rocid
            << (s.rocidStable ? "" : " (UNSTABLE)") << '\n';
        out << "         events=" << s.events << " bytes=" << s.bytes
            << " malformed=" << s.malformed << '\n';
        out << "         event size min=" << s.smallestEvent << " max=" << s.largestEvent
            << " bytes\n";
        out << "         timestamp first=" << s.firstTimestamp << " last=" << s.lastTimestamp;
        if (s.haveTimestamp && s.lastTimestamp >= s.firstTimestamp) {
            out << " span=" << std::setprecision(3)
                << (static_cast<double>(s.lastTimestamp - s.firstTimestamp) / 1e9) << " s";
        }
        out << '\n';
        out << "         timestamp regressions=" << s.timestampRegressions
            << " frame-counter gaps=" << s.frameCounterGaps << '\n';
    }
}

}  // namespace petsro
