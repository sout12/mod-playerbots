/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PvPGuildNames.h"
#include "Common.h"

std::vector<std::string> const PvPGuildNames::normalGuildNames =
{
    "Bloodthirsty Legion",
    "Honor Bound",
    "Battleborn Warriors",
    "Crimson Blades",
    "Arena Champions",
    "Victory or Death",
    "Relentless Onslaught",
    "Warforge Battalion",
    "Ironclad Soldiers",
    "Tactical Strike Force",
    "Blood and Thunder",
    "Savage Combatants",
    "Battle Hardened",
    "Veterans of War",
    "Steel Resolve",
    "Stormbreakers",
    "Titan's Fury",
    "Phoenix Rising",
    "Shadow Vanguard",
    "Eternal Conflict",
    // Additional names to support 30+ guilds
    "Warbringers",
    "Crimson Vanguard",
    "Dark Crusaders",
    "Hellfire Raiders",
    "Thunder Warriors",
    "Blood Oath",
    "Savage Executioners",
    "Iron Brotherhood",
    "Relentless Assault",
    "Brutal Force",
    "War Machine",
    "Deadly Precision",
    "Combat Elite",
    "Battlefront Heroes",
    "Warpath Legends"
};

std::vector<std::string> const PvPGuildNames::eliteGuildNames =
{
    "Gladiator's Supremacy",
    "Vengeful Elite",
    "Warlord's Chosen",
    "Grand Marshal's Guard",
    "High Warlord's Fury",
    "Duelist's Pride",
    "Rival's Dominion",
    "Challenger's Ascent",
    "Gladiator's Conquest",
    "Vengeful Templars",
    "Ruthless Champions",
    "Wrathful Victors",
    "Gladiator's Sanctuary",
    "Elite Command",
    "Merciless Execution",
    "Gladiator's Nemesis",
    "Brutal Warriors",
    "Furious Gladiators",
    "Relentless Champions",
    "Wrathful Legends",
    // Additional elite names to support 30+ guilds
    "Gladiator's Dominance",
    "Rank One Legends",
    "Elite Predators",
    "Gladiator's Wrath",
    "Unrivaled Champions",
    "Merciless Gladiators",
    "Duelist's Ascension",
    "Gladiator's Legacy",
    "Vengeful Champions",
    "Warlord's Elite",
    "Grand Marshal's Legion",
    "Gladiator's Empire",
    "Elite Vanguard",
    "Ruthless Domination",
    "Gladiator's Reign"
};

std::string PvPGuildNames::GetRandomPvPGuildName(bool isElite)
{
    std::vector<std::string> const& names = isElite ? eliteGuildNames : normalGuildNames;
    
    if (names.empty())
        return "PvP Guild";
    
    size_t index = rand() % names.size();
    return names[index];
}
