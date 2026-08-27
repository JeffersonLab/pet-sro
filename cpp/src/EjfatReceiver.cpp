// EjfatReceiver.cpp -- the only translation unit that knows how EJFAT packets
// become events.
//
// Lifted out of recv_main.cpp so that evio_ejfat_recv and the ERSAP actor share
// one implementation of the protocol setup. The sequence below is unchanged
// from the executable's original main(): parse URI, make the address, fill the
// ReassemblerFlags, construct, register, openAndStart.

#include "EjfatReceiver.hpp"

#include <sstream>
#include <utility>

#ifdef PETSRO_HAVE_E2SAR

#include <e2sar.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <vector>

namespace petsro {

struct EjfatReceiver::Impl {
    EjfatReceiverConfig config;
    std::unique_ptr<e2sar::Reassembler> reassembler;
    ReceiveStats stats;
    bool workerRegistered = false;
    bool stopped = false;
};

bool EjfatReceiver::available() noexcept { return true; }

EjfatReceiver::EjfatReceiver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EjfatReceiver::~EjfatReceiver() {
    stop();
    impl_.reset();
}

std::unique_ptr<EjfatReceiver> EjfatReceiver::create(const EjfatReceiverConfig& config,
                                                     std::string& error) {
    error = config.validate();
    if (!error.empty()) {
        return nullptr;
    }

    auto uriResult =
        e2sar::EjfatURI::getFromString(config.uri, e2sar::EjfatURI::TokenType::instance, false);
    if (uriResult.has_error()) {
        error = "cannot parse EJFAT URI: " + uriResult.error().message();
        return nullptr;
    }

    boost::asio::ip::address ip;
    try {
        ip = boost::asio::ip::make_address(config.recvIp);
    } catch (const std::exception& e) {
        error = "cannot parse recv-ip '" + config.recvIp + "': " + e.what();
        return nullptr;
    }

    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP = config.withCp;
    // Without a control plane there is no load balancer to strip the LB header
    // the segmenter prepended, so it is still on the wire when it reaches us.
    rflags.withLBHeader = !config.withCp;
    rflags.eventTimeout_ms = config.eventTimeoutMs;
    if (config.withCp) {
        rflags.validateCert = config.validateCert;
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;

    try {
        impl->reassembler = std::make_unique<e2sar::Reassembler>(
            uriResult.value(), ip, config.recvPort, config.recvThreads, rflags);
    } catch (const std::exception& e) {
        error = std::string("cannot construct Reassembler: ") + e.what();
        return nullptr;
    }

    if (config.withCp) {
        auto hostname = e2sar::NetUtil::getHostName();
        if (!hostname.has_error()) {
            auto reg = impl->reassembler->registerWorker(hostname.value());
            if (reg.has_error()) {
                // Not fatal: the executable has always continued past this, and
                // a receiver that cannot register still receives direct traffic.
                error = "registerWorker failed: " + reg.error().message();
            } else {
                impl->workerRegistered = true;
                error.clear();
            }
        }
    }

    auto openResult = impl->reassembler->openAndStart();
    if (openResult.has_error()) {
        error = "cannot start reassembler: " + openResult.error().message();
        return nullptr;
    }

    // A non-fatal registerWorker warning left in `error` above is dropped here:
    // create() succeeded, and its contract is that `error` describes a failure.
    error.clear();
    return std::unique_ptr<EjfatReceiver>(new EjfatReceiver(std::move(impl)));
}

ReceiveOutcome EjfatReceiver::receive(ReassembledEvent& out, std::string& error) {
    return receive(out, error, impl_->config.pollTimeoutMs);
}

ReceiveOutcome EjfatReceiver::receive(ReassembledEvent& out, std::string& error,
                                      int pollTimeoutMs) {
    out.reset();
    error.clear();

    if (!impl_->reassembler || impl_->stopped) {
        impl_->stats.recvErrors++;
        error = "receiver is not running";
        return ReceiveOutcome::Error;
    }

    std::uint8_t* buffer = nullptr;
    std::size_t size = 0;
    e2sar::EventNum_t eventNum = 0;
    std::uint16_t dataId = 0;

    // recvEvent takes the timeout as an unsigned quantity; a non-positive
    // window would wrap into a very long wait, so it is clamped rather than
    // trusted.
    const std::uint64_t window =
        (pollTimeoutMs > 0) ? static_cast<std::uint64_t>(pollTimeoutMs) : 0U;

    auto result = impl_->reassembler->recvEvent(&buffer, &size, &eventNum, &dataId, window);

    if (result.has_error()) {
        impl_->stats.recvErrors++;
        error = result.error().message();
        return ReceiveOutcome::Error;
    }
    if (result.value() == -1) {
        impl_->stats.timeouts++;  // nothing arrived within the poll window
        return ReceiveOutcome::Timeout;
    }

    out = ReassembledEvent{buffer, size, static_cast<std::uint64_t>(eventNum), dataId};
    return ReceiveOutcome::Event;
}

void EjfatReceiver::stop() noexcept {
    if (!impl_ || impl_->stopped || !impl_->reassembler) {
        return;
    }
    impl_->stopped = true;

    try {
        if (impl_->workerRegistered) {
            auto dereg = impl_->reassembler->deregisterWorker();
            static_cast<void>(dereg);  // best effort; nothing useful to do on failure
            impl_->workerRegistered = false;
        }
        impl_->reassembler->stopThreads();
    } catch (...) {
        // stop() is called from the destructor and from ERSAP's destroy path.
        // Neither may throw.
    }
}

const EjfatReceiverConfig& EjfatReceiver::config() const noexcept { return impl_->config; }

ReceiveStats& EjfatReceiver::stats() noexcept { return impl_->stats; }

const ReceiveStats& EjfatReceiver::stats() const noexcept { return impl_->stats; }

std::string EjfatReceiver::describeEndpoint() const {
    std::ostringstream oss;
    if (!impl_->reassembler) {
        oss << "not started";
        return oss.str();
    }
    oss << impl_->reassembler->get_dataIP() << " ports "
        << impl_->reassembler->get_recvPorts().first << ":"
        << impl_->reassembler->get_recvPorts().second << " with "
        << impl_->reassembler->get_numRecvThreads() << " thread(s), control plane "
        << (impl_->config.withCp ? "on" : "off (expecting the LB header on the wire)");
    return oss.str();
}

void EjfatReceiver::reportTransport(std::ostream& out) {
    if (!impl_->reassembler) {
        out << "\n  E2SAR reassembler: not started\n";
        return;
    }

    const auto rs = impl_->reassembler->getStats();
    out << "\n  E2SAR reassembler:\n";
    out << "    Packets received        : " << rs.totalPackets << '\n';
    out << "    Bytes received          : " << rs.totalBytes << '\n';
    out << "    Events reassembled      : " << rs.eventSuccess << '\n';
    out << "    Reassembly loss         : " << rs.reassemblyLoss << '\n';
    out << "    Enqueue loss            : " << rs.enqueueLoss << '\n';
    out << "    Data errors             : " << rs.dataErrCnt << '\n';
    out << "    gRPC errors             : " << rs.grpcErrCnt << '\n';

    std::vector<boost::tuple<e2sar::EventNum_t, u_int16_t, size_t>> lost;
    for (;;) {
        auto res = impl_->reassembler->get_LostEvent();
        if (res.has_error()) {
            break;
        }
        lost.push_back(res.value());
    }
    out << "    Lost events             : " << lost.size();
    if (!lost.empty()) {
        out << "  <eventNum:dataId/fragments>";
        // A full list can be enormous; the first few identify the pattern.
        const std::size_t show = std::min<std::size_t>(lost.size(), 10);
        for (std::size_t i = 0; i < show; ++i) {
            out << " <" << lost[i].get<0>() << ':' << lost[i].get<1>() << '/'
                << lost[i].get<2>() << '>';
        }
        if (lost.size() > show) {
            out << " ... and " << (lost.size() - show) << " more";
        }
    }
    out << '\n' << std::endl;
}

}  // namespace petsro

#else  // !PETSRO_HAVE_E2SAR

namespace petsro {

struct EjfatReceiver::Impl {
    EjfatReceiverConfig config;
    ReceiveStats stats;
};

bool EjfatReceiver::available() noexcept { return false; }

EjfatReceiver::EjfatReceiver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EjfatReceiver::~EjfatReceiver() = default;

std::unique_ptr<EjfatReceiver> EjfatReceiver::create(const EjfatReceiverConfig&,
                                                     std::string& error) {
    error =
        "this binary was built without E2SAR, so it cannot receive from a load balancer; "
        "rebuild with E2SAR available (see cpp/README.md)";
    return nullptr;
}

ReceiveOutcome EjfatReceiver::receive(ReassembledEvent& out, std::string& error) {
    return receive(out, error, 0);
}

ReceiveOutcome EjfatReceiver::receive(ReassembledEvent& out, std::string& error, int) {
    out.reset();
    error = "built without E2SAR";
    return ReceiveOutcome::Error;
}

void EjfatReceiver::stop() noexcept {}

const EjfatReceiverConfig& EjfatReceiver::config() const noexcept { return impl_->config; }

ReceiveStats& EjfatReceiver::stats() noexcept { return impl_->stats; }

const ReceiveStats& EjfatReceiver::stats() const noexcept { return impl_->stats; }

std::string EjfatReceiver::describeEndpoint() const { return "unavailable"; }

void EjfatReceiver::reportTransport(std::ostream& out) {
    out << "\n  E2SAR statistics unavailable (built without E2SAR)\n";
}

}  // namespace petsro

#endif  // PETSRO_HAVE_E2SAR

// ---------------------------------------------------------------------------
// Configuration validation, which needs no E2SAR and so is shared by both
// builds. Everything checked here is checked before a socket is opened or a
// receive thread is started.
// ---------------------------------------------------------------------------

namespace petsro {

std::string EjfatReceiverConfig::validate() const {
    if (uri.empty()) {
        return "uri is required and must not be empty";
    }
    if (recvIp.empty()) {
        return "recv-ip must not be empty";
    }
    if (recvPort == 0) {
        return "recv-port must be between 1 and 65535";
    }
    if (recvThreads == 0) {
        return "recv-threads must be at least 1";
    }
    if (eventTimeoutMs <= 0) {
        return "event-timeout must be greater than 0";
    }
    if (pollTimeoutMs <= 0) {
        return "poll-timeout must be greater than 0";
    }
    if (statsIntervalSeconds < 0.0) {
        return "stats-interval must not be negative";
    }
    if (verbose && quiet) {
        return "verbose and quiet are mutually exclusive; enable at most one";
    }
    return {};
}

}  // namespace petsro
