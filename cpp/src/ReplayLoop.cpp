#include "ReplayLoop.hpp"

#include "EventSynchronizer.hpp"
#include "Logging.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace petsro {

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

ReplayLoop::ReplayLoop(std::vector<std::unique_ptr<EvioFileReader>> readers, PacketSink& sink,
                       ReplayLoopConfig config)
    : readers_(std::move(readers)),
      sink_(sink),
      config_(config),
      rebaser_(readers_.size()) {
    stats_.streams.resize(readers_.size());
    for (std::size_t i = 0; i < readers_.size(); ++i) {
        stats_.streams[i].path = readers_[i]->path();
        stats_.streams[i].dataId =
            static_cast<std::uint16_t>(config_.dataIdBase + static_cast<std::uint16_t>(i));
    }
}

bool ReplayLoop::sendGroup(std::vector<EvioEvent>& group) {
    // Rebase before sending, so the bytes on the wire and every counter below
    // describe the same event. On the first loop the offset is zero and this
    // rewrites words 13-15 with the values already there.
    if (config_.rebaseTimestamps && !rebaser_.applyToGroup(group)) {
        lastError_ = rebaser_.lastError();
        LOG_ERROR << "cannot rebase timestamps: " << lastError_;
        return false;
    }

    for (std::size_t i = 0; i < group.size(); ++i) {
        const EvioEvent& event = group[i];

        OutgoingEvent out;
        out.data = event.data.data();
        out.length = event.data.size();
        out.eventNumber = nextEventNumber_;
        out.dataId = stats_.streams[i].dataId;
        out.entropy = config_.entropyPerSource
                          ? static_cast<std::uint16_t>(1U + static_cast<std::uint16_t>(i))
                          : static_cast<std::uint16_t>(0);

        const SendOutcome outcome = sink_.send(out);

        if (!outcome.ok) {
            stats_.sendErrors++;
            stats_.streams[i].sendErrors++;
            LOG_ERROR << "send failed for " << readers_[i]->path() << " event "
                      << out.eventNumber << " (dataId " << out.dataId << ", " << out.length
                      << " bytes) to " << sink_.destination() << ": " << outcome.error;
            lastError_ = outcome.error;
            return false;
        }

        ++nextEventNumber_;
        stats_.eventsSent++;
        stats_.packetsSent += outcome.packets;
        stats_.payloadBytesSent += out.length;
        stats_.streams[i].eventsSent++;
        stats_.streams[i].bytesSent += out.length;

        LOG_DEBUG << "sent event " << out.eventNumber << " dataId=" << out.dataId
                  << " ts=" << event.timestamp << " bytes=" << out.length
                  << " packets=" << outcome.packets;
    }

    stats_.groupsSent++;
    return true;
}

void ReplayLoop::collectReaderStats() {
    for (std::size_t i = 0; i < readers_.size(); ++i) {
        const ReaderStats& rs = readers_[i]->stats();
        stats_.streams[i].eventsRead = rs.eventsRead;
        stats_.streams[i].readErrors = rs.readErrors;
        stats_.streams[i].truncatedTails = rs.truncatedTails;
    }
}

bool ReplayLoop::runOnePass(const std::atomic<bool>& shutdown, bool& fatal) {
    fatal = false;

    for (auto& reader : readers_) {
        if (!reader->open()) {
            lastError_ = reader->lastError();
            LOG_ERROR << "cannot open input: " << lastError_;
            fatal = true;
            return false;
        }
    }

    // Readers are closed on every exit path below, including the fatal ones.
    struct CloseOnExit {
        std::vector<std::unique_ptr<EvioFileReader>>* readers;
        ~CloseOnExit() {
            for (auto& r : *readers) {
                r->close();
            }
        }
    } closer{&readers_};

    std::vector<EventSource*> sources;
    sources.reserve(readers_.size());
    for (auto& reader : readers_) {
        sources.push_back(reader.get());
    }
    EventSynchronizer sync(sources);

    const auto passStart = Clock::now();
    auto lastProgress = passStart;
    bool clean = true;

    for (;;) {
        const SyncStatus status = sync.nextGroup(shutdown);

        if (status == SyncStatus::Group) {
            if (!sendGroup(sync.mutableGroup())) {
                fatal = true;
                clean = false;
                break;
            }
            if (config_.groupDelayUs > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(config_.groupDelayUs));
            }
        } else if (status == SyncStatus::EndOfFile) {
            LOG_DEBUG << "pass reached end of file after " << sync.stats().groupsFormed
                      << " group(s)";
            break;
        } else if (status == SyncStatus::Shutdown) {
            clean = false;
            break;
        } else {  // SyncStatus::Error
            lastError_ = sync.lastError();
            LOG_ERROR << "synchronization failed: " << lastError_;
            fatal = true;
            clean = false;
            break;
        }

        if (config_.statsIntervalSeconds > 0.0 &&
            secondsSince(lastProgress) >= config_.statsIntervalSeconds) {
            LOG_INFO << stats_.progressLine(secondsSince(passStart));
            lastProgress = Clock::now();
        }
    }

    // Close out the replayed span so the next loop starts after it. Done here,
    // not in run(), so it happens on every exit path from a pass.
    if (config_.rebaseTimestamps) {
        const std::uint64_t span = rebaser_.currentPassSpan();
        rebaser_.endPass();
        if (span > 0) {
            LOG_INFO << "replayed " << (static_cast<double>(span) / 1e9)
                     << " s of capture time; next loop starts at offset "
                     << (static_cast<double>(rebaser_.timestampOffset()) / 1e9) << " s";
        }
    }

    // Fold the synchronizer's view into the run totals before it goes away.
    const SyncStats& ss = sync.stats();
    stats_.timestampMismatches += ss.timestampMismatches;
    stats_.timestampRegressions += ss.timestampRegressions;
    stats_.incompleteGroups += ss.incompleteGroups;

    const std::vector<std::uint64_t>& skipped = sync.skippedPerStream();
    for (std::size_t i = 0; i < skipped.size() && i < stats_.streams.size(); ++i) {
        stats_.streams[i].eventsSkipped += skipped[i];
        if (skipped[i] > 0) {
            LOG_INFO << readers_[i]->path() << ": skipped " << skipped[i]
                     << " event(s) during synchronization in this pass";
        }
    }

    collectReaderStats();
    stats_.readErrors = 0;
    for (const auto& s : stats_.streams) {
        stats_.readErrors += s.readErrors;
    }

    return clean;
}

bool ReplayLoop::run(const std::atomic<bool>& shutdown) {
    if (readers_.empty()) {
        lastError_ = "no input files";
        return false;
    }

    const auto runStart = Clock::now();
    LOG_INFO << "replaying " << readers_.size() << " stream(s) to " << sink_.destination()
             << "; max payload " << sink_.maxPayloadLength() << " bytes/packet";

    bool ok = true;

    while (!shutdown.load(std::memory_order_relaxed)) {
        const std::uint64_t loopNumber = stats_.loopsCompleted + 1;
        LOG_INFO << "starting replay loop " << loopNumber
                 << (config_.loopLimit > 0 ? " of " + std::to_string(config_.loopLimit) : "");

        bool fatal = false;
        const bool clean = runOnePass(shutdown, fatal);

        if (fatal) {
            ok = false;
            break;
        }

        if (!clean) {
            // Interrupted mid-pass by the shutdown flag. The pass does not count.
            LOG_INFO << "replay loop " << loopNumber << " interrupted";
            break;
        }

        stats_.loopsCompleted++;
        LOG_INFO << "replay loop " << stats_.loopsCompleted << " complete: "
                 << stats_.progressLine(secondsSince(runStart));

        if (config_.loopLimit > 0 && stats_.loopsCompleted >= config_.loopLimit) {
            LOG_INFO << "reached the configured loop limit of " << config_.loopLimit;
            break;
        }

        if (config_.resetEventNumbers) {
            LOG_DEBUG << "resetting the EJFAT event number to 1 for the next loop";
            nextEventNumber_ = 1;
        }
    }

    sink_.drain();
    return ok;
}

}  // namespace petsro
