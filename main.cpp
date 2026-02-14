//=============================================================================
// main.cpp
//
// Game Server entry point (console application).
// Runs a dedicated server with 32Hz tick rate using ENet networking.
//=============================================================================

#include "enet_server_network.h"
#include "game_server.h"
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

    // Parse optional port argument: --port=XXXX
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--port=", 7) == 0)
        {
            port = static_cast<uint16_t>(atoi(argv[i] + 7));
        }
    }

    printf("========================================\n");
    printf("  TriggerOn Game Server v1.0.2\n");
    printf("  Tick Rate: 32Hz\n");
    printf("  Port: %u\n", port);
    printf("========================================\n");

    // Initialize network
    ENetServerNetwork network;
    network.SetPort(port);
    network.Initialize();

    // Initialize game server logic
    GameServer server;
    server.Initialize(&network);

    printf("[Server] Running. Press Ctrl+C to stop.\n\n");

    // Server main loop
    auto lastTime = std::chrono::high_resolution_clock::now();
    uint32_t lastReportedTick = 0;

    while (g_Running)
    {
        auto now = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;

        // Poll network events (receive packets, handle connections)
        network.PollEvents();

        // Run server tick(s) at 32Hz
        server.Update(dt);

        // Periodic status report (every ~1 second = every 32 ticks)
        if (server.GetCurrentTick() - lastReportedTick >= 32)
        {
            lastReportedTick = server.GetCurrentTick();
            printf("[Server] Tick: %u | Time: %.1fs | Clients: %zu\n",
                server.GetCurrentTick(),
                server.GetServerTime(),
                network.GetConnectedClientCount());
        }

        // Sleep to avoid burning CPU (~1ms, well under tick duration of 31.25ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    printf("\n[Server] Shutting down...\n");
    server.Finalize();
    network.Finalize();

    return 0;
}
