// recv_main.cpp -- evio_ejfat_recv
//
// Receives EJFAT packets, reassembles them into EVIO events with the E2SAR
// Reassembler, and reports statistics about the EVIO events themselves: per
// source, per timestamp, with the integrity checks that show whether the
// stream survived the trip.
//
// The protocol setup, the per-event accounting and the summary printing now
// live in EjfatReceiver and ReceiveStats, which the ERSAP actor in src/actor
// drives as well. What remains here is the command line and the loop.
//
// This translation unit is compiled only when E2SAR is available.

#include "EjfatReceiver.hpp"
#include "Logging.hpp"
#include "ReceiveStats.hpp"
#include "SignalHandler.hpp"

#include <boost/program_options.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace po = boost::program_options;

namespace {

using Clock = std::chrono::steady_clock;

bool parseArgs(int argc, char* argv[], petsro::EjfatReceiverConfig& opts, int& exitCode) {
    exitCode = 0;

    // Defaults come from a default-constructed EjfatReceiverConfig, so the
    // executable and the ERSAP actor cannot drift apart.
    const petsro::EjfatReceiverConfig defaults;

    po::options_description desc("evio_ejfat_recv options");
    desc.add_options()
        ("help,h", "show this help message")
        ("uri,u", po::value<std::string>(&opts.uri),
         "EJFAT URI (required). Without --withcp only its data= address is used")
        ("recv-ip", po::value<std::string>(&opts.recvIp)->default_value(defaults.recvIp),
         "local IP address to listen on")
        ("recv-port", po::value<std::uint16_t>(&opts.recvPort)->default_value(defaults.recvPort),
         "starting UDP port to listen on; must match the sender's data= port")
        ("recv-threads",
         po::value<std::size_t>(&opts.recvThreads)->default_value(defaults.recvThreads),
         "number of reassembly threads")
        ("event-timeout",
         po::value<int>(&opts.eventTimeoutMs)->default_value(defaults.eventTimeoutMs),
         "milliseconds before an incomplete event is abandoned")
        ("poll-timeout",
         po::value<int>(&opts.pollTimeoutMs)->default_value(defaults.pollTimeoutMs),
         "milliseconds recvEvent() waits before returning empty-handed")
        ("withcp,c", po::bool_switch(&opts.withCp)->default_value(defaults.withCp),
         "use the control plane; omit for direct send with no load balancer")
        ("novalidate,V", po::bool_switch()->default_value(false),
         "don't validate the control plane's SSL certificate")
        ("max-events", po::value<std::uint64_t>(&opts.maxEvents)->default_value(defaults.maxEvents),
         "stop after this many events; 0 means run until Ctrl-C")
        ("stats-interval",
         po::value<double>(&opts.statsIntervalSeconds)
             ->default_value(defaults.statsIntervalSeconds),
         "seconds between progress lines; 0 disables them")
        ("verbose,v", po::bool_switch(&opts.verbose)->default_value(defaults.verbose),
         "log one line per received event (very noisy)")
        ("quiet,q", po::bool_switch(&opts.quiet)->default_value(defaults.quiet),
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

        const std::string problem = opts.validate();
        if (!problem.empty()) {
            throw std::runtime_error(problem);
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
    petsro::EjfatReceiverConfig opts;
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

    std::string error;
    auto receiver = petsro::EjfatReceiver::create(opts, error);
    if (!receiver) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }

    LOG_INFO << "listening on " << receiver->describeEndpoint();
    LOG_INFO << "waiting for events; press Ctrl-C to stop";

    petsro::ReceiveStats& stats = receiver->stats();
    const auto start = Clock::now();
    auto lastProgress = start;

    while (!petsro::shutdownRequested()) {
        petsro::ReassembledEvent event;
        const petsro::ReceiveOutcome outcome = receiver->receive(event, error);

        if (outcome == petsro::ReceiveOutcome::Event) {
            std::string problem;
            // Structural is what the executable has always applied: a decode
            // failure is malformed, a regression or a gap is reported and
            // counted but does not condemn the event.
            const bool ok = stats.accumulate(event.dataId(), event.data(), event.size(),
                                             petsro::ValidationLevel::Structural, problem);
            if (!ok || !problem.empty()) {
                LOG_WARN << "dataId " << event.dataId() << ": " << problem;
            }
            LOG_DEBUG << "event dataId=" << event.dataId() << " num=" << event.eventNumber()
                      << " bytes=" << event.size();
            // event's destructor frees the buffer the Reassembler allocated.

            if (opts.maxEvents > 0 && stats.events >= opts.maxEvents) {
                LOG_INFO << "reached --max-events " << opts.maxEvents;
                break;
            }
        } else if (outcome == petsro::ReceiveOutcome::Error) {
            LOG_DEBUG << "receive error: " << error;
        }
        // A Timeout is not an error and is counted inside receive().

        if (opts.statsIntervalSeconds > 0.0 &&
            std::chrono::duration<double>(Clock::now() - lastProgress).count() >=
                opts.statsIntervalSeconds) {
            LOG_INFO << stats.progressLine(
                std::chrono::duration<double>(Clock::now() - start).count());
            lastProgress = Clock::now();
        }
    }

    if (petsro::shutdownRequested()) {
        LOG_INFO << "shutting down cleanly (signal " << petsro::shutdownSignal() << ")";
    }

    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    stats.printFinal(std::cout, elapsed);
    receiver->reportTransport(std::cout);

    const std::uint64_t malformed = stats.malformed;
    receiver->stop();

    return (malformed > 0) ? 1 : 0;
}
