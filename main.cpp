//=============================================================================
// main.cpp
//
// Game Server entry point (console application).
// Runs a dedicated server with 32Hz tick rate using ENet networking.
//=============================================================================

#include "enet_server_network.h"
#include "game_server.h"
#include "server_log.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>

static volatile bool g_Running = true;

void SignalHandler(int)
{
    g_Running = false;
}

int main(int argc, char* argv[])
{
    // Handle Ctrl+C for graceful shutdown
    std::signal(SIGINT, SignalHandler);

    uint16_t port = 7777;
    const char* mapPath = "default.map";   // deployed next to the server binary

    // Parse optional args: --port=XXXX  --map=path
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--port=", 7) == 0)
            port = static_cast<uint16_t>(atoi(argv[i] + 7));
        else if (strncmp(argv[i], "--map=", 6) == 0)
            mapPath = argv[i] + 6;
    }

    printf("========================================\n");
    printf("  TriggerOn Game Server v2.0.1\n");
    printf("  Tick Rate: 32Hz\n");
    printf("  Port: %u\n", port);
    printf("========================================\n");

    // Initialize network
    ENetServerNetwork network;
    network.SetPort(port);
    network.Initialize();

    // Initialize game server logic
    GameServer server;
    server.Initialize(&network, mapPath);

    SLOG_INFO("Running. Press Ctrl+C to stop.");

    // Server main loop
    auto lastTime = std::chrono::high_resolution_clock::now();
    uint32_t lastReportedTick = 0;
    double maxWorkMs = 0.0;   // longest PollEvents+Update this report window (overrun visibility)

    while (g_Running)
    {
        auto now = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;

        // Poll network events (receive packets, handle connections) and run
        // server tick(s) at 32Hz. Measure this work so a flood-induced overrun
        // (work exceeding the 31.25ms tick duration) is visible in the status line.
        network.PollEvents();
        server.Update(dt);
        double workMs = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - now).count();
        if (workMs > maxWorkMs) maxWorkMs = workMs;

        // Periodic status report (every ~1 second = every 32 ticks)
        if (server.GetCurrentTick() - lastReportedTick >= 32)
        {
            lastReportedTick = server.GetCurrentTick();
            SLOG_INFO("Tick: %u | Time: %.1fs | Clients: %zu | Players: %zu | MaxWork: %.1fms",
                server.GetCurrentTick(),
                server.GetServerTime(),
                network.GetConnectedClientCount(),
                server.GetPlayerCount(),
                maxWorkMs);
            network.ReportRecvStatsAndReset();
            maxWorkMs = 0.0;
        }

        // Sleep to avoid burning CPU (~1ms, well under tick duration of 31.25ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    SLOG_INFO("Shutting down...");
    server.Finalize();
    network.Finalize();

    return 0;
}
