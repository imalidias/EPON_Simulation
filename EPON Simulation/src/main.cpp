// =============================================================================
// main.cpp — CLI entry point for the 10G EPON + Server A simulator.
//
// Parses options into SimConfig, builds the model, runs until simDuration,
// and writes results/onu_stats.csv and results/network_stats.csv.
// =============================================================================

#include "config.hpp"
#include "simulation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void printUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "  --N <int>              Total ONUs (default 8)\n"
        << "  --Nserver <int>        Server-A ONUs (default 2)\n"
        << "  --Dserver <int>        Server payload bytes (default 1500)\n"
        << "  --Tpoisson <sec>       OLT Poisson processing time (default 0.001)\n"
        << "  --Tserver <sec>        Server A processing time (default 0.012)\n"
        << "  --Npoll <int>          Max polls (default 10)\n"
        << "  --Niteration <int>     Pre/process/post iterations (default 2)\n"
        << "  --lambda <pkt/s>       Poisson rate per ONU (default 100)\n"
        << "  --duration <sec>       Simulation duration (default 1.0)\n"
        << "  --seed <uint>          RNG seed (default 42)\n"
        << "  --max-grant <bytes>    IPACT Wmax grant (default 15532)\n"
        << "  --guard-us <us>        IPACT guard time in microseconds (default 5)\n"
        << "  --pre <path>           presignalling.csv path\n"
        << "  --post <path>          postsignalling.csv path\n"
        << "  --onu-out <path>       Per-ONU stats CSV\n"
        << "  --net-out <path>       Network stats CSV\n";
}

// Fill cfg from argv; return false on help / bad args (caller should exit).
bool parseArgs(int argc, char** argv, SimConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + name);
            return argv[++i];
        };
        try {
            if (a == "--N") cfg.N = static_cast<std::size_t>(std::stoul(need("--N")));
            else if (a == "--Nserver")
                cfg.Nserver = static_cast<std::size_t>(std::stoul(need("--Nserver")));
            else if (a == "--Dserver")
                cfg.Dserver = static_cast<std::size_t>(std::stoul(need("--Dserver")));
            else if (a == "--Tpoisson") cfg.Tpoisson = std::stod(need("--Tpoisson"));
            else if (a == "--Tserver") cfg.Tserver = std::stod(need("--Tserver"));
            else if (a == "--Npoll")
                cfg.Npoll = static_cast<std::size_t>(std::stoul(need("--Npoll")));
            else if (a == "--Niteration")
                cfg.Niteration = static_cast<std::size_t>(std::stoul(need("--Niteration")));
            else if (a == "--lambda") cfg.poissonLambda = std::stod(need("--lambda"));
            else if (a == "--duration") cfg.simDuration = std::stod(need("--duration"));
            else if (a == "--seed") cfg.seed = static_cast<unsigned>(std::stoul(need("--seed")));
            else if (a == "--max-grant")
                cfg.maxGrantBytes = static_cast<std::size_t>(std::stoul(need("--max-grant")));
            else if (a == "--guard-us") cfg.guardTime = std::stod(need("--guard-us")) * 1e-6;
            else if (a == "--pre") cfg.preSignallingCsv = need("--pre");
            else if (a == "--post") cfg.postSignallingCsv = need("--post");
            else if (a == "--onu-out") cfg.onuStatsCsv = need("--onu-out");
            else if (a == "--net-out") cfg.networkStatsCsv = need("--net-out");
            else if (a == "-h" || a == "--help") {
                printUsage(argv[0]);
                return false;
            } else {
                std::cerr << "Unknown option: " << a << '\n';
                printUsage(argv[0]);
                return false;
            }
        } catch (const std::exception& ex) {
            std::cerr << "Argument error: " << ex.what() << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SimConfig cfg;
    if (!parseArgs(argc, argv, cfg)) return 1;

    try {
        Simulation sim(cfg);
        sim.build();
        sim.run();
        sim.writeResults();

        // Console summary (full tables are in the CSV files)
        const auto net = sim.metrics().computeNetworkStats();
        std::cout << "Network avg latency (s): " << net.avgLatency << '\n'
                  << "Network max latency (s): " << net.maxLatency << '\n'
                  << "Sent / received / loss: " << net.totalSent << " / " << net.totalReceived
                  << " / " << net.lossRate << '\n'
                  << "Total polling cycles: " << net.totalPolls << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "Simulation failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
