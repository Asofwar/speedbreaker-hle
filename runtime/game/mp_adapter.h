// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See mp_adapter.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game
{
    // Once a game frame, from GameFrameTime (game/frame_time.cpp): the multiplayer layer's settings, status and start barrier.
    void MpFramePump();
    // The race clock (seconds) the world should be at now so that the games in a multiplayer race stay together, or < 0 if there is none.
    double MpWantedClock();
    // The settings page: the car this player chose in the lobby (its type key, from game::CarCatalog) and whether it is ready for the next race. Any thread.
    void MpSetLobby(uint64_t carKey, bool ready);
    bool MpLobbyReady();
    // From game/car_select.cpp, on the guest thread: the player's vehicle was just created; the race script is about to create its opponents.
    void MpPlayerCreated(uint8_t* base);
    void MpBeforeOpponents(uint8_t* base);
    // One line for the settings page: off / waiting for a race / connecting / holding the start / connected with n other players.
    std::string MpStatus();

    // The players on the server, for the settings page: this player first.
    struct MpPlayer
    {
        std::string name;
        std::string car;      // the car it races in (its display name), empty when it is not in a race
        bool inRace = false;
        bool self = false;
    };
    std::vector<MpPlayer> MpPlayers();

    // Open-world mode (game/world_mode.cpp): the other players' cars are made in the game's open world. True when the mode is on (NFSMW_MP_WORLD=1).
    bool MpWorldMode();
    // The next remote player that needs a vehicle in the world: its id and the car's folder name (lower case; a default when it chose none). False if there is none now.
    bool MpWorldNextSpawn(uint8_t& id, std::string& car);
    // That player's vehicle was made: its owner object, which the multiplayer layer then drives (the same body hook as a race's stand-ins).
    void MpWorldSpawned(uint8_t id, uint32_t owner);
    // The owners of the game's vehicles (the vehicle table), the player first.
    std::vector<uint32_t> MpWorldVehicles(uint8_t* base);
    // The cars (lower-case folder names) of the players on the server now, one per player: the pool is made with these first, so that each gets its own model.
    std::vector<std::string> MpWorldWantedCars();
    // A pool vehicle that nobody drives: its body is held far from the world (and let go again when a player takes it, MpWorldSpawned).
    void MpWorldPark(uint32_t vehicle);
    // The physics body of one of those vehicles (0 if it has none): position at +0x10, velocity at +0x20 (floats).
    uint32_t MpWorldPhysBody(uint8_t* base, uint32_t vehicle);
}
