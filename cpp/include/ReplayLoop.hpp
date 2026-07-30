// ReplayLoop.hpp -- the application control logic.
//
//   initialize EJFAT sender
//   install Ctrl-C handler
//
//   while shutdown has not been requested:
//       open all N input files
//       synchronize and send event groups
//       continue until one or more files reach EOF
//       close all input files
//       reopen all files from the beginning
//
// The whole set restarts together. A single file reaching EOF ends the loop
// iteration for everyone, because letting one file wrap on its own would
// silently pair frames from different points in the capture.

#ifndef PETSRO_REPLAYLOOP_HPP
#define PETSRO_REPLAYLOOP_HPP

#include "EvioFileReader.hpp"
#include "PacketSink.hpp"
#include "ReplayStats.hpp"
#include "TimestampRebaser.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace petsro {

struct ReplayLoopConfig {
    /// EJFAT dataId of input file i is dataIdBase + i. See the README.
    std::uint16_t dataIdBase = 1;

    /// When true, entropy of input file i is set to 1 + i, so all packets of
    /// one source take one path through the load balancer. When false, entropy
    /// is left at 0 and E2SAR randomises it per event.
    bool entropyPerSource = false;

    /// When true, the EJFAT event number restarts at 1 on every replay loop.
    /// When false (the default) it keeps increasing across loops, so no two
    /// events in a run share a number.
    bool resetEventNumbers = false;

    /// When true (the default), each replay loop adds the span just replayed
    /// plus one frame period to the timestamps and frame counters written into
    /// the outgoing events, so replayed time never goes backwards. Setting this
    /// false replays the captured timestamps verbatim, which makes time jump
    /// back at every loop seam. See TimestampRebaser.
    bool rebaseTimestamps = true;

    /// Number of replay loops to run. 0 means forever, until Ctrl-C.
    std::uint64_t loopLimit = 0;

    /// Optional pause between synchronized groups, for replaying at a
    /// deliberate cadence when E2SAR rate shaping is not in use.
    std::uint64_t groupDelayUs = 0;

    /// Seconds between progress lines. 0 disables them.
    double statsIntervalSeconds = 5.0;
};

/// Owns the readers, drives the synchronizer, and feeds the sink.
class ReplayLoop {
  public:
    ReplayLoop(std::vector<std::unique_ptr<EvioFileReader>> readers, PacketSink& sink,
               ReplayLoopConfig config);

    /// Runs until the shutdown flag is set, the loop limit is reached, or an
    /// unrecoverable error occurs. Returns true if it stopped cleanly.
    bool run(const std::atomic<bool>& shutdown);

    const ReplayStats& stats() const noexcept { return stats_; }

    /// Description of the failure that stopped the run, if any.
    const std::string& lastError() const noexcept { return lastError_; }

  private:
    /// One pass over the whole file set. Returns false to stop the outer loop.
    bool runOnePass(const std::atomic<bool>& shutdown, bool& fatal);

    /// Rebases (if enabled) and sends the N events of one group. Takes the
    /// group by non-const reference because rebasing patches it in place.
    /// Returns false on an unrecoverable rebase or send failure.
    bool sendGroup(std::vector<EvioEvent>& group);

    void collectReaderStats();

    std::vector<std::unique_ptr<EvioFileReader>> readers_;
    PacketSink& sink_;
    ReplayLoopConfig config_;

    ReplayStats stats_;
    std::string lastError_;

    /// EJFAT event number of the next event to be sent. Starts at 1 because
    /// E2SAR treats 0 as "do not override the internal counter".
    std::uint64_t nextEventNumber_ = 1;

    TimestampRebaser rebaser_;
};

}  // namespace petsro

#endif  // PETSRO_REPLAYLOOP_HPP
