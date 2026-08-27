// SroWireFormat.hpp -- the FEB SRO event-socket wire format, in one place.
//
// This is a direct C++ transcription of
//   org.jlab.detimg.petiroc.daq.SroWireFormat
// from the PetirocJava tree. Keep the two in sync; the constants here are the
// authority for how this program locates block boundaries and timestamps.
//
// The FEB event socket carries a stream of self-framing, BIG-ENDIAN EVIO
// blocks. Word 0 is the block length in 32-bit words, *including itself*, so a
// block occupies blockLenWords * 4 bytes. Layout:
//
//   word  0      EVIO block length in words (including this word)
//   word  1..6   rest of the EVIO block header
//   word  7      EVIO magic 0xC0DA0100
//   word  8      event bank length
//   word  9      event bank tag/type -- rocid lives in the upper 16 bits
//   word 10      int32 sub-bank length
//   word 11      int32 sub-bank tag/type (0xFF302011)
//   word 12      fixed marker 0x31010003
//   word 13      frame counter
//   word 14      timestamp low  word
//   word 15      timestamp high word
//   word 16      fixed marker 0x41850001
//   word 17      fixed marker 0x00000081
//   word 18..23  slow-controls bank (recent firmware only)
//   word 24..    TDC hit words, in (leading-edge, channel+width) pairs
//
// Timestamps are in NANOSECONDS and advance by exactly FRAME_PERIOD_NS per
// frame; both the timestamp and the frame counter start at 0 when a run's
// stream is enabled.

#ifndef PETSRO_SROWIREFORMAT_HPP
#define PETSRO_SROWIREFORMAT_HPP

#include <cstddef>
#include <cstdint>

namespace petsro {
namespace sro {

/// Byte offset of a 32-bit word index, relative to the start of a block.
constexpr std::size_t wordOffset(std::size_t wordIndex) noexcept {
    return wordIndex * 4U;
}

/// Word 0: block length in words, including word 0 itself.
constexpr std::size_t WORD_BLOCK_LENGTH = 0;

/// Word 2: EVIO block header length in words. Always 8 for EVIO version 4.
constexpr std::size_t WORD_HEADER_LENGTH = 2;

/// Word 5: bit info in the upper 24 bits, EVIO version in the low 8.
constexpr std::size_t WORD_VERSION = 5;

/// Word 7 holds EVIO_MAGIC; the cheapest way to recognise a block or a byte order.
constexpr std::size_t WORD_MAGIC = 7;

/// Word 9: event bank tag/type. The rocid occupies its upper 16 bits.
constexpr std::size_t WORD_BANK_TAG = 9;

/// Word 13: frame counter, 32-bit unsigned, starts at 0.
constexpr std::size_t WORD_FRAME_COUNTER = 13;

/// Word 14: low 32 bits of the frame timestamp.
constexpr std::size_t WORD_TIMESTAMP_LO = 14;

/// Word 15: high 32 bits of the frame timestamp.
constexpr std::size_t WORD_TIMESTAMP_HI = 15;

/// Word 18: first word after the fixed header, on either firmware generation.
constexpr std::size_t WORD_FIRST_AFTER_HEADER = 18;

/// Smallest block from which a timestamp can be read: words 0..15 must exist.
constexpr std::uint32_t MIN_TIMESTAMPED_BLOCK_WORDS = WORD_TIMESTAMP_HI + 1;

/// EVIO magic, big-endian, at WORD_MAGIC.
constexpr std::uint32_t EVIO_MAGIC = 0xC0DA0100U;

/// Smallest plausible block: the EVIO block header alone. Also the value word 2
/// carries in every EVIO version 4 block.
constexpr std::uint32_t MIN_BLOCK_WORDS = 8;

/// The only EVIO version the FEB emits and this program understands.
constexpr std::uint32_t EVIO_VERSION = 4;

/// Extracts the version from word 5's low byte.
constexpr std::uint32_t versionFromWord(std::uint32_t versionWord) noexcept {
    return versionWord & 0xFFU;
}

/// Sanity ceiling on a block length, to reject garbage before allocating on it.
constexpr std::uint32_t MAX_BLOCK_WORDS = 100000;

/// Nominal frame period. The firmware's eb.frame_len is configured for 1 ms.
constexpr std::uint64_t FRAME_PERIOD_NS = 1000000ULL;

/// Largest rocid representable in word 9.
constexpr std::uint32_t MAX_ROCID = 0xFFFFU;

/// Reads the rocid from word 9's upper 16 bits.
constexpr std::uint32_t rocidFromBankTag(std::uint32_t bankTagWord) noexcept {
    return (bankTagWord >> 16) & 0xFFFFU;
}

/// Returns word 9 with its rocid replaced, leaving the low 16 bits untouched.
constexpr std::uint32_t withRocid(std::uint32_t bankTagWord, std::uint32_t rocid) noexcept {
    return (bankTagWord & 0x0000FFFFU) | ((rocid & 0xFFFFU) << 16);
}

// --- byte access helpers -------------------------------------------------
//
// Assembled byte by byte rather than cast-and-swapped: no aliasing games, no
// alignment requirement on the source pointer, and identical behaviour on a
// big- or little-endian host.

inline std::uint32_t readBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[3]) << 24) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[0]);
}

inline void writeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<std::uint8_t>(v & 0xFFU);
}

inline void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

/// Combines the two timestamp words the way ReplayFrameSource.index() does:
/// low word in the least significant half, both treated as unsigned.
constexpr std::uint64_t combineTimestamp(std::uint32_t lo, std::uint32_t hi) noexcept {
    return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

}  // namespace sro
}  // namespace petsro

#endif  // PETSRO_SROWIREFORMAT_HPP
