// EjfatSender.cpp -- the only translation unit that knows about E2SAR.
//
// Modelled on initializeSegmenter() and the send loop of
//   /Users/gurjyan/Documents/Devel/e2sar-utils/src/e2sar_root.cpp
// with the ROOT-specific batching removed: here one EVIO block is one EJFAT
// event, so there is no batch to accumulate.

#include "EjfatSender.hpp"

#include "Logging.hpp"

#include <sstream>
#include <utility>

#ifdef PETSRO_HAVE_E2SAR

#include <e2sar.hpp>

#include <boost/any.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace petsro {

namespace {

/// Retry budget for a full send queue, matching e2sar_root.cpp's MAX_RETRIES.
/// At 100 us per retry this is a one-second stall before giving up.
constexpr int MAX_QUEUE_RETRIES = 10000;
constexpr std::chrono::microseconds QUEUE_RETRY_SLEEP{100};

/// Callback for the asynchronous path: frees the copy made in send().
void freeCopiedEvent(boost::any a) {
    // The callback runs on an E2SAR send thread. Anything that throws here
    // would cross a library boundary, so the cast is checked rather than
    // trusted.
    auto* p = boost::any_cast<std::vector<std::uint8_t>*>(&a);
    if (p != nullptr) {
        delete *p;
    }
}

}  // namespace

struct EjfatSender::Impl {
    EjfatConfig config;
    std::unique_ptr<e2sar::Segmenter> segmenter;
    std::string destination;
};

bool EjfatSender::available() noexcept { return true; }

EjfatSender::EjfatSender(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EjfatSender::~EjfatSender() {
    // ~Segmenter() stops the threads. Nothing here may throw.
    impl_.reset();
}

std::unique_ptr<EjfatSender> EjfatSender::create(const EjfatConfig& config, std::string& error) {
    auto uriResult =
        e2sar::EjfatURI::getFromString(config.uri, e2sar::EjfatURI::TokenType::instance, false);
    if (uriResult.has_error()) {
        error = "cannot parse EJFAT URI: " + uriResult.error().message();
        return nullptr;
    }
    e2sar::EjfatURI uri = uriResult.value();

    if (config.withCp) {
        LOG_INFO << "registering sender with the load balancer control plane";
        e2sar::LBManager lbm(uri, config.validateCert);
        auto addResult = lbm.addSenderSelf();
        if (addResult.has_error()) {
            error = "unable to add sender to the LB allow list: " + addResult.error().message();
            return nullptr;
        }
        LOG_INFO << "sender registered";
    }

    e2sar::Segmenter::SegmenterFlags sflags;
    sflags.mtu = config.mtu;
    sflags.useCP = config.withCp;
    sflags.numSendSockets = config.numSendSockets;
    sflags.rateGbps = config.rateGbps;

    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->config = config;
    try {
        impl->segmenter = std::make_unique<e2sar::Segmenter>(uri, config.dataId,
                                                             config.eventSrcId, sflags);
    } catch (const std::exception& e) {
        error = std::string("cannot construct Segmenter: ") + e.what();
        return nullptr;
    }

    auto openResult = impl->segmenter->openAndStart();
    if (openResult.has_error()) {
        error = "cannot start segmenter: " + openResult.error().message();
        return nullptr;
    }

    {
        std::ostringstream oss;
        oss << config.uri << " via " << impl->segmenter->getIntf();
        impl->destination = oss.str();
    }

    LOG_INFO << "segmenter started: mtu=" << impl->segmenter->getMTU()
             << " maxPayload=" << impl->segmenter->getMaxPldLen()
             << " rate=" << config.rateGbps << " Gbps"
             << " sockets=" << config.numSendSockets
             << " mode=" << (config.async ? "async" : "sync");

    return std::unique_ptr<EjfatSender>(new EjfatSender(std::move(impl)));
}

SendOutcome EjfatSender::send(const OutgoingEvent& event) {
    SendOutcome outcome;
    outcome.packets = packetsFor(event.length);

    if (event.length == 0) {
        outcome.error = "refusing to send a zero-length event";
        return outcome;
    }

    if (!impl_->config.async) {
        // Synchronous: _send() fragments and writes on this thread, so the
        // reader's buffer can be reused the moment this returns. No copy.
        //
        // The const_cast is forced by E2SAR's signature, which takes u_int8_t*
        // although Segmenter::SendThreadState::_send() only ever reads the
        // buffer: it points an iovec entry at each fragment and hands it to
        // sendmsg(). The underlying object is the reader's vector, which is
        // genuinely non-const, so this is not undefined behaviour.
        auto result = impl_->segmenter->sendEvent(const_cast<std::uint8_t*>(event.data),
                                                  event.length, event.eventNumber,
                                                  event.dataId, event.entropy);
        if (result.has_error()) {
            outcome.error = result.error().message();
            return outcome;
        }
        outcome.ok = true;
        return outcome;
    }

    // Asynchronous: the Segmenter takes the pointer and sends later, so the
    // bytes must outlive this call. Copy, and free from the callback.
    auto* copy = new std::vector<std::uint8_t>(event.data, event.data + event.length);

    for (int retry = 0; retry <= MAX_QUEUE_RETRIES; ++retry) {
        auto result = impl_->segmenter->addToSendQueue(copy->data(), copy->size(),
                                                       event.eventNumber, event.dataId,
                                                       event.entropy, &freeCopiedEvent, copy);
        if (!result.has_error()) {
            outcome.ok = true;
            return outcome;
        }
        if (result.error().code() != e2sar::E2SARErrorc::MemoryError) {
            outcome.error = result.error().message();
            delete copy;
            return outcome;
        }
        // Queue full: back off briefly and try again.
        std::this_thread::sleep_for(QUEUE_RETRY_SLEEP);
    }

    outcome.error = "send queue still full after " + std::to_string(MAX_QUEUE_RETRIES) + " retries";
    delete copy;
    return outcome;
}

std::size_t EjfatSender::maxPayloadLength() const noexcept {
    return impl_->segmenter->getMaxPldLen();
}

std::uint16_t EjfatSender::mtu() const noexcept { return impl_->segmenter->getMTU(); }

std::string EjfatSender::destination() const { return impl_->destination; }

void EjfatSender::drain() {
    if (!impl_->config.async) {
        return;  // nothing is queued in synchronous mode
    }
    // There is no flush API on the Segmenter; e2sar_root.cpp waits out the send
    // threads the same way before reading final statistics.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void EjfatSender::reportSegmenterStats(std::ostream& out) const {
    const auto sendStats = impl_->segmenter->getSendStats();
    const auto syncStats = impl_->segmenter->getSyncStats();

    out << "  E2SAR fragments sent      : " << sendStats.msgCnt << '\n';
    out << "  E2SAR send errors         : " << sendStats.errCnt << '\n';
    if (sendStats.errCnt > 0) {
        out << "    last errno              : " << sendStats.lastErrno << " ("
            << std::strerror(sendStats.lastErrno) << ")\n";
        out << "    last E2SAR error        : "
            << make_error_code(sendStats.lastE2SARError).message() << '\n';
    }
    out << "  E2SAR sync messages sent  : " << syncStats.msgCnt << '\n';
    out << "  E2SAR sync errors         : " << syncStats.errCnt << '\n';
}

}  // namespace petsro

#else  // !PETSRO_HAVE_E2SAR

namespace petsro {

struct EjfatSender::Impl {};

bool EjfatSender::available() noexcept { return false; }

EjfatSender::EjfatSender(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EjfatSender::~EjfatSender() = default;

std::unique_ptr<EjfatSender> EjfatSender::create(const EjfatConfig&, std::string& error) {
    error =
        "this binary was built without E2SAR, so it cannot send to a load balancer; "
        "rebuild with E2SAR available (see cpp/README.md) or run with --dry-run";
    return nullptr;
}

SendOutcome EjfatSender::send(const OutgoingEvent&) {
    SendOutcome outcome;
    outcome.error = "built without E2SAR";
    return outcome;
}

std::size_t EjfatSender::maxPayloadLength() const noexcept { return 0; }

std::uint16_t EjfatSender::mtu() const noexcept { return 0; }

std::string EjfatSender::destination() const { return "unavailable"; }

void EjfatSender::drain() {}

void EjfatSender::reportSegmenterStats(std::ostream& out) const {
    out << "  E2SAR statistics unavailable (built without E2SAR)\n";
}

}  // namespace petsro

#endif  // PETSRO_HAVE_E2SAR
