// main.cpp -- evio_ejfat_replay
//
// Replays N EVIO capture files through an EJFAT load balancer, forever, in
// timestamp-synchronized groups of N events.
//
// The EJFAT options mirror those of e2sar-utils/src/e2sar_root.cpp so a working
// invocation of that program translates directly.

#include "EjfatSender.hpp"
#include "EvioFileReader.hpp"
#include "Logging.hpp"
#include "PacketSink.hpp"
#include "ReplayLoop.hpp"
#include "SignalHandler.hpp"

#include <boost/program_options.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace po = boost::program_options;

namespace {

struct Options {
    std::size_t fileCount = 0;
    std::vector<std::string> files;

    petsro::EjfatConfig ejfat;
    petsro::ReplayLoopConfig loop;

    bool dryRun = false;
    std::uint16_t dryRunMtu = 1500;
    bool verbose = false;
    bool quiet = false;
    /// Inverted into ReplayLoopConfig::rebaseTimestamps, which defaults to on.
    bool noRebase = false;
};

/// Same headers E2SAR accounts for when it computes its maximum payload:
/// IP (20) + UDP (8) + LB (16) + RE (20). Used only by --dry-run, so its packet
/// count matches what a real send would produce.
constexpr std::size_t EJFAT_TOTAL_HDR_LEN = 20 + 8 + 16 + 20;

void printUsage(const char* argv0, const po::options_description& desc) {
    std::cout
        << "evio_ejfat_replay -- replay EVIO captures through an EJFAT load balancer\n\n"
        << "Usage:\n"
        << "  " << argv0 << " --file-count N --file f1.evio --file f2.evio ... "
        << "--uri <ejfat_uri> [OPTIONS]\n"
        << "  " << argv0 << " --file-count N [OPTIONS] --uri <ejfat_uri> f1.evio f2.evio ...\n\n"
        << "Files may be given with repeated --file options or positionally; the two forms\n"
        << "may be mixed, and the total must equal --file-count.\n\n"
        << desc << '\n'
        << "Examples:\n"
        << "  Send two streams to a load balancer:\n"
        << "    " << argv0 << " --file-count 2 --uri ejfat://token@cp:18008/lb/1 \\\n"
        << "        --file evio_10.0.0.1.bin --file evio_10.0.0.2.bin\n\n"
        << "  Same, with jumbo frames, control plane and a 5 Gbps cap:\n"
        << "    " << argv0 << " --file-count 2 --uri ejfat://... --mtu 9000 --withcp "
        << "--rate 5.0 \\\n        f1.bin f2.bin\n\n"
        << "  Exercise the reader and synchronizer with no load balancer:\n"
        << "    " << argv0 << " --file-count 2 --dry-run --loop-limit 1 -v f1.bin f2.bin\n";
}

/// Returns false if the program should exit; `exitCode` says with what.
bool parseArgs(int argc, char* argv[], Options& opts, int& exitCode) {
    exitCode = 0;

    po::options_description input("Input");
    input.add_options()
        ("file-count,n", po::value<std::size_t>(&opts.fileCount),
         "number of EVIO input files (required); must equal the number of files supplied")
        ("file,f", po::value<std::vector<std::string>>(&opts.files)->composing(),
         "an EVIO input file; repeat once per stream (may also be given positionally)");

    po::options_description ejfat("EJFAT / E2SAR");
    ejfat.add_options()
        ("uri,u", po::value<std::string>(&opts.ejfat.uri),
         "EJFAT URI of the load balancer (required unless --dry-run)")
        ("dataid-base", po::value<std::uint16_t>(&opts.loop.dataIdBase)->default_value(1),
         "EJFAT dataId of the first input file; file i gets dataid-base + i")
        ("eventsrcid", po::value<std::uint32_t>(&opts.ejfat.eventSrcId)->default_value(1),
         "EJFAT event source id, carried in the Sync header")
        ("mtu", po::value<std::uint16_t>(&opts.ejfat.mtu)->default_value(1500),
         "MTU in bytes for the segmenter (576..9000)")
        ("rate", po::value<float>(&opts.ejfat.rateGbps)->default_value(1.0F),
         "send rate in Gbps; negative means no limit")
        ("sockets", po::value<std::size_t>(&opts.ejfat.numSendSockets)->default_value(4),
         "number of UDP send sockets the segmenter opens")
        ("withcp,c", po::bool_switch(&opts.ejfat.withCp)->default_value(false),
         "enable control plane interactions (register this sender with the LB)")
        ("novalidate,V", po::bool_switch()->default_value(false),
         "don't validate the control plane's SSL certificate")
        ("async", po::bool_switch(&opts.ejfat.async)->default_value(false),
         "queue events asynchronously (copies each event) instead of sending inline")
        ("entropy-per-source", po::bool_switch(&opts.loop.entropyPerSource)->default_value(false),
         "set LB entropy to 1 + file index instead of letting E2SAR randomise it");

    po::options_description replay("Replay");
    replay.add_options()
        ("loop-limit", po::value<std::uint64_t>(&opts.loop.loopLimit)->default_value(0),
         "stop after this many complete replay loops; 0 means run until Ctrl-C")
        ("group-delay-us", po::value<std::uint64_t>(&opts.loop.groupDelayUs)->default_value(0),
         "microseconds to pause between synchronized groups")
        ("reset-event-numbers", po::bool_switch(&opts.loop.resetEventNumbers)->default_value(false),
         "restart EJFAT event numbers at 1 on each replay loop (default: keep increasing)")
        ("no-rebase-timestamps", po::bool_switch(&opts.noRebase)->default_value(false),
         "replay captured timestamps verbatim; by default each loop adds the replayed "
         "span plus one frame period so time never goes backwards")
        ("dry-run", po::bool_switch(&opts.dryRun)->default_value(false),
         "read and synchronize but send nothing; no load balancer needed")
        ("dry-run-mtu", po::value<std::uint16_t>(&opts.dryRunMtu)->default_value(1500),
         "MTU used for --dry-run packet accounting");

    po::options_description reporting("Reporting");
    reporting.add_options()
        ("help,h", "show this help message")
        ("stats-interval", po::value<double>(&opts.loop.statsIntervalSeconds)->default_value(5.0),
         "seconds between progress lines; 0 disables them")
        ("verbose,v", po::bool_switch(&opts.verbose)->default_value(false),
         "log per-event, per-packet and per-mismatch detail (very noisy)")
        ("quiet,q", po::bool_switch(&opts.quiet)->default_value(false),
         "log only warnings and errors");

    po::options_description desc;
    desc.add(input).add(ejfat).add(replay).add(reporting);

    po::positional_options_description positional;
    positional.add("file", -1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv)
                      .options(desc)
                      .positional(positional)
                      .run(),
                  vm);

        if (vm.count("help") > 0) {
            printUsage(argv[0], desc);
            return false;
        }

        po::notify(vm);

        opts.ejfat.validateCert = !vm["novalidate"].as<bool>();
        opts.ejfat.dataId = opts.loop.dataIdBase;
        opts.loop.rebaseTimestamps = !opts.noRebase;

        if (vm.count("file-count") == 0) {
            throw std::runtime_error("--file-count is required");
        }
        if (opts.fileCount == 0) {
            throw std::runtime_error("--file-count must be at least 1");
        }
        if (opts.files.size() != opts.fileCount) {
            throw std::runtime_error("--file-count is " + std::to_string(opts.fileCount) +
                                     " but " + std::to_string(opts.files.size()) +
                                     " file(s) were supplied");
        }
        if (!opts.dryRun && opts.ejfat.uri.empty()) {
            throw std::runtime_error("--uri is required unless --dry-run is given");
        }
        if (opts.ejfat.mtu != 0 && (opts.ejfat.mtu < 576 || opts.ejfat.mtu > 9000)) {
            throw std::runtime_error("--mtu must be 0 (auto-detect) or between 576 and 9000");
        }
        if (opts.ejfat.numSendSockets == 0) {
            throw std::runtime_error("--sockets must be at least 1");
        }
        // dataId is 16 bits and file i takes dataid-base + i.
        if (static_cast<std::size_t>(opts.loop.dataIdBase) + opts.fileCount - 1 > 0xFFFFU) {
            throw std::runtime_error("--dataid-base plus --file-count overflows the 16-bit "
                                     "EJFAT dataId field");
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        printUsage(argv[0], desc);
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

    // --- transport -------------------------------------------------------
    // Two owners, one borrowed pointer: EjfatSender is kept in its own
    // unique_ptr because the final report needs its E2SAR-specific accessors.
    std::unique_ptr<petsro::NullPacketSink> nullSink;
    std::unique_ptr<petsro::EjfatSender> ejfatSender;
    petsro::PacketSink* activeSink = nullptr;

    if (opts.dryRun) {
        const std::size_t maxPld = opts.dryRunMtu > EJFAT_TOTAL_HDR_LEN
                                       ? opts.dryRunMtu - EJFAT_TOTAL_HDR_LEN
                                       : 1;
        nullSink = std::make_unique<petsro::NullPacketSink>(maxPld);
        activeSink = nullSink.get();
        LOG_INFO << "dry run: nothing will be sent (max payload " << maxPld << " bytes)";
    } else {
        std::string error;
        ejfatSender = petsro::EjfatSender::create(opts.ejfat, error);
        if (!ejfatSender) {
            std::cerr << "Error: " << error << '\n';
            return 1;
        }
        activeSink = ejfatSender.get();
    }

    // --- readers ---------------------------------------------------------
    std::vector<std::unique_ptr<petsro::EvioFileReader>> readers;
    readers.reserve(opts.files.size());
    for (std::size_t i = 0; i < opts.files.size(); ++i) {
        readers.push_back(std::make_unique<petsro::EvioFileReader>(opts.files[i]));
        LOG_INFO << "stream " << i << " -> dataId "
                 << (opts.loop.dataIdBase + static_cast<std::uint16_t>(i)) << ": "
                 << opts.files[i];
    }

    // --- run -------------------------------------------------------------
    const auto start = std::chrono::steady_clock::now();

    petsro::ReplayLoop loop(std::move(readers), *activeSink, opts.loop);
    const bool ok = loop.run(petsro::shutdownFlag());

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start).count();

    if (petsro::shutdownRequested()) {
        LOG_INFO << "shutting down cleanly (signal " << petsro::shutdownSignal() << ")";
    }

    loop.stats().printFinal(std::cout, elapsed);

    if (ejfatSender) {
        std::cout << "  EJFAT destination         : " << ejfatSender->destination() << '\n';
        ejfatSender->reportSegmenterStats(std::cout);
        std::cout << std::endl;
    }

    if (!ok) {
        std::cerr << "Replay stopped on error: " << loop.lastError() << '\n';
        return 1;
    }
    return 0;
}
