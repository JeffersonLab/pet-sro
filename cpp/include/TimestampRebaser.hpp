// TimestampRebaser.hpp -- keeps replayed time moving forward across loops.
//
// Reference: org.jlab.detimg.petiroc.replay.ReplayStream.advance(), which says
// this is mandatory rather than optional. If a finite capture were replayed
// verbatim, the timestamp would jump backwards at every loop point, and
// EventTimeSlice.canAccept()/isFull(), every coincidence window, and any
// time-slice-based load-balancer routing downstream would all break.
//
// So each wrap adds the capture's own span plus one frame period to a running
// offset. The extra frame period keeps the seam the same length as any other
// inter-frame gap, so a consumer sees no discontinuity at all.
//
// ONE DIFFERENCE FROM THE JAVA CODE, AND IT MATTERS.
//
// ReplayStream computes the offset per stream, from that stream's own
// ReplayFrameSource.durationNanos(). It can: its streams are independent
// emulated FEBs that are never compared to each other.
//
// Here the streams must keep EXACTLY equal timestamps or EventSynchronizer
// stops emitting groups entirely. Captures of unequal length -- which is the
// normal case, see the README -- would get different per-stream offsets and
// drift apart permanently after the first loop. So the timestamp offset is
// SHARED by all N streams and derived from the span of the synchronized groups
// actually sent, which is common to every stream by construction.
//
// The frame counter is different: nothing synchronizes on it, so it is rebased
// per stream from that stream's own first and last counter in the pass.

#ifndef PETSRO_TIMESTAMPREBASER_HPP
#define PETSRO_TIMESTAMPREBASER_HPP

#include "EvioFileReader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace petsro {

/// Rewrites words 13, 14 and 15 of each outgoing event so replayed time is
/// monotonic across replay loops.
class TimestampRebaser {
  public:
    explicit TimestampRebaser(std::size_t streamCount);

    /// Rebases every event of one synchronized group, in place, and records the
    /// group's extent so endPass() can compute the next loop's offset.
    ///
    /// Patching in place costs no copy: the buffer belongs to the reader and is
    /// overwritten by the next read anyway. On success each event's `timestamp`
    /// and `frameCounter` fields are updated to the values now in its bytes.
    ///
    /// Returns false and sets lastError() if a block is too short to hold the
    /// fields, which the reader should already have rejected.
    bool applyToGroup(std::vector<EvioEvent>& group);

    /// Call when a replay pass ends. Advances the offsets by the span just
    /// replayed plus one frame period. A pass that sent nothing changes nothing.
    void endPass();

    /// Nanoseconds currently added to every emitted timestamp.
    std::uint64_t timestampOffset() const noexcept { return timestampOffset_; }

    /// True once a group has been seen in the current pass.
    bool passStarted() const noexcept { return passStarted_; }

    /// Span of the groups sent in the current pass, in nanoseconds.
    std::uint64_t currentPassSpan() const noexcept {
        return passStarted_ ? lastRawTimestamp_ - firstRawTimestamp_ : 0;
    }

    const std::string& lastError() const noexcept { return lastError_; }

  private:
    std::uint64_t timestampOffset_ = 0;
    std::vector<std::uint64_t> frameCounterOffset_;

    bool passStarted_ = false;
    std::uint64_t firstRawTimestamp_ = 0;
    std::uint64_t lastRawTimestamp_ = 0;
    std::vector<std::uint64_t> firstRawFrameCounter_;
    std::vector<std::uint64_t> lastRawFrameCounter_;

    std::string lastError_;
};

}  // namespace petsro

#endif  // PETSRO_TIMESTAMPREBASER_HPP
