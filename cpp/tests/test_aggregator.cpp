// test_aggregator.cpp -- unit tests for EvioAggregator and the EVIO v6 record
// builder.
//
// No E2SAR here: the aggregator is driven directly with synthesized EVIO
// blocks from EvioFixtures, so all the behaviours -- completion, timeout,
// duplicates, over-cap eviction, partial-groups policy, EVIO v6 shape --
// exercise deterministically on any host.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "EvioAggregator.hpp"
#include "SroWireFormat.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace petsro;
using namespace petsro::test;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t FIRST_TS = 1'000'000ULL;

/// A synthesized EVIO v4 block payload as it would arrive out of the
/// reassembler. 24 words = 96 bytes, one bank of 15 words (16 with length).
std::vector<std::uint8_t> makeBlock(std::uint32_t rocid, std::uint64_t timestamp = FIRST_TS,
                                    std::uint32_t frame = 0, std::uint32_t words = 24) {
    return encodeWireDumpBlock(makeBlockWords(words, timestamp, frame, rocid));
}

/// Convenience: feed one arrival into the aggregator.
bool addOne(EvioAggregator& agg, std::uint64_t tick, std::uint16_t dataId,
            const std::vector<std::uint8_t>& block, Clock::time_point now,
            std::vector<AggregatedEvent>& emitted, std::string& problem) {
    return agg.add(tick, dataId, block.data(), block.size(), now, emitted, problem);
}

/// EVIO v6 record constants used to check emitted bytes. Kept private to the
/// tests: any change here is a deliberate spec change, not an accidental one.
constexpr std::size_t V6_HEADER_WORDS = 14;
constexpr std::size_t V6_HEADER_BYTES = V6_HEADER_WORDS * 4;
constexpr std::uint32_t V6_MAGIC = 0xC0DA0100U;

}  // namespace

// --- EVIO v4 block extraction ---------------------------------------------

TEST(a_well_formed_block_yields_a_bank_view) {
    const auto block = makeBlock(0x1234);
    EvioEventBankView view;
    std::string problem;
    CHECK(extractEventBank(block.data(), block.size(), view, problem));
    CHECK(problem.empty());
    // The bank starts at word 8, size = (bankLen + 1) * 4. In this fixture the
    // block is 24 words and the header is 8, so the bank is 16 words = 64 bytes.
    CHECK_EQ(view.size, std::size_t{64});
    CHECK_EQ(view.rocid, std::uint32_t{0x1234});
    CHECK_EQ(view.data, block.data() + 32);
}

TEST(a_block_with_wrong_magic_is_rejected) {
    auto block = makeBlock(1);
    // Corrupt the magic word.
    sro::writeBe32(block.data() + sro::wordOffset(sro::WORD_MAGIC), 0xDEADBEEFU);
    EvioEventBankView view;
    std::string problem;
    CHECK_FALSE(extractEventBank(block.data(), block.size(), view, problem));
    CHECK(problem.find("magic") != std::string::npos);
}

TEST(a_truncated_block_is_rejected) {
    auto block = makeBlock(1);
    block.resize(20);  // shorter than one header
    EvioEventBankView view;
    std::string problem;
    CHECK_FALSE(extractEventBank(block.data(), block.size(), view, problem));
    CHECK(problem.find("shorter") != std::string::npos || problem.find("header") != std::string::npos);
}

TEST(a_block_whose_declared_length_disagrees_is_rejected) {
    auto block = makeBlock(1);
    // Advertise a longer block than the buffer.
    sro::writeBe32(block.data() + sro::wordOffset(sro::WORD_BLOCK_LENGTH), 100U);
    EvioEventBankView view;
    std::string problem;
    CHECK_FALSE(extractEventBank(block.data(), block.size(), view, problem));
    CHECK(problem.find("disagrees") != std::string::npos);
}

// --- Aggregation: complete groups -----------------------------------------

TEST(a_group_of_one_member_completes_immediately) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 1;
    EvioAggregator agg(cfg);

    const auto block = makeBlock(0x0100);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 42, 1, block, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{1});
    CHECK(emitted[0].complete);
    CHECK_EQ(emitted[0].tick, std::uint64_t{42});
    CHECK_EQ(emitted[0].memberCount, std::uint16_t{1});
    CHECK_EQ(agg.stats().groupsComplete, std::uint64_t{1});
    CHECK_EQ(agg.stats().groupsEmitted, std::uint64_t{1});
    CHECK_EQ(agg.openGroupCount(), std::size_t{0});
}

TEST(a_group_of_three_members_completes_only_after_the_third_arrival) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 3;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0101);
    const auto b2 = makeBlock(0x0102);
    const auto b3 = makeBlock(0x0103);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 7, 1, b1, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{0});
    CHECK_EQ(agg.openGroupCount(), std::size_t{1});

    CHECK(addOne(agg, 7, 2, b2, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{0});
    CHECK_EQ(agg.openGroupCount(), std::size_t{1});

    CHECK(addOne(agg, 7, 3, b3, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{1});
    CHECK(emitted[0].complete);
    CHECK_EQ(emitted[0].memberCount, std::uint16_t{3});
    CHECK_EQ(agg.openGroupCount(), std::size_t{0});
}

TEST(the_emitted_record_has_a_correct_evio_v6_header) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0201, FIRST_TS + 0);
    const auto b2 = makeBlock(0x0202, FIRST_TS + 1);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 99, 1, b1, t0, emitted, problem));
    CHECK(addOne(agg, 99, 2, b2, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{1});

    const auto& payload = emitted[0].payload;
    CHECK(payload.size() >= V6_HEADER_BYTES);
    CHECK_EQ(payload.size() % 4, std::size_t{0});

    const std::uint32_t recordLenWords = sro::readBe32(payload.data() + sro::wordOffset(0));
    CHECK_EQ(recordLenWords * 4U, payload.size());

    const std::uint32_t recordNumber = sro::readBe32(payload.data() + sro::wordOffset(1));
    CHECK_EQ(recordNumber, std::uint32_t{99});

    const std::uint32_t headerLen = sro::readBe32(payload.data() + sro::wordOffset(2));
    CHECK_EQ(headerLen, std::uint32_t{V6_HEADER_WORDS});

    const std::uint32_t eventCount = sro::readBe32(payload.data() + sro::wordOffset(3));
    CHECK_EQ(eventCount, std::uint32_t{2});

    const std::uint32_t indexBytes = sro::readBe32(payload.data() + sro::wordOffset(4));
    CHECK_EQ(indexBytes, std::uint32_t{2 * 4});

    const std::uint32_t bitInfo = sro::readBe32(payload.data() + sro::wordOffset(5));
    CHECK_EQ(bitInfo & 0xFFU, std::uint32_t{6});  // version = 6

    const std::uint32_t magic = sro::readBe32(payload.data() + sro::wordOffset(7));
    CHECK_EQ(magic, V6_MAGIC);
}

TEST(the_emitted_record_carries_the_index_array_and_event_bytes) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    EvioAggregator agg(cfg);

    // Different sizes on purpose so the index array has to work.
    const auto b1 = makeBlock(0x0301, FIRST_TS, 0, 24);   // 24 words = 96 bytes
    const auto b2 = makeBlock(0x0302, FIRST_TS, 0, 40);   // 40 words = 160 bytes
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 5, 10, b1, t0, emitted, problem));
    CHECK(addOne(agg, 5, 20, b2, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{1});

    // The bank part of each block is (blockWords - 8) words: 16 and 32.
    const std::size_t bank1Bytes = (24 - 8) * 4;  // 64
    const std::size_t bank2Bytes = (40 - 8) * 4;  // 128

    const auto& payload = emitted[0].payload;
    const std::uint32_t idx0 = sro::readBe32(payload.data() + sro::wordOffset(V6_HEADER_WORDS));
    const std::uint32_t idx1 =
        sro::readBe32(payload.data() + sro::wordOffset(V6_HEADER_WORDS + 1));
    CHECK_EQ(idx0, static_cast<std::uint32_t>(bank1Bytes));
    CHECK_EQ(idx1, static_cast<std::uint32_t>(bank2Bytes));

    // Data follows the header + index array. Byte for byte from the original.
    const std::size_t dataOff = V6_HEADER_BYTES + 2 * 4;
    const std::uint8_t* bank1Start = b1.data() + 32;  // after the 8-word block header
    const std::uint8_t* bank2Start = b2.data() + 32;

    for (std::size_t i = 0; i < bank1Bytes; ++i) {
        CHECK_EQ(payload[dataOff + i], bank1Start[i]);
    }
    for (std::size_t i = 0; i < bank2Bytes; ++i) {
        CHECK_EQ(payload[dataOff + bank1Bytes + i], bank2Start[i]);
    }
    CHECK_EQ(payload.size(), dataOff + bank1Bytes + bank2Bytes);
}

TEST(members_are_ordered_by_dataId_regardless_of_arrival_order) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 3;
    EvioAggregator agg(cfg);

    // Arrive in reverse dataId order.
    const auto b3 = makeBlock(0x0403, FIRST_TS, 0, 24);
    const auto b1 = makeBlock(0x0401, FIRST_TS, 0, 28);
    const auto b2 = makeBlock(0x0402, FIRST_TS, 0, 32);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 1, 3, b3, t0, emitted, problem));
    CHECK(addOne(agg, 1, 1, b1, t0, emitted, problem));
    CHECK(addOne(agg, 1, 2, b2, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{1});

    // The index array should reflect dataId order: bank sizes 20, 24, 16 words
    // (from blocks of 28, 32, 24 words respectively), i.e. 80, 96, 64 bytes.
    const auto& payload = emitted[0].payload;
    const std::uint32_t idx0 =
        sro::readBe32(payload.data() + sro::wordOffset(V6_HEADER_WORDS));
    const std::uint32_t idx1 =
        sro::readBe32(payload.data() + sro::wordOffset(V6_HEADER_WORDS + 1));
    const std::uint32_t idx2 =
        sro::readBe32(payload.data() + sro::wordOffset(V6_HEADER_WORDS + 2));
    CHECK_EQ(idx0, std::uint32_t{(28 - 8) * 4});
    CHECK_EQ(idx1, std::uint32_t{(32 - 8) * 4});
    CHECK_EQ(idx2, std::uint32_t{(24 - 8) * 4});
}

// --- Aggregation: rejection paths -----------------------------------------

TEST(a_duplicate_dataId_within_a_group_is_counted_and_rejected) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 3;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0501);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 1, 5, b1, t0, emitted, problem));
    CHECK_FALSE(addOne(agg, 1, 5, b1, t0, emitted, problem));

    CHECK_EQ(agg.stats().duplicates, std::uint64_t{1});
    CHECK_EQ(agg.stats().membersReceived, std::uint64_t{1});
    CHECK_EQ(agg.openGroupCount(), std::size_t{1});
    CHECK_EQ(emitted.size(), std::size_t{0});
}

TEST(a_malformed_payload_is_rejected_without_touching_the_table) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    EvioAggregator agg(cfg);

    std::vector<std::uint8_t> junk(64, 0xAA);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK_FALSE(agg.add(1, 1, junk.data(), junk.size(), t0, emitted, problem));
    CHECK_EQ(agg.stats().malformed, std::uint64_t{1});
    CHECK_EQ(agg.openGroupCount(), std::size_t{0});
    CHECK(!problem.empty());
}

// --- Reaping / timeouts ----------------------------------------------------

TEST(reap_under_drop_discards_stale_groups) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    cfg.groupTimeoutMs = 100;
    cfg.onPartial = PartialGroupPolicy::Drop;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0601);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 1, 7, b1, t0, emitted, problem));
    CHECK_EQ(emitted.size(), std::size_t{0});

    // Not yet expired.
    agg.reap(t0 + std::chrono::milliseconds(50), emitted);
    CHECK_EQ(agg.openGroupCount(), std::size_t{1});
    CHECK_EQ(emitted.size(), std::size_t{0});

    // Expired.
    agg.reap(t0 + std::chrono::milliseconds(200), emitted);
    CHECK_EQ(agg.openGroupCount(), std::size_t{0});
    CHECK_EQ(emitted.size(), std::size_t{0});
    CHECK_EQ(agg.stats().groupsDropped, std::uint64_t{1});
    CHECK_EQ(agg.stats().groupsEmitted, std::uint64_t{0});
}

TEST(reap_under_emit_partial_emits_stale_groups_with_the_members_present) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 3;
    cfg.groupTimeoutMs = 100;
    cfg.onPartial = PartialGroupPolicy::EmitPartial;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0701);
    const auto b2 = makeBlock(0x0702);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 11, 1, b1, t0, emitted, problem));
    CHECK(addOne(agg, 11, 2, b2, t0, emitted, problem));

    agg.reap(t0 + std::chrono::milliseconds(500), emitted);

    CHECK_EQ(emitted.size(), std::size_t{1});
    CHECK_FALSE(emitted[0].complete);
    CHECK_EQ(emitted[0].memberCount, std::uint16_t{2});
    CHECK_EQ(emitted[0].tick, std::uint64_t{11});
    CHECK_EQ(agg.stats().groupsPartial, std::uint64_t{1});
    CHECK_EQ(agg.stats().groupsEmitted, std::uint64_t{1});

    // The emitted partial record's event count is 2, not 3.
    const auto& payload = emitted[0].payload;
    const std::uint32_t evCount = sro::readBe32(payload.data() + sro::wordOffset(3));
    CHECK_EQ(evCount, std::uint32_t{2});
}

TEST(reap_leaves_still_fresh_groups_alone) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    cfg.groupTimeoutMs = 100;
    EvioAggregator agg(cfg);

    const auto b1 = makeBlock(0x0801);
    const auto b2 = makeBlock(0x0802);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    // Two groups, second one arrives later so it should survive the sweep.
    CHECK(addOne(agg, 1, 1, b1, t0, emitted, problem));
    CHECK(addOne(agg, 2, 1, b2, t0 + std::chrono::milliseconds(150), emitted, problem));

    agg.reap(t0 + std::chrono::milliseconds(200), emitted);
    // Group 1 expired (age = 200ms > 100), group 2 fresh (age = 50ms < 100).
    CHECK_EQ(agg.openGroupCount(), std::size_t{1});
    CHECK_EQ(agg.stats().groupsDropped, std::uint64_t{1});
}

// --- Over-cap eviction -----------------------------------------------------

TEST(over_cap_eviction_removes_the_oldest_group) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    cfg.groupTimeoutMs = 60000;  // effectively no timeout
    cfg.maxOpenGroups = 3;
    cfg.onPartial = PartialGroupPolicy::Drop;
    EvioAggregator agg(cfg);

    const auto b = makeBlock(0x0901);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    // Fill to capacity with three ticks, each with one member.
    CHECK(addOne(agg, 1, 1, b, t0, emitted, problem));
    CHECK(addOne(agg, 2, 1, b, t0, emitted, problem));
    CHECK(addOne(agg, 3, 1, b, t0, emitted, problem));
    CHECK_EQ(agg.openGroupCount(), std::size_t{3});

    // The fourth insertion must evict the oldest (tick 1).
    CHECK(addOne(agg, 4, 1, b, t0, emitted, problem));
    CHECK_EQ(agg.openGroupCount(), std::size_t{3});
    CHECK_EQ(agg.stats().groupsDropped, std::uint64_t{1});
}

// --- flush -----------------------------------------------------------------

TEST(flush_drops_or_emits_everything_left_open) {
    EvioAggregatorConfig cfg;
    cfg.groupSize = 2;
    EvioAggregator agg(cfg);

    const auto b = makeBlock(0x0A01);
    std::vector<AggregatedEvent> emitted;
    std::string problem;
    const auto t0 = Clock::now();

    CHECK(addOne(agg, 1, 1, b, t0, emitted, problem));
    CHECK(addOne(agg, 2, 1, b, t0, emitted, problem));

    // Drop mode.
    agg.flush(emitted, /*emitPartial=*/false);
    CHECK_EQ(emitted.size(), std::size_t{0});
    CHECK_EQ(agg.openGroupCount(), std::size_t{0});
    CHECK_EQ(agg.stats().groupsDropped, std::uint64_t{2});

    // Emit-partial mode.
    CHECK(addOne(agg, 3, 1, b, t0, emitted, problem));
    agg.flush(emitted, /*emitPartial=*/true);
    CHECK_EQ(emitted.size(), std::size_t{1});
    CHECK_FALSE(emitted[0].complete);
    CHECK_EQ(agg.stats().groupsPartial, std::uint64_t{1});
}

// --- Policy parser ---------------------------------------------------------

TEST(partial_group_policy_parses_case_insensitively) {
    PartialGroupPolicy p = PartialGroupPolicy::Drop;
    CHECK(parsePartialGroupPolicy("drop", p));
    CHECK_EQ(static_cast<int>(p), static_cast<int>(PartialGroupPolicy::Drop));

    CHECK(parsePartialGroupPolicy("EMIT-PARTIAL", p));
    CHECK_EQ(static_cast<int>(p), static_cast<int>(PartialGroupPolicy::EmitPartial));

    CHECK(parsePartialGroupPolicy("Emit_Partial", p));
    CHECK_EQ(static_cast<int>(p), static_cast<int>(PartialGroupPolicy::EmitPartial));

    CHECK_FALSE(parsePartialGroupPolicy("something-else", p));
}

int main() { return petsro::test::runAll(); }
