#include "TimestampRebaser.hpp"

#include "Logging.hpp"
#include "SroWireFormat.hpp"

#include <sstream>

namespace petsro {

TimestampRebaser::TimestampRebaser(std::size_t streamCount)
    : frameCounterOffset_(streamCount, 0),
      firstRawFrameCounter_(streamCount, 0),
      lastRawFrameCounter_(streamCount, 0) {}

bool TimestampRebaser::applyToGroup(std::vector<EvioEvent>& group) {
    if (group.empty()) {
        return true;
    }
    if (group.size() != frameCounterOffset_.size()) {
        std::ostringstream oss;
        oss << "rebaser built for " << frameCounterOffset_.size() << " stream(s) but given "
            << group.size();
        lastError_ = oss.str();
        return false;
    }

    // Every event of a group carries the same timestamp; that is the whole
    // point of the synchronizer, so stream 0 speaks for the group.
    const std::uint64_t rawTimestamp = group[0].timestamp;

    if (!passStarted_) {
        firstRawTimestamp_ = rawTimestamp;
        for (std::size_t i = 0; i < group.size(); ++i) {
            firstRawFrameCounter_[i] = group[i].frameCounter;
        }
        passStarted_ = true;
    }
    lastRawTimestamp_ = rawTimestamp;

    const std::uint64_t emittedTimestamp = rawTimestamp + timestampOffset_;

    for (std::size_t i = 0; i < group.size(); ++i) {
        EvioEvent& event = group[i];
        lastRawFrameCounter_[i] = event.frameCounter;

        // The reader refuses any block shorter than this, so reaching the guard
        // means an event arrived from somewhere else.
        if (event.data.size() < sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS)) {
            std::ostringstream oss;
            oss << "stream " << i << ": block of " << event.data.size()
                << " bytes is too short to rebase; need at least "
                << sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS);
            lastError_ = oss.str();
            return false;
        }

        const std::uint64_t emittedFrameCounter = event.frameCounter + frameCounterOffset_[i];

        std::uint8_t* base = event.data.data();
        // Word 13 is a 32-bit field, so the counter wraps there exactly as it
        // does in ReplayStream, which casts to int for the same reason.
        sro::writeBe32(base + sro::wordOffset(sro::WORD_FRAME_COUNTER),
                       static_cast<std::uint32_t>(emittedFrameCounter & 0xFFFFFFFFU));
        sro::writeBe32(base + sro::wordOffset(sro::WORD_TIMESTAMP_LO),
                       static_cast<std::uint32_t>(emittedTimestamp & 0xFFFFFFFFU));
        sro::writeBe32(base + sro::wordOffset(sro::WORD_TIMESTAMP_HI),
                       static_cast<std::uint32_t>((emittedTimestamp >> 32) & 0xFFFFFFFFU));

        // Keep the decoded fields in step with the bytes, so logs and any
        // downstream accounting report what actually went on the wire.
        event.timestamp = emittedTimestamp;
        event.frameCounter = emittedFrameCounter;
    }

    return true;
}

void TimestampRebaser::endPass() {
    if (!passStarted_) {
        // Nothing was sent this pass, so there is no span to skip over and the
        // offsets must not move.
        return;
    }

    const std::uint64_t span = lastRawTimestamp_ - firstRawTimestamp_;
    timestampOffset_ += span + sro::FRAME_PERIOD_NS;

    for (std::size_t i = 0; i < frameCounterOffset_.size(); ++i) {
        frameCounterOffset_[i] +=
            (lastRawFrameCounter_[i] - firstRawFrameCounter_[i]) + 1;
    }

    LOG_DEBUG << "loop seam: replayed span " << span << " ns, timestamp offset now "
              << timestampOffset_ << " ns";

    passStarted_ = false;
}

}  // namespace petsro
