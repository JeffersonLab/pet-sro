// test_real_captures.cpp -- checks the reader against genuine FEB captures.
//
// Registered by CMake only when the capture files are present, because they
// are large and are not tracked in git. Capture paths arrive in argv.
//
// The expectations here were established by scanning the files with an
// independent decoder written from SroWireFormat, not by recording whatever
// this reader happened to produce.

#include "TestHarness.hpp"

#include "EvioFileReader.hpp"
#include "EventSynchronizer.hpp"
#include "PacketSink.hpp"
#include "ReplayLoop.hpp"
#include "SroWireFormat.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace petsro;

namespace {

std::vector<std::string> g_captures;

const std::atomic<bool>& neverShutdown() {
    static std::atomic<bool> flag{false};
    return flag;
}

/// Everything one pass over a capture reveals, gathered in a single read.
struct Summary {
    std::uint64_t events = 0;
    std::uint64_t bytes = 0;
    std::uint64_t firstTs = 0;
    std::uint64_t lastTs = 0;
    std::uint64_t rocid = 0;
    bool rocidStable = true;
    bool tsStepsAreOneFrame = true;
    bool frameCounterIsDense = true;
    ReadStatus terminal = ReadStatus::Ok;
};

Summary summarize(EvioFileReader& reader) {
    Summary s;
    EvioEvent event;
    bool first = true;
    std::uint64_t prevTs = 0;
    std::uint64_t prevFc = 0;

    for (;;) {
        const ReadStatus st = reader.next(event);
        if (st != ReadStatus::Ok) {
            s.terminal = st;
            break;
        }
        if (first) {
            s.firstTs = event.timestamp;
            s.rocid = event.rocid;
            first = false;
        } else {
            if (event.timestamp - prevTs != sro::FRAME_PERIOD_NS) {
                s.tsStepsAreOneFrame = false;
            }
            if (event.frameCounter - prevFc != 1) {
                s.frameCounterIsDense = false;
            }
        }
        if (event.rocid != s.rocid) {
            s.rocidStable = false;
        }
        prevTs = event.timestamp;
        prevFc = event.frameCounter;
        s.lastTs = event.timestamp;
        s.events++;
        s.bytes += event.data.size();
    }
    return s;
}

/// Decodes each outgoing event's timestamp straight from its bytes and tracks
/// monotonicity, without retaining any payload -- these captures are 44 MB and
/// a recording sink would hold every replayed copy.
class MonotonicCheckSink final : public PacketSink {
  public:
    SendOutcome send(const OutgoingEvent& event) override {
        SendOutcome outcome;
        outcome.ok = true;
        outcome.packets = packetsFor(event.length);

        if (event.length < sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS)) {
            ++shortEvents;
            return outcome;
        }
        const std::uint64_t ts = sro::combineTimestamp(
            sro::readBe32(event.data + sro::wordOffset(sro::WORD_TIMESTAMP_LO)),
            sro::readBe32(event.data + sro::wordOffset(sro::WORD_TIMESTAMP_HI)));

        if (seen) {
            if (ts < lastTimestamp) {
                ++regressions;
            } else if (ts > lastTimestamp) {
                // A new group. Record the step so seams can be distinguished
                // from ordinary inter-frame gaps.
                const std::uint64_t step = ts - lastTimestamp;
                if (step != sro::FRAME_PERIOD_NS) {
                    ++irregularSteps;
                }
                ++groupTransitions;
            }
        }
        lastTimestamp = ts;
        seen = true;
        ++events;
        return outcome;
    }

    std::size_t maxPayloadLength() const noexcept override { return 1436; }
    std::string destination() const override { return "monotonic-check"; }

    std::uint64_t events = 0;
    std::uint64_t regressions = 0;
    std::uint64_t irregularSteps = 0;
    std::uint64_t groupTransitions = 0;
    std::uint64_t shortEvents = 0;
    std::uint64_t lastTimestamp = 0;
    bool seen = false;
};

}  // namespace

TEST(real_captures_are_recognised_as_big_endian_wire_dumps) {
    for (const std::string& path : g_captures) {
        EvioFileReader reader(path);
        CHECK(reader.open());
        CHECK(reader.format() == EvioFormat::WireDump);
    }
}

TEST(real_captures_read_to_a_clean_end_of_file) {
    for (const std::string& path : g_captures) {
        EvioFileReader reader(path);
        CHECK(reader.open());

        const Summary s = summarize(reader);

        // Every block parsed: no malformed record, no I/O error, and no
        // partial block left at the tail.
        CHECK(s.terminal == ReadStatus::EndOfFile);
        CHECK_EQ(reader.stats().readErrors, 0U);
        CHECK_EQ(reader.stats().truncatedTails, 0U);
        CHECK(s.events > 100000U);
    }
}

TEST(real_captures_consume_every_byte_of_the_file) {
    // The blocks tile the file exactly; nothing is skipped or double-counted.
    for (const std::string& path : g_captures) {
        std::ifstream probe(path, std::ios::binary | std::ios::ate);
        CHECK(probe.is_open());
        const std::uint64_t fileSize = static_cast<std::uint64_t>(probe.tellg());

        EvioFileReader reader(path);
        CHECK(reader.open());
        const Summary s = summarize(reader);

        CHECK_EQ(s.bytes, fileSize);
    }
}

TEST(real_capture_timestamps_start_at_zero_and_step_one_frame_period) {
    for (const std::string& path : g_captures) {
        EvioFileReader reader(path);
        CHECK(reader.open());
        const Summary s = summarize(reader);

        // Both timestamp and frame counter start at 0 when a run's stream is
        // enabled, and advance by exactly one frame period per frame.
        CHECK_EQ(s.firstTs, 0U);
        CHECK(s.tsStepsAreOneFrame);
        CHECK(s.frameCounterIsDense);
        CHECK_EQ(s.lastTs, (s.events - 1) * sro::FRAME_PERIOD_NS);
    }
}

TEST(each_real_capture_carries_one_stable_rocid) {
    std::vector<std::uint64_t> rocids;
    for (const std::string& path : g_captures) {
        EvioFileReader reader(path);
        CHECK(reader.open());
        const Summary s = summarize(reader);
        CHECK(s.rocidStable);
        rocids.push_back(s.rocid);
    }
    // Distinct captures come from distinct read-out controllers.
    for (std::size_t i = 1; i < rocids.size(); ++i) {
        CHECK(rocids[i] != rocids[0]);
    }
}

TEST(reopening_a_real_capture_reproduces_the_first_pass_exactly) {
    for (const std::string& path : g_captures) {
        EvioFileReader reader(path);

        CHECK(reader.open());
        const Summary first = summarize(reader);

        CHECK(reader.reopen());
        const Summary second = summarize(reader);

        CHECK_EQ(second.events, first.events);
        CHECK_EQ(second.bytes, first.bytes);
        CHECK_EQ(second.firstTs, first.firstTs);
        CHECK_EQ(second.lastTs, first.lastTs);
    }
}

TEST(real_captures_synchronize_frame_for_frame_without_skipping) {
    if (g_captures.size() < 2) {
        return;  // needs at least two streams to synchronize
    }

    std::vector<std::unique_ptr<EvioFileReader>> readers;
    std::vector<EventSource*> sources;
    for (const std::string& path : g_captures) {
        readers.push_back(std::make_unique<EvioFileReader>(path));
        CHECK(readers.back()->open());
        sources.push_back(readers.back().get());
    }

    EventSynchronizer sync(sources);

    std::uint64_t groups = 0;
    std::uint64_t expectedTs = 0;
    for (;;) {
        const SyncStatus st = sync.nextGroup(neverShutdown());
        if (st == SyncStatus::EndOfFile) {
            break;
        }
        CHECK(st == SyncStatus::Group);

        // All streams agree, and the group advances one frame period at a time.
        for (const EvioEvent& e : sync.group()) {
            CHECK_EQ(e.timestamp, expectedTs);
        }
        expectedTs += sro::FRAME_PERIOD_NS;
        ++groups;
    }

    CHECK(groups > 100000U);
    // These captures are aligned, so the merge should never have to skip.
    CHECK_EQ(sync.stats().eventsSkipped, 0U);
    CHECK_EQ(sync.stats().timestampMismatches, 0U);
    CHECK_EQ(sync.stats().timestampRegressions, 0U);

    // The number of groups is set by the SHORTEST capture: the pass ends when
    // the first stream runs out, and any partial group is discarded.
    std::uint64_t shortest = UINT64_MAX;
    for (const auto& r : readers) {
        EvioFileReader counter(r->path());
        CHECK(counter.open());
        const Summary s = summarize(counter);
        shortest = std::min(shortest, s.events);
    }
    CHECK_EQ(groups, shortest);
}

TEST(rebasing_keeps_real_replayed_time_moving_forward_across_loops) {
    std::vector<std::unique_ptr<EvioFileReader>> readers;
    for (const std::string& path : g_captures) {
        readers.push_back(std::make_unique<EvioFileReader>(path));
    }

    MonotonicCheckSink sink;
    ReplayLoopConfig config;
    config.loopLimit = 3;  // two seams to cross
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(std::move(readers), sink, config);
    CHECK(loop.run(neverShutdown()));
    CHECK_EQ(loop.stats().loopsCompleted, 3U);

    CHECK(sink.events > 0U);
    CHECK_EQ(sink.shortEvents, 0U);

    // The whole point: over three passes of a finite capture, time never goes
    // backwards, and every step -- including the two loop seams -- is exactly
    // one frame period, so a seam is indistinguishable from any other gap.
    CHECK_EQ(sink.regressions, 0U);
    CHECK_EQ(sink.irregularSteps, 0U);
    CHECK_EQ(sink.groupTransitions, (loop.stats().groupsSent - 1));
}

TEST(without_rebasing_real_replayed_time_jumps_backwards_at_each_seam) {
    // The control for the test above: verbatim replay really does regress, so
    // the check there is measuring something.
    std::vector<std::unique_ptr<EvioFileReader>> readers;
    for (const std::string& path : g_captures) {
        readers.push_back(std::make_unique<EvioFileReader>(path));
    }

    MonotonicCheckSink sink;
    ReplayLoopConfig config;
    config.loopLimit = 3;
    config.rebaseTimestamps = false;
    config.statsIntervalSeconds = 0.0;

    ReplayLoop loop(std::move(readers), sink, config);
    CHECK(loop.run(neverShutdown()));

    // One regression per seam, and the captures have two seams in three loops.
    CHECK_EQ(sink.regressions, 2U);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        g_captures.emplace_back(argv[i]);
    }
    if (g_captures.empty()) {
        std::cout << "no capture files given; nothing to check\n";
        return 0;
    }
    std::cout << "checking " << g_captures.size() << " real capture(s)\n";
    return petsro::test::runAll();
}
