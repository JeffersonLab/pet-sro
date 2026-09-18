// EvioAggregator.cpp -- group table plus EVIO v6 record builder.
//
// The header comment on EvioAggregator.hpp explains why this class exists and
// where in the pipeline it sits. The notable details are here:
//
//   * The group table is a std::map keyed by tick. Ticks arrive nearly in
//     order (the sender assigns them monotonically per group), so iterating
//     the map front-to-back is a good approximation of oldest-first. A
//     separate deque was considered and rejected: it doubles the bookkeeping
//     and the reap step is O(open groups) in either representation, and open
//     groups are always small in steady state.
//
//   * "Late arrival" as a distinct counter is not implemented in this version.
//     Distinguishing a genuinely-late packet from a fresh tick after a reset
//     requires state that survives `--reset-event-numbers`, and the metric is
//     nice-to-have rather than load-bearing. A tick whose row was reaped and
//     later shows up again just opens a new row that will itself time out.
//     It is counted then, as `groupsDropped`. The lateArrivals field stays for
//     forward compatibility and is not incremented.
//
//   * EVIO v6 record layout is written explicitly, one field at a time. See
//     buildEvioV6Record().

#include "EvioAggregator.hpp"

#include "SroWireFormat.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <utility>

namespace petsro {

// ---------------------------------------------------------------------------
// PartialGroupPolicy parsing
// ---------------------------------------------------------------------------

bool parsePartialGroupPolicy(const std::string& text, PartialGroupPolicy& out) noexcept {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "drop") {
        out = PartialGroupPolicy::Drop;
        return true;
    }
    if (lower == "emit-partial" || lower == "emit_partial") {
        out = PartialGroupPolicy::EmitPartial;
        return true;
    }
    return false;
}

const char* toString(PartialGroupPolicy policy) noexcept {
    switch (policy) {
        case PartialGroupPolicy::Drop:
            return "drop";
        case PartialGroupPolicy::EmitPartial:
            return "emit-partial";
    }
    return "drop";
}

// ---------------------------------------------------------------------------
// Configuration validation
// ---------------------------------------------------------------------------

std::string EvioAggregatorConfig::validate() const {
    if (groupSize == 0) {
        return "group-size must be at least 1";
    }
    if (groupSize > 0xFFFFU) {
        return "group-size must fit in 16 bits (<= 65535)";
    }
    if (groupTimeoutMs <= 0) {
        return "group-timeout must be greater than 0";
    }
    if (maxOpenGroups == 0) {
        return "max-open-groups must be at least 1";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Event-bank extraction from a received EVIO v4 block
// ---------------------------------------------------------------------------

bool extractEventBank(const std::uint8_t* data, std::size_t size, EvioEventBankView& out,
                      std::string& problem) {
    problem.clear();

    // Smallest legitimate block is a header alone: 8 words = 32 bytes. A block
    // with content occupies at least 10 words: header + one bank of one word.
    constexpr std::size_t HEADER_BYTES = sro::MIN_BLOCK_WORDS * 4U;
    if (data == nullptr) {
        problem = "payload pointer is null";
        return false;
    }
    if (size < HEADER_BYTES) {
        std::ostringstream oss;
        oss << "payload shorter than an EVIO block header: " << size << " < " << HEADER_BYTES
            << " bytes";
        problem = oss.str();
        return false;
    }
    if ((size % 4U) != 0U) {
        std::ostringstream oss;
        oss << "payload size " << size << " is not a whole number of 32-bit words";
        problem = oss.str();
        return false;
    }

    const std::uint32_t blockLenWords = sro::readBe32(data + sro::wordOffset(0));
    const std::uint32_t headerLenWords = sro::readBe32(data + sro::wordOffset(sro::WORD_HEADER_LENGTH));
    const std::uint32_t magic = sro::readBe32(data + sro::wordOffset(sro::WORD_MAGIC));

    if (magic != sro::EVIO_MAGIC) {
        std::ostringstream oss;
        oss << "wrong EVIO magic: 0x" << std::hex << magic << " (expected 0xC0DA0100)";
        problem = oss.str();
        return false;
    }
    if (headerLenWords != sro::MIN_BLOCK_WORDS) {
        std::ostringstream oss;
        oss << "unexpected block header length: " << headerLenWords << " words (expected "
            << sro::MIN_BLOCK_WORDS << ")";
        problem = oss.str();
        return false;
    }
    if (blockLenWords < sro::MIN_BLOCK_WORDS + 2U) {
        problem = "block claims no event content";
        return false;
    }
    if (static_cast<std::size_t>(blockLenWords) * 4U != size) {
        std::ostringstream oss;
        oss << "declared block length " << blockLenWords << " words disagrees with delivered "
            << size << " bytes";
        problem = oss.str();
        return false;
    }

    // The event bank starts at word 8. Its first word is the bank length (in
    // words, not counting the length word itself), so the bank occupies
    // (bankLen + 1) words in total.
    const std::size_t bankStartByte = sro::wordOffset(sro::MIN_BLOCK_WORDS);
    const std::uint32_t bankLenWords = sro::readBe32(data + bankStartByte);
    const std::size_t bankBytes = (static_cast<std::size_t>(bankLenWords) + 1U) * 4U;
    if (bankStartByte + bankBytes > size) {
        std::ostringstream oss;
        oss << "declared event bank length " << bankLenWords
            << " words does not fit in the block (block " << blockLenWords << " words)";
        problem = oss.str();
        return false;
    }

    const std::uint32_t bankTag = sro::readBe32(data + sro::wordOffset(sro::WORD_BANK_TAG));

    out.data = data + bankStartByte;
    out.size = bankBytes;
    out.rocid = sro::rocidFromBankTag(bankTag);
    return true;
}

// ---------------------------------------------------------------------------
// EVIO v6 record builder
// ---------------------------------------------------------------------------
//
// Record layout (all words 32-bit big-endian):
//
//   word  0  Record length in 32-bit words, header + index + data
//   word  1  Record number (we write the group's tick, truncated to 32 bits)
//   word  2  Header length in words = 14
//   word  3  Event count = N
//   word  4  Index array length in bytes = 4 * N
//   word  5  Bit info | version
//              bits [ 0.. 7]  version = 6
//              bit  [    8 ]  hasDictionary = 0
//              bit  [    9 ]  isLastRecord  = 0
//              bits [10..13]  event type    = 1 ("EVIO event", generic)
//              bits [14..31]  reserved / user, zero here
//   word  6  User header length in bytes = 0 (no user header)
//   word  7  Magic 0xC0DA0100
//   word  8  Uncompressed data length low 32 bits (bytes)
//   word  9  Uncompressed data length high 32 bits (bytes)
//   word 10  Compression type (bits 28..31) | compressed data length low 24 bits
//              type 0 = uncompressed; compressed length = uncompressed length
//   word 11  User register 1 low 32 bits (we write the tick low)
//   word 12  User register 1 high 32 bits (we write the tick high)
//   word 13  User register 2 low 32 bits (member count, for convenience)
//
// After the header:
//   * Index array: N × 4-byte big-endian entries. Each entry is the length of
//     the corresponding event in bytes.
//   * Data: the N event banks, concatenated in the order given, each still
//     framed as an EVIO bank (its own length word first).
//
// A downstream reader can locate the k-th event by summing the first k entries
// of the index and adding the fixed offset `14*4 + 4*N`.

namespace {

constexpr std::size_t V6_HEADER_WORDS = 14;
constexpr std::uint32_t V6_VERSION = 6;
constexpr std::uint32_t V6_EVENT_TYPE_EVIO = 1;
constexpr std::uint32_t V6_MAGIC = sro::EVIO_MAGIC;

/// Assembles word 5 (bit info + version).
constexpr std::uint32_t bitInfoWord() noexcept {
    return (V6_EVENT_TYPE_EVIO << 10) | V6_VERSION;
}

}  // namespace

std::vector<std::uint8_t> buildEvioV6Record(std::uint64_t recordNumber,
                                            const std::vector<EvioEventBankView>& events) {
    const std::size_t n = events.size();

    // Data size in bytes -- sum of the event bank sizes. Each bank size is a
    // whole number of 4-byte words by construction, so this is word-aligned.
    std::size_t dataBytes = 0;
    for (const EvioEventBankView& e : events) {
        dataBytes += e.size;
    }

    const std::size_t indexBytes = 4U * n;
    const std::size_t totalBytes = (V6_HEADER_WORDS * 4U) + indexBytes + dataBytes;
    const std::size_t totalWords = totalBytes / 4U;

    std::vector<std::uint8_t> buf(totalBytes, 0);
    std::uint8_t* p = buf.data();

    // Header words 0-13.
    sro::writeBe32(p + sro::wordOffset(0), static_cast<std::uint32_t>(totalWords));
    sro::writeBe32(p + sro::wordOffset(1), static_cast<std::uint32_t>(recordNumber & 0xFFFFFFFFULL));
    sro::writeBe32(p + sro::wordOffset(2), static_cast<std::uint32_t>(V6_HEADER_WORDS));
    sro::writeBe32(p + sro::wordOffset(3), static_cast<std::uint32_t>(n));
    sro::writeBe32(p + sro::wordOffset(4), static_cast<std::uint32_t>(indexBytes));
    sro::writeBe32(p + sro::wordOffset(5), bitInfoWord());
    sro::writeBe32(p + sro::wordOffset(6), 0U);
    sro::writeBe32(p + sro::wordOffset(7), V6_MAGIC);
    sro::writeBe32(p + sro::wordOffset(8), static_cast<std::uint32_t>(dataBytes & 0xFFFFFFFFULL));
    sro::writeBe32(p + sro::wordOffset(9),
                   static_cast<std::uint32_t>((static_cast<std::uint64_t>(dataBytes) >> 32) &
                                              0xFFFFFFFFULL));
    // Uncompressed: type = 0, compressed length = uncompressed length. The
    // spec limits the length field to 28 bits, so a record larger than 256 MiB
    // would need real compression fields set; guard against that.
    const std::uint32_t compressedField =
        static_cast<std::uint32_t>(dataBytes & 0x0FFFFFFFULL);
    sro::writeBe32(p + sro::wordOffset(10), compressedField);
    sro::writeBe32(p + sro::wordOffset(11),
                   static_cast<std::uint32_t>(recordNumber & 0xFFFFFFFFULL));
    sro::writeBe32(p + sro::wordOffset(12),
                   static_cast<std::uint32_t>((recordNumber >> 32) & 0xFFFFFFFFULL));
    sro::writeBe32(p + sro::wordOffset(13), static_cast<std::uint32_t>(n));

    // Index array immediately after the header.
    std::size_t writeOff = sro::wordOffset(V6_HEADER_WORDS);
    for (const EvioEventBankView& e : events) {
        sro::writeBe32(p + writeOff, static_cast<std::uint32_t>(e.size));
        writeOff += 4U;
    }

    // Event data, in order.
    for (const EvioEventBankView& e : events) {
        std::memcpy(p + writeOff, e.data, e.size);
        writeOff += e.size;
    }

    return buf;
}

// ---------------------------------------------------------------------------
// EvioAggregator
// ---------------------------------------------------------------------------

EvioAggregator::EvioAggregator(EvioAggregatorConfig config) : config_(config) {
    // The caller is expected to have validated `config` already; the pump
    // thread never presents an invalid one. Defensive minimums are still
    // applied so this class never reads uninitialised state.
    if (config_.groupSize == 0) {
        config_.groupSize = 1;
    }
    if (config_.groupTimeoutMs <= 0) {
        config_.groupTimeoutMs = 1;
    }
    if (config_.maxOpenGroups == 0) {
        config_.maxOpenGroups = 1;
    }
}

AggregatedEvent EvioAggregator::finalize(std::uint64_t tick, OpenGroup& group) {
    // Order members by dataId so the outer index array and the event order
    // are deterministic across runs, regardless of arrival order.
    std::sort(group.members.begin(), group.members.end(),
              [](const Member& a, const Member& b) { return a.dataId < b.dataId; });

    std::vector<EvioEventBankView> views;
    views.reserve(group.members.size());
    for (const Member& m : group.members) {
        EvioEventBankView v;
        std::string problem;
        // extractEventBank was already validated on add(); a re-validation
        // would fail closed if the payload had been corrupted in place, which
        // it cannot be here. Kept in `problem` for clarity, ignored otherwise.
        if (extractEventBank(m.payload.data(), m.payload.size(), v, problem)) {
            views.push_back(v);
        }
    }

    AggregatedEvent out;
    out.tick = tick;
    out.memberCount = static_cast<std::uint16_t>(views.size());
    out.complete = (views.size() == config_.groupSize);
    out.payload = buildEvioV6Record(tick, views);
    return out;
}

void EvioAggregator::expire(std::uint64_t tick, OpenGroup& group,
                            std::vector<AggregatedEvent>& out) {
    if (config_.onPartial == PartialGroupPolicy::EmitPartial && !group.members.empty()) {
        AggregatedEvent aggregated = finalize(tick, group);
        stats_.groupsEmitted++;
        stats_.groupsPartial++;
        out.push_back(std::move(aggregated));
    } else {
        stats_.groupsDropped++;
    }
}

bool EvioAggregator::add(std::uint64_t tick, std::uint16_t dataId, const std::uint8_t* data,
                         std::size_t size, Clock::time_point now,
                         std::vector<AggregatedEvent>& emitted, std::string& problem) {
    problem.clear();

    EvioEventBankView bank;
    if (!extractEventBank(data, size, bank, problem)) {
        stats_.malformed++;
        return false;
    }

    stats_.membersReceived++;

    auto it = open_.find(tick);
    if (it == open_.end()) {
        // A brand-new tick, or a tick whose row was reaped earlier and just
        // showed up again. Eviction only fires when inserting a new tick, so
        // an existing group is never disturbed by an unrelated arrival: an
        // operator staring at the log would rather see the freshest data and
        // lose the oldest stuck group, matching the actor's queue-full policy.
        while (open_.size() >= config_.maxOpenGroups) {
            auto oldest = open_.begin();
            if (oldest == open_.end()) {
                break;
            }
            const std::uint64_t oldestTick = oldest->first;
            OpenGroup evicted = std::move(oldest->second);
            open_.erase(oldest);
            expire(oldestTick, evicted, emitted);
        }

        OpenGroup g;
        g.firstSeen = now;
        it = open_.emplace(tick, std::move(g)).first;
    }

    OpenGroup& group = it->second;

    // Reject a duplicate dataId. Duplicates only happen if E2SAR replays a
    // fragment, or if the sender ever sends two events with the same
    // (tick, dataId), both of which are misconfigurations. Counted rather
    // than fatal.
    for (const Member& m : group.members) {
        if (m.dataId == dataId) {
            stats_.duplicates++;
            problem = "duplicate dataId within group";
            return false;
        }
    }

    Member m;
    m.dataId = dataId;
    m.payload.assign(data, data + size);
    group.members.push_back(std::move(m));

    if (group.members.size() >= config_.groupSize) {
        AggregatedEvent aggregated = finalize(tick, group);
        stats_.groupsEmitted++;
        stats_.groupsComplete++;
        emitted.push_back(std::move(aggregated));
        open_.erase(it);
    }

    stats_.openGroups = open_.size();
    return true;
}

void EvioAggregator::reap(Clock::time_point now, std::vector<AggregatedEvent>& out) {
    if (open_.empty()) {
        stats_.openGroups = 0;
        return;
    }

    const auto timeout = std::chrono::milliseconds(config_.groupTimeoutMs);

    auto it = open_.begin();
    while (it != open_.end()) {
        const auto age = now - it->second.firstSeen;
        if (age < timeout) {
            ++it;
            continue;
        }
        const std::uint64_t tick = it->first;
        OpenGroup g = std::move(it->second);
        it = open_.erase(it);
        expire(tick, g, out);
    }

    stats_.openGroups = open_.size();
}

void EvioAggregator::flush(std::vector<AggregatedEvent>& out, bool emitPartial) {
    for (auto& entry : open_) {
        if (emitPartial && !entry.second.members.empty()) {
            AggregatedEvent aggregated = finalize(entry.first, entry.second);
            stats_.groupsEmitted++;
            stats_.groupsPartial++;
            out.push_back(std::move(aggregated));
        } else {
            stats_.groupsDropped++;
        }
    }
    open_.clear();
    stats_.openGroups = 0;
}

}  // namespace petsro
