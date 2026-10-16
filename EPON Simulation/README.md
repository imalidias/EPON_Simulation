# 10G EPON + Server A Event-Driven Simulation

C++ event-driven network simulator for a 10G EPON with **N** ONUs, an OLT, and an external **Server A** connected to the OLT by a 10 Gbps optical link.

## Build and Run

Requires a C++17 compiler (`c++` / `clang++` / `g++`).

```bash
make          # build ./epon_sim
make run      # build and run with defaults
```

Example with custom parameters:

```bash
./epon_sim --N 16 --Nserver 4 --Dserver 1500 --Tpoisson 0.001 --Tserver 0.012 \
  --Npoll 10 --Niteration 2 --lambda 100 --duration 1.0
```

Outputs:

- `results/onu_stats.csv` — per-ONU average/max latency and loss
- `results/network_stats.csv` — network-wide aggregates

Signalling input files:

- `data/presignalling.csv`
- `data/postsignalling.csv`

CSV columns: `seq,direction,size_bytes,delay_us` with `direction` = `OLT_TO_SERVER` or `SERVER_TO_OLT`.

## Scenario Overview

```
ONUs (1..Nserver) ──Dserver (1500B, once)──► OLT ──10G optical──► Server A
ONUs (Nserver+1..N) ──Poisson continuous──► OLT (process Tpoisson) ──response──► ONU
```

### Traffic model

| ONU group | Count | Traffic | Destination / processing |
|-----------|-------|---------|---------------------------|
| Server ONUs | `Nserver` | Fixed `Dserver` bytes (default 1500), **once** per simulation | Sent to Server A via OLT |
| Poisson ONUs | `N - Nserver` | Continuous Poisson arrivals | Processed at OLT for `Tpoisson`, one response packet back to the ONU |

### Server-A path (per job, for `Niteration` iterations)

1. **Pre-process signalling** between OLT and Server A (driven by `presignalling.csv`)
2. OLT sends `Dserver` bytes to Server A
3. Server A processes for `Tserver`; OLT **polls every 5 ms** (up to `Npoll` times)
4. Server A sends confirmation; **post-process signalling** (driven by `postsignalling.csv`)
5. Repeat steps 1–4 for `Niteration` iterations
6. OLT sends a **final result** packet to the respective ONU

### Upstream DBA: IPACT (MPCP GATE / REPORT)

ONU → OLT upstream uses **limited-service IPACT**:

1. User traffic is queued at each ONU (not sent immediately).
2. OLT issues interleaved **GATE** messages with `(grantStart, grantBytes)`.
3. Grant size = `min(last REPORT queue length, maxGrantBytes)` (`Wmax`).
4. At `grantStart`, the ONU sends granted data, then an MPCP **REPORT** with remaining queue bytes.
5. Grants are separated by a **guard time** on the shared upstream; the next GATE is issued without waiting for the prior REPORT (interleaved polling).

Downstream (GATE, RESPONSE, FINAL_RESULT) uses a simple delay model. OLT → Server A remains separate (round-robin below).

### Multi-ONU OLT → Server A policy (round-robin)

Only **one** Server-A job may use the OLT ↔ Server A path at a time.

- Arriving `DATA_SERVER` jobs are placed in a per-ONU waiting slot.
- When the path is free, the OLT picks the next waiting Server ONU in **round-robin** order (`0 .. Nserver-1`).
- After each completed iteration (pre + process + post), the job is **re-queued** so other ONUs get a turn before the same ONU’s next iteration.
- When all `Niteration` loops finish, the OLT sends `FINAL_RESULT` to that ONU and schedules the next waiting ONU.

### Metrics

Per-ONU and network-wide:

- Average latency
- Maximum latency
- Packet loss

Written to two CSV files (see [MetricsCollector](#10-metricscollector)).

---

## Configuration Parameters

| Symbol | Meaning |
|--------|---------|
| `N` | Total number of ONUs |
| `Nserver` | Number of ONUs that generate one-shot Server-A traffic |
| `Dserver` | Payload size for Server-A traffic (1500 bytes) |
| `Tpoisson` | OLT processing time for each Poisson packet |
| `Tserver` | Server A processing time |
| `Npoll` | Maximum number of polling attempts (interval = 5 ms) |
| `Niteration` | Number of pre-signalling + processing + post-signalling loops |

Shared structures: `SimConfig`, `Packet`, `Event`, `EventType`.

**Packet types:** `DATA_SERVER`, `DATA_POISSON`, `GATE`, `REPORT`, `POLL`, `POLL_ACK`, `CONFIRM`, `SIG_PRE`, `SIG_POST`, `RESPONSE`, `FINAL_RESULT`

---

## Architecture: Classes and Functions

### 1. `EventScheduler` (event engine)

- `schedule(Event e)`
- `cancel(eventId)` (optional)
- `run()` / `runUntil(tEnd)`
- `now()` — current simulation time
- `isEmpty()`

Uses a min-heap (`priority_queue`) ordered by `Event.time`.

### 2. `Link` / `OpticalLink`

Models the 10 Gbps OLT ↔ Server A link and ONU ↔ OLT links.

- `transmissionDelay(bytes, rateGbps)`
- `propagationDelay(distance_m)` (optional)
- `send(Packet p, Node* from, Node* to)` — schedules `ARRIVAL` at `now + tx + prop`

### 3. `ONU`

**Server ONUs (`id < Nserver`):**

- `generateServerTrafficOnce()` — create one `DATA_SERVER` of size `Dserver`, send toward OLT
- `onFinalResult(Packet)` — record latency for the Server-A workflow

**Poisson ONUs (`id >= Nserver`):**

- `scheduleNextPoissonArrival()` — exponential inter-arrival
- `generatePoissonPacket()` — create `DATA_POISSON`, send to OLT
- `onPoissonResponse(Packet)` — record RTT / latency

**Common:**

- `enqueueUplink(Packet)`
- `onGrant(...)` / `transmitUplink(...)` (if DBA/MPCP is modelled; can be simplified)
- `recordSent` / `recordReceived` for metrics

### 4. `OLT`

- `onPacketArrival(Packet)` — dispatch by type / source
- `handleServerDataArrival(Packet)` — start pre-signalling for a Server-A job
- `handlePoissonArrival(Packet)` — queue or start `Tpoisson` processing
- `finishPoissonProcessing(jobId)` — send one `RESPONSE` packet back to ONU
- `startServerJob(onuId, Packet)` — kick off iteration loop
- `sendDataToServer(Dserver)` — after pre-signalling
- `startPolling(jobId)` — schedule first poll at +5 ms
- `onPollResponse(jobId, done)` — continue or stop polling
- `onServerConfirm(jobId)` — start post-signalling
- `onIterationComplete(jobId)` — if `iter < Niteration` restart pre-signalling; else send final result
- `sendFinalResultToONU(onuId)` — one packet OLT → ONU

### 5. `ServerA`

- `onPreSignallingComplete()`
- `onDataArrival(Packet Dserver)` — schedule processing done at `now + Tserver`
- `onPoll(Packet)` — reply busy/done; after finish (and within `Npoll`), allow done
- `sendConfirmation()` — triggers post-signalling path
- `onPostSignallingComplete()`

**Polling rule:** processing finishes at `start + Tserver`; OLT polls every 5 ms; Server reports done on/after finish; OLT stops after `Npoll` attempts (define timeout behaviour: loss vs force-complete).

### 6. `PreSignalling` (reads `presignalling.csv`)

- `load(const string& path)` — parse message sequence
- `start(jobId, callback)` — schedule message-by-message exchanges over the 10G link
- `onMessageSent` / `onMessageArrived` — advance state machine
- `isComplete()` — invoke OLT callback to send `Dserver`

**Suggested CSV columns:** `seq, direction, size_bytes, delay_us`  
`direction`: `OLT_TO_SERVER` | `SERVER_TO_OLT`

### 7. `PostSignalling` (reads `postsignalling.csv`)

Same interface as pre-signalling:

- `load`, `start`, `advance`, `isComplete`

### 8. `PollingManager`

- `start(jobId, olt, server)` — schedule poll at +5 ms
- `sendPoll(jobId)`
- `onPollReply(jobId, finished)`
- `stop(jobId)` — when finished or `pollCount >= Npoll`
- `pollCount(jobId)`

### 9. `TrafficGenerator`

- `initServerOnus(vector<ONU*>&)`
- `initPoissonOnus(vector<ONU*>&, lambda)`
- `poissonInterArrival()` — `Exp(λ)`

### 10. `MetricsCollector`

- `onPacketCreated(Packet)`
- `onPacketDelivered(Packet)`
- `onPacketDropped(Packet, reason)`
- `computePerOnuStats()`
- `computeNetworkStats()`
- `writePerOnuCsv("onu_stats.csv")`
- `writeNetworkCsv("network_stats.csv")`

**`onu_stats.csv` columns:**  
`onu_id, traffic_type, avg_latency_s, max_latency_s, packets_sent, packets_received, packet_loss_rate, poll_count, Nserver, Niteration`

**`network_stats.csv` columns:**  
`avg_latency_s, max_latency_s, total_sent, total_received, packet_loss_rate, total_polls, Nserver, Niteration`

### 11. `Simulation` / `main`

- `loadConfig(...)`
- `buildTopology()` — OLT, Server A, N ONUs, links
- `PreSignalling::load("presignalling.csv")`
- `PostSignalling::load("postsignalling.csv")`
- `seedInitialEvents()` — Server ONU one-shots + Poisson first arrivals
- `scheduler.run()`
- `metrics.write...()`

---

## Event Types

| Event | Handler |
|-------|---------|
| `ONU_SERVER_TX` | ONU sends `Dserver` once |
| `ONU_POISSON_ARRIVAL` | Generate Poisson packet + reschedule next |
| `PKT_ARRIVAL_OLT` | `OLT::onPacketArrival` |
| `PKT_ARRIVAL_SERVER` | `ServerA` data / signalling RX |
| `PKT_ARRIVAL_ONU` | ONU RX (response / final result) |
| `PRE_SIG_STEP` | `PreSignalling` next message |
| `POST_SIG_STEP` | `PostSignalling` next message |
| `SERVER_PROCESS_DONE` | Mark job ready for confirm |
| `POLL_TIMER` | `PollingManager::sendPoll` |
| `POISSON_PROCESS_DONE` | OLT sends one response to ONU |
| `ITERATION_NEXT` | Start next of `Niteration` |
| `SIM_END` | Stop simulation and write CSVs |

---

## Server-A Call Flow

1. `ONU::generateServerTrafficOnce` → uplink → `OLT::handleServerDataArrival`
2. `PreSignalling::start` (CSV-driven) until complete
3. `OLT::sendDataToServer(Dserver)`
4. `ServerA` starts `Tserver`; `PollingManager` polls every 5 ms (≤ `Npoll`)
5. Server done → confirmation + `PostSignalling::start`
6. Repeat 2–5 for `Niteration`
7. `OLT::sendFinalResultToONU` → metrics update

**Poisson path (parallel):** arrive at OLT → wait `Tpoisson` → one response packet → metrics.

---

## Implemented File Layout

```
src/
  main.cpp                 # CLI entry point
  config.hpp               # SimConfig parameters
  packet.hpp               # Packet / endpoint types
  event_scheduler.hpp      # Discrete-event engine
  signalling.hpp|.cpp      # Pre/post CSV signalling
  polling_manager.hpp|.cpp # 5 ms polling (≤ Npoll)
  metrics.hpp|.cpp         # Latency / loss + CSV writers
  simulation.hpp|.cpp      # ONU / OLT / Server A logic
data/
  presignalling.csv
  postsignalling.csv
results/
  onu_stats.csv            # generated
  network_stats.csv        # generated
Makefile
README.md
```

---

## Recommended Implementation Order

1. `EventScheduler` + `Packet` + `Link`
2. Topology: OLT, ONUs, Server A (no signalling yet)
3. Poisson path end-to-end + metrics
4. Pre/post CSV signalling + `Dserver` path
5. `PollingManager` + `Niteration` + final ONU packet
6. CSV stats writers

---

## Signalling Data Files

| File | Role |
|------|------|
| `data/presignalling.csv` | Message sequence for OLT ↔ Server A **before** `Dserver` transfer |
| `data/postsignalling.csv` | Message sequence for OLT ↔ Server A **after** confirmation |

A dedicated class/function loads each CSV and drives the corresponding signalling exchange when Server-A traffic arrives at the OLT (pre) or when Server A confirms completion (post).
