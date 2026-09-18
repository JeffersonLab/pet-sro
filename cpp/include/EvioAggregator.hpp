// EvioAggregator.hpp -- correlate N per-stream events by tick and emit one
// aggregated EVIO v6 record.
//
// The sender assigns the same EJFAT event number (LB tick) to every event in a
// synchronized group, so the load balancer routes all N group members to the
// same receiver host. On the receiver side this class collects the members of
// one group -- keyed by tick, distinguished by RE-header dataId -- and, once
// the group is complete or its wait window has elapsed, produces one EVIO v6
// record whose payload is the N members' EVIO event banks concatenated in
// dataId order.
//
// The output is a self-framing EVIO v6 record (14-word header + index array +
// events + magic 0xC0DA0100 at word 7), not a re-tagged v4 block, so a
// downstream v6 reader can consume it directly. See buildRecord() for the
// exact wire layout.
//
// This class is single-threaded by design: the actor's pump thread owns it and
// no other thread ever calls into it. That is what keeps the group table and
// the statistics lock-free.

#ifndef PETSRO_EVIOAGGREGATOR_HPP
#define PETSRO_EVIOAGGREGATOR_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace petsro {

/// How an incomplete group is handled when its wait window expires.
enum class PartialGroupPolicy {
    Drop,          ///< discard silently, count it, never emit
    EmitPartial    ///< emit an aggregated record with only the members present
};

/// Parses "drop" / "emit-partial" (case-insensitive). Returns false on any
/// other string, leaving `out` untouched.
bool parsePartialGroupPolicy(const std::string& text, PartialGroupPolicy& out) noexcept;

/// Returns "drop" or "emit-partial".
const char* toString(PartialGroupPolicy policy) noexcept;

struct EvioAggregatorConfig {
    /// Number of streams the sender is producing, i.e. the expected number of
    /// members per synchronized group. Required, must be at least 1.
    std::size_t groupSize = 1;

    /// Milliseconds a group may sit open before being reaped. Measured from
    /// the first member's arrival, not from the last.
    int groupTimeoutMs = 500;

    /// What to do with a group that ages out with fewer than groupSize members.
    PartialGroupPolicy onPartial = PartialGroupPolicy::Drop;

    /// Safety ceiling on the number of open groups. A steady-state stream keeps
    /// only a handful in flight; hitting this cap means groups are timing out
    /// faster than they close, and the oldest ones are evicted (as if reaped
    /// under the partial-groups policy) to bound memory.
    std::size_t maxOpenGroups = 4096;

    /// Non-empty message when this config cannot be used, empty otherwise.
    std::string validate() const;
};

/// One aggregated group ready to publish. `payload` holds the complete EVIO v6
/// record bytes in big-endian wire form.
struct AggregatedEvent {
    std::vector<std::uint8_t> payload;
    std::uint64_t tick = 0;
    std::uint16_t memberCount = 0;   ///< how many of groupSize members were included
    bool complete = false;           ///< memberCount == groupSize
};

struct AggregatorStats {
    std::uint64_t membersReceived = 0;   ///< payloads handed to add()
    std::uint64_t groupsEmitted = 0;     ///< complete + partial
    std::uint64_t groupsComplete = 0;    ///< emitted with all N members
    std::uint64_t groupsPartial = 0;     ///< emitted with fewer than N (emit-partial mode)
    std::uint64_t groupsDropped = 0;     ///< aged out and discarded (drop mode, or over-cap eviction)
    std::uint64_t duplicates = 0;        ///< (tick, dataId) already present -- new payload ignored
    std::uint64_t malformed = 0;         ///< payload rejected as not an EVIO block
    std::uint64_t lateArrivals = 0;      ///< tick arrived that had already been reaped

    /// Groups currently open (in flight, waiting for members). Reported for
    /// progress lines; not a counter, just a live gauge.
    std::size_t openGroups = 0;
};

/// The group table plus the EVIO v6 record builder.
class EvioAggregator {
  public:
    using Clock = std::chrono::steady_clock;

    explicit EvioAggregator(EvioAggregatorConfig config);

    /// Feeds one reassembled per-stream event into the table.
    ///
    /// `now` is the timestamp the caller wants recorded on this arrival; it is
    /// separate from Clock::now() so unit tests can advance time deterministically.
    ///
    /// If this arrival completes a group, the aggregated record is appended to
    /// `emitted`. If it also causes over-cap eviction, the evicted groups are
    /// appended too. The caller drains `emitted` after every call; the vector
    /// is a scratch buffer, not owned by the aggregator.
    ///
    /// Returns false and populates `problem` if the payload cannot be treated
    /// as one EVIO block (structural check only). The malformed counter is
    /// incremented and no group state changes on rejection.
    bool add(std::uint64_t tick, std::uint16_t dataId, const std::uint8_t* data,
             std::size_t size, Clock::time_point now,
             std::vector<AggregatedEvent>& emitted, std::string& problem);

    /// Walks the open table and evicts any group older than groupTimeoutMs.
    /// Under Drop the group is discarded; under EmitPartial it is emitted with
    /// whatever members are present. Appends any emitted records to `out`.
    void reap(Clock::time_point now, std::vector<AggregatedEvent>& out);

    /// Emits or drops every remaining open group, per `emitPartial`. Call once
    /// at shutdown so nothing sits in the table forever.
    void flush(std::vector<AggregatedEvent>& out, bool emitPartial);

    const AggregatorStats& stats() const noexcept { return stats_; }

    /// Number of groups currently held open. Exposed so the pump thread can
    /// include it in progress lines.
    std::size_t openGroupCount() const noexcept { return open_.size(); }

  private:
    struct Member {
        std::uint16_t dataId = 0;
        std::vector<std::uint8_t> payload;   ///< the whole reassembled EVIO block
    };

    struct OpenGroup {
        Clock::time_point firstSeen{};
        std::vector<Member> members;         ///< kept ordered by insertion; stable for dedup
    };

    /// Builds one AggregatedEvent from a completed OpenGroup and returns it.
    AggregatedEvent finalize(std::uint64_t tick, OpenGroup& group);

    /// Emits or drops one open group, per the configured partial policy.
    /// Called by reap() and by over-cap eviction.
    void expire(std::uint64_t tick, OpenGroup& group, std::vector<AggregatedEvent>& out);

    EvioAggregatorConfig config_;
    AggregatorStats stats_;

    /// tick -> open group. std::map because the sender assigns ticks
    /// monotonically per group, so iteration order is a very close
    /// approximation of oldest-first for reaping. Also gives O(log N) lookup
    /// and stable iterators, which the reap loop relies on.
    std::map<std::uint64_t, OpenGroup> open_;
};

/// Extracts the "event bank" portion of a received EVIO v4 block: the bytes
/// starting at word 8 (after the 8-word block header) and continuing for the
/// bank's declared length. Returns false with `problem` populated on any
/// structural mismatch: wrong magic, wrong header length, block length
/// disagrees with byte count, or the declared bank does not fit.
///
/// The returned pointer is a borrow into `data`.
struct EvioEventBankView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint32_t rocid = 0;   ///< upper 16 bits of the bank tag/type word
};
bool extractEventBank(const std::uint8_t* data, std::size_t size, EvioEventBankView& out,
                      std::string& problem);

/// Builds one EVIO v6 record. The record contains `events` events, one entry
/// per event in the index array, concatenated in the order given. All bytes
/// are big-endian; total length is a whole number of 32-bit words.
///
/// `recordNumber` is written into header word 1 (typically the group's tick).
std::vector<std::uint8_t> buildEvioV6Record(std::uint64_t recordNumber,
                                            const std::vector<EvioEventBankView>& events);

}  // namespace petsro

#endif  // PETSRO_EVIOAGGREGATOR_HPP
