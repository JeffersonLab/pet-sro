// EvioEventView.hpp -- decode and validate a reassembled EVIO block.
//
// The receiving half of the round trip. EvioFileReader validates a block on the
// way out of a file; this validates one that arrived over the network, without
// owning or copying it, so the receiver can report on EVIO events rather than
// on anonymous byte buffers.
//
// The checks are deliberately the same ones the reader makes, plus one the
// reader gets for free and the receiver does not: that the block-length word
// agrees with the number of bytes actually delivered. That single comparison is
// what proves reassembly returned the whole event and nothing more.

#ifndef PETSRO_EVIOEVENTVIEW_HPP
#define PETSRO_EVIOEVENTVIEW_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace petsro {

/// A non-owning decode of one EVIO block.
struct EvioEventView {
    bool valid = false;
    std::string problem;  ///< populated only when !valid

    std::uint32_t blockWords = 0;   ///< word 0, the declared length
    std::uint64_t timestamp = 0;    ///< words 14/15, nanoseconds
    std::uint64_t frameCounter = 0; ///< word 13
    std::uint32_t rocid = 0;        ///< word 9, upper 16 bits
};

/// Decodes `length` bytes at `data` as a big-endian EVIO block.
///
/// Never reads outside [data, data + length). A null pointer or a length below
/// the fixed header is reported as invalid rather than treated as a bug.
EvioEventView inspectEvioEvent(const std::uint8_t* data, std::size_t length) noexcept;

}  // namespace petsro

#endif  // PETSRO_EVIOEVENTVIEW_HPP
