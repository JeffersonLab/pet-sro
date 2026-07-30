// PacketSink.hpp -- where packetized events go.
//
// The replay loop knows nothing about E2SAR. It hands whole EVIO blocks to a
// PacketSink, which is either the real EjfatSender or, in tests and in
// --dry-run, something that just counts. That is what lets the packetization
// behaviour be tested without a live load balancer.

#ifndef PETSRO_PACKETSINK_HPP
#define PETSRO_PACKETSINK_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace petsro {

/// One event handed to the transport.
///
/// `data` is borrowed for the duration of the call. A synchronous sink may use
/// it directly; an asynchronous one must copy.
struct OutgoingEvent {
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;
    std::uint64_t eventNumber = 0;  ///< EJFAT event number; must be non-zero
    std::uint16_t dataId = 0;       ///< EJFAT source identifier
    std::uint16_t entropy = 0;      ///< 0 means "let E2SAR pick at random"
};

struct SendOutcome {
    bool ok = false;
    std::uint64_t packets = 0;  ///< datagrams the event was split into
    std::string error;          ///< populated only when !ok
};

class PacketSink {
  public:
    virtual ~PacketSink() = default;

    virtual SendOutcome send(const OutgoingEvent& event) = 0;

    /// Largest event payload that fits in one datagram, i.e. MTU minus the IP,
    /// UDP, LB and RE headers. Used for the packet accounting below.
    virtual std::size_t maxPayloadLength() const noexcept = 0;

    /// Human-readable destination, for logs.
    virtual std::string destination() const = 0;

    /// Blocks until queued data has been handed to the kernel. No-op for a
    /// synchronous sink.
    virtual void drain() {}

    /// Number of datagrams an event of `length` bytes becomes. Mirrors the
    /// ceiling division E2SAR's Segmenter::SendThreadState::_send() does.
    std::uint64_t packetsFor(std::size_t length) const noexcept {
        const std::size_t maxPld = maxPayloadLength();
        if (maxPld == 0) {
            return 0;
        }
        return static_cast<std::uint64_t>((length + maxPld - 1) / maxPld);
    }
};

/// Counts and discards. Backs --dry-run, so the reader, synchronizer and
/// command line can be exercised with no load balancer in reach.
class NullPacketSink final : public PacketSink {
  public:
    explicit NullPacketSink(std::size_t maxPayloadLength) : maxPld_(maxPayloadLength) {}

    SendOutcome send(const OutgoingEvent& event) override {
        SendOutcome outcome;
        outcome.ok = true;
        outcome.packets = packetsFor(event.length);
        return outcome;
    }

    std::size_t maxPayloadLength() const noexcept override { return maxPld_; }

    std::string destination() const override { return "dry-run (discarded)"; }

  private:
    std::size_t maxPld_;
};

}  // namespace petsro

#endif  // PETSRO_PACKETSINK_HPP
