// EjfatReceiver.hpp -- EJFAT reception and reassembly, behind one interface.
//
// The counterpart of EjfatSender: everything about E2SAR lives behind a pimpl
// so that <e2sar.hpp> is included by exactly one translation unit, and the rest
// of the tree compiles without E2SAR present.
//
// The E2SAR Reassembler does the work: it strips the UDP, LB and RE headers,
// collects the fragments of one event and hands back the segmenter's original
// payload. For this project's sender that payload is exactly one big-endian
// EVIO block -- block-length word included, nothing prepended, nothing removed.
// See cpp/README.md, "What the reassembler returns".
//
// Both evio_ejfat_recv and the ERSAP actor in src/actor drive this class, so
// they cannot drift apart in defaults or in behaviour.

#ifndef PETSRO_EJFATRECEIVER_HPP
#define PETSRO_EJFATRECEIVER_HPP

#include "ReceiveStats.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

namespace petsro {

/// Receiver settings, named to match the long options of evio_ejfat_recv. The
/// defaults here are the authority: both the command line and the ERSAP actor
/// configuration start from a default-constructed instance and overwrite only
/// what the user supplied.
struct EjfatReceiverConfig {
    std::string uri;                       ///< --uri, required
    std::string recvIp = "127.0.0.1";      ///< --recv-ip
    std::uint16_t recvPort = 10000;        ///< --recv-port
    std::size_t recvThreads = 1;           ///< --recv-threads
    int eventTimeoutMs = 500;              ///< --event-timeout
    int pollTimeoutMs = 1000;              ///< --poll-timeout
    bool withCp = false;                   ///< --withcp
    bool validateCert = true;              ///< inverse of --novalidate
    std::uint64_t maxEvents = 0;           ///< --max-events, 0 means unlimited
    double statsIntervalSeconds = 5.0;     ///< --stats-interval, 0 disables
    bool verbose = false;                  ///< --verbose
    bool quiet = false;                    ///< --quiet

    /// Returns an empty string when the configuration can be used, otherwise a
    /// message naming the first problem. Checks only what can be decided
    /// without touching the network; the URI and the IP address are parsed by
    /// create(), which reports their failures the same way.
    std::string validate() const;
};

/// One reassembled event, owning the buffer the Reassembler allocated.
///
/// The Reassembler allocates with new[] and transfers ownership to the caller,
/// which is not something a std::vector or a std::unique_ptr<std::uint8_t[]>
/// can express without either a copy or an easy mistake, so it gets its own
/// move-only holder. Nothing here copies the payload.
class ReassembledEvent final {
  public:
    ReassembledEvent() = default;

    ReassembledEvent(std::uint8_t* data, std::size_t size, std::uint64_t eventNumber,
                     std::uint16_t dataId) noexcept
      : data_(data), size_(size), eventNumber_(eventNumber), dataId_(dataId) {}

    ~ReassembledEvent() { reset(); }

    ReassembledEvent(const ReassembledEvent&) = delete;
    ReassembledEvent& operator=(const ReassembledEvent&) = delete;

    ReassembledEvent(ReassembledEvent&& other) noexcept
      : data_(other.data_), size_(other.size_), eventNumber_(other.eventNumber_),
        dataId_(other.dataId_) {
        other.release();
    }

    ReassembledEvent& operator=(ReassembledEvent&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            eventNumber_ = other.eventNumber_;
            dataId_ = other.dataId_;
            other.release();
        }
        return *this;
    }

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t eventNumber() const noexcept { return eventNumber_; }
    std::uint16_t dataId() const noexcept { return dataId_; }
    bool empty() const noexcept { return data_ == nullptr || size_ == 0; }

    /// Frees the payload and returns to the empty state.
    void reset() noexcept {
        delete[] data_;
        release();
    }

  private:
    /// Drops ownership without freeing. Only ever used on a moved-from object.
    void release() noexcept {
        data_ = nullptr;
        size_ = 0;
        eventNumber_ = 0;
        dataId_ = 0;
    }

    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::uint64_t eventNumber_ = 0;
    std::uint16_t dataId_ = 0;
};

/// Outcome of one receive() call.
enum class ReceiveOutcome {
    Event,    ///< an event was reassembled and moved into `out`
    Timeout,  ///< the poll window expired with nothing to show. Not an error.
    Error     ///< the transport reported a failure; `error` says what
};

class EjfatReceiver final {
  public:
    /// True when the binary was compiled against E2SAR. When false, create()
    /// always fails: unlike sending there is no useful degraded mode.
    static bool available() noexcept;

    /// Validates the configuration, parses the URI, constructs the Reassembler,
    /// optionally registers with the control plane and starts the receive
    /// threads. Returns nullptr and fills `error` on any failure.
    static std::unique_ptr<EjfatReceiver> create(const EjfatReceiverConfig& config,
                                                 std::string& error);

    ~EjfatReceiver();

    EjfatReceiver(const EjfatReceiver&) = delete;
    EjfatReceiver& operator=(const EjfatReceiver&) = delete;

    /// Waits up to the configured --poll-timeout for one complete event.
    ///
    /// On ReceiveOutcome::Event, `out` owns the payload. On Timeout and Error,
    /// `out` is reset. Timeouts and errors are counted in stats() as they
    /// happen, so a caller that only forwards events still gets the full
    /// picture at shutdown.
    ReceiveOutcome receive(ReassembledEvent& out, std::string& error);

    /// Same as receive(), but with an explicit poll window in milliseconds.
    /// Used by callers that must return to a control thread sooner than
    /// --poll-timeout would allow.
    ReceiveOutcome receive(ReassembledEvent& out, std::string& error, int pollTimeoutMs);

    /// Deregisters the worker and stops the receive threads. Idempotent, and
    /// called by the destructor, so an error path may simply destroy the
    /// object. Never throws.
    void stop() noexcept;

    const EjfatReceiverConfig& config() const noexcept;

    ReceiveStats& stats() noexcept;
    const ReceiveStats& stats() const noexcept;

    /// "127.0.0.1 ports 10000:10000 with 1 thread(s), control plane off", for logs.
    std::string describeEndpoint() const;

    /// Appends the E2SAR Reassembler's own packet-level counters, which count
    /// what never made it up to us. Drains the lost-event list as a side
    /// effect, so call it once, at shutdown.
    void reportTransport(std::ostream& out);

  private:
    struct Impl;
    explicit EjfatReceiver(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace petsro

#endif  // PETSRO_EJFATRECEIVER_HPP
