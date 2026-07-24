//=============================================================================
// flood_client.cpp
//
// Headless ENet load/flood test client for the TriggerOn game server.
// Used to capture before/after data for the single-client-flood mitigation
// (see the Phase 1/2 investigation). This is a TEST TOOL, not shipped code.
//
// Modes:
//   --rate N   Send N InputCmd/sec (a simulated normal player) AND measure the
//              interval between received Snapshots — the victim's view of server
//              health. A healthy server broadcasts every tick (~32/s => ~31ms).
//   --flood    Send InputCmd as fast as possible (the attacker).
//   --junk     Send wrong-length packets as fast as possible (malformed flood,
//              exercises the receive loop without producing valid input).
//
// Options: --host H (default 127.0.0.1)  --port P (7777)
//          --secs S (30)  --label L (tag for log lines)
//
// Build (from the game_server directory, same flags as the server Makefile):
//   g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/flood_client.cpp
//       -o floodtest -LThirdParty/enet/lib -lenet -lpthread   (link: -Lenet lib)
//=============================================================================

#include <enet/enet.h>
#include "net_common.h"   // InputCmd, Snapshot layout (POD)
#include "net_packet.h"   // PacketType

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

using Clock = std::chrono::steady_clock;

// Build a valid 33-byte INPUT_CMD payload. All float fields are finite so the
// packet passes the server's NaN/Inf gate and reaches the tagged-input queue —
// i.e. this exercises the full (worst-case) receive path, not an early reject.
static void BuildInputPacket(uint8_t* buf, uint32_t tickId)
{
    buf[0] = static_cast<uint8_t>(PacketType::INPUT_CMD);
    InputCmd cmd{};
    cmd.tickId       = tickId;
    cmd.moveAxisX    = 0.0f;
    cmd.moveAxisY    = 0.0f;
    cmd.yaw          = 0.0f;
    cmd.pitch        = 0.0f;
    cmd.buttons      = 0;
    cmd.viewTick     = 0;
    cmd.viewTickFrac = 0.0f;
    std::memcpy(buf + 1, &cmd, sizeof(InputCmd));
}

int main(int argc, char** argv)
{
    std::string mode = "rate";
    std::string host = "127.0.0.1";
    std::string label = "client";
    int port = 7777;
    int rate = 60;
    int secs = 30;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--flood") mode = "flood";
        else if (a == "--junk")  mode = "junk";
        else if (a == "--rate" && i + 1 < argc) { mode = "rate"; rate = atoi(argv[++i]); }
        else if (a == "--host" && i + 1 < argc) host  = argv[++i];
        else if (a == "--port" && i + 1 < argc) port  = atoi(argv[++i]);
        else if (a == "--secs" && i + 1 < argc) secs  = atoi(argv[++i]);
        else if (a == "--label"&& i + 1 < argc) label = argv[++i];
    }

    if (enet_initialize() != 0)
    {
        fprintf(stderr, "[%s] enet_initialize failed\n", label.c_str());
        return 1;
    }
    atexit(enet_deinitialize);

    ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!client) { fprintf(stderr, "[%s] host_create failed\n", label.c_str()); return 1; }

    ENetAddress addr;
    enet_address_set_host(&addr, host.c_str());
    addr.port = static_cast<uint16_t>(port);

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    if (!peer) { fprintf(stderr, "[%s] connect failed\n", label.c_str()); return 1; }

    // Wait up to 5s for the connection handshake.
    ENetEvent ev;
    bool connected = false;
    for (int waited = 0; waited < 5000 && !connected; waited += 100)
    {
        while (enet_host_service(client, &ev, 100) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) { connected = true; break; }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
    }
    if (!connected)
    {
        fprintf(stderr, "[%s] no CONNECT within 5s\n", label.c_str());
        enet_host_destroy(client);
        return 1;
    }
    printf("[%s] connected to %s:%d | mode=%s rate=%d secs=%d\n",
           label.c_str(), host.c_str(), port, mode.c_str(), rate, secs);
    fflush(stdout);

    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(secs);

    uint64_t sent = 0, snaps = 0;
    uint32_t tickId = 1;

    // Victim metric: interval between consecutive received snapshots (per-second window).
    auto lastSnap = Clock::now();
    double sumIntervalMs = 0.0, maxIntervalMs = 0.0;
    uint64_t intervalCount = 0;

    // Rate pacing.
    auto nextSend = Clock::now();
    const std::chrono::nanoseconds sendPeriod(rate > 0 ? (1000000000LL / rate) : 0);

    // Per-second progress reporting.
    auto lastReport = Clock::now();
    uint64_t sentAtReport = 0, snapAtReport = 0;

    uint8_t inbuf[1 + sizeof(InputCmd)];
    const uint8_t junkbuf[5] = { 0xEE, 1, 2, 3, 4 };  // wrong length + unknown type

    bool running = true;
    while (running && Clock::now() < deadline)
    {
        // 1. Drain incoming; time the gaps between snapshots.
        while (enet_host_service(client, &ev, 0) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                if (ev.packet->dataLength >= 1 &&
                    ev.packet->data[0] == static_cast<uint8_t>(PacketType::SNAPSHOT))
                {
                    auto now = Clock::now();
                    double ms = std::chrono::duration<double, std::milli>(now - lastSnap).count();
                    lastSnap = now;
                    sumIntervalMs += ms;
                    if (ms > maxIntervalMs) maxIntervalMs = ms;
                    intervalCount++;
                    snaps++;
                }
                enet_packet_destroy(ev.packet);
            }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                fprintf(stderr, "[%s] disconnected by server\n", label.c_str());
                running = false;
                break;
            }
        }

        // 2. Send according to mode.
        if (mode == "flood")
        {
            for (int b = 0; b < 512; ++b)
            {
                BuildInputPacket(inbuf, tickId++);
                ENetPacket* p = enet_packet_create(inbuf, sizeof(inbuf), ENET_PACKET_FLAG_UNSEQUENCED);
                if (p) { enet_peer_send(peer, 0, p); sent++; }
            }
            enet_host_flush(client);
        }
        else if (mode == "junk")
        {
            for (int b = 0; b < 512; ++b)
            {
                ENetPacket* p = enet_packet_create(junkbuf, sizeof(junkbuf), ENET_PACKET_FLAG_UNSEQUENCED);
                if (p) { enet_peer_send(peer, 0, p); sent++; }
            }
            enet_host_flush(client);
        }
        else // rate
        {
            auto now = Clock::now();
            while (now >= nextSend)
            {
                BuildInputPacket(inbuf, tickId++);
                ENetPacket* p = enet_packet_create(inbuf, sizeof(inbuf), ENET_PACKET_FLAG_UNSEQUENCED);
                if (p) { enet_peer_send(peer, 0, p); sent++; }
                nextSend += sendPeriod;
            }
            enet_host_flush(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // 3. Per-second progress line.
        auto now = Clock::now();
        if (now - lastReport >= std::chrono::seconds(1))
        {
            double dt = std::chrono::duration<double>(now - lastReport).count();
            double sps = (sent - sentAtReport) / dt;
            double rps = (snaps - snapAtReport) / dt;
            double avgInt = intervalCount ? (sumIntervalMs / intervalCount) : 0.0;
            printf("[%s] send=%.0f/s snapRecv=%.0f/s snapInterval(avg/max)=%.1f/%.1fms\n",
                   label.c_str(), sps, rps, avgInt, maxIntervalMs);
            fflush(stdout);
            lastReport = now;
            sentAtReport = sent;
            snapAtReport = snaps;
            sumIntervalMs = 0.0; maxIntervalMs = 0.0; intervalCount = 0;  // reset window
        }
    }

    double total = std::chrono::duration<double>(Clock::now() - start).count();
    printf("[%s] DONE: sent=%llu (%.0f/s) snapRecv=%llu (%.0f/s) over %.1fs\n",
           label.c_str(),
           (unsigned long long)sent, total > 0 ? sent / total : 0.0,
           (unsigned long long)snaps, total > 0 ? snaps / total : 0.0,
           total);
    fflush(stdout);

    enet_peer_disconnect(peer, 0);
    for (int waited = 0; waited < 1000; waited += 100)
    {
        bool done = false;
        while (enet_host_service(client, &ev, 100) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { done = true; break; }
        }
        if (done) break;
    }
    enet_host_destroy(client);
    return 0;
}
