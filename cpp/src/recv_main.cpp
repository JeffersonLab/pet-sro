// recv_main.cpp -- evio_ejfat_recv
//
// Receives EJFAT packets, reassembles them into EVIO events with the E2SAR
// Reassembler, and reports statistics about the EVIO events themselves: per
// source, per timestamp, with the integrity checks that show whether the
// stream survived the trip.
//
// Reference: the Reassembler setup in
//   /Users/gurjyan/Documents/Devel/e2sar-utils/src/ejfat_receiver_actor.cpp
// with the ERSAP/xMsg publishing replaced by EVIO decoding and accounting.
//
// This translation unit is compiled only when E2SAR is available.

#include "EvioEventView.hpp"
#include "Logging.hpp"
#include "SignalHandler.hpp"
#include "SroWireFormat.hpp"

#include <e2sar.hpp>

#include <boost/asio.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace po = boost::program_options;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string uri;
    std::string recvIp = "127.0.0.1";
    std::uint16_t recvPort = 10000;
    std::size_t recvThreads = 1;
    int eventTimeoutMs = 500;
    int pollTimeoutMs = 1000;
    bool withCp = false;
    bool validateCert = true;
    double statsIntervalSeconds = 5.0;
    std::uint64_t maxEvents = 0;  ///< 0 means run until Ctrl-C
    bool verbose = false;
    bool quiet = false;
};

/// What one EJFAT source (one dataId, i.e. one input file on the sender) sent.
struct SourceStats {
    std::uint64_t events = 0;
    std::uint64_t bytes = 0;
    std::uint64_t malformed = 0;

    std::uint32_t rocid = 0;
    bool rocidStable = true;

    std::uint64_t firstTimestamp = 0;
    std::uint64_t lastTimestamp = 0;
    bool haveTimestamp = false;

    std::uint64_t timestampRegressions = 0;
    std::uint64_t frameCounterGaps = 0;
    std::uint64_t lastFrameCounter = 0;
    bool haveFrameCounter = false;

    std::size_t smallestEvent = 0;
    std::size_t largestEvent = 0;
};

struct ReceiveStats {
    std::uint64_t events = 0;
    std::uint64_t bytes = 0;
    std::uint64_t malformed = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t recvErrors = 0;
    std::map<std::uint16_t, SourceStats> perSource;
};

void accumulate(ReceiveStats& stats, std::uint16_t dataId, const std::uint8_t* buffer,
                std::size_t size) {
    stats.events++;
    stats.bytes += size;

    SourceStats& src = stats.perSource[dataId];
    src.events++;
    src.bytes += size;
    src.smallestEvent = (src.smallestEvent == 0) ? size : std::min(src.smallestEvent, size);
    src.largestEvent = std::max(src.largestEvent, size);

    const petsro::EvioEventView view = petsro::inspectEvioEvent(buffer, size);
    if (!view.valid) {
        stats.malformed++;
        src.malformed++;
        LOG_WARN << "dataId " << dataId << ": " << view.problem;
        return;
    }

    if (!src.haveTimestamp) {
        src.firstTimestamp = view.timestamp;
        src.rocid = view.rocid;
        src.haveTimestamp = true;
    } else if (view.timestamp < src.lastTimestamp) {
        // With rebasing enabled on the sender this should never happen, not
        // even across a replay-loop seam.
        src.timestampRegressions++;
        LOG_WARN << "dataId " << dataId << ": timestamp went backwards, " << src.lastTimestamp
                 << " -> " << view.timestamp;
    }
    src.lastTimestamp = view.timestamp;

    if (view.rocid != src.rocid) {
        src.rocidStable = false;
    }

    if (src.haveFrameCounter && view.frameCounter != src.lastFrameCounter + 1) {
        // Reassembly drops show up here as well as in the E2SAR loss counters,
        // and this view is the one that matches what a physics consumer sees.
        src.frameCounterGaps++;
    }
    src.lastFrameCounter = view.frameCounter;
    src.haveFrameCounter = true;

    LOG_DEBUG << "event dataId=" << dataId << " ts=" << view.timestamp
              << " fc=" << view.frameCounter << " rocid=" << view.rocid << " bytes=" << size;
}

void printProgress(const ReceiveStats& stats, double elapsed) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "events " << stats.events << " | sources " << stats.perSource.size() << " | "
        << (static_cast<double>(stats.bytes) / (1024.0 * 1024.0)) << " MiB";
    if (elapsed > 0.0) {
        oss << " | " << std::setprecision(1)
            << (static_cast<double>(stats.bytes) * 8.0 / 1.0e6) / elapsed << " Mbps"
            << " | " << (static_cast<double>(stats.events) / elapsed) << " evt/s";
    }
    if (stats.malformed > 0) {
        oss << " | malformed " << stats.malformed;
    }
    LOG_INFO << oss.str();
}

void printFinal(std::ostream& out, const ReceiveStats& stats, e2sar::Reassembler& reassembler,
                double elapsed) {
    out << "\n========== Reception Summary ==========\n";
    out << std::fixed << std::setprecision(3);
    out << "  Elapsed                   : " << elapsed << " s\n";
    out << "  EVIO events received      : " << stats.events << '\n';
    out << "  Payload bytes received    : " << stats.bytes << " (" << std::setprecision(2)
        << (static_cast<double>(stats.bytes) / (1024.0 * 1024.0)) << " MiB)\n";
    out << std::setprecision(3);
    out << "  Malformed EVIO events     : " << stats.malformed << '\n';
    out << "  Receive timeouts          : " << stats.timeouts << '\n';
    out << "  Receive errors            : " << stats.recvErrors << '\n';
    if (elapsed > 0.0) {
        out << "  Average rate              : " << std::setprecision(2)
            << (static_cast<double>(stats.bytes) * 8.0 / 1.0e6) / elapsed << " Mbps, "
            << (static_cast<double>(stats.events) / elapsed) << " events/s\n";
        out << std::setprecision(3);
    }

    out << "\n  Per EJFAT source (dataId):\n";
    for (const auto& entry : stats.perSource) {
        const SourceStats& s = entry.second;
        out << "    dataId=" << entry.first << " rocid=" << s.rocid
            << (s.rocidStable ? "" : " (UNSTABLE)") << '\n';
        out << "         events=" << s.events << " bytes=" << s.bytes
            << " malformed=" << s.malformed << '\n';
        out << "         event size min=" << s.smallestEvent << " max=" << s.largestEvent
            << " bytes\n";
        out << "         timestamp first=" << s.firstTimestamp << " last=" << s.lastTimestamp;
        if (s.haveTimestamp && s.lastTimestamp >= s.firstTimestamp) {
            out << " span=" << std::setprecision(3)
                << (static_cast<double>(s.lastTimestamp - s.firstTimestamp) / 1e9) << " s";
        }
        out << '\n';
        out << "         timestamp regressions=" << s.timestampRegressions
            << " frame-counter gaps=" << s.frameCounterGaps << '\n';
    }

    // E2SAR's own view, which counts what never made it up to us.
    const auto rs = reassembler.getStats();
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
        auto res = reassembler.get_LostEvent();
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

bool parseArgs(int argc, char* argv[], Options& opts, int& exitCode) {
    exitCode = 0;

    po::options_description desc("evio_ejfat_recv options");
    desc.add_options()
        ("help,h", "show this help message")
        ("uri,u", po::value<std::string>(&opts.uri),
         "EJFAT URI (required). Without --withcp only its data= address is used")
        ("recv-ip", po::value<std::string>(&opts.recvIp)->default_value("127.0.0.1"),
         "local IP address to listen on")
        ("recv-port", po::value<std::uint16_t>(&opts.recvPort)->default_value(10000),
         "starting UDP port to listen on; must match the sender's data= port")
        ("recv-threads", po::value<std::size_t>(&opts.recvThreads)->default_value(1),
         "number of reassembly threads")
        ("event-timeout", po::value<int>(&opts.eventTimeoutMs)->default_value(500),
         "milliseconds before an incomplete event is abandoned")
        ("poll-timeout", po::value<int>(&opts.pollTimeoutMs)->default_value(1000),
         "milliseconds recvEvent() waits before returning empty-handed")
        ("withcp,c", po::bool_switch(&opts.withCp)->default_value(false),
         "use the control plane; omit for direct send with no load balancer")
        ("novalidate,V", po::bool_switch()->default_value(false),
         "don't validate the control plane's SSL certificate")
        ("max-events", po::value<std::uint64_t>(&opts.maxEvents)->default_value(0),
         "stop after this many events; 0 means run until Ctrl-C")
        ("stats-interval", po::value<double>(&opts.statsIntervalSeconds)->default_value(5.0),
         "seconds between progress lines; 0 disables them")
        ("verbose,v", po::bool_switch(&opts.verbose)->default_value(false),
         "log one line per received event (very noisy)")
        ("quiet,q", po::bool_switch(&opts.quiet)->default_value(false),
         "log only warnings and errors");

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);

        if (vm.count("help") > 0) {
            std::cout
                << "evio_ejfat_recv -- reassemble EJFAT packets into EVIO events and report\n\n"
                << "Usage:\n  " << argv[0] << " --uri <ejfat_uri> [OPTIONS]\n\n"
                << desc << '\n'
                << "Example (no load balancer, loopback):\n"
                << "  " << argv[0]
                << " --uri 'ejfat://useless@127.0.0.1:9876/lb/1?sync=127.0.0.1:12345"
                   "&data=127.0.0.1' \\\n      --recv-ip 127.0.0.1 --recv-port 10000\n";
            return false;
        }

        po::notify(vm);
        opts.validateCert = !vm["novalidate"].as<bool>();

        if (opts.uri.empty()) {
            throw std::runtime_error("--uri is required");
        }
        if (opts.recvThreads == 0) {
            throw std::runtime_error("--recv-threads must be at least 1");
        }
        if (opts.eventTimeoutMs <= 0) {
            throw std::runtime_error("--event-timeout must be greater than 0");
        }
        if (opts.pollTimeoutMs <= 0) {
            throw std::runtime_error("--poll-timeout must be greater than 0");
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << desc << std::endl;
        exitCode = 2;
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options opts;
    int exitCode = 0;
    if (!parseArgs(argc, argv, opts, exitCode)) {
        return exitCode;
    }

    petsro::setLogLevel(opts.verbose ? petsro::LogLevel::Debug
                        : opts.quiet ? petsro::LogLevel::Warn
                                     : petsro::LogLevel::Info);

    if (!petsro::installSignalHandlers()) {
        std::cerr << "Warning: could not install signal handlers; Ctrl-C will not be clean\n";
    }

    auto uriResult =
        e2sar::EjfatURI::getFromString(opts.uri, e2sar::EjfatURI::TokenType::instance, false);
    if (uriResult.has_error()) {
        std::cerr << "Error: cannot parse EJFAT URI: " << uriResult.error().message() << '\n';
        return 1;
    }

    boost::asio::ip::address ip;
    try {
        ip = boost::asio::ip::make_address(opts.recvIp);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot parse --recv-ip '" << opts.recvIp << "': " << e.what()
                  << '\n';
        return 1;
    }

    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP = opts.withCp;
    // Without a control plane there is no load balancer to strip the LB header
    // the segmenter prepended, so it is still on the wire when it reaches us.
    rflags.withLBHeader = !opts.withCp;
    rflags.eventTimeout_ms = opts.eventTimeoutMs;
    if (opts.withCp) {
        rflags.validateCert = opts.validateCert;
    }

    std::unique_ptr<e2sar::Reassembler> reassembler;
    try {
        reassembler = std::make_unique<e2sar::Reassembler>(uriResult.value(), ip, opts.recvPort,
                                                          opts.recvThreads, rflags);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot construct Reassembler: " << e.what() << '\n';
        return 1;
    }

    bool workerRegistered = false;
    if (opts.withCp) {
        auto hostname = e2sar::NetUtil::getHostName();
        if (!hostname.has_error()) {
            auto reg = reassembler->registerWorker(hostname.value());
            if (reg.has_error()) {
                std::cerr << "Warning: registerWorker failed: " << reg.error().message() << '\n';
            } else {
                workerRegistered = true;
            }
        }
    }

    auto openResult = reassembler->openAndStart();
    if (openResult.has_error()) {
        std::cerr << "Error: cannot start reassembler: " << openResult.error().message() << '\n';
        return 1;
    }

    LOG_INFO << "listening on " << reassembler->get_dataIP() << " ports "
             << reassembler->get_recvPorts().first << ":"
             << reassembler->get_recvPorts().second << " with "
             << reassembler->get_numRecvThreads() << " thread(s), control plane "
             << (opts.withCp ? "on" : "off (expecting the LB header on the wire)");
    LOG_INFO << "waiting for events; press Ctrl-C to stop";

    ReceiveStats stats;
    const auto start = Clock::now();
    auto lastProgress = start;

    while (!petsro::shutdownRequested()) {
        std::uint8_t* buffer = nullptr;
        std::size_t size = 0;
        e2sar::EventNum_t eventNum = 0;
        std::uint16_t dataId = 0;

        // recvEvent takes the timeout as an unsigned quantity; the option is
        // validated positive at parse time.
        auto result = reassembler->recvEvent(&buffer, &size, &eventNum, &dataId,
                                             static_cast<std::uint64_t>(opts.pollTimeoutMs));

        if (result.has_error()) {
            stats.recvErrors++;
        } else if (result.value() == -1) {
            stats.timeouts++;  // nothing arrived within the poll window
        } else {
            accumulate(stats, dataId, buffer, size);
            // The Reassembler allocates with new[]; we own it from here.
            delete[] buffer;

            if (opts.maxEvents > 0 && stats.events >= opts.maxEvents) {
                LOG_INFO << "reached --max-events " << opts.maxEvents;
                break;
            }
        }

        if (opts.statsIntervalSeconds > 0.0 &&
            std::chrono::duration<double>(Clock::now() - lastProgress).count() >=
                opts.statsIntervalSeconds) {
            printProgress(stats, std::chrono::duration<double>(Clock::now() - start).count());
            lastProgress = Clock::now();
        }
    }

    if (petsro::shutdownRequested()) {
        LOG_INFO << "shutting down cleanly (signal " << petsro::shutdownSignal() << ")";
    }

    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    printFinal(std::cout, stats, *reassembler, elapsed);

    if (workerRegistered) {
        auto dereg = reassembler->deregisterWorker();
        if (dereg.has_error()) {
            std::cerr << "Warning: deregisterWorker failed: " << dereg.error().message() << '\n';
        }
    }
    reassembler->stopThreads();

    return (stats.malformed > 0) ? 1 : 0;
}
