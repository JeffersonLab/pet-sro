/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * Implementation of the EJFAT receiver ERSAP actor.
 *
 * What is published
 * -----------------
 * The E2SAR Reassembler returns the segmenter's original event payload with
 * every transport header (UDP, LB, RE) already removed. For this project's
 * sender -- ReplayLoop::sendGroup(), one OutgoingEvent per EvioEvent -- that
 * payload is exactly one EVIO block in big-endian wire form, block-length word
 * included. This actor forwards those bytes unchanged. It adds no length
 * prefix, no envelope and no metadata to the payload; the EJFAT event number
 * travels in the ERSAP communication id instead, where it costs the payload
 * nothing.
 *
 * Byte order on the wire to Java
 * ------------------------------
 * ERSAP carries the byte order in xMsgMeta.byteOrder, which is a proto2
 * `optional` with no explicit default over an enum that declares `Little = 1`
 * first -- so an unset field reads back as Little. ersap-cpp never sets it, and
 * Java's DataUtil.deserialize() therefore calls
 * `bb.order(ByteOrder.LITTLE_ENDIAN)` on every buffer a C++ actor publishes.
 * The bytes are right; only the ByteBuffer's order flag is wrong for a
 * big-endian EVIO payload, and `EngineData::meta_` is private, so this actor
 * has no way to correct it from here.
 *
 * That is why the default output type is binary/data-evio rather than a plain
 * raw-bytes type: its Java counterpart, EvioBlockDataType, restores
 * BIG_ENDIAN in the deserializer, where no processing actor can forget it.
 *
 * Why not binary/coda-time-frame
 * ------------------------------
 * CodaTimeFrameBinaryDataType carries a decoded object (time frames -> ROC
 * banks -> FADC hits), not EVIO bytes. Producing one from a FEB SRO block
 * requires the bit layout of the TDC hit words at word 24 and beyond, which is
 * documented only in org.jlab.detimg.petiroc (PetirocJava); that tree is not
 * present in this checkout, and SroWireFormat.hpp names the hit words without
 * decoding them. Rather than invent a mapping, configure() rejects that MIME
 * type with an explanation. See cpp/src/actor/README.md.
 *
 * @author gurjyan
 * @project pet-sro
 */

#include "EjfatReceiverActor.hpp"

#include <ersap/any.hpp>
#include <ersap/serializer.hpp>
#include <ersap/stdlib/json_utils.hpp>
#include <ersap/third_party/json11.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>

extern "C" std::unique_ptr<ersap::Engine> create_engine() {
    return std::make_unique<petsro::actor::EjfatReceiverActor>();
}

namespace petsro {
namespace actor {

const char* const MIME_EVIO_BLOCK = "binary/data-evio";
const char* const MIME_JOBJ = "binary/data-jobj";
const char* const MIME_BYTES = "binary/bytes";
const char* const MIME_CODA_TIME_FRAME = "binary/coda-time-frame";

namespace {

using Clock = std::chrono::steady_clock;

/// Raw byte payload, transported verbatim.
///
/// Deliberately identical to ersap-cpp's RawBytesSerializer and to the Java
/// EngineDataType.BYTES serializer that JavaObjectType.JOBJ reuses: the wire
/// image is the byte sequence itself, with no framing of any kind. That is what
/// makes "binary/data-jobj" from this actor readable by a Java actor declaring
/// JavaObjectType.JOBJ without a single byte of translation.
class RawPayloadSerializer final : public ersap::Serializer {
  public:
    std::vector<std::uint8_t> write(const ersap::any& data) const override {
        return ersap::any_cast<std::vector<std::uint8_t>>(data);
    }

    std::vector<std::uint8_t> write(ersap::any&& data) const override {
        // Moves the payload out of the EngineData instead of copying it.
        return ersap::any_cast<std::vector<std::uint8_t>>(std::move(data));
    }

    ersap::any read(const std::vector<std::uint8_t>& buffer) const override {
        return ersap::any{buffer};
    }

    ersap::any read(std::vector<std::uint8_t>&& buffer) const override {
        return ersap::any{std::move(buffer)};
    }
};

/// The default data type: one big-endian EVIO block. A function-local static so
/// that its lifetime covers every published event and no static-initialisation
/// order matters.
const ersap::EngineDataType& evioBlockType() {
    static const ersap::EngineDataType type{MIME_EVIO_BLOCK,
                                            std::make_unique<RawPayloadSerializer>()};
    return type;
}

/// The JOBJ-compatible data type, for a Java chain that already declares
/// JavaObjectType.JOBJ and corrects the byte order itself.
const ersap::EngineDataType& jobjType() {
    static const ersap::EngineDataType type{MIME_JOBJ,
                                            std::make_unique<RawPayloadSerializer>()};
    return type;
}

/// ersap::type::BYTES under its own name, for chaining behind a native actor.
const ersap::EngineDataType& bytesType() {
    static const ersap::EngineDataType type{MIME_BYTES,
                                            std::make_unique<RawPayloadSerializer>()};
    return type;
}

/// Every key configure() understands. Anything else is reported, so a typo in
/// the YAML is visible immediately rather than silently ignored.
const char* const KNOWN_KEYS[] = {
    "uri",          "recv-ip",     "recv-port",      "recv-threads", "event-timeout",
    "poll-timeout", "withcp",      "novalidate",     "max-events",   "stats-interval",
    "verbose",      "quiet",       "validation",     "output-mime",  "queue-size",
};

bool isKnownKey(const std::string& key) {
    for (const char* known : KNOWN_KEYS) {
        if (key == known) {
            return true;
        }
    }
    return false;
}

/// Reads a JSON boolean, insisting on the right type rather than silently
/// treating a string "true" as false.
bool readBool(const json11::Json& cfg, const char* key, bool current, std::string& problem) {
    const json11::Json& v = cfg[key];
    if (v.is_null()) {
        return current;
    }
    if (!v.is_bool()) {
        problem = std::string(key) + " must be a boolean";
        return current;
    }
    return v.bool_value();
}

/// Reads a JSON number, with an inclusive range check. `lo`/`hi` are doubles so
/// one helper serves the 16-bit port, the 64-bit event cap and the interval.
double readNumber(const json11::Json& cfg, const char* key, double current, double lo, double hi,
                  std::string& problem) {
    const json11::Json& v = cfg[key];
    if (v.is_null()) {
        return current;
    }
    if (!v.is_number()) {
        problem = std::string(key) + " must be a number";
        return current;
    }
    const double n = v.number_value();
    if (n < lo || n > hi) {
        std::ostringstream oss;
        oss << key << " must be between " << lo << " and " << hi;
        problem = oss.str();
        return current;
    }
    return n;
}

std::string readString(const json11::Json& cfg, const char* key, const std::string& current,
                       std::string& problem) {
    const json11::Json& v = cfg[key];
    if (v.is_null()) {
        return current;
    }
    if (!v.is_string()) {
        problem = std::string(key) + " must be a string";
        return current;
    }
    return v.string_value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

EjfatReceiverActor::~EjfatReceiverActor() { shutdown(); }

std::string EjfatReceiverActor::configurationHelp() {
    std::ostringstream oss;
    oss << "EjfatReceiverActor configuration keys (all optional except uri):\n"
        << "  uri            string   (required)   EJFAT URI. Without withcp only its\n"
        << "                                       data= address is used.\n"
        << "  recv-ip        string   127.0.0.1    local IP address to listen on\n"
        << "  recv-port      integer  10000        starting UDP port; must match the\n"
        << "                                       sender's data= port\n"
        << "  recv-threads   integer  1            number of reassembly threads (>= 1)\n"
        << "  event-timeout  integer  500          ms before an incomplete event is\n"
        << "                                       abandoned (> 0)\n"
        << "  poll-timeout   integer  1000         ms recvEvent() waits before returning\n"
        << "                                       without an event (> 0)\n"
        << "  withcp         boolean  false        use the EJFAT control plane\n"
        << "  novalidate     boolean  false        skip control-plane SSL certificate\n"
        << "                                       validation; applies only with withcp\n"
        << "  max-events     integer  0            stop after this many complete events;\n"
        << "                                       0 means run until the actor is stopped\n"
        << "  stats-interval integer  5            seconds between progress messages;\n"
        << "                                       0 disables them\n"
        << "  verbose        boolean  false        log one line per received event\n"
        << "  quiet          boolean  false        log only warnings and errors;\n"
        << "                                       mutually exclusive with verbose\n"
        << "\n"
        << "  Actor-specific keys, not present in evio_ejfat_recv:\n"
        << "  validation     string   structural   payload validation depth:\n"
        << "                                       none | structural | strict\n"
        << "  output-mime    string   " << MIME_EVIO_BLOCK << "  published ERSAP data type:\n"
        << "                                       " << MIME_EVIO_BLOCK << " (default; its Java\n"
        << "                                       counterpart restores BIG_ENDIAN)\n"
        << "                                       " << MIME_JOBJ << " | " << MIME_BYTES
        << '\n'
        << "  queue-size     integer  256          events buffered between the receive\n"
        << "                                       thread and execute() (>= 1)\n";
    return oss.str();
}

ersap::EngineData EjfatReceiverActor::configure(ersap::EngineData& input) {
    ersap::EngineData output;

    // A reconfigure must not leave the previous receiver running.
    shutdown();

    // Start from the defaults the executable uses, so the two cannot drift.
    receiverConfig_ = EjfatReceiverConfig{};
    validation_ = ValidationLevel::Structural;
    outputMime_ = MIME_EVIO_BLOCK;
    queueSize_ = 256;

    auto fail = [&output](const std::string& reason) {
        const std::string message = "EjfatReceiverActor configuration error: " + reason;
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description(message);
        std::cerr << message << "\n\n" << configurationHelp() << std::endl;
        return output;
    };

    if (input.mime_type() != ersap::type::JSON.mime_type()) {
        return fail("expected a JSON configuration, got mime type '" + input.mime_type() + "'");
    }

    json11::Json cfg;
    try {
        cfg = ersap::stdlib::parse_json(input);
    } catch (const std::exception& e) {
        return fail(std::string("cannot parse JSON: ") + e.what());
    }

    if (!cfg.is_object()) {
        return fail("the configuration must be a JSON object");
    }

    for (const auto& entry : cfg.object_items()) {
        if (!isKnownKey(entry.first)) {
            std::cerr << "EjfatReceiverActor: ignoring unknown configuration key '"
                      << entry.first << "'" << std::endl;
        }
    }

    std::string problem;
    auto note = [&problem](const std::string& p) {
        if (problem.empty() && !p.empty()) {
            problem = p;
        }
    };

    std::string field;

    receiverConfig_.uri = readString(cfg, "uri", receiverConfig_.uri, field);
    note(field);
    field.clear();

    receiverConfig_.recvIp = readString(cfg, "recv-ip", receiverConfig_.recvIp, field);
    note(field);
    field.clear();

    receiverConfig_.recvPort = static_cast<std::uint16_t>(
        readNumber(cfg, "recv-port", static_cast<double>(receiverConfig_.recvPort), 1.0,
                   65535.0, field));
    note(field);
    field.clear();

    receiverConfig_.recvThreads = static_cast<std::size_t>(readNumber(
        cfg, "recv-threads", static_cast<double>(receiverConfig_.recvThreads), 1.0, 1024.0,
        field));
    note(field);
    field.clear();

    receiverConfig_.eventTimeoutMs = static_cast<int>(readNumber(
        cfg, "event-timeout", static_cast<double>(receiverConfig_.eventTimeoutMs), 1.0,
        3600000.0, field));
    note(field);
    field.clear();

    receiverConfig_.pollTimeoutMs = static_cast<int>(
        readNumber(cfg, "poll-timeout", static_cast<double>(receiverConfig_.pollTimeoutMs),
                   1.0, 3600000.0, field));
    note(field);
    field.clear();

    receiverConfig_.withCp = readBool(cfg, "withcp", receiverConfig_.withCp, field);
    note(field);
    field.clear();

    // novalidate is the inverse of the receiver's validateCert, exactly as the
    // executable's --novalidate switch is, and it only reaches E2SAR when the
    // control plane is in use.
    const bool novalidate = readBool(cfg, "novalidate", false, field);
    note(field);
    field.clear();
    receiverConfig_.validateCert = !novalidate;

    receiverConfig_.maxEvents = static_cast<std::uint64_t>(
        readNumber(cfg, "max-events", static_cast<double>(receiverConfig_.maxEvents), 0.0,
                   9.007199254740992e15, field));
    note(field);
    field.clear();

    receiverConfig_.statsIntervalSeconds = readNumber(
        cfg, "stats-interval", receiverConfig_.statsIntervalSeconds, 0.0, 86400.0, field);
    note(field);
    field.clear();

    receiverConfig_.verbose = readBool(cfg, "verbose", receiverConfig_.verbose, field);
    note(field);
    field.clear();

    receiverConfig_.quiet = readBool(cfg, "quiet", receiverConfig_.quiet, field);
    note(field);
    field.clear();

    const std::string validationText =
        readString(cfg, "validation", toString(validation_), field);
    note(field);
    field.clear();
    if (!parseValidationLevel(validationText, validation_)) {
        note("validation must be one of none, structural, strict (got '" + validationText +
             "')");
    }

    outputMime_ = readString(cfg, "output-mime", outputMime_, field);
    note(field);
    field.clear();

    queueSize_ = static_cast<std::size_t>(readNumber(
        cfg, "queue-size", static_cast<double>(queueSize_), 1.0, 1000000.0, field));
    note(field);
    field.clear();

    if (!problem.empty()) {
        return fail(problem);
    }

    if (outputMime_ == MIME_CODA_TIME_FRAME) {
        return fail(
            "output-mime '" + std::string(MIME_CODA_TIME_FRAME) +
            "' is not supported: that type carries decoded FADC hits, and the bit layout of "
            "the FEB TDC hit words needed to build them is not defined anywhere in this "
            "repository. Publish " + std::string(MIME_JOBJ) +
            " and decode downstream, or supply the hit decoder first");
    }
    if (outputMime_ != MIME_EVIO_BLOCK && outputMime_ != MIME_JOBJ &&
        outputMime_ != MIME_BYTES) {
        return fail("output-mime must be one of '" + std::string(MIME_EVIO_BLOCK) + "', '" +
                    std::string(MIME_JOBJ) + "' or '" + std::string(MIME_BYTES) + "' (got '" +
                    outputMime_ + "')");
    }

    // Everything the receiver itself insists on: uri present, positive
    // timeouts, at least one thread, verbose and quiet not both set.
    const std::string receiverProblem = receiverConfig_.validate();
    if (!receiverProblem.empty()) {
        return fail(receiverProblem);
    }

    std::string error;
    receiver_ = EjfatReceiver::create(receiverConfig_, error);
    if (!receiver_) {
        return fail(error);
    }

    published_ = 0;
    dropped_ = 0;
    starved_ = 0;
    malformed_ = 0;
    startTime_ = Clock::now();

    stopRequested_ = false;
    running_ = true;
    try {
        pumpThread_ = std::thread(&EjfatReceiverActor::pump, this);
    } catch (const std::exception& e) {
        running_ = false;
        receiver_.reset();
        return fail(std::string("cannot start the receive thread: ") + e.what());
    }

    if (!receiverConfig_.quiet) {
        std::cout << "EjfatReceiverActor configured:"
                  << "\n  uri            = " << receiverConfig_.uri
                  << "\n  recv-ip        = " << receiverConfig_.recvIp
                  << "\n  recv-port      = " << receiverConfig_.recvPort
                  << "\n  recv-threads   = " << receiverConfig_.recvThreads
                  << "\n  event-timeout  = " << receiverConfig_.eventTimeoutMs << " ms"
                  << "\n  poll-timeout   = " << receiverConfig_.pollTimeoutMs << " ms"
                  << "\n  withcp         = " << (receiverConfig_.withCp ? "true" : "false")
                  << "\n  novalidate     = " << (receiverConfig_.validateCert ? "false" : "true")
                  << "\n  max-events     = " << receiverConfig_.maxEvents
                  << "\n  stats-interval = " << receiverConfig_.statsIntervalSeconds << " s"
                  << "\n  verbose        = " << (receiverConfig_.verbose ? "true" : "false")
                  << "\n  quiet          = " << (receiverConfig_.quiet ? "true" : "false")
                  << "\n  validation     = " << toString(validation_)
                  << "\n  output-mime    = " << outputMime_
                  << "\n  queue-size     = " << queueSize_
                  << "\n  listening on   = " << receiver_->describeEndpoint() << std::endl;
    }

    return output;
}

void EjfatReceiverActor::shutdown() noexcept {
    stopRequested_ = true;
    queueReady_.notify_all();

    if (pumpThread_.joinable()) {
        try {
            pumpThread_.join();
        } catch (...) {
            // Nothing useful to do; this runs from the destructor.
        }
    }
    running_ = false;

    if (receiver_) {
        try {
            const double elapsed =
                std::chrono::duration<double>(Clock::now() - startTime_).count();
            if (!receiverConfig_.quiet) {
                receiver_->stats().printFinal(std::cout, elapsed);
                std::cout << "  Events published          : " << published_.load() << '\n'
                          << "  Events dropped (queue)    : " << dropped_.load() << '\n'
                          << "  execute() came up empty   : " << starved_.load() << '\n';
                receiver_->reportTransport(std::cout);
            }
            receiver_->stop();
        } catch (...) {
            // Reporting must never take the process down.
        }
        receiver_.reset();
    }

    try {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    } catch (...) {
        // shutdown() runs from the destructor and must not throw.
    }
}

void EjfatReceiverActor::reset() {
    // ERSAP resets a service between runs. Tear the receiver down so the next
    // configure() starts from a clean transport; leaving sockets bound would
    // make the next openAndStart() fail.
    shutdown();
}

// ---------------------------------------------------------------------------
// Receive thread
// ---------------------------------------------------------------------------

void EjfatReceiverActor::pump() {
    ReceiveStats& stats = receiver_->stats();
    auto lastProgress = Clock::now();

    // Never sit in recvEvent() longer than this, so a stop request is acted on
    // promptly however large poll-timeout is.
    const int pollStepMs = std::min(receiverConfig_.pollTimeoutMs, 200);

    while (!stopRequested_.load()) {
        ReassembledEvent event;
        std::string error;
        const ReceiveOutcome outcome = receiver_->receive(event, error, pollStepMs);

        if (outcome == ReceiveOutcome::Error) {
            // A transport error is counted inside receive(). Log sparsely: a
            // broken socket produces one of these per poll step.
            logRateLimited(0, 5.0, "EjfatReceiverActor: receive error: " + error);
        } else if (outcome == ReceiveOutcome::Event) {
            std::string problem;
            const bool ok = stats.accumulate(event.dataId(), event.data(), event.size(),
                                             validation_, problem);
            if (!ok) {
                malformed_++;
                logRateLimited(1, 1.0,
                               "EjfatReceiverActor: malformed payload from dataId " +
                                   std::to_string(event.dataId()) + ": " + problem);
                // Malformed events are dropped rather than published: a
                // downstream EVIO decoder cannot do anything useful with them,
                // and the counters above record that they arrived.
            } else {
                if (!problem.empty()) {
                    logRateLimited(2, 1.0, "EjfatReceiverActor: dataId " +
                                               std::to_string(event.dataId()) + ": " + problem);
                }

                QueuedEvent queued;
                // The Reassembler hands back memory it allocated with new[],
                // which no standard container can adopt, so this is the one
                // unavoidable copy on the path. Everything after it moves.
                queued.payload.assign(event.data(), event.data() + event.size());
                queued.eventNumber = event.eventNumber();
                queued.dataId = event.dataId();

                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    if (queue_.size() >= queueSize_) {
                        // Bound the memory and keep the freshest data: an
                        // unbounded queue in front of a stalled chain is how a
                        // receiver runs a node out of memory.
                        queue_.pop_front();
                        dropped_++;
                        logRateLimited(3, 5.0,
                                       "EjfatReceiverActor: output queue full, dropping the "
                                       "oldest event; downstream is slower than the stream");
                    }
                    queue_.push_back(std::move(queued));
                }
                queueReady_.notify_one();

                if (receiverConfig_.verbose) {
                    std::cout << "EjfatReceiverActor: event " << event.eventNumber()
                              << " dataId=" << event.dataId() << " bytes=" << event.size()
                              << std::endl;
                }
            }

            if (receiverConfig_.maxEvents > 0 && stats.events >= receiverConfig_.maxEvents) {
                if (!receiverConfig_.quiet) {
                    std::cout << "EjfatReceiverActor: reached max-events "
                              << receiverConfig_.maxEvents << "; stopping the receive loop"
                              << std::endl;
                }
                // Stop receiving, but leave the actor alive so ERSAP can shut
                // the pipeline down in its own time. execute() will drain what
                // is still queued and then report starvation.
                break;
            }
        }
        // A Timeout is normal and is counted inside receive(); it must not log.

        if (receiverConfig_.statsIntervalSeconds > 0.0 && !receiverConfig_.quiet &&
            std::chrono::duration<double>(Clock::now() - lastProgress).count() >=
                receiverConfig_.statsIntervalSeconds) {
            const double elapsed =
                std::chrono::duration<double>(Clock::now() - startTime_).count();
            std::cout << "EjfatReceiverActor: " << stats.progressLine(elapsed)
                      << " | published " << published_.load() << " | dropped "
                      << dropped_.load() << std::endl;
            lastProgress = Clock::now();
        }
    }

    running_ = false;
    // Wake any execute() blocked on the queue so it does not wait out its full
    // timeout after the pump has finished.
    queueReady_.notify_all();
}

void EjfatReceiverActor::logRateLimited(std::size_t slot, double everySeconds,
                                        const std::string& message) {
    if (slot >= RATE_LIMIT_SLOTS) {
        return;
    }
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        const auto& last = lastLogged_[slot];
        if (last.time_since_epoch().count() != 0 &&
            std::chrono::duration<double>(now - last).count() < everySeconds) {
            return;
        }
        lastLogged_[slot] = now;
    }
    std::cerr << message << std::endl;
}

// ---------------------------------------------------------------------------
// Event pump seen from ERSAP
// ---------------------------------------------------------------------------

ersap::EngineData EjfatReceiverActor::execute(ersap::EngineData& /*input*/) {
    ersap::EngineData output;

    if (!receiver_) {
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description("EjfatReceiverActor: not configured");
        return output;
    }

    QueuedEvent event;
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        // Wait at most poll-timeout, matching what the executable's recvEvent()
        // call would have blocked for, then return empty-handed.
        const bool got = queueReady_.wait_for(
            lock, std::chrono::milliseconds(receiverConfig_.pollTimeoutMs),
            [this] { return !queue_.empty() || stopRequested_.load() || !running_.load(); });

        if (got && !queue_.empty()) {
            event = std::move(queue_.front());
            queue_.pop_front();
        } else {
            starved_++;
            // Not an error: an idle stream, a poll-timeout expiry, or a
            // receive loop that has reached max-events all land here. WARNING
            // is what the reference EJFAT actor returns, and it does not stop
            // the chain.
            output.set_status(ersap::EngineStatus::WARNING);
            output.set_description("EjfatReceiverActor: no event within poll-timeout");
            return output;
        }
    }

    published_++;
    output.set_communication_id(static_cast<long>(event.eventNumber));

    // The payload is moved into the EngineData, and the serializer's rvalue
    // overload moves it again into the outgoing message, so the copy made on
    // the receive thread is the only one.
    if (outputMime_ == MIME_BYTES) {
        output.set_data(bytesType(), std::move(event.payload));
    } else if (outputMime_ == MIME_JOBJ) {
        output.set_data(jobjType(), std::move(event.payload));
    } else {
        output.set_data(evioBlockType(), std::move(event.payload));
    }

    return output;
}

ersap::EngineData EjfatReceiverActor::execute_group(
    const std::vector<ersap::EngineData>& /*inputs*/) {
    ersap::EngineData output;
    output.set_status(ersap::EngineStatus::WARNING);
    output.set_description("EjfatReceiverActor: execute_group is not supported");
    return output;
}

// ---------------------------------------------------------------------------
// Data type / metadata declarations
// ---------------------------------------------------------------------------

std::vector<ersap::EngineDataType> EjfatReceiverActor::input_data_types() const {
    // Any input acts as a trigger. SINT32 mirrors the Java trigger-source
    // pattern, JSON carries the configuration, and the two byte types allow
    // chaining behind another native actor.
    return {ersap::type::SINT32, ersap::type::JSON, evioBlockType(), bytesType(), jobjType()};
}

std::vector<ersap::EngineDataType> EjfatReceiverActor::output_data_types() const {
    return {evioBlockType(), jobjType(), bytesType()};
}

std::set<std::string> EjfatReceiverActor::states() const { return {}; }

std::string EjfatReceiverActor::name() const { return "EjfatReceiverActor"; }

std::string EjfatReceiverActor::author() const { return "gurjyan"; }

std::string EjfatReceiverActor::description() const {
    return "Receives EJFAT packets, reassembles them with the E2SAR Reassembler, and "
           "publishes each complete EVIO block as a raw byte payload for a downstream "
           "ERSAP actor.";
}

std::string EjfatReceiverActor::version() const { return "1.0.0"; }

}  // namespace actor
}  // namespace petsro
