// =============================================================================
// config.hpp — Simulation parameters for the 10G EPON + Server A model.
//
// Defaults can be overridden from the command line (see main.cpp).
// Time units are seconds unless noted; rates are bits per second.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>

struct SimConfig {
    // --- Topology / traffic ---
    std::size_t N = 8;              // total ONUs
    std::size_t Nserver = 2;        // first Nserver ONUs send one-shot Dserver traffic to Server A
    std::size_t Dserver = 1500;     // Server-A payload size (bytes)
    double Tpoisson = 0.001;        // OLT processing time for each Poisson packet (s)
    double Tserver = 0.012;         // Server A processing time after receiving Dserver (s)
    std::size_t Npoll = 10;         // max OLT→Server A status polls per iteration
    double pollInterval = 0.005;    // poll spacing (5 ms)
    std::size_t Niteration = 2;     // pre-signalling + process + post-signalling loops

    // --- Link model ---
    double linkRateBps = 10e9;      // 10 Gbps (PON and OLT–Server optical links)
    double propDelayOnuOlt = 50e-6; // one-way ONU↔OLT propagation (s)
    double propDelayOltServer = 20e-6; // one-way OLT↔Server A propagation (s)

    // --- Poisson sources (ONUs Nserver .. N-1) ---
    double poissonLambda = 100.0;   // packets/s per Poisson ONU
    std::size_t poissonPktSize = 1518;

    // --- Run control ---
    double simDuration = 1.0;       // simulated time horizon (s)
    std::size_t oltQueueLimit = 10000;

    // --- IPACT / MPCP upstream DBA (limited service) ---
    double guardTime = 5e-6;              // guard band between ONU upstream grants (s)
    std::size_t maxGrantBytes = 15532;    // Wmax (~10×1518 + REPORT headroom)
    std::size_t gateSizeBytes = 64;       // MPCP GATE frame size
    std::size_t reportSizeBytes = 64;     // MPCP REPORT frame size
    std::size_t onuQueueLimitPkts = 1000; // per-ONU uplink drop threshold (packets)

    // --- I/O paths (relative to working directory when running epon_sim) ---
    std::string preSignallingCsv = "presignalling.csv";
    std::string postSignallingCsv = "postsignalling.csv";
    std::string onuStatsCsv = "results/onu_stats.csv";
    std::string networkStatsCsv = "results/network_stats.csv";
    unsigned seed = 42;                   // RNG seed for Poisson arrivals
};
