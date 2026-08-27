/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * ERSAP source actor that receives EJFAT packets, reassembles them with the
 * E2SAR Reassembler, and publishes each complete payload to the next actor in
 * the chain -- typically a Java ERSAP processing actor.
 *
 * @author gurjyan
 * @project pet-sro
 */

#ifndef PETSRO_EJFATRECEIVERACTOR_HPP
#define PETSRO_EJFATRECEIVERACTOR_HPP

#include "EjfatReceiver.hpp"
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

/// MIME type of EvioBlockDataType, the default. Names the payload for what it
/// is -- one big-endian EVIO block -- and its Java deserializer forces the
/// ByteBuffer to BIG_ENDIAN, which is the one thing a plain raw-bytes type
/// cannot do. See the byte-order note in the .cpp.
extern const char* const MIME_EVIO_BLOCK;

/// MIME type of JavaObjectType.JOBJ, the type PetMultiStreamSourceEngine
/// declares and PetGeometryProcessorEngine accepts. Its serializer on both
/// sides is the raw-bytes one, so a C++ actor publishing under this name hands
/// the Java actor a ByteBuffer holding exactly the bytes written here -- but
/// one marked LITTLE_ENDIAN, which the consuming actor must correct itself.
extern const char* const MIME_JOBJ;

/// MIME type of ersap::type::BYTES, for chaining behind another native actor.
extern const char* const MIME_BYTES;

/// MIME type of CodaTimeFrameBinaryDataType, accepted here only so that
/// configure() can reject it with an explanation rather than a type error at
/// the first event. See the header comment of the .cpp.
extern const char* const MIME_CODA_TIME_FRAME;

/**
 * Receives EJFAT events and publishes their reassembled payloads.
 *
 * Threading
 * ---------
 * A dedicated pump thread owns the EjfatReceiver and drains it continuously
 * into a bounded queue. execute() only pops from that queue, so no ERSAP
 * thread ever sits inside recvEvent(), and the E2SAR receive queues keep
 * draining even when the downstream chain stalls. When the queue is full the
 * oldest event is dropped and counted, which bounds memory at
 * queue-size x event-size and keeps the newest data flowing.
 *
 * Output
 * ------
 * One published payload is one reassembled EJFAT event, which for this
 * project's sender is exactly one big-endian EVIO block, byte for byte as it
 * left the sender: no length prefix, no envelope, no transport headers.
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
    /// One event on its way from the pump thread to execute().
    struct QueuedEvent {
        std::vector<std::uint8_t> payload;
        std::uint64_t eventNumber = 0;
        std::uint16_t dataId = 0;
    };

    /// Body of the pump thread.
    void pump();

    /// Stops the pump thread and the receiver. Idempotent, never throws.
    void shutdown() noexcept;

    /// Emits `message` at most once per `everySeconds`, keyed by `slot`, so a
    /// storm of identical receive errors costs one line rather than millions.
    void logRateLimited(std::size_t slot, double everySeconds, const std::string& message);

    // --- configuration, all set by configure() -----------------------------
    EjfatReceiverConfig receiverConfig_;
    ValidationLevel validation_ = ValidationLevel::Structural;
    std::string outputMime_;
    std::size_t queueSize_ = 256;

    // --- runtime ----------------------------------------------------------
    std::unique_ptr<EjfatReceiver> receiver_;
    std::thread pumpThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    mutable std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<QueuedEvent> queue_;

    // --- statistics -------------------------------------------------------
    // The per-event ReceiveStats live inside the receiver and are only touched
    // by the pump thread. These are read by execute() as well, so they are
    // atomic.
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> starved_{0};
    std::atomic<std::uint64_t> malformed_{0};

    std::chrono::steady_clock::time_point startTime_{};

    static constexpr std::size_t RATE_LIMIT_SLOTS = 4;
    std::chrono::steady_clock::time_point lastLogged_[RATE_LIMIT_SLOTS]{};
    std::mutex logMutex_;
};

}  // namespace actor
}  // namespace petsro

// ERSAP plugin entry point.
extern "C" std::unique_ptr<ersap::Engine> create_engine();

#endif  // PETSRO_EJFATRECEIVERACTOR_HPP
