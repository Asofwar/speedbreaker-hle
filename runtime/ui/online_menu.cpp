// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The Online page of the settings menu, under its options: AndhikaFW's
// multiplayer races and open world (net/mp_*, game/mp_adapter.cpp), a
// dedicated server relaying each player's car into the game's own races.
#include <stdafx.h>
#include "ui.h"

#include <game/car_select.h>
#include <game/mp_adapter.h>
#include <net/multiplayer_config.h>
#include <user/settings.h>

#include <imgui.h>

namespace ui
{
    namespace
    {
        // Multiplayer: the new layer (net/mp_*, game/mp_adapter.cpp): a dedicated server relays each player's car; the races are the game's own Quick Race / Custom Race.
        void DrawMultiplayerSection()
        {
            static mpcfg::Config cfg;
            static char server[96], name[24];
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                cfg = mpcfg::Load();
                snprintf(server, sizeof server, "%s", cfg.server.c_str());
                snprintf(name, sizeof name, "%s", cfg.name.c_str());
            }
            bool changed = false;
            ImGui::Spacing();
            ImGui::TextUnformatted("Multiplayer races");
            ImGui::TextWrapped("Race other players in the game's own races: start the same race (same track and direction, with at least one opponent) on every machine; "
                "each one waits at the start for the others. Needs a multiplayer server (mp_server) that every player can reach.");
            ImGui::Spacing();
            changed |= ImGui::Checkbox("Join multiplayer races", &cfg.enabled);
            ImGui::TextUnformatted("Server (address:port)");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##mpserver", server, sizeof server);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextUnformatted("Your name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##mpname", name, sizeof name);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextUnformatted("Your car (the other players' games make an opponent of it)");
            const std::vector<game::CarEntry> cars = game::CarCatalog();
            uint64_t carKey = 0;
            if (cars.empty())
                ImGui::TextDisabled("(the car list appears once the game has loaded: wait a moment)");
            else
            {
                std::string shown = "Choose a car";
                for (const auto& c : cars)
                    if (c.name == cfg.car)
                    {
                        shown = c.display;
                        carKey = c.key;
                    }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##mpcar", shown.c_str()))
                {
                    for (const auto& c : cars)
                        if (ImGui::Selectable(c.display.c_str(), c.name == cfg.car))
                        {
                            cfg.car = c.name;
                            carKey = c.key;
                            changed = true;
                            game::MpSetLobby(carKey, game::MpLobbyReady());
                        }
                    ImGui::EndCombo();
                }
            }
            bool ready = game::MpLobbyReady();
            if (ImGui::Checkbox("Ready for the next race", &ready))
                game::MpSetLobby(carKey, ready && carKey != 0);
            ImGui::TextWrapped("Tick Ready when you are about to start the race, with the car above chosen in the game's car selection too. Everyone who is ready is in the next race, "
                "and each game makes its opponents as their cars. (Not ready: the number below decides how many players the start waits for.)");
            ImGui::TextUnformatted("Players to wait for at the start when not ready in the lobby (1 = don't wait)");
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::SliderInt("##mpplayers", &cfg.players, 1, 8);
            changed |= ImGui::Checkbox("Open world mode (applies the next time the game starts)", &cfg.world);
            ImGui::TextWrapped("The game starts in the open world with the car chosen above, with no menus; the other players are cars in it, and the server's races start from markers "
                "you stop in. Players on the server when your world loads are drawn; join before the others start their game.");
            if (changed)
            {
                cfg.server = server;
                cfg.name = name;
                mpcfg::Save(cfg);
            }
            ImGui::Spacing();
            const std::string status = game::MpStatus();
            ImGui::TextWrapped("%s", status.c_str());
            const std::vector<game::MpPlayer> players = game::MpPlayers();
            if (!players.empty())
            {
                ImGui::Spacing();
                ImGui::TextUnformatted("Players");
                for (const auto& p : players)
                    ImGui::BulletText("%s%s: %s%s", p.name.c_str(), p.self ? " (you)" : "", p.inRace ? "ready / in a race" : "in the menus", p.car.empty() ? "" : (", " + p.car).c_str());
            }
            if (!players.empty())
            {
                // Chat: agree the race, the track and the direction here (the lobby doesn't set them). The lines show over the game for 15 s too.
                ImGui::Spacing();
                ImGui::TextUnformatted("Chat");
                const std::vector<std::string> log = game::MpChatLog();
                for (size_t i = log.size() > 8 ? log.size() - 8 : 0; i < log.size(); ++i)
                    ImGui::TextWrapped("%s", log[i].c_str());
                static char say[161];
                ImGui::SetNextItemWidth(-90.f * Scale());
                const bool enter = ImGui::InputText("##mpsay", say, sizeof say, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Send") || enter) && say[0])
                {
                    game::MpSendChat(say);
                    say[0] = 0;
                }
            }
            if (std::getenv("NFSMW_MP_SERVER") || std::getenv("NFSMW_MP_NAME") || std::getenv("NFSMW_MP_PLAYERS"))
                ImGui::TextWrapped("(NFSMW_MP_* environment variables are set and override these values.)");
        }
    }

    void DrawOnlinePage()
    {
        ImGui::Spacing();
        ImGui::Separator();
        DrawMultiplayerSection();
    }
}
