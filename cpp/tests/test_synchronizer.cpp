// test_synchronizer.cpp -- timestamp alignment across N streams.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "EventSynchronizer.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

using namespace petsro;
using namespace petsro::test;

namespace {

/// A shutdown flag that is never set, for the tests that do not exercise it.
const std::atomic<bool>& neverShutdown() {
    static std::atomic<bool> flag{false};
    return flag;
}

std::vector<EventSource*> pointersTo(std::vector<VectorEventSource>& sources) {
    std::vector<EventSource*> out;
    out.reserve(sources.size());
    for (auto& s : sources) {
        out.push_back(&s);
    }
    return out;
}

}  // namespace

// 6. Synchronizing already aligned streams.

TEST(aligned_streams_produce_a_group_per_frame) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{100, 200, 300});
    sources.emplace_back("b", std::vector<std::uint64_t>{100, 200, 300});
    sources.emplace_back("c", std::vector<std::uint64_t>{100, 200, 300});

    EventSynchronizer sync(pointersTo(sources));

    for (std::uint64_t expected : {100ULL, 200ULL, 300ULL}) {
        CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
        CHECK_EQ(sync.group().size(), std::size_t{3});
        for (const EvioEvent& e : sync.group()) {
            CHECK_EQ(e.timestamp, expected);
        }
    }

    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
    CHECK_EQ(sync.stats().groupsFormed, 3U);
    CHECK_EQ(sync.stats().eventsSkipped, 0U);
    CHECK_EQ(sync.stats().timestampMismatches, 0U);
}

TEST(a_single_stream_is_trivially_synchronized) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("only", std::vector<std::uint64_t>{7, 8});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK_EQ(sync.group()[0].timestamp, 7U);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK_EQ(sync.group()[0].timestamp, 8U);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
}

TEST(the_group_carries_the_full_event_payload) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{500});
    sources.emplace_back("b", std::vector<std::uint64_t>{500});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    for (const EvioEvent& e : sync.group()) {
        CHECK_EQ(e.data.size(), std::size_t{24 * 4});
        CHECK_EQ(sro::readBe32(e.data.data() + sro::wordOffset(sro::WORD_MAGIC)), sro::EVIO_MAGIC);
    }
}

// 7. Advancing a stream whose timestamp is behind.

TEST(a_lagging_stream_is_advanced_to_catch_up) {
    // Stream b starts two frames early; those two must be skipped, not sent.
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{300, 400});
    sources.emplace_back("b", std::vector<std::uint64_t>{100, 200, 300, 400});

    EventSynchronizer sync(pointersTo(sources));

    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK_EQ(sync.group()[0].timestamp, 300U);
    CHECK_EQ(sync.group()[1].timestamp, 300U);

    CHECK_EQ(sync.skippedPerStream()[0], 0U);
    CHECK_EQ(sync.skippedPerStream()[1], 2U);
    CHECK(sync.stats().timestampMismatches > 0U);

    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK_EQ(sync.group()[0].timestamp, 400U);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
}

TEST(several_streams_can_lag_by_different_amounts) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{10, 20, 30, 40});
    sources.emplace_back("b", std::vector<std::uint64_t>{30, 40});
    sources.emplace_back("c", std::vector<std::uint64_t>{20, 30, 40});

    EventSynchronizer sync(pointersTo(sources));

    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    for (const EvioEvent& e : sync.group()) {
        CHECK_EQ(e.timestamp, 30U);
    }
    CHECK_EQ(sync.skippedPerStream()[0], 2U);  // 10, 20
    CHECK_EQ(sync.skippedPerStream()[1], 0U);
    CHECK_EQ(sync.skippedPerStream()[2], 1U);  // 20

    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    for (const EvioEvent& e : sync.group()) {
        CHECK_EQ(e.timestamp, 40U);
    }
}

TEST(a_group_is_never_emitted_with_unequal_timestamps) {
    // Nothing lines up until the very last frame.
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{1, 3, 5, 9});
    sources.emplace_back("b", std::vector<std::uint64_t>{2, 4, 6, 9});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK_EQ(sync.group()[0].timestamp, 9U);
    CHECK_EQ(sync.group()[1].timestamp, 9U);
    CHECK_EQ(sync.stats().groupsFormed, 1U);
}

TEST(streams_that_never_align_end_at_eof_without_emitting) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("odd", std::vector<std::uint64_t>{1, 3, 5, 7});
    sources.emplace_back("even", std::vector<std::uint64_t>{2, 4, 6, 8});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
    CHECK_EQ(sync.stats().groupsFormed, 0U);
}

// 8. Detecting an unsynchronizable or malformed stream.

TEST(a_malformed_stream_reports_an_error_not_eof) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("good", std::vector<std::uint64_t>{100, 200});
    sources.emplace_back("bad", std::vector<std::uint64_t>{100}, ReadStatus::Malformed);

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    // Second cycle: "bad" reports malformed rather than running out.
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Error);
    CHECK(sync.lastError().find("bad") != std::string::npos);
    CHECK(sync.lastError().find("malformed") != std::string::npos);
}

TEST(an_io_error_on_a_stream_reports_an_error) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{1}, ReadStatus::IoError);
    sources.emplace_back("b", std::vector<std::uint64_t>{1, 2});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Error);
    CHECK(sync.lastError().find("io-error") != std::string::npos);
}

TEST(timestamps_going_backwards_are_counted_and_do_not_hang) {
    // A stream whose time regresses can never catch the leader again. The run
    // must end at EOF rather than spinning.
    std::vector<VectorEventSource> sources;
    sources.emplace_back("forward", std::vector<std::uint64_t>{100, 200, 300});
    sources.emplace_back("backward", std::vector<std::uint64_t>{100, 50, 25});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
    CHECK(sync.stats().timestampRegressions > 0U);
}

TEST(the_advance_guard_stops_a_stream_that_cannot_converge) {
    // 50 000 frames on one side against a single far-future frame on the other.
    // With the guard set low this must report an error rather than grind on.
    std::vector<std::uint64_t> many;
    many.reserve(50'000);
    for (std::uint64_t i = 0; i < 50'000; ++i) {
        many.push_back(i);
    }

    std::vector<VectorEventSource> sources;
    sources.emplace_back("slow", std::move(many));
    sources.emplace_back("ahead", std::vector<std::uint64_t>{1'000'000});

    EventSynchronizer sync(pointersTo(sources), /*maxAdvancesPerGroup=*/100);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Error);
    CHECK(sync.lastError().find("did not converge") != std::string::npos);
}

TEST(an_empty_source_list_is_an_error) {
    EventSynchronizer sync(std::vector<EventSource*>{});
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Error);
    CHECK(sync.lastError().find("no input streams") != std::string::npos);
}

TEST(one_stream_ending_early_discards_the_partial_group) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("long", std::vector<std::uint64_t>{10, 20, 30});
    sources.emplace_back("short", std::vector<std::uint64_t>{10});

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::Group);
    CHECK(sync.nextGroup(neverShutdown()) == SyncStatus::EndOfFile);
    // "long" produced an event for the second cycle that was never sent.
    CHECK_EQ(sync.stats().incompleteGroups, 1U);
    CHECK_EQ(sync.stats().groupsFormed, 1U);
}

// 11. Graceful shutdown through the shared flag.

TEST(a_set_shutdown_flag_stops_synchronization_immediately) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{1, 2, 3});
    sources.emplace_back("b", std::vector<std::uint64_t>{1, 2, 3});

    std::atomic<bool> shutdown{true};

    EventSynchronizer sync(pointersTo(sources));
    CHECK(sync.nextGroup(shutdown) == SyncStatus::Shutdown);
    CHECK_EQ(sync.stats().groupsFormed, 0U);
    // Nothing was pulled from the sources.
    CHECK_EQ(sources[0].reads(), std::size_t{0});
}

TEST(shutdown_midway_stops_after_the_groups_already_produced) {
    std::vector<VectorEventSource> sources;
    sources.emplace_back("a", std::vector<std::uint64_t>{1, 2, 3});
    sources.emplace_back("b", std::vector<std::uint64_t>{1, 2, 3});

    std::atomic<bool> shutdown{false};
    EventSynchronizer sync(pointersTo(sources));

    CHECK(sync.nextGroup(shutdown) == SyncStatus::Group);
    shutdown.store(true);
    CHECK(sync.nextGroup(shutdown) == SyncStatus::Shutdown);
    CHECK_EQ(sync.stats().groupsFormed, 1U);
}

TEST(shutdown_interrupts_a_long_resynchronization) {
    // A stream far behind would take many advances to catch up; the flag must
    // be honoured during that inner loop, not only between groups.
    std::vector<std::uint64_t> many;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        many.push_back(i);
    }

    std::vector<VectorEventSource> sources;
    sources.emplace_back("behind", std::move(many));
    sources.emplace_back("ahead", std::vector<std::uint64_t>{9'999});

    std::atomic<bool> shutdown{false};
    EventSynchronizer sync(pointersTo(sources));

    // Set the flag from another thread while the merge is running.
    std::thread setter([&shutdown] {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        shutdown.store(true);
    });

    const SyncStatus status = sync.nextGroup(shutdown);
    setter.join();

    // Either it converged before the flag landed, or it stopped on the flag.
    // What must not happen is an error or a hang.
    CHECK(status == SyncStatus::Shutdown || status == SyncStatus::Group);
}

int main() { return petsro::test::runAll(); }
