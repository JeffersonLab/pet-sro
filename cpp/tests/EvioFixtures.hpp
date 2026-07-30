// EvioFixtures.hpp -- synthetic captures and stub sources for the tests.
//
// There are no real FEB captures in this repository, so the reader tests build
// their own files from the layout SroWireFormat documents. That keeps the tests
// honest about the one thing they can check without real data: that the reader
// agrees with the written-down wire format.

#ifndef PETSRO_EVIOFIXTURES_HPP
#define PETSRO_EVIOFIXTURES_HPP

#include "EvioFileReader.hpp"
#include "PacketSink.hpp"
#include "SroWireFormat.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace petsro {
namespace test {

/// Builds one block's worth of 32-bit words in host form, laid out per
/// SroWireFormat. `totalWords` includes word 0.
inline std::vector<std::uint32_t> makeBlockWords(std::uint32_t totalWords, std::uint64_t timestamp,
                                                 std::uint32_t frameCounter, std::uint32_t rocid) {
    if (totalWords < sro::MIN_TIMESTAMPED_BLOCK_WORDS) {
        totalWords = sro::MIN_TIMESTAMPED_BLOCK_WORDS;
    }
    std::vector<std::uint32_t> w(totalWords, 0);

    w[sro::WORD_BLOCK_LENGTH] = totalWords;
    w[sro::WORD_MAGIC] = sro::EVIO_MAGIC;
    w[sro::WORD_BANK_TAG] = sro::withRocid(0x0000FF10U, rocid);
    w[11] = 0xFF302011U;
    w[12] = 0x31010003U;
    w[sro::WORD_FRAME_COUNTER] = frameCounter;
    w[sro::WORD_TIMESTAMP_LO] = static_cast<std::uint32_t>(timestamp & 0xFFFFFFFFU);
    w[sro::WORD_TIMESTAMP_HI] = static_cast<std::uint32_t>((timestamp >> 32) & 0xFFFFFFFFU);
    if (totalWords > 16) {
        w[16] = 0x41850001U;
    }
    if (totalWords > 17) {
        w[17] = 0x00000081U;
    }
    // Anything past the fixed header is filler that must survive untouched.
    for (std::uint32_t i = sro::WORD_FIRST_AFTER_HEADER; i < totalWords; ++i) {
        w[i] = 0xA0000000U | i;
    }
    return w;
}

/// One block as it appears in a WIRE_DUMP file: every word big-endian, length
/// word present.
inline std::vector<std::uint8_t> encodeWireDumpBlock(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        sro::writeBe32(bytes.data() + i * 4, words[i]);
    }
    return bytes;
}

/// One frame as pet_sro_eth.c writes it: a little-endian word count, then that
/// many little-endian words, with the EVIO block-length word stripped.
inline std::vector<std::uint8_t> encodeFebStreamFrame(const std::vector<std::uint32_t>& words) {
    const std::uint32_t payloadWords = static_cast<std::uint32_t>(words.size() - 1);
    std::vector<std::uint8_t> bytes((payloadWords + 1U) * 4U);
    sro::writeLe32(bytes.data(), payloadWords);
    for (std::uint32_t i = 0; i < payloadWords; ++i) {
        sro::writeLe32(bytes.data() + 4U + i * 4U, words[i + 1U]);
    }
    return bytes;
}

/// A capture file that deletes itself. Tests never leave litter behind.
class TempFile {
  public:
    explicit TempFile(std::string name) : path_(std::move(name)) {}

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    ~TempFile() { std::remove(path_.c_str()); }

    const std::string& path() const noexcept { return path_; }

    void write(const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
    }

  private:
    std::string path_;
};

/// Writes `count` blocks whose timestamps start at `firstTimestamp` and step by
/// `step`. Returns the bytes written, so a test can truncate or corrupt them.
inline std::vector<std::uint8_t> makeWireDumpCapture(std::uint32_t count, std::uint64_t firstTimestamp,
                                                     std::uint64_t step, std::uint32_t rocid,
                                                     std::uint32_t blockWords = 24) {
    std::vector<std::uint8_t> all;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto words = makeBlockWords(blockWords, firstTimestamp + i * step, i, rocid);
        const auto block = encodeWireDumpBlock(words);
        all.insert(all.end(), block.begin(), block.end());
    }
    return all;
}

inline std::vector<std::uint8_t> makeFebStreamCapture(std::uint32_t count, std::uint64_t firstTimestamp,
                                                      std::uint64_t step, std::uint32_t rocid,
                                                      std::uint32_t blockWords = 24) {
    std::vector<std::uint8_t> all;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto words = makeBlockWords(blockWords, firstTimestamp + i * step, i, rocid);
        const auto frame = encodeFebStreamFrame(words);
        all.insert(all.end(), frame.begin(), frame.end());
    }
    return all;
}

// --- stub event source ---------------------------------------------------

/// Replays a canned list of events, then reports a configurable status. Lets
/// the synchronizer be tested with no files at all.
class VectorEventSource final : public EventSource {
  public:
    VectorEventSource(std::string name, std::vector<std::uint64_t> timestamps,
                      ReadStatus terminalStatus = ReadStatus::EndOfFile)
        : name_(std::move(name)), timestamps_(std::move(timestamps)), terminal_(terminalStatus) {}

    ReadStatus next(EvioEvent& out) override {
        if (index_ >= timestamps_.size()) {
            return terminal_;
        }
        const std::uint64_t ts = timestamps_[index_];
        const auto words = makeBlockWords(24, ts, static_cast<std::uint32_t>(index_), 100);
        out.data = encodeWireDumpBlock(words);
        out.timestamp = ts;
        out.frameCounter = index_;
        out.rocid = 100;
        ++index_;
        ++reads_;
        return ReadStatus::Ok;
    }

    const std::string& name() const noexcept override { return name_; }

    std::size_t reads() const noexcept { return reads_; }
    void rewind() noexcept { index_ = 0; }

  private:
    std::string name_;
    std::vector<std::uint64_t> timestamps_;
    ReadStatus terminal_;
    std::size_t index_ = 0;
    std::size_t reads_ = 0;
};

// --- recording sink ------------------------------------------------------

/// Records what was sent and how it fragmented, and can be told to fail.
/// This is the injectable packet output the tests use instead of a live LB.
class MockPacketSink final : public PacketSink {
  public:
    struct Record {
        std::uint64_t eventNumber = 0;
        std::uint16_t dataId = 0;
        std::uint16_t entropy = 0;
        std::size_t length = 0;
        std::uint64_t packets = 0;
        std::vector<std::uint8_t> payload;
    };

    explicit MockPacketSink(std::size_t maxPayloadLength) : maxPld_(maxPayloadLength) {}

    SendOutcome send(const OutgoingEvent& event) override {
        SendOutcome outcome;
        if (failAfter_ >= 0 && static_cast<std::size_t>(failAfter_) <= sent_.size()) {
            outcome.error = "injected send failure";
            return outcome;
        }

        Record r;
        r.eventNumber = event.eventNumber;
        r.dataId = event.dataId;
        r.entropy = event.entropy;
        r.length = event.length;
        r.packets = packetsFor(event.length);
        r.payload.assign(event.data, event.data + event.length);
        sent_.push_back(std::move(r));

        outcome.ok = true;
        outcome.packets = sent_.back().packets;
        return outcome;
    }

    std::size_t maxPayloadLength() const noexcept override { return maxPld_; }
    std::string destination() const override { return "mock"; }
    void drain() override { ++drains_; }

    /// Makes the (failAfter+1)-th send fail. Negative disables the injection.
    void failAfter(int count) noexcept { failAfter_ = count; }

    const std::vector<Record>& sent() const noexcept { return sent_; }
    std::size_t drains() const noexcept { return drains_; }

  private:
    std::size_t maxPld_;
    std::vector<Record> sent_;
    int failAfter_ = -1;
    std::size_t drains_ = 0;
};

}  // namespace test
}  // namespace petsro

#endif  // PETSRO_EVIOFIXTURES_HPP
