// =============================================================================
// signalling.cpp — Load signalling CSVs and drive OLT↔Server message exchanges.
// =============================================================================

#include "signalling.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

double usToSec(double us) { return us * 1e-6; }

} // namespace

// Parse CSV: seq,direction,size_bytes,delay_us  (header row optional).
void SignallingSequence::load(const std::string& path) {
    messages_.clear();
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open signalling CSV: " + path);

    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (first) {
            first = false;
            // Skip header if the first line looks like column names
            if (line.find("seq") != std::string::npos || line.find("direction") != std::string::npos) {
                continue;
            }
        }

        std::stringstream ss(line);
        std::string seqStr, dirStr, sizeStr, delayStr;
        if (!std::getline(ss, seqStr, ',')) continue;
        if (!std::getline(ss, dirStr, ',')) continue;
        if (!std::getline(ss, sizeStr, ',')) continue;
        if (!std::getline(ss, delayStr, ',')) delayStr = "0";

        SigMessage m;
        m.seq = std::stoi(trim(seqStr));
        dirStr = trim(dirStr);
        if (dirStr == "OLT_TO_SERVER") m.direction = SigDirection::OLT_TO_SERVER;
        else if (dirStr == "SERVER_TO_OLT") m.direction = SigDirection::SERVER_TO_OLT;
        else throw std::runtime_error("Unknown signalling direction: " + dirStr);
        m.sizeBytes = static_cast<std::size_t>(std::stoul(trim(sizeStr)));
        m.delaySec = usToSec(std::stod(trim(delayStr)));
        messages_.push_back(m);
    }
}

SignallingSession::SignallingSession(EventScheduler& sched, const SignallingSequence& seq,
                                     PacketType sigType)
    : sched_(sched), seq_(seq), sigType_(sigType) {}

void SignallingSession::start(int jobId, int ownerOnu, double createTime, TxFn tx, DoneFn done) {
    jobId_ = jobId;
    ownerOnu_ = ownerOnu;
    createTime_ = createTime;
    tx_ = std::move(tx);
    done_ = std::move(done);
    nextIdx_ = 0;
    active_ = true;
    if (seq_.empty()) {
        active_ = false;
        if (done_) done_();
        return;
    }
    sendNext();
}

// Emit the next CSV message (or invoke done_ when the sequence is finished).
void SignallingSession::sendNext() {
    if (!active_ || nextIdx_ >= seq_.messages().size()) {
        active_ = false;
        if (done_) done_();
        return;
    }

    const auto& m = seq_.messages()[nextIdx_];
    Packet p;
    p.id = nextPktId_++;
    p.type = sigType_;
    p.sizeBytes = m.sizeBytes;
    p.createTime = createTime_;
    p.sendTime = sched_.now();
    p.ownerOnu = ownerOnu_;
    p.jobId = jobId_;
    p.sigSeq = m.seq;

    if (m.direction == SigDirection::OLT_TO_SERVER) {
        p.src = {NodeKind::OLT, 0};
        p.dst = {NodeKind::SERVER, 0};
    } else {
        p.src = {NodeKind::SERVER, 0};
        p.dst = {NodeKind::OLT, 0};
    }

    if (tx_) tx_(p);
}

// After the expected message arrives, wait delay_us then send the next one.
void SignallingSession::onArrival(const Packet& p) {
    if (!active_ || p.jobId != jobId_) return;
    if (nextIdx_ >= seq_.messages().size()) return;

    const auto& m = seq_.messages()[nextIdx_];
    if (p.sigSeq != m.seq) return;

    ++nextIdx_;
    const double delay = m.delaySec;
    if (delay > 0.0) {
        sched_.scheduleAt(sched_.now() + delay, EventType::PRE_SIG_STEP, [this]() { sendNext(); },
                          jobId_);
    } else {
        sendNext();
    }
}
