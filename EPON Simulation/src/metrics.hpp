// =============================================================================
// metrics.hpp — Per-ONU and network-wide latency / loss / poll counters.
//
// Latency is measured from packet createTime to RESPONSE or FINAL_RESULT
// delivery at the ONU. Poll counts track OLT→Server A status polls only.
// =============================================================================
#pragma once

#include "packet.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct OnuStats {
    int onuId = 0;
    std::string trafficType;   // "server" or "poisson"
    double avgLatency = 0.0;   // seconds
    double maxLatency = 0.0;
    std::size_t sent = 0;
    std::size_t received = 0;
    double lossRate = 0.0;
    std::size_t pollCount = 0; // Server-A path polls attributed to this ONU
};

struct NetworkStats {
    double avgLatency = 0.0;
    double maxLatency = 0.0;
    std::size_t totalSent = 0;
    std::size_t totalReceived = 0;
    double lossRate = 0.0;
    std::size_t totalPolls = 0;
    std::size_t nserver = 0;     // run parameter echoed into CSVs
    std::size_t niteration = 0;
};

class MetricsCollector {
public:
    explicit MetricsCollector(std::size_t numOnus);

    void setTrafficType(int onuId, const std::string& type);
    void setRunParams(std::size_t nserver, std::size_t niteration);

    void onPacketCreated(const Packet& p);
    void onPacketDelivered(const Packet& p, double deliveryTime);
    void onPacketDropped(const Packet& p);
    void onPollSent(int onuId);

    OnuStats computeOnuStats(int onuId) const;
    NetworkStats computeNetworkStats() const;

    void writePerOnuCsv(const std::string& path) const;
    void writeNetworkCsv(const std::string& path) const;

private:
    struct OnuAccum {
        std::string trafficType = "unknown";
        std::size_t sent = 0;
        std::size_t received = 0;
        std::size_t dropped = 0;
        std::size_t pollCount = 0;
        double latencySum = 0.0;
        double maxLatency = 0.0;
    };

    std::vector<OnuAccum> onus_;
    std::size_t nserver_ = 0;
    std::size_t niteration_ = 0;
};
