// test_event_view.cpp -- validation of reassembled EVIO events.
//
// This is what the receiver uses to decide whether what came off the network is
// still an EVIO event, so the interesting cases are the damaged ones.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "EvioEventView.hpp"
#include "SroWireFormat.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace petsro;
using namespace petsro::test;

TEST(a_well_formed_event_decodes) {
    const auto words = makeBlockWords(24, 0x0000'00AB'1234'5678ULL, 77, 512);
    const auto data = encodeWireDumpBlock(words);

    const EvioEventView view = inspectEvioEvent(data.data(), data.size());
    CHECK(view.valid);
    CHECK(view.problem.empty());
    CHECK_EQ(view.blockWords, 24U);
    CHECK_EQ(view.timestamp, 0x0000'00AB'1234'5678ULL);
    CHECK_EQ(view.frameCounter, 77U);
    CHECK_EQ(view.rocid, 512U);
}

TEST(a_null_buffer_is_reported_not_dereferenced) {
    const EvioEventView view = inspectEvioEvent(nullptr, 96);
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("null") != std::string::npos);
}

TEST(a_zero_length_event_is_rejected) {
    const std::uint8_t byte = 0;
    const EvioEventView view = inspectEvioEvent(&byte, 0);
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("shorter") != std::string::npos);
}

TEST(an_event_too_short_for_a_timestamp_is_rejected) {
    // 15 words: word 15, the timestamp high word, is missing.
    const auto words = makeBlockWords(24, 1000, 0, 1);
    const auto data = encodeWireDumpBlock(words);

    const EvioEventView view = inspectEvioEvent(data.data(), 15 * 4);
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("shorter") != std::string::npos);
}

TEST(a_bad_magic_word_is_rejected) {
    auto words = makeBlockWords(24, 1000, 0, 1);
    words[sro::WORD_MAGIC] = 0xDEADBEEFU;
    const auto data = encodeWireDumpBlock(words);

    const EvioEventView view = inspectEvioEvent(data.data(), data.size());
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("bad EVIO magic") != std::string::npos);
}

TEST(a_truncated_delivery_is_caught_by_the_length_word) {
    // The one check the file reader never needs and the receiver always does:
    // a short reassembly still looks like a valid header.
    const auto words = makeBlockWords(24, 1000, 0, 1);
    const auto data = encodeWireDumpBlock(words);

    const EvioEventView view = inspectEvioEvent(data.data(), data.size() - 4);
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("declares 24 words") != std::string::npos);
    CHECK(view.problem.find("92 bytes were delivered") != std::string::npos);
}

TEST(an_over_long_delivery_is_also_caught) {
    const auto words = makeBlockWords(24, 1000, 0, 1);
    auto data = encodeWireDumpBlock(words);
    data.resize(data.size() + 8, 0xFF);

    const EvioEventView view = inspectEvioEvent(data.data(), data.size());
    CHECK_FALSE(view.valid);
    CHECK(view.problem.find("104 bytes were delivered") != std::string::npos);
}

TEST(a_large_event_decodes_the_same_way) {
    const auto words = makeBlockWords(2048, 999'000'000ULL, 12345, 7);
    const auto data = encodeWireDumpBlock(words);
    CHECK_EQ(data.size(), std::size_t{8192});

    const EvioEventView view = inspectEvioEvent(data.data(), data.size());
    CHECK(view.valid);
    CHECK_EQ(view.blockWords, 2048U);
    CHECK_EQ(view.timestamp, 999'000'000ULL);
    CHECK_EQ(view.frameCounter, 12345U);
}

TEST(the_view_agrees_with_the_reader_on_the_same_bytes) {
    // Round trip: what EvioFileReader produced is what the receiver decodes.
    TempFile file("test_view_roundtrip.evio");
    file.write(makeWireDumpCapture(3, 5'000'000ULL, sro::FRAME_PERIOD_NS, 33));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    for (int i = 0; i < 3; ++i) {
        CHECK(reader.next(event) == ReadStatus::Ok);
        const EvioEventView view = inspectEvioEvent(event.data.data(), event.data.size());
        CHECK(view.valid);
        CHECK_EQ(view.timestamp, event.timestamp);
        CHECK_EQ(view.frameCounter, event.frameCounter);
        CHECK_EQ(view.rocid, event.rocid);
    }
}

int main() { return petsro::test::runAll(); }
