// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See car_select.cpp.
//
// Which cars the cars of a race are: a catalogue of the game's cars (from its own name table) and a way to make the race script create its opponents as given cars.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game
{
    struct CarEntry
    {
        std::string name;     // the game's name for the car (its folder: IS300, BMWM3GTRE46)
        std::string display;  // what to show a player
        uint64_t key = 0;     // the car's 64-bit type key: the game's string key of `name`
    };

    // The cars the game has a template for, in the table's order. Empty until the game has loaded its tables and the first frame after has built it. Any thread.
    std::vector<CarEntry> CarCatalog();

    // The cars the race script's next opponents are created as, in order (0 = leave that one as the game chose). Used by the next race(s) until set again. Any thread.
    void SetOpponentCars(const std::vector<uint64_t>& keys);
    // The car the player's own vehicle is made, from the next race on (0 = the one chosen in the front end). Any thread.
    void SetPlayerCar(uint64_t key);
    // How many opponents the race script has created in this race so far, and how long ago the last one was created (seconds; large if none). Any thread.
    int OpponentsCreated();
    double SecondsSinceLastOpponent();
    // Start counting the opponents of a race from the first again (a race is over). Any thread.
    void ResetOpponentCount();
    // The car type key of the vehicle whose creation record is at `record`, or 0 (the local player's car: owner + 0x74 points at it, the key is at +16).
    uint64_t CarKeyOfRecord(const uint8_t* base, uint32_t record);
    // The game's racer collection key for a car name (the hash of the lower-case folder name): what its opponent-creation native takes to make that car.
    uint32_t RacerKeyOf(const std::string& carName);
    // Whether the cars this thread makes from now on stand for other players (stock parts instead of the game's random ones; NFSMW_MP_STOCK_PARTS=0 turns it off). Returns the old value.
    bool StockPartsFor(bool on);
}
