// =============================================================================
// metrics.cpp — Accumulate stats and write CSV result files.
// =============================================================================

#include "metrics.hpp"

#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace {

// Create parent directory for path if missing (best-effort).
void ensureParentDir(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return;
    std::string dir = path.substr(0, pos);
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

} // namespace

MetricsCollector::MetricsCollector(std::size_t numOnus) : onus_(numOnus) {
    for (std::size_t i = 0; i < numOnus; ++i) {
        onus_[i].trafficType = "unknown";
    }
}

void MetricsCollector::setTrafficType(int onuId, const std::string& type) {
    if (onuId < 0 || static_cast<std::size_t>(onuId) >= onus_.size()) return;
    onus_[onuId].trafficType = type;
}

void MetricsCollector::setRunParams(std::size_t nserver, std::size_t niteration) {
    nserver_ = nserver;
    niteration_ = niteration;
}

void MetricsCollector::onPacketCreated(const Packet& p) {
    if (p.ownerOnu < 0 || static_cast<std::size_t>(p.ownerOnu) >= onus_.size()) return;
    // Count only end-to-end request packets (not control / signalling)
    if (p.type == PacketType::DATA_SERVER || p.type == PacketType::DATA_POISSON) {
        onus_[p.ownerOnu].sent++;
    }
}

// Latency sample: createTime → RESPONSE or FINAL_RESULT arrival at ONU.
void MetricsCollector::onPacketDelivered(const Packet& p, double deliveryTime) {
    if (p.ownerOnu < 0 || static_cast<std::size_t>(p.ownerOnu) >= onus_.size()) return;
    if (p.type != PacketType::RESPONSE && p.type != PacketType::FINAL_RESULT) return;

    const double lat = deliveryTime - p.createTime;
    auto& a = onus_[p.ownerOnu];
    a.received++;
    a.latencySum += lat;
    if (lat > a.maxLatency) a.maxLatency = lat;
}

void MetricsCollector::onPacketDropped(const Packet& p) {
    if (p.ownerOnu < 0 || static_cast<std::size_t>(p.ownerOnu) >= onus_.size()) return;
    onus_[p.ownerOnu].dropped++;
}

void MetricsCollector::onPollSent(int onuId) {
    if (onuId < 0 || static_cast<std::size_t>(onuId) >= onus_.size()) return;
    onus_[onuId].pollCount++;
}

OnuStats MetricsCollector::computeOnuStats(int onuId) const {
    OnuStats s;
    if (onuId < 0 || static_cast<std::size_t>(onuId) >= onus_.size()) return s;
    const auto& a = onus_[onuId];
    s.onuId = onuId;
    s.trafficType = a.trafficType;
    s.sent = a.sent;
    s.received = a.received;
    s.pollCount = a.pollCount;
    s.avgLatency = (a.received > 0) ? (a.latencySum / static_cast<double>(a.received)) : 0.0;
    s.maxLatency = a.maxLatency;
    const double denom = static_cast<double>(a.sent);
    s.lossRate = (denom > 0.0) ? (static_cast<double>(a.dropped) / denom) : 0.0;
    return s;
}

NetworkStats MetricsCollector::computeNetworkStats() const {
    NetworkStats n;
    double latSum = 0.0;
    std::size_t recv = 0;
    std::size_t drops = 0;
    for (const auto& a : onus_) {
        n.totalSent += a.sent;
        n.totalReceived += a.received;
        n.totalPolls += a.pollCount;
        latSum += a.latencySum;
        recv += a.received;
        drops += a.dropped;
        if (a.maxLatency > n.maxLatency) n.maxLatency = a.maxLatency;
    }
    n.avgLatency = (recv > 0) ? (latSum / static_cast<double>(recv)) : 0.0;
    n.lossRate = (n.totalSent > 0)
                     ? (static_cast<double>(drops) / static_cast<double>(n.totalSent))
                     : 0.0;
    n.nserver = nserver_;
    n.niteration = niteration_;
    return n;
}

void MetricsCollector::writePerOnuCsv(const std::string& path) const {
    ensureParentDir(path);
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path);
    out << "onu_id,traffic_type,avg_latency_s,max_latency_s,packets_sent,packets_received,"
           "packet_loss_rate,poll_count,Nserver,Niteration\n";
    for (std::size_t i = 0; i < onus_.size(); ++i) {
        auto s = computeOnuStats(static_cast<int>(i));
        out << s.onuId << ',' << s.trafficType << ',' << s.avgLatency << ',' << s.maxLatency
            << ',' << s.sent << ',' << s.received << ',' << s.lossRate << ',' << s.pollCount
            << ',' << nserver_ << ',' << niteration_ << '\n';
    }
}

void MetricsCollector::writeNetworkCsv(const std::string& path) const {
    ensureParentDir(path);
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path);
    auto n = computeNetworkStats();
    out << "avg_latency_s,max_latency_s,total_sent,total_received,packet_loss_rate,total_polls,"
           "Nserver,Niteration\n";
    out << n.avgLatency << ',' << n.maxLatency << ',' << n.totalSent << ',' << n.totalReceived
        << ',' << n.lossRate << ',' << n.totalPolls << ',' << n.nserver << ',' << n.niteration
        << '\n';
}
