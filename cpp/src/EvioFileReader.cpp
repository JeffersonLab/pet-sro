#include "EvioFileReader.hpp"

#include "Logging.hpp"
#include "SroWireFormat.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

namespace petsro {

namespace {

/// Both formats place the EVIO magic here, which is what makes the byte order
/// of those four bytes sufficient to tell them apart. See ReplayFrameSource.
constexpr std::size_t MAGIC_PROBE_OFFSET = 28;
constexpr std::size_t PROBE_BYTES = MAGIC_PROBE_OFFSET + 4;

std::string errnoText() {
    // strerror is not thread-safe in general, but this is only reached on an
    // error path in the single-threaded control flow of the replay loop.
    return std::strerror(errno);
}

}  // namespace

const char* toString(ReadStatus status) noexcept {
    switch (status) {
        case ReadStatus::Ok:        return "ok";
        case ReadStatus::EndOfFile: return "end-of-file";
        case ReadStatus::Malformed: return "malformed";
        case ReadStatus::IoError:   return "io-error";
    }
    return "unknown";
}

const char* toString(EvioFormat format) noexcept {
    switch (format) {
        case EvioFormat::WireDump:      return "WIRE_DUMP (big-endian, length word present)";
        case EvioFormat::FebStreamDump: return "FEB_STREAM_DUMP (little-endian, length word stripped)";
        case EvioFormat::Unknown:       return "unknown";
    }
    return "unknown";
}

EvioFileReader::EvioFileReader(std::string path) : path_(std::move(path)) {}

bool EvioFileReader::open() {
    close();

    stream_.open(path_, std::ios::binary);
    if (!stream_.is_open()) {
        lastError_ = "cannot open " + path_ + ": " + errnoText();
        return false;
    }

    if (!detectFormat()) {
        stream_.close();
        return false;
    }

    offset_ = 0;
    stats_.opens++;
    stream_.clear();
    stream_.seekg(0, std::ios::beg);
    if (!stream_.good()) {
        lastError_ = "cannot rewind " + path_ + " after format probe";
        stream_.close();
        return false;
    }

    LOG_INFO << "opened " << path_ << " format=" << toString(format_)
             << " (open #" << stats_.opens << ")";
    return true;
}

bool EvioFileReader::reopen() {
    close();
    return open();
}

void EvioFileReader::close() noexcept {
    // ifstream::close() can set failbit but does not throw with the default
    // exception mask, which is what we rely on: this runs from cleanup paths.
    if (stream_.is_open()) {
        stream_.close();
    }
    stream_.clear();
    offset_ = 0;
}

bool EvioFileReader::detectFormat() {
    std::uint8_t probe[PROBE_BYTES] = {};

    stream_.clear();
    stream_.seekg(0, std::ios::beg);
    const std::size_t got = readExactly(probe, PROBE_BYTES);
    if (got < PROBE_BYTES) {
        std::ostringstream oss;
        oss << path_ << " is too small to be a capture (" << got
            << " bytes available, need at least " << PROBE_BYTES << ")";
        lastError_ = oss.str();
        format_ = EvioFormat::Unknown;
        return false;
    }

    const std::uint32_t asBig = sro::readBe32(probe + MAGIC_PROBE_OFFSET);
    const std::uint32_t asLittle = sro::readLe32(probe + MAGIC_PROBE_OFFSET);

    if (asBig == sro::EVIO_MAGIC) {
        format_ = EvioFormat::WireDump;
        return true;
    }
    if (asLittle == sro::EVIO_MAGIC) {
        format_ = EvioFormat::FebStreamDump;
        return true;
    }

    std::ostringstream oss;
    oss << path_ << ": no EVIO magic at byte " << MAGIC_PROBE_OFFSET << " (found 0x"
        << std::hex << asBig << " big-endian / 0x" << asLittle << std::dec
        << " little-endian); not an evio_*.bin wire dump nor a pet_sro_feb_stream*.bin dump";
    lastError_ = oss.str();
    format_ = EvioFormat::Unknown;
    return false;
}

std::size_t EvioFileReader::readExactly(std::uint8_t* dst, std::size_t count) {
    if (count == 0) {
        return 0;
    }
    stream_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(count));
    const std::streamsize got = stream_.gcount();
    return got < 0 ? 0 : static_cast<std::size_t>(got);
}

ReadStatus EvioFileReader::fail(ReadStatus status, const std::string& message) {
    lastError_ = message;
    stats_.readErrors++;
    LOG_ERROR << message;
    return status;
}

ReadStatus EvioFileReader::next(EvioEvent& out) {
    if (!stream_.is_open()) {
        return fail(ReadStatus::IoError, path_ + ": read attempted while closed");
    }

    const ReadStatus status = (format_ == EvioFormat::FebStreamDump)
                                  ? nextFebStreamDump(out)
                                  : nextWireDump(out);
    if (status == ReadStatus::Ok) {
        stats_.eventsRead++;
        stats_.bytesRead += out.data.size();
    }
    return status;
}

ReadStatus EvioFileReader::nextWireDump(EvioEvent& out) {
    const std::uint64_t blockStart = offset_;

    std::uint8_t lenWord[4];
    const std::size_t gotLen = readExactly(lenWord, sizeof lenWord);
    if (gotLen == 0) {
        // Exactly at a block boundary with nothing left: the clean end.
        return ReadStatus::EndOfFile;
    }
    if (gotLen < sizeof lenWord) {
        // ReplayFrameSource logs and stops the scan here rather than failing;
        // a capture cut mid-word by a killed writer is common and harmless.
        stats_.truncatedTails++;
        LOG_WARN << path_ << ": " << gotLen << " stray byte(s) at offset " << blockStart
                 << ", treating as end of file";
        return ReadStatus::EndOfFile;
    }

    const std::uint32_t blockWords = sro::readBe32(lenWord);
    if (blockWords < sro::MIN_BLOCK_WORDS || blockWords > sro::MAX_BLOCK_WORDS) {
        std::ostringstream oss;
        oss << path_ << ": implausible block length " << blockWords << " words at byte "
            << blockStart;
        return fail(ReadStatus::Malformed, oss.str());
    }

    const std::size_t blockBytes = static_cast<std::size_t>(blockWords) * 4U;
    out.data.resize(blockBytes);  // reuses the previous allocation when big enough
    out.data[0] = lenWord[0];
    out.data[1] = lenWord[1];
    out.data[2] = lenWord[2];
    out.data[3] = lenWord[3];

    const std::size_t want = blockBytes - 4U;
    const std::size_t got = readExactly(out.data.data() + 4, want);
    if (got < want) {
        if (stream_.bad()) {
            std::ostringstream oss;
            oss << path_ << ": read error at byte " << blockStart << ": " << errnoText();
            return fail(ReadStatus::IoError, oss.str());
        }
        stats_.truncatedTails++;
        LOG_WARN << path_ << ": truncated final block at byte " << blockStart << " ("
                 << blockBytes << " bytes declared, " << (got + 4) << " available), dropping it";
        return ReadStatus::EndOfFile;
    }

    offset_ = blockStart + blockBytes;
    return decodeHeader(out, blockStart);
}

ReadStatus EvioFileReader::nextFebStreamDump(EvioEvent& out) {
    const std::uint64_t blockStart = offset_;

    std::uint8_t countWord[4];
    const std::size_t gotCount = readExactly(countWord, sizeof countWord);
    if (gotCount == 0) {
        return ReadStatus::EndOfFile;
    }
    if (gotCount < sizeof countWord) {
        stats_.truncatedTails++;
        LOG_WARN << path_ << ": " << gotCount << " stray byte(s) at offset " << blockStart
                 << ", treating as end of file";
        return ReadStatus::EndOfFile;
    }

    // pet_sro_eth.c writes the word count *without* the EVIO block-length word,
    // which sock_read_event_socket() already consumed for framing. So the
    // plausible range is one word below the wire-format range.
    const std::uint32_t payloadWords = sro::readLe32(countWord);
    if (payloadWords < sro::MIN_BLOCK_WORDS - 1U || payloadWords > sro::MAX_BLOCK_WORDS) {
        std::ostringstream oss;
        oss << path_ << ": implausible frame word count " << payloadWords << " at byte "
            << blockStart;
        return fail(ReadStatus::Malformed, oss.str());
    }

    const std::size_t payloadBytes = static_cast<std::size_t>(payloadWords) * 4U;
    out.data.resize(payloadBytes + 4U);

    // Restore the length word the C writer dropped. It counts itself, so the
    // wire value is payloadWords + 1.
    sro::writeBe32(out.data.data(), payloadWords + 1U);

    const std::size_t got = readExactly(out.data.data() + 4, payloadBytes);
    if (got < payloadBytes) {
        if (stream_.bad()) {
            std::ostringstream oss;
            oss << path_ << ": read error at byte " << blockStart << ": " << errnoText();
            return fail(ReadStatus::IoError, oss.str());
        }
        stats_.truncatedTails++;
        LOG_WARN << path_ << ": truncated final frame at byte " << blockStart << " ("
                 << payloadWords << " words declared, " << got << " bytes left), dropping it";
        return ReadStatus::EndOfFile;
    }

    // Byte-swap every payload word back to network order, in place.
    for (std::size_t w = 0; w < payloadWords; ++w) {
        std::uint8_t* p = out.data.data() + 4U + w * 4U;
        sro::writeBe32(p, sro::readLe32(p));
    }

    offset_ = blockStart + 4U + payloadBytes;
    return decodeHeader(out, blockStart);
}

ReadStatus EvioFileReader::decodeHeader(EvioEvent& out, std::uint64_t blockStart) {
    const std::size_t bytes = out.data.size();

    // Every field read below is bounds-checked against this one test: the
    // block must be long enough to contain word 15, the timestamp high word.
    if (bytes < sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS)) {
        std::ostringstream oss;
        oss << path_ << ": block at byte " << blockStart << " is only " << bytes
            << " bytes; need at least " << sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS)
            << " to hold a timestamp";
        return fail(ReadStatus::Malformed, oss.str());
    }

    const std::uint8_t* base = out.data.data();

    const std::uint32_t magic = sro::readBe32(base + sro::wordOffset(sro::WORD_MAGIC));
    if (magic != sro::EVIO_MAGIC) {
        std::ostringstream oss;
        oss << path_ << ": bad EVIO magic 0x" << std::hex << magic << std::dec
            << " in block starting at byte " << blockStart;
        return fail(ReadStatus::Malformed, oss.str());
    }

    const std::uint32_t lo = sro::readBe32(base + sro::wordOffset(sro::WORD_TIMESTAMP_LO));
    const std::uint32_t hi = sro::readBe32(base + sro::wordOffset(sro::WORD_TIMESTAMP_HI));
    out.timestamp = sro::combineTimestamp(lo, hi);
    out.frameCounter = sro::readBe32(base + sro::wordOffset(sro::WORD_FRAME_COUNTER));
    out.rocid = sro::rocidFromBankTag(sro::readBe32(base + sro::wordOffset(sro::WORD_BANK_TAG)));

    return ReadStatus::Ok;
}

}  // namespace petsro
