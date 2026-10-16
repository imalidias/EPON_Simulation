// =============================================================================
// packet.hpp — Packet types and endpoint identifiers used on all simulated links.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

// Logical packet kinds on the PON, OLT–Server path, and control plane.
enum class PacketType {
    DATA_SERVER,   // one-shot Server-A request from a Server ONU
    DATA_POISSON,  // Poisson uplink data (processed at OLT)
    RESPONSE,      // OLT → ONU reply after Tpoisson
    FINAL_RESULT,  // OLT → ONU after Niteration Server-A loops
    GATE,          // MPCP GATE (OLT → ONU), IPACT grant
    REPORT,        // MPCP REPORT (ONU → OLT), queue advertisement
    POLL,          // OLT → Server A status poll during Tserver
    POLL_ACK,      // Server A → OLT poll reply
    CONFIRM,       // Server A → OLT task-complete confirmation
    SIG_PRE,       // pre-process signalling message (CSV-driven)
    SIG_POST       // post-process signalling message (CSV-driven)
};

enum class NodeKind { ONU, OLT, SERVER };

// Addressing: for ONU endpoints, id is the ONU index (0 .. N-1).
struct Endpoint {
    NodeKind kind = NodeKind::OLT;
    int id = 0;
};

inline std::string packetTypeName(PacketType t) {
    switch (t) {
        case PacketType::DATA_SERVER: return "DATA_SERVER";
        case PacketType::DATA_POISSON: return "DATA_POISSON";
        case PacketType::RESPONSE: return "RESPONSE";
        case PacketType::FINAL_RESULT: return "FINAL_RESULT";
        case PacketType::GATE: return "GATE";
        case PacketType::REPORT: return "REPORT";
        case PacketType::POLL: return "POLL";
        case PacketType::POLL_ACK: return "POLL_ACK";
        case PacketType::CONFIRM: return "CONFIRM";
        case PacketType::SIG_PRE: return "SIG_PRE";
        case PacketType::SIG_POST: return "SIG_POST";
    }
    return "UNKNOWN";
}

struct Packet {
    std::uint64_t id = 0;
    PacketType type = PacketType::DATA_POISSON;
    Endpoint src;
    Endpoint dst;
    std::size_t sizeBytes = 0;
    double createTime = 0.0;  // sim time when request was generated (latency start)
    double sendTime = 0.0;    // sim time when placed on a link
    int ownerOnu = -1;        // originating ONU (metrics / Server-A job ownership)
    int jobId = -1;           // Server-A job id (when applicable)
    int sigSeq = -1;          // CSV signalling sequence number
    bool pollDone = false;    // POLL_ACK: true if Server A finished processing

    // MPCP IPACT fields (used by GATE / REPORT)
    double grantStart = 0.0;          // GATE: absolute time ONU may start upstream TX
    std::size_t grantBytes = 0;       // GATE: max data bytes allowed in this window
    std::size_t reportQueueBytes = 0; // REPORT: remaining ONU uplink queue (bytes)
};
