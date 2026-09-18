/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * ERSAP source actor that receives EJFAT packets, reassembles them with the
 * E2SAR Reassembler, aggregates the per-stream events of one synchronized
 * group by their common LB tick, and publishes one combined EVIO v6 record
 * per group to the next actor in the chain.
 *
 * The aggregation is what makes this actor meaningful for a multi-stream
 * capture: every event on the wire carries the same tick as its group
 * peers, and the aggregator collects them, emits a single self-framing
 * EVIO v6 record, and hands that record to the ERSAP pipeline.
 *
 * @author gurjyan
 * @project pet-sro
 */

#ifndef PETSRO_EJFATRECEIVERACTOR_HPP
#define PETSRO_EJFATRECEIVERACTOR_HPP

#include "EjfatReceiver.hpp"
#include "EvioAggregator.hpp"
#include "ReceiveStats.hpp"

#include <ersap/engine.hpp>
#include <ersap/engine_data.hpp>
#include <ersap/engine_data_type.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace petsro {
namespace actor {

/// MIME type of EvioBlockDataType, the default. Names the payload as EVIO
/// wire bytes and, on the Java side, restores BIG_ENDIAN so the receiver's
/// v6 record is decoded correctly. See the byte-order note in the .cpp.
extern const char* const MIME_EVIO_BLOCK;

/// MIME type of JavaObjectType.JOBJ, drop-in for a chain that already
/// declares JOBJ. The consumer must correct the byte order itself.
extern const char* const MIME_JOBJ;

/// MIME type of ersap::type::BYTES, for chaining behind another native actor.
extern const char* const MIME_BYTES;

/// MIME type of CodaTimeFrameBinaryDataType, accepted here only so that
/// configure() can reject it with an explanation rather than a type error at
/// the first event. See the header comment of the .cpp.
extern const char* const MIME_CODA_TIME_FRAME;

/**
 * Receives EJFAT events, aggregates them by LB tick, and publishes one
 * combined EVIO v6 record per synchronized group.
 *
 * Threading
 * ---------
 * A dedicated pump thread owns the EjfatReceiver and the EvioAggregator,
 * draining the receiver and feeding the aggregator continuously. Completed
 * groups and reaped groups are pushed to a bounded output queue. execute()
 * only pops from that queue, so no ERSAP thread ever sits inside recvEvent()
 * and E2SAR's internal receive queues keep draining even when the downstream
 * chain stalls. When the output queue is full the oldest aggregated event is
 * dropped and counted, which bounds memory at queue-size x record-size and
 * keeps the newest data flowing.
 *
 * Output
 * ------
 * One published payload is one EVIO v6 record, in big-endian wire form:
 *
 *   [ 14-word record header (magic 0xC0DA0100 at word 7) ]
 *   [ N-word index array: each entry the length of one event in bytes ]
 *   [ N EVIO event banks, concatenated ]
 *
 * See EvioAggregator::buildEvioV6Record() for the exact layout. Downstream
 * readers can decode the record with a standard v6 reader; no framing is
 * added or removed.
 */
class EjfatReceiverActor final : public ersap::Engine {
  public:
    EjfatReceiverActor() = default;
    ~EjfatReceiverActor() override;

    EjfatReceiverActor(const EjfatReceiverActor&) = delete;
    EjfatReceiverActor& operator=(const EjfatReceiverActor&) = delete;

    ersap::EngineData configure(ersap::EngineData& input) override;
    ersap::EngineData execute(ersap::EngineData& input) override;
    ersap::EngineData execute_group(const std::vector<ersap::EngineData>& inputs) override;

    std::vector<ersap::EngineDataType> input_data_types() const override;
    std::vector<ersap::EngineDataType> output_data_types() const override;
    std::set<std::string> states() const override;

    void reset() override;

    std::string name() const override;
    std::string author() const override;
    std::string description() const override;
    std::string version() const override;

    /// The supported configuration keys, their types, defaults and meaning.
    /// Reported when configuration validation fails, so an operator never has
    /// to read the source to find out what a key is called.
    static std::string configurationHelp();

  private:
    /// One aggregated group on its way from the pump thread to execute().
    struct QueuedGroup {
        std::vector<std::uint8_t> payload;   ///< the EVIO v6 record bytes
        std::uint64_t tick = 0;
        std::uint16_t memberCount = 0;
        bool complete = false;
    };

    /// Body of the pump thread.
    void pump();

    /// Enqueues an aggregated event onto the output queue, dropping the oldest
    /// if the queue is at capacity. Called from the pump thread only.
    void enqueue(QueuedGroup&& group);

    /// Stops the pump thread and the receiver. Idempotent, never throws.
    void shutdown() noexcept;

    /// Emits `message` at most once per `everySeconds`, keyed by `slot`, so a
    /// storm of identical errors costs one line rather than millions.
    void logRateLimited(std::size_t slot, double everySeconds, const std::string& message);

    // --- configuration, all set by configure() -----------------------------
    EjfatReceiverConfig receiverConfig_;
    EvioAggregatorConfig aggregatorConfig_;
    ValidationLevel validation_ = ValidationLevel::Structural;
    std::string outputMime_;
    std::size_t queueSize_ = 256;

    // --- runtime ----------------------------------------------------------
    std::unique_ptr<EjfatReceiver> receiver_;
    std::unique_ptr<EvioAggregator> aggregator_;
    std::thread pumpThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    mutable std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<QueuedGroup> queue_;

    // --- statistics -------------------------------------------------------
    // The per-event ReceiveStats and per-group AggregatorStats live inside
    // their owners and are only touched by the pump thread. These are read
    // from execute() as well, so they are atomic.
    std::atomic<std::uint64_t> published_{0};     ///< aggregated groups published downstream
    std::atomic<std::uint64_t> dropped_{0};       ///< aggregated groups evicted for a full output queue
    std::atomic<std::uint64_t> starved_{0};       ///< execute() calls that came up empty
    std::atomic<std::uint64_t> malformed_{0};     ///< payloads rejected by ReceiveStats::accumulate

    std::chrono::steady_clock::time_point startTime_{};

    static constexpr std::size_t RATE_LIMIT_SLOTS = 5;
    std::chrono::steady_clock::time_point lastLogged_[RATE_LIMIT_SLOTS]{};
    std::mutex logMutex_;
};

}  // namespace actor
}  // namespace petsro

// ERSAP plugin entry point.
extern "C" std::unique_ptr<ersap::Engine> create_engine();

#endif  // PETSRO_EJFATRECEIVERACTOR_HPP
