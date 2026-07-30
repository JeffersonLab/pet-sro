// test_evio_reader.cpp -- opening, reading, timestamps, EOF, reopen.

#include "EvioFixtures.hpp"
#include "TestHarness.hpp"

#include "EvioFileReader.hpp"
#include "SroWireFormat.hpp"

using namespace petsro;
using namespace petsro::test;

namespace {

constexpr std::uint64_t FIRST_TS = 1'000'000'000ULL;
constexpr std::uint64_t STEP_NS = sro::FRAME_PERIOD_NS;

}  // namespace

// 1. Opening a valid EVIO file (both accepted formats).

TEST(open_valid_wire_dump_detects_big_endian) {
    TempFile file("test_open_wire.evio");
    file.write(makeWireDumpCapture(3, FIRST_TS, STEP_NS, 42));

    EvioFileReader reader(file.path());
    CHECK(reader.open());
    CHECK(reader.format() == EvioFormat::WireDump);
    CHECK(reader.isOpen());
    CHECK_EQ(reader.stats().opens, 1U);
}

TEST(open_valid_feb_stream_dump_detects_little_endian) {
    TempFile file("test_open_feb.evio");
    file.write(makeFebStreamCapture(3, FIRST_TS, STEP_NS, 42));

    EvioFileReader reader(file.path());
    CHECK(reader.open());
    CHECK(reader.format() == EvioFormat::FebStreamDump);
}

TEST(open_rejects_a_file_without_evio_magic) {
    TempFile file("test_open_garbage.evio");
    file.write(std::vector<std::uint8_t>(64, 0x5A));

    EvioFileReader reader(file.path());
    CHECK_FALSE(reader.open());
    CHECK(reader.lastError().find("no EVIO magic") != std::string::npos);
    CHECK_FALSE(reader.isOpen());
}

TEST(open_rejects_a_file_too_small_to_probe) {
    TempFile file("test_open_tiny.evio");
    file.write(std::vector<std::uint8_t>(8, 0x00));

    EvioFileReader reader(file.path());
    CHECK_FALSE(reader.open());
    CHECK(reader.lastError().find("too small") != std::string::npos);
}

TEST(open_reports_a_missing_file) {
    EvioFileReader reader("test_this_file_does_not_exist.evio");
    CHECK_FALSE(reader.open());
    CHECK(reader.lastError().find("cannot open") != std::string::npos);
}

// 2. Reading one complete EVIO event, and 3. extracting a known timestamp.

TEST(reads_one_complete_event_with_the_expected_bytes) {
    const std::uint32_t blockWords = 24;
    TempFile file("test_read_one.evio");
    const auto bytes = makeWireDumpCapture(1, FIRST_TS, STEP_NS, 7, blockWords);
    file.write(bytes);

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);

    // The whole block, byte for byte, payload unaltered.
    CHECK_EQ(event.data.size(), static_cast<std::size_t>(blockWords) * 4U);
    CHECK(event.data == bytes);

    // The block-length word survives and still counts itself.
    CHECK_EQ(sro::readBe32(event.data.data()), blockWords);
    CHECK_EQ(sro::readBe32(event.data.data() + sro::wordOffset(sro::WORD_MAGIC)), sro::EVIO_MAGIC);
}

TEST(extracts_the_known_timestamp_from_words_14_and_15) {
    // A timestamp that occupies both words, so a low/high mix-up cannot pass.
    const std::uint64_t ts = 0x0000'00AB'1234'5678ULL;

    TempFile file("test_read_ts.evio");
    file.write(makeWireDumpCapture(1, ts, 0, 9));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK_EQ(event.timestamp, ts);
    CHECK_EQ(event.rocid, 9U);
    CHECK_EQ(event.frameCounter, 0U);
}

TEST(feb_stream_dump_yields_identical_events_to_the_wire_dump) {
    // The two on-disk formats hold the same frames; after normalisation the
    // reader must not be able to tell them apart.
    TempFile wire("test_equiv_wire.evio");
    TempFile feb("test_equiv_feb.evio");
    wire.write(makeWireDumpCapture(4, FIRST_TS, STEP_NS, 13));
    feb.write(makeFebStreamCapture(4, FIRST_TS, STEP_NS, 13));

    EvioFileReader wireReader(wire.path());
    EvioFileReader febReader(feb.path());
    CHECK(wireReader.open());
    CHECK(febReader.open());
    CHECK(wireReader.format() == EvioFormat::WireDump);
    CHECK(febReader.format() == EvioFormat::FebStreamDump);

    for (int i = 0; i < 4; ++i) {
        EvioEvent a;
        EvioEvent b;
        CHECK(wireReader.next(a) == ReadStatus::Ok);
        CHECK(febReader.next(b) == ReadStatus::Ok);
        CHECK_EQ(a.timestamp, b.timestamp);
        CHECK_EQ(a.rocid, b.rocid);
        CHECK_EQ(a.frameCounter, b.frameCounter);
        CHECK(a.data == b.data);
    }
}

TEST(timestamps_advance_by_one_frame_period) {
    TempFile file("test_read_seq.evio");
    file.write(makeWireDumpCapture(5, FIRST_TS, STEP_NS, 1));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(reader.next(event) == ReadStatus::Ok);
        CHECK_EQ(event.timestamp, FIRST_TS + i * STEP_NS);
        CHECK_EQ(event.frameCounter, i);
    }
}

// 4. Detecting EOF, and distinguishing it from malformed input.

TEST(detects_clean_end_of_file) {
    TempFile file("test_eof.evio");
    file.write(makeWireDumpCapture(2, FIRST_TS, STEP_NS, 1));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::EndOfFile);
    // EOF is not an error and must not be counted as one.
    CHECK_EQ(reader.stats().readErrors, 0U);
    CHECK_EQ(reader.stats().eventsRead, 2U);
}

TEST(a_truncated_final_block_ends_the_file_rather_than_failing) {
    // ReplayFrameSource logs and stops the scan for a partial trailing block;
    // a capture cut short by a killed writer is common and not corruption.
    TempFile file("test_truncated.evio");
    auto bytes = makeWireDumpCapture(2, FIRST_TS, STEP_NS, 1);
    bytes.resize(bytes.size() - 20);
    file.write(bytes);

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::EndOfFile);
    CHECK_EQ(reader.stats().truncatedTails, 1U);
    CHECK_EQ(reader.stats().readErrors, 0U);
}

TEST(an_implausible_block_length_is_malformed_not_eof) {
    TempFile file("test_badlen.evio");
    auto bytes = makeWireDumpCapture(2, FIRST_TS, STEP_NS, 1);
    const std::size_t blockBytes = bytes.size() / 2;
    sro::writeBe32(bytes.data() + blockBytes, 999'999'999U);  // second block's length
    file.write(bytes);

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::Malformed);
    CHECK_EQ(reader.stats().readErrors, 1U);
    CHECK(reader.lastError().find("implausible block length") != std::string::npos);
}

TEST(a_bad_magic_word_mid_file_is_malformed) {
    TempFile file("test_badmagic.evio");
    auto bytes = makeWireDumpCapture(2, FIRST_TS, STEP_NS, 1);
    const std::size_t blockBytes = bytes.size() / 2;
    sro::writeBe32(bytes.data() + blockBytes + sro::wordOffset(sro::WORD_MAGIC), 0xDEADBEEFU);
    file.write(bytes);

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::Malformed);
    CHECK(reader.lastError().find("bad EVIO magic") != std::string::npos);
}

TEST(a_block_too_short_to_hold_a_timestamp_is_malformed) {
    // Word 0 says 10 words: a legal EVIO block header, but words 14 and 15 do
    // not exist, so the timestamp read must be refused rather than run off the
    // end of the buffer.
    std::vector<std::uint32_t> words(10, 0);
    words[sro::WORD_BLOCK_LENGTH] = 10;
    words[sro::WORD_MAGIC] = sro::EVIO_MAGIC;

    // The probe needs 32 bytes, so pad the file with a valid block behind it.
    auto bytes = encodeWireDumpBlock(words);
    const auto tail = makeWireDumpCapture(1, FIRST_TS, STEP_NS, 1);
    bytes.insert(bytes.end(), tail.begin(), tail.end());

    TempFile file("test_shortblock.evio");
    file.write(bytes);

    EvioFileReader reader(file.path());
    // Byte 28 falls inside the short block's word 7 region only for a 24-word
    // block; for this 10-word one the probe sees word 7 of the first block.
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Malformed);
    CHECK(reader.lastError().find("timestamp") != std::string::npos);
}

TEST(reading_from_a_closed_reader_is_an_io_error) {
    TempFile file("test_closed.evio");
    file.write(makeWireDumpCapture(1, FIRST_TS, STEP_NS, 1));

    EvioFileReader reader(file.path());
    CHECK(reader.open());
    reader.close();

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::IoError);
}

// 5. Reopening a file and reading again from the beginning.

TEST(reopen_restarts_from_the_first_event) {
    TempFile file("test_reopen.evio");
    file.write(makeWireDumpCapture(3, FIRST_TS, STEP_NS, 5));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK(reader.next(event) == ReadStatus::EndOfFile);

    CHECK(reader.reopen());
    CHECK_EQ(reader.stats().opens, 2U);

    // Same first event as the first pass.
    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK_EQ(event.timestamp, FIRST_TS);
    CHECK_EQ(event.frameCounter, 0U);

    // Read counters accumulate across opens; they are lifetime totals.
    CHECK_EQ(reader.stats().eventsRead, 4U);
}

TEST(reopen_works_repeatedly) {
    TempFile file("test_reopen_many.evio");
    file.write(makeWireDumpCapture(2, FIRST_TS, STEP_NS, 5));

    EvioFileReader reader(file.path());
    EvioEvent event;

    for (int loop = 0; loop < 4; ++loop) {
        CHECK(reader.open());
        CHECK(reader.next(event) == ReadStatus::Ok);
        CHECK_EQ(event.timestamp, FIRST_TS);
        CHECK(reader.next(event) == ReadStatus::Ok);
        CHECK(reader.next(event) == ReadStatus::EndOfFile);
        reader.close();
    }
    CHECK_EQ(reader.stats().opens, 4U);
    CHECK_EQ(reader.stats().eventsRead, 8U);
}

TEST(event_buffers_are_reused_across_reads) {
    // The reader resizes rather than reallocates, so a same-sized event should
    // keep the previous buffer. Not a correctness requirement, but it is the
    // documented behaviour that keeps the replay loop allocation-free.
    TempFile file("test_reuse.evio");
    file.write(makeWireDumpCapture(3, FIRST_TS, STEP_NS, 1));

    EvioFileReader reader(file.path());
    CHECK(reader.open());

    EvioEvent event;
    CHECK(reader.next(event) == ReadStatus::Ok);
    const std::uint8_t* firstBuffer = event.data.data();
    const std::size_t firstCapacity = event.data.capacity();

    CHECK(reader.next(event) == ReadStatus::Ok);
    CHECK_EQ(event.data.data(), firstBuffer);
    CHECK_EQ(event.data.capacity(), firstCapacity);
}

int main() { return petsro::test::runAll(); }
