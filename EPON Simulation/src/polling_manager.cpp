// =============================================================================
// polling_manager.cpp — Timed OLT→Server A polls during job processing.
// =============================================================================

#include "polling_manager.hpp"

PollingManager::PollingManager(EventScheduler& sched, const SimConfig& cfg)
    : sched_(sched), cfg_(cfg) {}

void PollingManager::start(int jobId, SendPollFn sendPoll, ExhaustedFn onExhausted) {
    auto& st = jobs_[jobId];
    st.count = 0;
    st.active = true;
    st.sendPoll = std::move(sendPoll);
    st.onExhausted = std::move(onExhausted);
    scheduleNext(jobId);
}

// Schedule one POLL after pollInterval; stop and call onExhausted if Npoll reached.
void PollingManager::scheduleNext(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end() || !it->second.active) return;

    sched_.scheduleAt(sched_.now() + cfg_.pollInterval, EventType::POLL_TIMER,
                      [this, jobId]() {
                          auto jt = jobs_.find(jobId);
                          if (jt == jobs_.end() || !jt->second.active) return;
                          if (jt->second.count >= static_cast<int>(cfg_.Npoll)) {
                              jt->second.active = false;
                              if (jt->second.onExhausted) jt->second.onExhausted(jobId);
                              return;
                          }
                          jt->second.count++;
                          if (jt->second.sendPoll) jt->second.sendPoll(jobId);
                      },
                      jobId);
}

void PollingManager::onPollReply(int jobId, bool finished) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end() || !it->second.active) return;
    if (finished) {
        it->second.active = false;
        return;
    }
    if (it->second.count >= static_cast<int>(cfg_.Npoll)) {
        it->second.active = false;
        if (it->second.onExhausted) it->second.onExhausted(jobId);
        return;
    }
    scheduleNext(jobId);
}

void PollingManager::stop(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    it->second.active = false;
}

int PollingManager::pollCount(int jobId) const {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return 0;
    return it->second.count;
}
