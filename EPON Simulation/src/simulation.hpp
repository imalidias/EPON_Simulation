// =============================================================================
// simulation.hpp — Top-level 10G EPON + Server A discrete-event simulation.
//
// Responsibilities:
//   • IPACT upstream (MPCP GATE/REPORT) between ONUs and OLT
//   • Poisson path: OLT processes for Tpoisson, returns one RESPONSE
//   • Server-A path: pre-signalling → Dserver → poll → post-signalling × Niteration
//   • Round-robin sharing of the OLT↔Server A path among Server ONUs
// =============================================================================
#pragma once

#include "config.hpp"
#include "event_scheduler.hpp"
#include "metrics.hpp"
#include "packet.hpp"
#include "polling_manager.hpp"
#include "signalling.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

class Simulation {
public:
    explicit Simulation(SimConfig cfg);

    void build();              // topology, CSV load, seed initial events
    void run();                // process events until simDuration
    void writeResults() const; // onu_stats.csv + network_stats.csv

    const MetricsCollector& metrics() const { return metrics_; }

private:
    // --- Link helpers ---
    std::uint64_t nextPacketId();
    double txDelay(std::size_t bytes) const; // serialization delay at linkRateBps
    void sendOnLink(Packet p, double propDelay);

    // --- ONU: traffic generation + IPACT grant execution ---
    void onuEnqueueUplink(int onuId, Packet p);
    void onuGenerateServerTrafficOnce(int onuId);
    void onuScheduleNextPoisson(int onuId);
    void onuGeneratePoissonPacket(int onuId);
    void onuOnPacketArrival(const Packet& p);
    void onuHandleGate(const Packet& gate);
    void onuExecuteGrant(int onuId, std::size_t grantBytes, double grantStart);

    // --- IPACT DBA at OLT ---
    void ipactStart();
    void ipactIssueGate(int onuId);
    void oltHandleReport(const Packet& report);

    // --- OLT: data handling + Server-A job state machine ---
    void oltOnPacketArrival(Packet p);
    void oltHandleServerData(Packet p);
    void oltHandlePoissonData(Packet p);
    void oltFinishPoisson(int jobLocalId, Packet req);
    void oltStartServerJob(Packet p);
    void oltEnqueueServerJob(int jobId);
    void oltTryScheduleServerPath(); // round-robin among waiting Server ONUs
    void oltReleaseServerPath();
    void oltStartPreSignalling(int jobId);
    void oltAfterPreSignalling(int jobId);
    void oltSendDserverToServer(int jobId);
    void oltStartPolling(int jobId);
    void oltSendPoll(int jobId);
    void oltOnPollAck(const Packet& p);
    void oltOnConfirm(const Packet& p);
    void oltStartPostSignalling(int jobId);
    void oltAfterPostSignalling(int jobId);
    void oltSendFinalResult(int jobId);
    void oltDropJob(int jobId, const char* reason);

    // --- Server A ---
    void serverOnPacketArrival(Packet p);
    void serverOnDataArrival(const Packet& p);
    void serverOnPoll(const Packet& p);
    void serverMarkProcessDone(int jobId);
    void serverSendConfirm(int jobId);

    // One Server-A workflow instance (may span Niteration loops).
    struct ServerJob {
        int jobId = -1;
        int onuId = -1;
        double createTime = 0.0;
        std::size_t dataBytes = 0;
        int iteration = 0;
        bool processing = false;
        bool processDone = false;
        bool waitingConfirm = false;
        Packet request; // original DATA_SERVER (for drop metrics)
    };

    // Per-ONU IPACT uplink buffer.
    struct OnuUplink {
        std::deque<Packet> queue;
        std::size_t queueBytes = 0;
    };

    // Limited-service IPACT state: last reported queue drives next grant.
    struct IpactOnuState {
        std::size_t lastReportBytes = 0;
        bool everReported = false;
    };

    SimConfig cfg_;
    EventScheduler sched_;
    MetricsCollector metrics_;
    PollingManager polling_;

    SignallingSequence preSeq_;
    SignallingSequence postSeq_;
    // Per-job signalling sessions (allows concurrent logical jobs; RR serializes Server path)
    std::unordered_map<int, std::unique_ptr<SignallingSession>> preSessions_;
    std::unordered_map<int, std::unique_ptr<SignallingSession>> postSessions_;

    std::mt19937 rng_;
    std::uint64_t pktId_ = 1;
    int nextJobId_ = 1;
    int nextPoissonJob_ = 1;

    std::vector<bool> serverOnu_;
    std::vector<OnuUplink> onuUplink_;
    std::vector<IpactOnuState> ipactOnu_;
    double upstreamFreeTime_ = 0.0; // next allowed ONU TX start on shared upstream
    int ipactNextOnu_ = 0;

    std::unordered_map<int, ServerJob> jobs_;
    std::unordered_map<int, double> serverFinishTime_;

    // Round-robin OLT → Server A: only one job uses the path at a time
    int activeServerJobId_ = -1;
    int rrNextOnu_ = 0;
    std::unordered_map<int, int> waitingByOnu_; // onuId → jobId
};
