// EjfatSender.hpp -- PacketSink backed by the E2SAR Segmenter.
//
// Everything about EJFAT lives behind a pimpl so that <e2sar.hpp> is included
// by exactly one translation unit. The rest of the program, and all of the
// tests, compile and link without E2SAR present.
//
// The Segmenter does the packetization: for each event it prepends an LB+RE
// header pair to every fragment and pushes them out over UDP. See the README's
// design section for the field-by-field account.

#ifndef PETSRO_EJFATSENDER_HPP
#define PETSRO_EJFATSENDER_HPP

#include "PacketSink.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

namespace petsro {

/// EJFAT/E2SAR configuration, named to match the options of e2sar_root.cpp.
struct EjfatConfig {
    std::string uri;                  ///< ejfat:// URI, as --uri
    std::uint16_t dataId = 1;         ///< default source id; overridden per event
    std::uint32_t eventSrcId = 1;     ///< carried in the Sync header
    std::uint16_t mtu = 1500;         ///< 0 lets E2SAR auto-detect (Linux only)
    float rateGbps = 1.0F;            ///< negative means unlimited
    bool withCp = false;              ///< register with the control plane
    bool validateCert = true;
    std::size_t numSendSockets = 4;
    /// Use Segmenter::addToSendQueue() (asynchronous, copies the event) instead
    /// of Segmenter::sendEvent() (synchronous, zero-copy).
    bool async = false;
};

class EjfatSender final : public PacketSink {
  public:
    /// True when the binary was compiled against E2SAR. When false, create()
    /// always fails and only --dry-run can run.
    static bool available() noexcept;

    /// Parses the URI, optionally registers the sender with the load balancer,
    /// and starts the Segmenter's threads. Returns nullptr and fills `error`
    /// on any failure.
    static std::unique_ptr<EjfatSender> create(const EjfatConfig& config, std::string& error);

    ~EjfatSender() override;

    EjfatSender(const EjfatSender&) = delete;
    EjfatSender& operator=(const EjfatSender&) = delete;

    SendOutcome send(const OutgoingEvent& event) override;
    std::size_t maxPayloadLength() const noexcept override;
    std::string destination() const override;
    void drain() override;

    /// MTU the Segmenter settled on, which may differ from the requested one.
    std::uint16_t mtu() const noexcept;

    /// Prints the Segmenter's own fragment and error counters.
    void reportSegmenterStats(std::ostream& out) const;

  private:
    struct Impl;
    explicit EjfatSender(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace petsro

#endif  // PETSRO_EJFATSENDER_HPP
