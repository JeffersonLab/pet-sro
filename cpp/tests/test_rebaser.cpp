// test_rebaser.cpp -- loop-seam timestamp rebasing.
//
// The property under test is the one ReplayStream's comment insists on: across
// a loop seam, replayed time must keep moving forward, and the seam must be
// indistinguishable from any other inter-frame gap.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "PacketSink.hpp"
#include "ReplayLoop.hpp"
#include "SroWireFormat.hpp"
#include "TimestampRebaser.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace petsro;
using namespace petsro::test;

namespace {

constexpr std::size_t MAX_PLD_1500 = 1436;
constexpr std::uint64_t STEP = sro::FRAME_PERIOD_NS;

const std::atomic<bool>& neverShutdown() {
    static std::atomic<bool> flag{false};
    return flag;
}

/// Reads the timestamp back out of the event bytes, which is what a receiver
/// would do. Checking the struct field alone would not prove the wire changed.
std::uint64_t timestampFromBytes(const std::vector<std::uint8_t>& data) {
    return sro::combineTimestamp(
        sro::readBe32(data.data() + sro::wordOffset(sro::WORD_TIMESTAMP_LO)),
        sro::readBe32(data.data() + sro::wordOffset(sro::WORD_TIMESTAMP_HI)));
}

std::uint64_t frameCounterFromBytes(const std::vector<std::uint8_t>& data) {
    return sro::readBe32(data.data() + sro::wordOffset(sro::WORD_FRAME_COUNTER));
}

/// One synthetic group of `streams` events, all at `timestamp`.
std::vector<EvioEvent> makeGroup(std::size_t streams, std::uint64_t timestamp,
                                 std::uint32_t frameCounter) {
    std::vector<EvioEvent> group(streams);
    for (std::size_t i = 0; i < streams; ++i) {
        const auto words = makeBlockWords(24, timestamp, frameCounter,
                                          static_cast<std::uint32_t>(100 + i));
        group[i].data = encodeWireDumpBlock(words);
        group[i].timestamp = timestamp;
        group[i].frameCounter = frameCounter;
        group[i].rocid = static_cast<std::uint32_t>(100 + i);
    }
    return group;
}

std::vector<std::unique_ptr<TempFile>> makeAlignedCaptures(std::size_t count,
                                                           std::uint32_t frames) {
    std::vector<std::unique_ptr<TempFile>> files;
    for (std::size_t i = 0; i < count; ++i) {
        auto f = std::make_unique<TempFile>("test_rebase_stream" + std::to_string(i) + ".evio");
        f->write(makeWireDumpCapture(frames, 0, STEP, static_cast<std::uint32_t>(100 + i)));
        files.push_back(std::move(f));
    }
    return files;
}

std::vector<std::unique_ptr<EvioFileReader>> readersFor(
    const std::vector<std::unique_ptr<TempFile>>& files) {
    std::vector<std::unique_ptr<EvioFileReader>> readers;
    for (const auto& f : files) {
        readers.push_back(std::make_unique<EvioFileReader>(f->path()));
    }
    return readers;
}

}  // namespace

// --- the rebaser in isolation -------------------------------------------

TEST(the_first_pass_is_emitted_verbatim) {
    TimestampRebaser rebaser(2);

    auto group = makeGroup(2, 5000, 7);
    CHECK(rebaser.applyToGroup(group));

    // Offset is still zero, so the bytes must be unchanged.
    CHECK_EQ(timestampFromBytes(group[0].data), 5000U);
    CHECK_EQ(frameCounterFromBytes(group[0].data), 7U);
    CHECK_EQ(group[0].timestamp, 5000U);
    CHECK_EQ(rebaser.timestampOffset(), 0U);
}

TEST(the_second_pass_is_offset_by_the_span_plus_one_frame_period) {
    TimestampRebaser rebaser(1);

    // A pass spanning three frames: 0, STEP, 2*STEP.
    for (std::uint32_t i = 0; i < 3; ++i) {
        auto group = makeGroup(1, i * STEP, i);
        CHECK(rebaser.applyToGroup(group));
    }
    CHECK_EQ(rebaser.currentPassSpan(), 2U * STEP);

    rebaser.endPass();
    // span (2 frames) + one frame period = 3 frame periods.
    CHECK_EQ(rebaser.timestampOffset(), 3U * STEP);

    // The next pass replays the same raw timestamps, shifted.
    auto group = makeGroup(1, 0, 0);
    CHECK(rebaser.applyToGroup(group));
    CHECK_EQ(timestampFromBytes(group[0].data), 3U * STEP);
}

TEST(the_loop_seam_is_exactly_one_frame_period_long) {
    // This is the property ReplayStream's comment is about: the gap across the
    // seam must be indistinguishable from any ordinary inter-frame gap.
    TimestampRebaser rebaser(1);

    std::uint64_t lastEmitted = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        auto group = makeGroup(1, i * STEP, i);
        CHECK(rebaser.applyToGroup(group));
        lastEmitted = timestampFromBytes(group[0].data);
    }
    rebaser.endPass();

    auto firstOfNextLoop = makeGroup(1, 0, 0);
    CHECK(rebaser.applyToGroup(firstOfNextLoop));
    const std::uint64_t firstEmitted = timestampFromBytes(firstOfNextLoop[0].data);

    CHECK_EQ(firstEmitted - lastEmitted, STEP);
}

TEST(frame_counters_are_rebased_too) {
    TimestampRebaser rebaser(1);

    for (std::uint32_t i = 0; i < 5; ++i) {
        auto group = makeGroup(1, i * STEP, i);
        CHECK(rebaser.applyToGroup(group));
    }
    rebaser.endPass();

    auto group = makeGroup(1, 0, 0);
    CHECK(rebaser.applyToGroup(group));
    // Counters 0..4 were replayed, so the next loop starts at 5.
    CHECK_EQ(frameCounterFromBytes(group[0].data), 5U);
}

TEST(all_streams_share_one_timestamp_offset) {
    // The whole reason this differs from ReplayStream: a per-stream offset
    // would desynchronize streams of unequal length after the first loop.
    TimestampRebaser rebaser(3);

    for (std::uint32_t i = 0; i < 4; ++i) {
        auto group = makeGroup(3, i * STEP, i);
        CHECK(rebaser.applyToGroup(group));
    }
    rebaser.endPass();

    auto group = makeGroup(3, 0, 0);
    CHECK(rebaser.applyToGroup(group));

    // Still equal across every stream, which is what the synchronizer needs.
    const std::uint64_t ts = timestampFromBytes(group[0].data);
    CHECK_EQ(timestampFromBytes(group[1].data), ts);
    CHECK_EQ(timestampFromBytes(group[2].data), ts);
    CHECK_EQ(ts, 4U * STEP);
}

TEST(a_pass_that_sent_nothing_does_not_move_the_offset) {
    TimestampRebaser rebaser(1);
    CHECK_FALSE(rebaser.passStarted());

    rebaser.endPass();
    CHECK_EQ(rebaser.timestampOffset(), 0U);

    // A real pass still starts from zero afterwards.
    auto group = makeGroup(1, 42, 0);
    CHECK(rebaser.applyToGroup(group));
    CHECK_EQ(timestampFromBytes(group[0].data), 42U);
}

TEST(the_offset_accumulates_over_many_loops) {
    TimestampRebaser rebaser(1);

    for (int loop = 0; loop < 10; ++loop) {
        for (std::uint32_t i = 0; i < 3; ++i) {
            auto group = makeGroup(1, i * STEP, i);
            CHECK(rebaser.applyToGroup(group));
        }
        rebaser.endPass();
    }
    // Ten passes, each advancing by 3 frame periods.
    CHECK_EQ(rebaser.timestampOffset(), 30U * STEP);
}

TEST(a_group_of_the_wrong_size_is_refused) {
    TimestampRebaser rebaser(2);
    auto group = makeGroup(3, 0, 0);
    CHECK_FALSE(rebaser.applyToGroup(group));
    CHECK(rebaser.lastError().find("2 stream(s) but given 3") != std::string::npos);
}

TEST(a_block_too_short_to_hold_a_timestamp_is_refused) {
    TimestampRebaser rebaser(1);

    std::vector<EvioEvent> group(1);
    group[0].data.assign(40, 0);  // 10 words: no room for words 14/15
    group[0].timestamp = 0;

    CHECK_FALSE(rebaser.applyToGroup(group));
    CHECK(rebaser.lastError().find("too short to rebase") != std::string::npos);
}

TEST(a_timestamp_spanning_both_words_rebases_correctly) {
    // Forces a carry out of the low word, which a 32-bit-only implementation
    // would get wrong.
    TimestampRebaser rebaser(1);

    auto group = makeGroup(1, 0xFFFFFFFEULL, 0);
    CHECK(rebaser.applyToGroup(group));
    rebaser.endPass();  // span 0, so offset becomes one frame period

    auto next = makeGroup(1, 0xFFFFFFFEULL, 0);
    CHECK(rebaser.applyToGroup(next));
    CHECK_EQ(timestampFromBytes(next[0].data), 0xFFFFFFFEULL + STEP);
    // The high word must now be 1, not 0.
    CHECK_EQ(sro::readBe32(next[0].data.data() + sro::wordOffset(sro::WORD_TIMESTAMP_HI)), 1U);
}

// --- through the replay loop --------------------------------------------

TEST(replayed_timestamps_are_monotonic_across_loops) {
    const auto files = makeAlignedCaptures(2, 5);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 4;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{40});  // 4 loops x 5 frames x 2 streams

    // Every group's timestamp is one frame period after the previous group's,
    // with no discontinuity at any of the three seams.
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < sent.size(); i += 2) {
        const std::uint64_t a = timestampFromBytes(sent[i].payload);
        const std::uint64_t b = timestampFromBytes(sent[i + 1].payload);
        CHECK_EQ(a, expected);
        CHECK_EQ(b, expected);  // both streams agree
        expected += STEP;
    }
}

TEST(replayed_frame_counters_are_monotonic_across_loops) {
    const auto files = makeAlignedCaptures(1, 4);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 3;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{12});
    for (std::size_t i = 0; i < sent.size(); ++i) {
        CHECK_EQ(frameCounterFromBytes(sent[i].payload), static_cast<std::uint64_t>(i));
    }
}

TEST(rebasing_can_be_switched_off_for_verbatim_replay) {
    const auto files = makeAlignedCaptures(1, 3);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 2;
    config.rebaseTimestamps = false;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{6});
    // Time jumps back to 0 at the seam, which is exactly what rebasing exists
    // to prevent.
    CHECK_EQ(timestampFromBytes(sent[2].payload), 2U * STEP);
    CHECK_EQ(timestampFromBytes(sent[3].payload), 0U);
}

TEST(rebasing_leaves_every_other_byte_of_the_event_untouched) {
    const auto files = makeAlignedCaptures(1, 2);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 2;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    // Same frame, first loop versus second: only words 13, 14 and 15 differ.
    const auto& first = sink.sent()[0].payload;
    const auto& second = sink.sent()[2].payload;
    CHECK_EQ(first.size(), second.size());

    for (std::size_t w = 0; w * 4 < first.size(); ++w) {
        const bool rebasedWord = (w == sro::WORD_FRAME_COUNTER ||
                                  w == sro::WORD_TIMESTAMP_LO || w == sro::WORD_TIMESTAMP_HI);
        const std::uint32_t a = sro::readBe32(first.data() + w * 4);
        const std::uint32_t b = sro::readBe32(second.data() + w * 4);
        if (!rebasedWord) {
            CHECK_EQ(a, b);
        }
    }
    // And the block is still a valid EVIO block.
    CHECK_EQ(sro::readBe32(second.data() + sro::wordOffset(sro::WORD_MAGIC)), sro::EVIO_MAGIC);
    CHECK_EQ(sro::readBe32(second.data()), static_cast<std::uint32_t>(second.size() / 4));
}

int main() { return petsro::test::runAll(); }
