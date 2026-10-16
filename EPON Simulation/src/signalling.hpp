// =============================================================================
// signalling.hpp — CSV-driven pre/post signalling between OLT and Server A.
//
// CSV columns: seq,direction,size_bytes,delay_us
//   direction = OLT_TO_SERVER | SERVER_TO_OLT
// delay_us is applied after a message arrives, before the next is sent.
// =============================================================================
#pragma once

#include "event_scheduler.hpp"
#include "packet.hpp"

#include <functional>
#include <string>
#include <vector>

enum class SigDirection { OLT_TO_SERVER, SERVER_TO_OLT };

struct SigMessage {
    int seq = 0;
    SigDirection direction = SigDirection::OLT_TO_SERVER;
    std::size_t sizeBytes = 64;
    double delaySec = 0.0; // optional processing delay after arrival (converted from us)
};

// Immutable message list loaded once from presignalling.csv / postsignalling.csv.
class SignallingSequence {
public:
    void load(const std::string& path);
    const std::vector<SigMessage>& messages() const { return messages_; }
    bool empty() const { return messages_.empty(); }

private:
    std::vector<SigMessage> messages_;
};

// One active exchange for a single Server-A job (pre or post).
// Advances message-by-message; invokes DoneFn when the CSV sequence completes.
class SignallingSession {
public:
    using TxFn = std::function<void(Packet)>;
    using DoneFn = std::function<void()>;

    SignallingSession(EventScheduler& sched, const SignallingSequence& seq, PacketType sigType);

    void start(int jobId, int ownerOnu, double createTime, TxFn tx, DoneFn done);

    // Advance state when the expected signalling packet arrives at its destination.
    void onArrival(const Packet& p);

private:
    void sendNext();

    EventScheduler& sched_;
    const SignallingSequence& seq_;
    PacketType sigType_; // SIG_PRE or SIG_POST

    int jobId_ = -1;
    int ownerOnu_ = -1;
    double createTime_ = 0.0;
    std::size_t nextIdx_ = 0;
    bool active_ = false;
    TxFn tx_;
    DoneFn done_;
    std::uint64_t nextPktId_ = 1;
};
