// EventSynchronizer.hpp -- selects one event per stream, all with equal timestamps.
//
// The capture files are produced by independent FEB streams that were started
// together, so in practice their timestamps are already aligned frame for
// frame. This class does not assume that. It runs an explicit merge: hold one
// current event per stream, and advance every stream that is behind the
// largest current timestamp until they agree.
//
// Assumptions about timestamp ordering, stated because the algorithm depends
// on them:
//
//  - Timestamps are nanoseconds and NON-DECREASING within a file. SroWireFormat
//    documents them as advancing by exactly FRAME_PERIOD_NS per frame from 0.
//    A stream that goes backwards is logged as a discontinuity and still
//    advanced, so a corrupt file degrades into "no group found" rather than a
//    hang.
//  - Timestamp equality is exact. There is no tolerance window: the frames were
//    generated from one clock, so near-misses mean a genuine misalignment worth
//    seeing rather than papering over.
//  - Progress is guaranteed because every non-matching pass consumes at least
//    one event from at least one finite file. maxAdvancesPerGroup is a second
//    belt on top of that, for pathological input whose timestamps never
//    converge.

#ifndef PETSRO_EVENTSYNCHRONIZER_HPP
#define PETSRO_EVENTSYNCHRONIZER_HPP

#include "EvioFileReader.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace petsro {

/// Outcome of an attempt to assemble one synchronized group.
enum class SyncStatus {
    Group,      ///< group() holds one event per stream, all timestamps equal.
    EndOfFile,  ///< At least one stream ran out. The replay loop should restart.
    Error,      ///< Malformed input or an I/O failure. Replay should stop.
    Shutdown    ///< The shutdown flag was observed. Nothing was produced.
};

const char* toString(SyncStatus status) noexcept;

struct SyncStats {
    std::uint64_t groupsFormed = 0;
    std::uint64_t eventsSkipped = 0;         ///< advanced past, across all streams
    std::uint64_t timestampMismatches = 0;   ///< passes that needed an advance
    std::uint64_t timestampRegressions = 0;  ///< a stream's timestamp went backwards
    std::uint64_t incompleteGroups = 0;      ///< discarded because a stream hit EOF
};

/// Merges N event sources into groups sharing one timestamp.
///
/// Holds a reference to the sources; they must outlive the synchronizer.
class EventSynchronizer {
  public:
    /// @param sources one per input file, in command-line order
    /// @param maxAdvancesPerGroup bail out after this many advances without
    ///        converging. 0 disables the guard.
    explicit EventSynchronizer(std::vector<EventSource*> sources,
                               std::uint64_t maxAdvancesPerGroup = 100000);

    /// Assembles the next group. Checks `shutdown` between advances so a
    /// Ctrl-C during a long resynchronization is honoured promptly.
    SyncStatus nextGroup(const std::atomic<bool>& shutdown);

    /// Valid only immediately after nextGroup() returned SyncStatus::Group.
    /// Indexed by stream, in command-line order.
    const std::vector<EvioEvent>& group() const noexcept { return current_; }

    /// The same group, writable, for TimestampRebaser to patch in place before
    /// the events are sent. Edits are lost on the next nextGroup() call, which
    /// overwrites every slot, so this cannot corrupt synchronization state --
    /// note that the merge compares the timestamps read from the files, not
    /// these buffers.
    std::vector<EvioEvent>& mutableGroup() noexcept { return current_; }

    /// Description of the most recent Error, for reporting.
    const std::string& lastError() const noexcept { return lastError_; }

    const SyncStats& stats() const noexcept { return stats_; }

    /// Per-stream count of events advanced past. Indexed like group().
    const std::vector<std::uint64_t>& skippedPerStream() const noexcept { return skipped_; }

    std::size_t streamCount() const noexcept { return sources_.size(); }

  private:
    /// Pulls one event into slot `i`, logging a regression if time went back.
    ReadStatus advance(std::size_t i);

    std::vector<EventSource*> sources_;
    std::vector<EvioEvent> current_;
    std::vector<bool> primed_;  ///< slot i holds a valid unconsumed event
    std::vector<std::uint64_t> skipped_;
    std::vector<std::uint64_t> lastTimestamp_;
    std::vector<bool> haveLastTimestamp_;

    std::uint64_t maxAdvances_;
    std::string lastError_;
    SyncStats stats_;
};

}  // namespace petsro

#endif  // PETSRO_EVENTSYNCHRONIZER_HPP
