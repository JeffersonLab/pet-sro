// test_packetization.cpp -- fragmentation, EJFAT identity, and the replay loop.
//
// The packet accounting mirrors E2SAR's Segmenter::SendThreadState::_send():
// an event of B bytes becomes ceil(B / maxPayloadLen) datagrams, each carrying
// an LB+RE header pair, and the RE header's bufferLength field is the whole
// event length rather than the fragment length. No load balancer is involved:
// the sink is the injectable MockPacketSink.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "PacketSink.hpp"
#include "ReplayLoop.hpp"
#include "SroWireFormat.hpp"
#include "SignalHandler.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace petsro;
using namespace petsro::test;

namespace {

/// MTU 1500 minus IP(20) + UDP(8) + LB(16) + RE(20) = 1436 payload bytes.
constexpr std::size_t MAX_PLD_1500 = 1436;

constexpr std::uint64_t FIRST_TS = 1'000'000ULL;
constexpr std::uint64_t STEP_NS = 1'000'000ULL;

const std::atomic<bool>& neverShutdown() {
    static std::atomic<bool> flag{false};
    return flag;
}

OutgoingEvent makeOutgoing(const std::vector<std::uint8_t>& data, std::uint64_t eventNumber,
                           std::uint16_t dataId) {
    OutgoingEvent e;
    e.data = data.data();
    e.length = data.size();
    e.eventNumber = eventNumber;
    e.dataId = dataId;
    return e;
}

}  // namespace

// 9. Packetizing events smaller than one packet.

TEST(an_event_below_the_mtu_becomes_one_packet) {
    MockPacketSink sink(MAX_PLD_1500);

    // A 24-word block is 96 bytes: comfortably inside one datagram.
    const auto words = makeBlockWords(24, FIRST_TS, 0, 1);
    const auto data = encodeWireDumpBlock(words);
    CHECK_EQ(data.size(), std::size_t{96});

    const SendOutcome outcome = sink.send(makeOutgoing(data, 1, 7));
    CHECK(outcome.ok);
    CHECK_EQ(outcome.packets, 1U);
    CHECK_EQ(sink.sent().size(), std::size_t{1});
    CHECK_EQ(sink.sent()[0].dataId, 7U);
    CHECK_EQ(sink.sent()[0].length, data.size());
}

TEST(an_event_exactly_one_payload_long_is_still_one_packet) {
    MockPacketSink sink(MAX_PLD_1500);
    const std::vector<std::uint8_t> data(MAX_PLD_1500, 0xAB);
    CHECK_EQ(sink.send(makeOutgoing(data, 1, 1)).packets, 1U);
}

TEST(the_payload_reaches_the_sink_byte_for_byte) {
    MockPacketSink sink(MAX_PLD_1500);
    const auto words = makeBlockWords(40, 0x1234'5678'9ABCULL, 11, 500);
    const auto data = encodeWireDumpBlock(words);

    CHECK(sink.send(makeOutgoing(data, 1, 1)).ok);
    // The EVIO bytes are the EJFAT payload, unmodified.
    CHECK(sink.sent()[0].payload == data);
}

// 10. Packetizing events requiring multiple packets.

TEST(an_event_one_byte_over_the_payload_becomes_two_packets) {
    MockPacketSink sink(MAX_PLD_1500);
    const std::vector<std::uint8_t> data(MAX_PLD_1500 + 1, 0xCD);
    CHECK_EQ(sink.send(makeOutgoing(data, 1, 1)).packets, 2U);
}

TEST(a_large_event_fragments_by_ceiling_division) {
    MockPacketSink sink(MAX_PLD_1500);

    struct Case {
        std::size_t bytes;
        std::uint64_t packets;
    };
    const Case cases[] = {
        {1, 1},
        {MAX_PLD_1500 - 1, 1},
        {MAX_PLD_1500, 1},
        {MAX_PLD_1500 + 1, 2},
        {MAX_PLD_1500 * 2, 2},
        {MAX_PLD_1500 * 2 + 1, 3},
        {MAX_PLD_1500 * 10, 10},
        {1024 * 1024, (1024 * 1024 + MAX_PLD_1500 - 1) / MAX_PLD_1500},
    };

    for (const Case& c : cases) {
        CHECK_EQ(sink.packetsFor(c.bytes), c.packets);
    }
}

TEST(a_jumbo_mtu_reduces_the_packet_count) {
    // MTU 9000 leaves 8936 payload bytes.
    MockPacketSink jumbo(9000 - 64);
    MockPacketSink standard(MAX_PLD_1500);

    const std::size_t bytes = 100'000;
    CHECK(jumbo.packetsFor(bytes) < standard.packetsFor(bytes));
    CHECK_EQ(jumbo.packetsFor(bytes), (bytes + 8936 - 1) / 8936);
}

TEST(a_multi_packet_evio_event_keeps_its_whole_payload) {
    MockPacketSink sink(MAX_PLD_1500);

    // 2048 words = 8192 bytes: six datagrams at MTU 1500.
    const auto words = makeBlockWords(2048, FIRST_TS, 0, 3);
    const auto data = encodeWireDumpBlock(words);
    CHECK_EQ(data.size(), std::size_t{8192});

    const SendOutcome outcome = sink.send(makeOutgoing(data, 42, 9));
    CHECK(outcome.ok);
    CHECK_EQ(outcome.packets, 6U);
    // Fragmentation is a transport concern; the sink still sees one event.
    CHECK_EQ(sink.sent().size(), std::size_t{1});
    CHECK(sink.sent()[0].payload == data);
    CHECK_EQ(sink.sent()[0].eventNumber, 42U);
}

TEST(the_null_sink_accounts_for_packets_the_same_way) {
    NullPacketSink sink(MAX_PLD_1500);
    CHECK_EQ(sink.send(makeOutgoing(std::vector<std::uint8_t>(5000, 0), 1, 1)).packets, 4U);
    CHECK(sink.destination().find("dry-run") != std::string::npos);
}

// --- the replay loop: identity, numbering, restart, shutdown -------------

namespace {

/// Builds `count` capture files whose frames are aligned across all of them.
std::vector<std::unique_ptr<TempFile>> makeAlignedCaptures(std::size_t count,
                                                           std::uint32_t frames,
                                                           std::uint32_t blockWords = 24) {
    std::vector<std::unique_ptr<TempFile>> files;
    for (std::size_t i = 0; i < count; ++i) {
        auto f = std::make_unique<TempFile>("test_loop_stream" + std::to_string(i) + ".evio");
        f->write(makeWireDumpCapture(frames, FIRST_TS, STEP_NS,
                                     static_cast<std::uint32_t>(100 + i), blockWords));
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

TEST(the_loop_sends_one_event_per_stream_per_group) {
    const auto files = makeAlignedCaptures(3, 4);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    CHECK_EQ(loop.stats().loopsCompleted, 1U);
    CHECK_EQ(loop.stats().groupsSent, 4U);
    CHECK_EQ(loop.stats().eventsSent, 12U);   // 4 groups x 3 streams
    CHECK_EQ(loop.stats().packetsSent, 12U);  // 96-byte events, one packet each
    CHECK_EQ(loop.stats().payloadBytesSent, 12U * 96U);
    CHECK_EQ(sink.sent().size(), std::size_t{12});
}

TEST(data_ids_map_deterministically_from_the_file_index) {
    const auto files = makeAlignedCaptures(3, 2);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.dataIdBase = 10;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    // Events are emitted in stream order within each group, so dataId cycles
    // 10, 11, 12, 10, 11, 12.
    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{6});
    for (std::size_t i = 0; i < sent.size(); ++i) {
        CHECK_EQ(sent[i].dataId, static_cast<std::uint16_t>(10 + (i % 3)));
    }
}

TEST(event_numbers_are_monotonic_and_start_at_one) {
    const auto files = makeAlignedCaptures(2, 3);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 2;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{12});  // 2 loops x 3 groups x 2 streams
    for (std::size_t i = 0; i < sent.size(); ++i) {
        // E2SAR treats event number 0 as "do not override", so numbering must
        // begin at 1 and never repeat across replay loops.
        CHECK_EQ(sent[i].eventNumber, static_cast<std::uint64_t>(i + 1));
    }
}

TEST(event_numbers_restart_when_configured_to) {
    const auto files = makeAlignedCaptures(2, 2);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 2;
    config.resetEventNumbers = true;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{8});
    CHECK_EQ(sent[0].eventNumber, 1U);
    CHECK_EQ(sent[3].eventNumber, 4U);
    CHECK_EQ(sent[4].eventNumber, 1U);  // second loop restarts
    CHECK_EQ(sent[7].eventNumber, 4U);
}

TEST(entropy_is_per_source_when_requested) {
    const auto files = makeAlignedCaptures(3, 1);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.entropyPerSource = true;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    CHECK_EQ(sink.sent()[0].entropy, 1U);
    CHECK_EQ(sink.sent()[1].entropy, 2U);
    CHECK_EQ(sink.sent()[2].entropy, 3U);
}

TEST(entropy_is_left_to_e2sar_by_default) {
    const auto files = makeAlignedCaptures(2, 1);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    // 0 is E2SAR's "generate a random value for this event".
    CHECK_EQ(sink.sent()[0].entropy, 0U);
    CHECK_EQ(sink.sent()[1].entropy, 0U);
}

TEST(the_loop_reopens_every_file_after_eof) {
    const auto files = makeAlignedCaptures(2, 3);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 3;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    CHECK_EQ(loop.stats().loopsCompleted, 3U);
    CHECK_EQ(loop.stats().groupsSent, 9U);

    // Each pass replays the same three frames, in the same order. Rebasing
    // rewrites words 13-15 on every loop, so the frames recur identically
    // apart from those three words -- test_rebaser covers the rewritten
    // values themselves.
    const auto& sent = sink.sent();
    CHECK_EQ(sent.size(), std::size_t{18});

    const auto sameApartFromRebasedWords = [](const std::vector<std::uint8_t>& a,
                                              const std::vector<std::uint8_t>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t w = 0; w * 4 < a.size(); ++w) {
            if (w == sro::WORD_FRAME_COUNTER || w == sro::WORD_TIMESTAMP_LO ||
                w == sro::WORD_TIMESTAMP_HI) {
                continue;
            }
            if (sro::readBe32(a.data() + w * 4) != sro::readBe32(b.data() + w * 4)) {
                return false;
            }
        }
        return true;
    };

    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(sameApartFromRebasedWords(sent[i * 2].payload, sent[(i + 3) * 2].payload));
        CHECK(sameApartFromRebasedWords(sent[i * 2].payload, sent[(i + 6) * 2].payload));
    }
}

TEST(events_larger_than_the_mtu_are_reported_as_several_packets_by_the_loop) {
    // 512-word blocks are 2048 bytes: two datagrams each at MTU 1500.
    const auto files = makeAlignedCaptures(2, 3, 512);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));

    CHECK_EQ(loop.stats().eventsSent, 6U);
    CHECK_EQ(loop.stats().packetsSent, 12U);
    CHECK_EQ(loop.stats().payloadBytesSent, 6U * 2048U);
}

TEST(a_send_failure_stops_the_run_and_is_reported) {
    const auto files = makeAlignedCaptures(2, 5);
    MockPacketSink sink(MAX_PLD_1500);
    sink.failAfter(3);  // the fourth send fails

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK_FALSE(loop.run(neverShutdown()));

    CHECK_EQ(loop.stats().eventsSent, 3U);
    CHECK_EQ(loop.stats().sendErrors, 1U);
    CHECK(loop.lastError().find("injected send failure") != std::string::npos);
}

TEST(a_missing_input_file_stops_the_run_with_a_useful_message) {
    std::vector<std::unique_ptr<EvioFileReader>> readers;
    readers.push_back(std::make_unique<EvioFileReader>("test_no_such_capture.evio"));

    MockPacketSink sink(MAX_PLD_1500);
    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(std::move(readers), sink, config);
    CHECK_FALSE(loop.run(neverShutdown()));
    CHECK(loop.lastError().find("test_no_such_capture.evio") != std::string::npos);
}

// 11. Graceful shutdown through the shared shutdown flag.

TEST(the_loop_stops_when_the_shutdown_flag_is_already_set) {
    const auto files = makeAlignedCaptures(2, 100);
    MockPacketSink sink(MAX_PLD_1500);

    std::atomic<bool> shutdown{true};

    ReplayLoopConfig config;
    config.statsIntervalSeconds = 0.0;  // loopLimit 0: would run forever otherwise

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(shutdown));

    CHECK_EQ(loop.stats().eventsSent, 0U);
    CHECK_EQ(loop.stats().loopsCompleted, 0U);
    // Shutdown is clean, not an error.
    CHECK(loop.lastError().empty());
}

TEST(an_infinite_replay_stops_promptly_on_the_shutdown_flag) {
    const auto files = makeAlignedCaptures(2, 50);
    MockPacketSink sink(MAX_PLD_1500);

    std::atomic<bool> shutdown{false};

    ReplayLoopConfig config;
    config.loopLimit = 0;  // forever, until the flag is set
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);

    std::thread stopper([&shutdown] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        shutdown.store(true);
    });

    const bool ok = loop.run(shutdown);
    stopper.join();

    CHECK(ok);
    CHECK(loop.lastError().empty());
    // It did real work before stopping, and it stopped.
    CHECK(loop.stats().eventsSent > 0U);
    CHECK(shutdown.load());
}

TEST(the_sink_is_drained_on_shutdown) {
    const auto files = makeAlignedCaptures(1, 2);
    MockPacketSink sink(MAX_PLD_1500);

    ReplayLoopConfig config;
    config.loopLimit = 1;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(readersFor(files), sink, config);
    CHECK(loop.run(neverShutdown()));
    CHECK_EQ(sink.drains(), std::size_t{1});
}

TEST(the_global_shutdown_flag_round_trips) {
    resetShutdownForTesting();
    CHECK_FALSE(shutdownRequested());

    requestShutdown();
    CHECK(shutdownRequested());
    CHECK(shutdownFlag().load());

    resetShutdownForTesting();
    CHECK_FALSE(shutdownRequested());
}

int main() { return petsro::test::runAll(); }
