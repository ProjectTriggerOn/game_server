//=============================================================================
// session_test.cpp
//
// Headless conformance test for the connect/join split: an ENet peer is a
// TRANSPORT connection, not a match participant. A client opens its peer at
// process start and sits on the title screen; the server must not spawn it,
// must not count it toward MatchConfig::MIN_PLAYERS, and must not broadcast
// snapshots to it, until it asks to join.
//
// This is a TEST TOOL, not shipped code. Exit code 0 = PASS, 1 = FAIL.
//
// Scenarios:
//   --scenario lurker   One peer connects and never joins. It must receive
//                       MAP_INFO (so it can verify the map before committing)
//                       and ZERO snapshots.
//   --scenario join     One peer connects, holds without joining (zero
//                       snapshots), then sends JOIN_REQUEST and must start
//                       receiving snapshots naming it as the local player.
//   --scenario phantom  Two lurkers plus one joined player. The room holds one
//                       real player against MIN_PLAYERS=2, so the match must
//                       stay in WAITING for the whole window: two clients
//                       parked on the title screen cannot burn a match.
//   --scenario match    The control for `phantom`, which would also pass if
//                       joining were broken outright: two peers that DO join
//                       must carry the room WAITING -> COUNTDOWN -> PLAYING.
//
// Build (from the game_server directory, same flags as the Makefile):
//   g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/session_test.cpp
//       -o session_test -LThirdParty/enet/lib -lenet -lpthread
//=============================================================================

#include <enet/enet.h>
#include "net_common.h"   // Snapshot, MatchState, MAX_PLAYERS (POD)
#include "net_packet.h"   // PacketType, MapInfo

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

using Clock = std::chrono::steady_clock;

static const char* StateName(uint8_t s)
{
    switch (s)
    {
    case MatchState::WAITING:   return "WAITING";
    case MatchState::COUNTDOWN: return "COUNTDOWN";
    case MatchState::PLAYING:   return "PLAYING";
    case MatchState::ENDED:     return "ENDED";
    default:                    return "UNKNOWN";
    }
}

//-----------------------------------------------------------------------------
// One simulated client. Own host per peer, exactly like the real client.
//-----------------------------------------------------------------------------
struct TestPeer
{
    std::string name;
    ENetHost*   host = nullptr;
    ENetPeer*   peer = nullptr;

    bool     connected      = false;
    bool     gotMapInfo     = false;
    bool     joinSent       = false;
    uint32_t snapsBeforeJoin = 0;
    uint32_t snapsAfterJoin  = 0;
    uint8_t  localPlayerId  = 0xFF;
    // Every distinct matchState this peer was told about, in order seen.
    std::vector<uint8_t> statesSeen;

    bool Open(const std::string& label, const char* host_, uint16_t port)
    {
        name = label;
        host = enet_host_create(nullptr, 1, 2, 0, 0);
        if (!host) return false;
        ENetAddress addr;
        enet_address_set_host(&addr, host_);
        addr.port = port;
        peer = enet_host_connect(host, &addr, 2, 0);
        return peer != nullptr;
    }

    void Close()
    {
        if (peer) enet_peer_disconnect_now(peer, 0);
        if (host) enet_host_destroy(host);
        peer = nullptr;
        host = nullptr;
    }

    // Ask to join the match room. This is what the real client sends when the
    // player leaves the title screen, not when the process starts.
    void SendJoin()
    {
        if (!peer || joinSent) return;
        uint8_t type = static_cast<uint8_t>(PacketType::JOIN_REQUEST);
        ENetPacket* pkt = enet_packet_create(&type, 1, ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) return;
        enet_peer_send(peer, 0, pkt);
        joinSent = true;
    }

    void Pump()
    {
        if (!host) return;
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0)
        {
            switch (ev.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                connected = true;
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                connected = false;
                peer = nullptr;
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                if (ev.packet->dataLength >= 1)
                {
                    const PacketType t = static_cast<PacketType>(ev.packet->data[0]);
                    if (t == PacketType::SNAPSHOT &&
                        ev.packet->dataLength == 1 + sizeof(Snapshot))
                    {
                        Snapshot s;
                        std::memcpy(&s, ev.packet->data + 1, sizeof(Snapshot));
                        if (joinSent) snapsAfterJoin++; else snapsBeforeJoin++;
                        localPlayerId = s.localPlayerId;
                        if (statesSeen.empty() || statesSeen.back() != s.matchState)
                            statesSeen.push_back(s.matchState);
                    }
                    else if (t == PacketType::MAP_INFO &&
                             ev.packet->dataLength == 1 + sizeof(MapInfo))
                    {
                        gotMapInfo = true;
                    }
                }
                enet_packet_destroy(ev.packet);
                break;

            default:
                break;
            }
        }
    }

    void ReportStates() const
    {
        std::printf("      [%s] matchStates seen:", name.c_str());
        if (statesSeen.empty()) std::printf(" (none)");
        for (uint8_t s : statesSeen) std::printf(" %s", StateName(s));
        std::printf("\n");
    }
};

//-----------------------------------------------------------------------------
// Pump every peer for `seconds`, calling `onTick(elapsed)` each pass.
//-----------------------------------------------------------------------------
template <typename F>
static void Run(std::vector<TestPeer*>& peers, double seconds, F onTick)
{
    const auto t0 = Clock::now();
    for (;;)
    {
        const double el = std::chrono::duration<double>(Clock::now() - t0).count();
        if (el >= seconds) break;
        for (TestPeer* p : peers) p->Pump();
        onTick(el);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

static bool Check(const char* what, bool ok)
{
    std::printf("   %-4s %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

//=============================================================================
int main(int argc, char** argv)
{
    std::string scenario = "lurker";
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    double secs = 0.0;   // 0 = per-scenario default

    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--secs") && i + 1 < argc) secs = std::atof(argv[++i]);
    }

    if (enet_initialize() != 0)
    {
        std::printf("FAIL: enet_initialize\n");
        return 1;
    }

    bool ok = true;
    std::printf("=== session_test: %s (%s:%u) ===\n", scenario.c_str(), host.c_str(), port);

    //-------------------------------------------------------------------------
    if (scenario == "lurker")
    {
        // A client parked on the title screen: the peer is open, nothing else.
        if (secs <= 0.0) secs = 10.0;

        TestPeer a;
        if (!a.Open("lurker", host.c_str(), port)) { std::printf("FAIL: connect\n"); return 1; }
        std::vector<TestPeer*> peers{ &a };
        Run(peers, secs, [](double) {});

        ok &= Check("peer is connected at the transport level", a.connected);
        ok &= Check("server sent MAP_INFO (so the map can be verified before joining)", a.gotMapInfo);
        ok &= Check("a peer that never joined received ZERO snapshots", a.snapsBeforeJoin == 0);
        std::printf("      snapshots received while never joined: %u\n", a.snapsBeforeJoin);
        a.Close();
    }
    //-------------------------------------------------------------------------
    else if (scenario == "join")
    {
        // Connect at process start, join later — the real client's lifecycle.
        if (secs <= 0.0) secs = 10.0;
        const double joinAt = secs * 0.4;

        TestPeer a;
        if (!a.Open("joiner", host.c_str(), port)) { std::printf("FAIL: connect\n"); return 1; }
        std::vector<TestPeer*> peers{ &a };

        uint32_t snapsAtJoin = 0;
        Run(peers, secs, [&](double el) {
            if (el >= joinAt && !a.joinSent)
            {
                snapsAtJoin = a.snapsBeforeJoin;
                a.SendJoin();
            }
        });

        ok &= Check("no snapshots between CONNECT and JOIN_REQUEST", snapsAtJoin == 0);
        ok &= Check("snapshots start flowing after JOIN_REQUEST", a.snapsAfterJoin > 0);
        ok &= Check("the joined peer is named as a live player", a.localPlayerId < MAX_PLAYERS);
        std::printf("      before join: %u   after join: %u   localPlayerId: %u\n",
                    snapsAtJoin, a.snapsAfterJoin, a.localPlayerId);
        a.Close();
    }
    //-------------------------------------------------------------------------
    else if (scenario == "phantom")
    {
        // Two clients idling on the title screen next to one real player. The
        // real player is alone against MIN_PLAYERS=2, so nothing may start.
        if (secs <= 0.0) secs = 20.0;

        TestPeer t1, t2, j;
        if (!t1.Open("title-1", host.c_str(), port) ||
            !t2.Open("title-2", host.c_str(), port) ||
            !j.Open("player",  host.c_str(), port))
        {
            std::printf("FAIL: connect\n");
            return 1;
        }
        std::vector<TestPeer*> peers{ &t1, &t2, &j };

        Run(peers, secs, [&](double el) {
            if (el >= 1.0 && !j.joinSent && j.gotMapInfo) j.SendJoin();
        });

        bool stayedWaiting = true;
        for (uint8_t s : j.statesSeen) if (s != MatchState::WAITING) stayedWaiting = false;

        ok &= Check("the real player did join", j.joinSent && j.snapsAfterJoin > 0);
        ok &= Check("two title-screen peers received ZERO snapshots",
                    t1.snapsBeforeJoin == 0 && t2.snapsBeforeJoin == 0);
        ok &= Check("match stayed in WAITING: idle peers do not reach MIN_PLAYERS",
                    stayedWaiting);
        j.ReportStates();
        std::printf("      title-1 snapshots: %u   title-2 snapshots: %u\n",
                    t1.snapsBeforeJoin, t2.snapsBeforeJoin);
        t1.Close(); t2.Close(); j.Close();
    }
    //-------------------------------------------------------------------------
    else if (scenario == "match")
    {
        // Two real players. MIN_PLAYERS is met, so the room must actually run:
        // this is what stops `phantom` from passing for the wrong reason.
        if (secs <= 0.0) secs = MatchConfig::COUNTDOWN_DURATION + 8.0;

        TestPeer p1, p2;
        if (!p1.Open("player-1", host.c_str(), port) ||
            !p2.Open("player-2", host.c_str(), port))
        {
            std::printf("FAIL: connect\n");
            return 1;
        }
        std::vector<TestPeer*> peers{ &p1, &p2 };

        Run(peers, secs, [&](double el) {
            if (el >= 1.0 && p1.gotMapInfo) p1.SendJoin();
            if (el >= 2.0 && p2.gotMapInfo) p2.SendJoin();
        });

        bool sawWaiting = false, sawCountdown = false, sawPlaying = false;
        for (uint8_t s : p1.statesSeen)
        {
            if (s == MatchState::WAITING)   sawWaiting = true;
            if (s == MatchState::COUNTDOWN) sawCountdown = true;
            if (s == MatchState::PLAYING)   sawPlaying = true;
        }

        ok &= Check("both peers joined", p1.snapsAfterJoin > 0 && p2.snapsAfterJoin > 0);
        ok &= Check("the first player waited alone (WAITING)", sawWaiting);
        ok &= Check("the second player's join started the COUNTDOWN", sawCountdown);
        ok &= Check("the countdown handed over to PLAYING", sawPlaying);
        p1.ReportStates();
        p1.Close(); p2.Close();
    }
    //-------------------------------------------------------------------------
    else
    {
        std::printf("unknown scenario '%s'\n", scenario.c_str());
        enet_deinitialize();
        return 2;
    }

    enet_deinitialize();
    std::printf("=== %s: %s ===\n", scenario.c_str(), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
