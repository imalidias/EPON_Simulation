// =============================================================================
// polling_manager.hpp — OLT→Server A status polling during Tserver processing.
//
// After Dserver is sent, the OLT polls every pollInterval (default 5 ms),
// up to Npoll times, until Server A reports done (or polls are exhausted).
// =============================================================================
#pragma once

#include "config.hpp"
#include "event_scheduler.hpp"

#include <functional>
#include <unordered_map>

class PollingManager {
public:
    using SendPollFn = std::function<void(int jobId)>;
    using ExhaustedFn = std::function<void(int jobId)>;

    PollingManager(EventScheduler& sched, const SimConfig& cfg);

    // Begin polling for jobId; first POLL is scheduled after one pollInterval.
    void start(int jobId, SendPollFn sendPoll, ExhaustedFn onExhausted);

    // Handle POLL_ACK: stop if finished, else schedule another poll (if under Npoll).
    void onPollReply(int jobId, bool finished);

    void stop(int jobId);
    int pollCount(int jobId) const;

private:
    void scheduleNext(int jobId);

    EventScheduler& sched_;
    const SimConfig& cfg_;

    struct JobState {
        int count = 0;       // number of POLL messages sent so far
        bool active = false;
        SendPollFn sendPoll;
        ExhaustedFn onExhausted;
    };

    std::unordered_map<int, JobState> jobs_;
};
