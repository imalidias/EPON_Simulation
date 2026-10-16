// =============================================================================
// event_scheduler.hpp — Discrete-event engine (min-heap by event time).
//
// All simulated actions are scheduled callbacks. Time is continuous (double, s).
// Events at the same time are ordered by increasing event id (FIFO among ties).
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

enum class EventType {
    ONU_SERVER_TX,       // enqueue one-shot Dserver at a Server ONU
    ONU_POISSON_ARRIVAL, // generate next Poisson packet at an ONU
    PKT_ARRIVAL,         // generic packet arrival at a node
    PRE_SIG_STEP,        // advance / complete pre-signalling
    POST_SIG_STEP,       // advance / complete post-signalling
    SERVER_PROCESS_DONE, // Server A finished Tserver for a job
    POLL_TIMER,          // OLT→Server A poll timer (5 ms)
    POISSON_PROCESS_DONE,// OLT finished Tpoisson for a Poisson packet
    ITERATION_NEXT,      // reserved / unused (iteration advance via callbacks)
    IPACT_ISSUE_GATE,    // OLT issues next IPACT GATE
    ONU_GRANT_TX,        // ONU starts transmitting inside a GATE window
    SIM_END              // stop horizon marker
};

struct Event {
    double time = 0.0;
    EventType type = EventType::SIM_END;
    std::uint64_t id = 0;           // unique id; break ties for equal times
    int a = 0;                      // optional payload (e.g. onu id, job id)
    int b = 0;
    std::uint64_t packetId = 0;
    std::function<void()> handler;  // action executed when the event fires
};

// priority_queue is a max-heap; invert comparison so earliest time is on top.
struct EventCompare {
    bool operator()(const Event& lhs, const Event& rhs) const {
        if (lhs.time != rhs.time) return lhs.time > rhs.time;
        return lhs.id > rhs.id;
    }
};

class EventScheduler {
public:
    double now() const { return now_; }
    bool empty() const { return queue_.empty(); }

    std::uint64_t schedule(Event e) {
        e.id = nextId_++;
        queue_.push(std::move(e));
        return e.id;
    }

    // Convenience: schedule a typed callback at an absolute simulation time.
    std::uint64_t scheduleAt(double time, EventType type, std::function<void()> handler,
                             int a = 0, int b = 0, std::uint64_t packetId = 0) {
        Event e;
        e.time = time;
        e.type = type;
        e.handler = std::move(handler);
        e.a = a;
        e.b = b;
        e.packetId = packetId;
        return schedule(std::move(e));
    }

    // Process events with time <= tEnd, then set now_ = tEnd.
    void runUntil(double tEnd) {
        while (!queue_.empty()) {
            Event e = queue_.top();
            if (e.time > tEnd) break;
            queue_.pop();
            now_ = e.time;
            if (e.handler) e.handler();
        }
        now_ = tEnd;
    }

    // Drain the queue completely (no time limit).
    void run() {
        while (!queue_.empty()) {
            Event e = queue_.top();
            queue_.pop();
            now_ = e.time;
            if (e.handler) e.handler();
        }
    }

private:
    double now_ = 0.0;
    std::uint64_t nextId_ = 1;
    std::priority_queue<Event, std::vector<Event>, EventCompare> queue_;
};
