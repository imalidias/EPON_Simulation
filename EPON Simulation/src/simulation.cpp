// =============================================================================
// simulation.cpp — Event-driven EPON + Server A model implementation.
//
// Major flows:
//   1) IPACT: ONU queues → GATE/REPORT → data arrives at OLT
//   2) Poisson: OLT holds Tpoisson → RESPONSE to ONU
//   3) Server A (RR): pre-sig → Dserver → poll → post-sig × Niteration → FINAL_RESULT
// =============================================================================

#include "simulation.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

Simulation::Simulation(SimConfig cfg)
    : cfg_(std::move(cfg)),
      metrics_(cfg_.N),
      polling_(sched_, cfg_),
      rng_(cfg_.seed),
      serverOnu_(cfg_.N, false),
      onuUplink_(cfg_.N),
      ipactOnu_(cfg_.N) {}

void Simulation::build() {
    if (cfg_.Nserver > cfg_.N) {
        throw std::runtime_error("Nserver cannot exceed N");
    }

    metrics_.setRunParams(cfg_.Nserver, cfg_.Niteration);

    for (std::size_t i = 0; i < cfg_.N; ++i) {
        serverOnu_[i] = (i < cfg_.Nserver);
        metrics_.setTrafficType(static_cast<int>(i), serverOnu_[i] ? "server" : "poisson");
        // Discovery: first GATE may grant up to Wmax before any REPORT arrives
        ipactOnu_[i].lastReportBytes = cfg_.maxGrantBytes;
        ipactOnu_[i].everReported = false;
    }

    // Load OLT↔Server A signalling scripts (paths from SimConfig)
    preSeq_.load(cfg_.preSignallingCsv);
    postSeq_.load(cfg_.postSignallingCsv);

    // Seed Server-ONU one-shot traffic at t=0 (queued for IPACT upstream)
    for (std::size_t i = 0; i < cfg_.Nserver; ++i) {
        const int onuId = static_cast<int>(i);
        sched_.scheduleAt(0.0, EventType::ONU_SERVER_TX,
                          [this, onuId]() { onuGenerateServerTrafficOnce(onuId); }, onuId);
    }

    // Seed Poisson ONUs
    for (std::size_t i = cfg_.Nserver; i < cfg_.N; ++i) {
        onuScheduleNextPoisson(static_cast<int>(i));
    }

    // Start IPACT interleaved polling after initial enqueues at t=0
    sched_.scheduleAt(0.0, EventType::IPACT_ISSUE_GATE, [this]() { ipactStart(); });

    sched_.scheduleAt(cfg_.simDuration, EventType::SIM_END, []() {});
}

void Simulation::run() {
    sched_.runUntil(cfg_.simDuration);
}

void Simulation::writeResults() const {
    metrics_.writePerOnuCsv(cfg_.onuStatsCsv);
    metrics_.writeNetworkCsv(cfg_.networkStatsCsv);
    std::cout << "Wrote " << cfg_.onuStatsCsv << " and " << cfg_.networkStatsCsv << '\n';
}

std::uint64_t Simulation::nextPacketId() { return pktId_++; }

// Serialization delay: bits / link rate.
double Simulation::txDelay(std::size_t bytes) const {
    return (static_cast<double>(bytes) * 8.0) / cfg_.linkRateBps;
}

// Schedule arrival at destination after TX + propagation (no exclusive channel lock).
void Simulation::sendOnLink(Packet p, double propDelay) {
    p.sendTime = sched_.now();
    const double arrive = sched_.now() + txDelay(p.sizeBytes) + propDelay;
    const Packet copy = p;
    sched_.scheduleAt(arrive, EventType::PKT_ARRIVAL,
                      [this, copy]() {
                          if (copy.dst.kind == NodeKind::OLT) {
                              oltOnPacketArrival(copy);
                          } else if (copy.dst.kind == NodeKind::SERVER) {
                              serverOnPacketArrival(copy);
                          } else if (copy.dst.kind == NodeKind::ONU) {
                              onuOnPacketArrival(copy);
                          }
                      },
                      copy.ownerOnu, copy.jobId, copy.id);
}

// -------------------- ONU uplink / IPACT --------------------

void Simulation::onuEnqueueUplink(int onuId, Packet p) {
    auto& up = onuUplink_[onuId];
    if (up.queue.size() >= cfg_.onuQueueLimitPkts) {
        metrics_.onPacketDropped(p);
        return;
    }
    up.queueBytes += p.sizeBytes;
    up.queue.push_back(std::move(p));
}

void Simulation::onuGenerateServerTrafficOnce(int onuId) {
    Packet p;
    p.id = nextPacketId();
    p.type = PacketType::DATA_SERVER;
    p.src = {NodeKind::ONU, onuId};
    p.dst = {NodeKind::OLT, 0};
    p.sizeBytes = cfg_.Dserver;
    p.createTime = sched_.now();
    p.ownerOnu = onuId;
    metrics_.onPacketCreated(p);
    onuEnqueueUplink(onuId, std::move(p));
}

void Simulation::onuScheduleNextPoisson(int onuId) {
    std::exponential_distribution<double> expDist(cfg_.poissonLambda);
    const double dt = expDist(rng_);
    const double t = sched_.now() + dt;
    if (t > cfg_.simDuration) return;
    sched_.scheduleAt(t, EventType::ONU_POISSON_ARRIVAL,
                      [this, onuId]() { onuGeneratePoissonPacket(onuId); }, onuId);
}

void Simulation::onuGeneratePoissonPacket(int onuId) {
    Packet p;
    p.id = nextPacketId();
    p.type = PacketType::DATA_POISSON;
    p.src = {NodeKind::ONU, onuId};
    p.dst = {NodeKind::OLT, 0};
    p.sizeBytes = cfg_.poissonPktSize;
    p.createTime = sched_.now();
    p.ownerOnu = onuId;
    metrics_.onPacketCreated(p);
    onuEnqueueUplink(onuId, std::move(p));
    onuScheduleNextPoisson(onuId);
}

void Simulation::onuOnPacketArrival(const Packet& p) {
    if (p.type == PacketType::GATE) {
        onuHandleGate(p);
        return;
    }
    if (p.type == PacketType::RESPONSE || p.type == PacketType::FINAL_RESULT) {
        metrics_.onPacketDelivered(p, sched_.now());
    }
}

void Simulation::onuHandleGate(const Packet& gate) {
    const int onuId = gate.dst.id;
    const double start = std::max(gate.grantStart, sched_.now());
    const std::size_t grantBytes = gate.grantBytes;
    sched_.scheduleAt(start, EventType::ONU_GRANT_TX,
                      [this, onuId, grantBytes, start]() {
                          onuExecuteGrant(onuId, grantBytes, start);
                      },
                      onuId);
}

void Simulation::onuExecuteGrant(int onuId, std::size_t grantBytes, double grantStart) {
    auto& up = onuUplink_[onuId];
    std::size_t used = 0;
    double txCursor = grantStart;

    while (!up.queue.empty()) {
        const auto& front = up.queue.front();
        if (used + front.sizeBytes > grantBytes) break;

        Packet pkt = up.queue.front();
        up.queue.pop_front();
        up.queueBytes -= pkt.sizeBytes;
        used += pkt.sizeBytes;

        const double sendAt = txCursor;
        const double pktTx = txDelay(pkt.sizeBytes);
        txCursor += pktTx;

        sched_.scheduleAt(sendAt, EventType::PKT_ARRIVAL,
                          [this, pkt, pktTx]() {
                              Packet p = pkt;
                              p.sendTime = sched_.now();
                              const double arrive =
                                  sched_.now() + pktTx + cfg_.propDelayOnuOlt;
                              sched_.scheduleAt(arrive, EventType::PKT_ARRIVAL,
                                                [this, p]() { oltOnPacketArrival(p); },
                                                p.ownerOnu, p.jobId, p.id);
                          },
                          onuId);
    }

    // MPCP REPORT with remaining queue (after data in this grant)
    Packet report;
    report.id = nextPacketId();
    report.type = PacketType::REPORT;
    report.src = {NodeKind::ONU, onuId};
    report.dst = {NodeKind::OLT, 0};
    report.sizeBytes = cfg_.reportSizeBytes;
    report.createTime = grantStart;
    report.ownerOnu = onuId;
    report.reportQueueBytes = up.queueBytes;

    const double reportSendAt = txCursor;
    const double reportTx = txDelay(report.sizeBytes);
    sched_.scheduleAt(reportSendAt, EventType::PKT_ARRIVAL,
                      [this, report, reportTx]() {
                          Packet r = report;
                          r.sendTime = sched_.now();
                          const double arrive =
                              sched_.now() + reportTx + cfg_.propDelayOnuOlt;
                          sched_.scheduleAt(arrive, EventType::PKT_ARRIVAL,
                                            [this, r]() { oltHandleReport(r); },
                                            r.ownerOnu, r.jobId, r.id);
                      },
                      onuId);
}

void Simulation::ipactStart() {
    upstreamFreeTime_ = 0.0;
    ipactNextOnu_ = 0;
    if (cfg_.N == 0) return;
    ipactIssueGate(0);
}

// Limited-service IPACT: grant = min(last REPORT, Wmax); always reserve room for REPORT.
void Simulation::ipactIssueGate(int onuId) {
    if (sched_.now() > cfg_.simDuration) return;
    if (onuId < 0 || static_cast<std::size_t>(onuId) >= cfg_.N) return;

    std::size_t grantData = std::min(ipactOnu_[onuId].lastReportBytes, cfg_.maxGrantBytes);

    // Always include REPORT in the upstream window
    const std::size_t windowBytes = grantData + cfg_.reportSizeBytes;
    const double windowTime = txDelay(windowBytes);
    const double gateTx = txDelay(cfg_.gateSizeBytes);

    // GATE must arrive before grant start; interleaved schedule uses upstreamFreeTime_
    const double earliestStart = sched_.now() + gateTx + cfg_.propDelayOnuOlt;
    const double grantStart = std::max(earliestStart, upstreamFreeTime_);
    upstreamFreeTime_ = grantStart + windowTime + cfg_.guardTime;

    Packet gate;
    gate.id = nextPacketId();
    gate.type = PacketType::GATE;
    gate.src = {NodeKind::OLT, 0};
    gate.dst = {NodeKind::ONU, onuId};
    gate.sizeBytes = cfg_.gateSizeBytes;
    gate.createTime = sched_.now();
    gate.ownerOnu = onuId;
    gate.grantStart = grantStart;
    gate.grantBytes = grantData;
    sendOnLink(gate, cfg_.propDelayOnuOlt);

    // Interleaved: issue next ONU's GATE after this GATE finishes TX on downstream.
    // Cap how far ahead grants are scheduled to avoid flooding the event queue.
    const int nextOnu = (onuId + 1) % static_cast<int>(cfg_.N);
    ipactNextOnu_ = nextOnu;
    const double maxAhead =
        2.0 * cfg_.propDelayOnuOlt +
        static_cast<double>(cfg_.N) * (txDelay(cfg_.reportSizeBytes) + cfg_.guardTime) + 1e-4;
    double nextIssueAt = sched_.now() + gateTx;
    if (upstreamFreeTime_ - nextIssueAt > maxAhead) {
        nextIssueAt = upstreamFreeTime_ - maxAhead;
    }
    if (nextIssueAt <= cfg_.simDuration) {
        sched_.scheduleAt(nextIssueAt, EventType::IPACT_ISSUE_GATE,
                          [this, nextOnu]() { ipactIssueGate(nextOnu); }, nextOnu);
    }
}

void Simulation::oltHandleReport(const Packet& report) {
    const int onuId = report.ownerOnu;
    if (onuId < 0 || static_cast<std::size_t>(onuId) >= cfg_.N) return;
    ipactOnu_[onuId].lastReportBytes = report.reportQueueBytes;
    ipactOnu_[onuId].everReported = true;
}

// -------------------- OLT --------------------

void Simulation::oltOnPacketArrival(Packet p) {
    switch (p.type) {
        case PacketType::DATA_SERVER:
            oltHandleServerData(std::move(p));
            break;
        case PacketType::DATA_POISSON:
            oltHandlePoissonData(std::move(p));
            break;
        case PacketType::REPORT:
            oltHandleReport(p);
            break;
        case PacketType::POLL_ACK:
            oltOnPollAck(p);
            break;
        case PacketType::CONFIRM:
            oltOnConfirm(p);
            break;
        case PacketType::SIG_PRE: {
            auto it = preSessions_.find(p.jobId);
            if (it != preSessions_.end()) it->second->onArrival(p);
            break;
        }
        case PacketType::SIG_POST: {
            auto it = postSessions_.find(p.jobId);
            if (it != postSessions_.end()) it->second->onArrival(p);
            break;
        }
        default:
            break;
    }
}

void Simulation::oltHandleServerData(Packet p) {
    oltStartServerJob(std::move(p));
}

void Simulation::oltHandlePoissonData(Packet p) {
    const int localId = nextPoissonJob_++;
    Packet req = p;
    sched_.scheduleAt(sched_.now() + cfg_.Tpoisson, EventType::POISSON_PROCESS_DONE,
                      [this, localId, req]() { oltFinishPoisson(localId, req); }, localId);
}

void Simulation::oltFinishPoisson(int /*jobLocalId*/, Packet req) {
    Packet resp;
    resp.id = nextPacketId();
    resp.type = PacketType::RESPONSE;
    resp.src = {NodeKind::OLT, 0};
    resp.dst = {NodeKind::ONU, req.ownerOnu};
    resp.sizeBytes = 64;
    resp.createTime = req.createTime;
    resp.ownerOnu = req.ownerOnu;
    sendOnLink(resp, cfg_.propDelayOnuOlt);
}

void Simulation::oltStartServerJob(Packet p) {
    ServerJob job;
    job.jobId = nextJobId_++;
    job.onuId = p.ownerOnu;
    job.createTime = p.createTime;
    job.dataBytes = p.sizeBytes;
    job.iteration = 0;
    job.request = p;
    jobs_[job.jobId] = job;
    oltEnqueueServerJob(job.jobId);
}

void Simulation::oltEnqueueServerJob(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    waitingByOnu_[it->second.onuId] = jobId;
    oltTryScheduleServerPath();
}

// Pick the next waiting Server ONU in round-robin order and start pre-signalling.
void Simulation::oltTryScheduleServerPath() {
    if (activeServerJobId_ >= 0) return;
    if (waitingByOnu_.empty() || cfg_.Nserver == 0) return;

    for (std::size_t i = 0; i < cfg_.Nserver; ++i) {
        const int onu = static_cast<int>((rrNextOnu_ + static_cast<int>(i)) %
                                         static_cast<int>(cfg_.Nserver));
        auto wit = waitingByOnu_.find(onu);
        if (wit == waitingByOnu_.end()) continue;

        const int jobId = wit->second;
        waitingByOnu_.erase(wit);
        activeServerJobId_ = jobId;
        rrNextOnu_ = (onu + 1) % static_cast<int>(cfg_.Nserver);
        oltStartPreSignalling(jobId);
        return;
    }
}

void Simulation::oltReleaseServerPath() {
    activeServerJobId_ = -1;
    oltTryScheduleServerPath();
}

void Simulation::oltStartPreSignalling(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    auto& job = it->second;

    auto session = std::make_unique<SignallingSession>(sched_, preSeq_, PacketType::SIG_PRE);
    auto* raw = session.get();
    preSessions_[jobId] = std::move(session);

    raw->start(
        jobId, job.onuId, job.createTime,
        [this](Packet pkt) { sendOnLink(pkt, cfg_.propDelayOltServer); },
        [this, jobId]() {
            sched_.scheduleAt(sched_.now(), EventType::PRE_SIG_STEP,
                              [this, jobId]() { oltAfterPreSignalling(jobId); }, jobId);
        });
}

void Simulation::oltAfterPreSignalling(int jobId) {
    preSessions_.erase(jobId);
    oltSendDserverToServer(jobId);
}

void Simulation::oltSendDserverToServer(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    auto& job = it->second;

    Packet data;
    data.id = nextPacketId();
    data.type = PacketType::DATA_SERVER;
    data.src = {NodeKind::OLT, 0};
    data.dst = {NodeKind::SERVER, 0};
    data.sizeBytes = job.dataBytes;
    data.createTime = job.createTime;
    data.ownerOnu = job.onuId;
    data.jobId = jobId;
    sendOnLink(data, cfg_.propDelayOltServer);

    oltStartPolling(jobId);
}

void Simulation::oltStartPolling(int jobId) {
    polling_.start(
        jobId, [this](int jid) { oltSendPoll(jid); },
        [this](int jid) { oltDropJob(jid, "poll_exhausted"); });
}

void Simulation::oltSendPoll(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;

    Packet poll;
    poll.id = nextPacketId();
    poll.type = PacketType::POLL;
    poll.src = {NodeKind::OLT, 0};
    poll.dst = {NodeKind::SERVER, 0};
    poll.sizeBytes = 64;
    poll.createTime = it->second.createTime;
    poll.ownerOnu = it->second.onuId;
    poll.jobId = jobId;
    metrics_.onPollSent(it->second.onuId);
    sendOnLink(poll, cfg_.propDelayOltServer);
}

void Simulation::oltOnPollAck(const Packet& p) {
    polling_.onPollReply(p.jobId, p.pollDone);
    if (p.pollDone) {
        polling_.stop(p.jobId);
    }
}

void Simulation::oltOnConfirm(const Packet& p) {
    polling_.stop(p.jobId);
    oltStartPostSignalling(p.jobId);
}

void Simulation::oltStartPostSignalling(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    auto& job = it->second;

    auto session = std::make_unique<SignallingSession>(sched_, postSeq_, PacketType::SIG_POST);
    auto* raw = session.get();
    postSessions_[jobId] = std::move(session);

    raw->start(
        jobId, job.onuId, job.createTime,
        [this](Packet pkt) { sendOnLink(pkt, cfg_.propDelayOltServer); },
        [this, jobId]() {
            sched_.scheduleAt(sched_.now(), EventType::POST_SIG_STEP,
                              [this, jobId]() { oltAfterPostSignalling(jobId); }, jobId);
        });
}

void Simulation::oltAfterPostSignalling(int jobId) {
    postSessions_.erase(jobId);
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    auto& job = it->second;

    job.iteration++;
    if (job.iteration < static_cast<int>(cfg_.Niteration)) {
        job.processing = false;
        job.processDone = false;
        job.waitingConfirm = false;
        serverFinishTime_.erase(jobId);
        activeServerJobId_ = -1;
        oltEnqueueServerJob(jobId);
    } else {
        oltSendFinalResult(jobId);
    }
}

void Simulation::oltSendFinalResult(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    auto& job = it->second;

    Packet fin;
    fin.id = nextPacketId();
    fin.type = PacketType::FINAL_RESULT;
    fin.src = {NodeKind::OLT, 0};
    fin.dst = {NodeKind::ONU, job.onuId};
    fin.sizeBytes = 64;
    fin.createTime = job.createTime;
    fin.ownerOnu = job.onuId;
    fin.jobId = jobId;
    sendOnLink(fin, cfg_.propDelayOnuOlt);
    jobs_.erase(it);
    if (activeServerJobId_ == jobId) activeServerJobId_ = -1;
    oltTryScheduleServerPath();
}

void Simulation::oltDropJob(int jobId, const char* reason) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    std::cerr << "Job " << jobId << " dropped: " << reason << '\n';
    const int onuId = it->second.onuId;
    metrics_.onPacketDropped(it->second.request);
    jobs_.erase(it);
    serverFinishTime_.erase(jobId);
    preSessions_.erase(jobId);
    postSessions_.erase(jobId);
    polling_.stop(jobId);
    waitingByOnu_.erase(onuId);
    if (activeServerJobId_ == jobId) activeServerJobId_ = -1;
    oltTryScheduleServerPath();
}

// -------------------- Server A --------------------

void Simulation::serverOnPacketArrival(Packet p) {
    switch (p.type) {
        case PacketType::DATA_SERVER:
            serverOnDataArrival(p);
            break;
        case PacketType::POLL:
            serverOnPoll(p);
            break;
        case PacketType::SIG_PRE: {
            auto it = preSessions_.find(p.jobId);
            if (it != preSessions_.end()) it->second->onArrival(p);
            break;
        }
        case PacketType::SIG_POST: {
            auto it = postSessions_.find(p.jobId);
            if (it != postSessions_.end()) it->second->onArrival(p);
            break;
        }
        default:
            break;
    }
}

void Simulation::serverOnDataArrival(const Packet& p) {
    auto it = jobs_.find(p.jobId);
    if (it == jobs_.end()) return;
    it->second.processing = true;
    it->second.processDone = false;
    const double finish = sched_.now() + cfg_.Tserver;
    serverFinishTime_[p.jobId] = finish;
    sched_.scheduleAt(finish, EventType::SERVER_PROCESS_DONE,
                      [this, jobId = p.jobId]() { serverMarkProcessDone(jobId); }, p.jobId);
}

void Simulation::serverMarkProcessDone(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    it->second.processDone = true;
    it->second.processing = false;
}

void Simulation::serverOnPoll(const Packet& p) {
    auto it = jobs_.find(p.jobId);
    if (it == jobs_.end()) return;

    const bool done = it->second.processDone;

    Packet ack;
    ack.id = nextPacketId();
    ack.type = PacketType::POLL_ACK;
    ack.src = {NodeKind::SERVER, 0};
    ack.dst = {NodeKind::OLT, 0};
    ack.sizeBytes = 64;
    ack.createTime = it->second.createTime;
    ack.ownerOnu = it->second.onuId;
    ack.jobId = p.jobId;
    ack.pollDone = done;
    sendOnLink(ack, cfg_.propDelayOltServer);

    if (done && !it->second.waitingConfirm) {
        it->second.waitingConfirm = true;
        serverSendConfirm(p.jobId);
    }
}

void Simulation::serverSendConfirm(int jobId) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;

    Packet conf;
    conf.id = nextPacketId();
    conf.type = PacketType::CONFIRM;
    conf.src = {NodeKind::SERVER, 0};
    conf.dst = {NodeKind::OLT, 0};
    conf.sizeBytes = 64;
    conf.createTime = it->second.createTime;
    conf.ownerOnu = it->second.onuId;
    conf.jobId = jobId;
    sendOnLink(conf, cfg_.propDelayOltServer);
}
