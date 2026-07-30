#include "EventSynchronizer.hpp"

#include "Logging.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace petsro {

const char* toString(SyncStatus status) noexcept {
    switch (status) {
        case SyncStatus::Group:     return "group";
        case SyncStatus::EndOfFile: return "end-of-file";
        case SyncStatus::Error:     return "error";
        case SyncStatus::Shutdown:  return "shutdown";
    }
    return "unknown";
}

EventSynchronizer::EventSynchronizer(std::vector<EventSource*> sources,
                                     std::uint64_t maxAdvancesPerGroup)
    : sources_(std::move(sources)),
      current_(sources_.size()),
      primed_(sources_.size(), false),
      skipped_(sources_.size(), 0),
      lastTimestamp_(sources_.size(), 0),
      haveLastTimestamp_(sources_.size(), false),
      maxAdvances_(maxAdvancesPerGroup) {}

ReadStatus EventSynchronizer::advance(std::size_t i) {
    const ReadStatus status = sources_[i]->next(current_[i]);
    if (status != ReadStatus::Ok) {
        primed_[i] = false;
        if (status == ReadStatus::Malformed || status == ReadStatus::IoError) {
            lastError_ = std::string(sources_[i]->name()) + ": " + toString(status);
        }
        return status;
    }

    primed_[i] = true;

    const std::uint64_t ts = current_[i].timestamp;
    if (haveLastTimestamp_[i] && ts < lastTimestamp_[i]) {
        // Not fatal: the merge below still terminates, because the file is
        // finite. Worth a line, because it means the capture is not what the
        // wire format promises.
        stats_.timestampRegressions++;
        LOG_WARN << sources_[i]->name() << ": timestamp went backwards, " << lastTimestamp_[i]
                 << " -> " << ts << " (frame counter " << current_[i].frameCounter << ")";
    }
    lastTimestamp_[i] = ts;
    haveLastTimestamp_[i] = true;

    return ReadStatus::Ok;
}

SyncStatus EventSynchronizer::nextGroup(const std::atomic<bool>& shutdown) {
    const std::size_t n = sources_.size();
    if (n == 0) {
        lastError_ = "no input streams";
        return SyncStatus::Error;
    }

    std::uint64_t advances = 0;

    // Prime every slot that is empty: the first call fills all of them, later
    // calls refill exactly the ones consumed by the previous group.
    for (std::size_t i = 0; i < n; ++i) {
        if (primed_[i]) {
            continue;
        }
        if (shutdown.load(std::memory_order_relaxed)) {
            return SyncStatus::Shutdown;
        }
        const ReadStatus status = advance(i);
        if (status == ReadStatus::EndOfFile) {
            // A partially assembled group is dropped whole. Sending some of the
            // N events of a cycle would hand the load balancer an event group
            // that never completes downstream. It only counts as incomplete if
            // some other stream had already produced its event for this cycle;
            // every file ending together is the ordinary case.
            const bool anyPrimed =
                std::any_of(primed_.begin(), primed_.end(), [](bool p) { return p; });
            if (anyPrimed) {
                stats_.incompleteGroups++;
                LOG_DEBUG << "discarding incomplete group at EOF of " << sources_[i]->name();
            }
            return SyncStatus::EndOfFile;
        }
        if (status != ReadStatus::Ok) {
            return SyncStatus::Error;
        }
        ++advances;
    }

    for (;;) {
        if (shutdown.load(std::memory_order_relaxed)) {
            return SyncStatus::Shutdown;
        }

        std::uint64_t maxTs = current_[0].timestamp;
        bool allEqual = true;
        for (std::size_t i = 1; i < n; ++i) {
            const std::uint64_t ts = current_[i].timestamp;
            if (ts != current_[0].timestamp) {
                allEqual = false;
            }
            maxTs = std::max(maxTs, ts);
        }

        if (allEqual) {
            stats_.groupsFormed++;
            // Slots stay primed until the caller has used the group; the next
            // nextGroup() call marks them consumed by refilling them.
            for (std::size_t i = 0; i < n; ++i) {
                primed_[i] = false;
            }
            return SyncStatus::Group;
        }

        stats_.timestampMismatches++;
        if (logEnabled(LogLevel::Debug)) {
            std::ostringstream oss;
            oss << "timestamp mismatch, target " << maxTs << ":";
            for (std::size_t i = 0; i < n; ++i) {
                oss << ' ' << sources_[i]->name() << '=' << current_[i].timestamp;
            }
            LOG_DEBUG << oss.str();
        }

        // Advance every stream that is behind. Streams already at maxTs stay
        // put, which is what makes this converge instead of chasing.
        for (std::size_t i = 0; i < n; ++i) {
            while (current_[i].timestamp < maxTs) {
                if (shutdown.load(std::memory_order_relaxed)) {
                    return SyncStatus::Shutdown;
                }

                skipped_[i]++;
                stats_.eventsSkipped++;
                LOG_DEBUG << "skipping event on " << sources_[i]->name() << " at timestamp "
                          << current_[i].timestamp << " (behind " << maxTs << ")";

                const ReadStatus status = advance(i);
                if (status == ReadStatus::EndOfFile) {
                    stats_.incompleteGroups++;
                    return SyncStatus::EndOfFile;
                }
                if (status != ReadStatus::Ok) {
                    return SyncStatus::Error;
                }

                if (maxAdvances_ != 0 && ++advances > maxAdvances_) {
                    std::ostringstream oss;
                    oss << "streams did not converge after " << advances
                        << " advances; timestamps are not usable for synchronization"
                        << " (target " << maxTs << ", " << sources_[i]->name() << " at "
                        << current_[i].timestamp << ")";
                    lastError_ = oss.str();
                    LOG_ERROR << lastError_;
                    return SyncStatus::Error;
                }
            }
        }
    }
}

}  // namespace petsro
