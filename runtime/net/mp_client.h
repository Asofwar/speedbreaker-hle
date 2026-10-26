// The multiplayer client (net/mp_client.cpp): connects to an mp::Server, sends the local car's state and keeps, for
// every other player, a short history from which RemoteCar() gives the pose to draw now (interpolated, then dead
// reckoned when packets are late). No game code in here; game/mp_adapter ties it to the game's vehicles.
#pragma once
#include "mp_proto.h"
#include "mp_race.h"
#include <functional>
#include <map>
#include <memory>

namespace mp
{
    struct Remote
    {
        uint8_t id = 0;
        std::string name;
        bool hasCar = false;
        CarState latest;   // newest state received
        uint64_t carKey = 0;   // the car it chose in the lobby (type key, 0: none yet)
        bool lobbyReady = false;   // it is ready to race (its game makes cars for the ones who are)
        uint32_t raceKey = 0;      // the race its game has selected in the front end (0: none)
        std::shared_ptr<const SkinData> skin;   // its car's skin as its game sent it (null: none yet)
    };

    class Client
    {
    public:
        Client();
        ~Client();
        bool Connect(const std::string& host, uint16_t port, const std::string& name);
        void Disconnect();
        // Pump the network (non-blocking). Call every frame.
        void Service();
        bool Connected() const;     // Welcome received
        bool Failed() const;        // refused or lost
        uint8_t MyId() const;
        void SendCar(const CarState& c);   // unreliable; call at the send rate (about 30 Hz)
        void SendChat(const std::string& text);
        // My car's skin, to the server (which keeps it for the others, also those who join later).
        void SendSkin(const SkinData& skin);
        // Tell everyone which car this player chose in the lobby (the game's car type key, 0: none) and whether it is ready to race.
        void SetLobby(uint64_t carKey, bool ready, uint32_t raceKey = 0);
        uint64_t MyCarKey() const;
        // The start barrier is the server's: Hold says this game is held at the start of a race that needs `need` players (the world clock is what the games line up by),
        // Unhold leaves it, BarrierStatus is what the server last said (held, needed), and TakeGo hands over the server's release once: the slowest world clock and the client
        // time (NowMs()) at which that clock's game starts.
        void SendHold(uint8_t need, float worldClock);
        void SendUnhold();
        void BarrierStatus(int& held, int& need) const;
        bool TakeGo(float& slowestClock, uint32_t& startAtMs);
        const std::map<uint8_t, Remote>& Remotes() const;
        // The pose of a remote car for the local clock `nowMs` (ms since the client started, see NowMs()), delayed by
        // the interpolation delay and extrapolated by velocity if the data is late. False if nothing was received.
        bool RemoteCar(uint8_t id, uint32_t nowMs, CarState& out) const;
        // The open world's races (from the server) and the race in progress as the server last said, with its time carried forward to now. Any thread.
        std::vector<RaceDef> Races() const;
        // The best time of each race, in the order of Races() (ms 0: none yet).
        std::vector<RaceRecord> Records() const;
        RaceView CurrentRace() const;
        uint32_t NowMs() const;
        uint32_t RttMs() const;
        std::function<void(uint8_t, const std::string&)> onChat;
    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // Blend two states (position, velocity, steering linearly, rotation by slerp); t in 0..1.
    CarState Lerp(const CarState& a, const CarState& b, float t);
}
