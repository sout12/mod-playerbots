/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ArenaOpenerValue.h"
#include "Playerbots.h"
#include "Group.h"
#include "GroupReference.h"
#include <unordered_map>
#include <set>
#include <ctime>
#include <vector>
#include <algorithm>

namespace
{
    constexpr time_t ARENA_OPENER_WINDOW = 12;

    struct ArenaOpenerState
    {
        time_t startTime = 0;
        bool logged = false;
    };

    std::unordered_map<uint32, ArenaOpenerState> openerStates;

    uint32 DetermineArenaMapFeatures(BattlegroundTypeId type, bool shadowSightActive)
    {
        uint32 flags = AMF_NONE;
        switch (type)
        {
            case BATTLEGROUND_DS:
                if (shadowSightActive)
                    flags |= AMF_SHADOW_SIGHT;
                flags |= AMF_WATER_FLUSH | AMF_WATERFALL;
                break;
            case BATTLEGROUND_RL:
                if (shadowSightActive)
                    flags |= AMF_SHADOW_SIGHT;
                flags |= AMF_PILLAR_ROTATION;
                break;
            case BATTLEGROUND_BE:
                flags |= AMF_PILLAR_ROTATION;
                break;
            case BATTLEGROUND_NA:
                if (shadowSightActive)
                    flags |= AMF_SHADOW_SIGHT;
                break;
            case BATTLEGROUND_RV:
                flags |= AMF_PILLAR_ROTATION | AMF_FLAME_WALL;
                break;
            default:
                break;
        }

        return flags;
    }

    bool HasSpell(Player* player, std::vector<uint32> const& spells)
    {
        if (!player)
            return false;

        for (uint32 spellId : spells)
        {
            if (player->HasSpell(spellId))
                return true;
        }

        return false;
    }

    bool HasclassSpell(std::vector<Player*> const& players, std::vector<uint32> const& spells)
    {
        for (Player* player : players)
            if (HasSpell(player, spells))
                return true;

        return false;
    }

    ArenaOpenerCombo DetectCombo(std::unordered_map<uint8, std::vector<Player*>> const& nodePlayers,
                                 std::set<uint8> const& classes)
    {
        auto has = [&](uint8 cls) { return nodePlayers.count(cls) && !nodePlayers.at(cls).empty(); };

        bool hasRogue = has(CLASS_ROGUE);
        bool hasMage = has(CLASS_MAGE);
        bool hasPriest = has(CLASS_PRIEST);
        bool hasWarlock = has(CLASS_WARLOCK);
        bool hasShaman = has(CLASS_SHAMAN);
        bool hasWarrior = has(CLASS_WARRIOR);
        bool hasPaladin = has(CLASS_PALADIN);
        bool hasDK = has(CLASS_DEATH_KNIGHT);
        bool hasHunter = has(CLASS_HUNTER);
        bool hasDruid = has(CLASS_DRUID);

        if (hasRogue && hasMage && hasPriest)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_ROGUE), {408}) && HasclassSpell(nodePlayers.at(CLASS_MAGE), {45438}) &&
                HasclassSpell(nodePlayers.at(CLASS_PRIEST), {47540}))
                return AO_RMP;
        }

        if (hasRogue && hasWarlock && hasShaman)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_ROGUE), {408}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARLOCK), {5782, 6358}) &&
                HasclassSpell(nodePlayers.at(CLASS_SHAMAN), {51514}))
                return AO_RLS;
        }

        if (hasRogue && hasMage)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_ROGUE), {408}) && HasclassSpell(nodePlayers.at(CLASS_MAGE), {45438}))
                return AO_RM;
        }

        if (hasMage && hasWarlock)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_MAGE), {45438}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARLOCK), {5782, 6358}))
                return AO_ML;
        }

        if (hasWarrior && hasPaladin)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_WARRIOR), {23922}) &&
                HasclassSpell(nodePlayers.at(CLASS_PALADIN), {853}))
                return AO_WP;
        }

        if (hasRogue && hasDK)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_ROGUE), {408}) &&
                HasclassSpell(nodePlayers.at(CLASS_DEATH_KNIGHT), {49576}))
                return AO_RD;
        }

        if (hasHunter && hasShaman)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_HUNTER), {1543}) &&
                HasclassSpell(nodePlayers.at(CLASS_SHAMAN), {51514}))
                return AO_HS;
        }

        // MLS (Mage / Lock / Shaman)
        if (hasMage && hasWarlock && hasShaman)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_MAGE), {45438}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARLOCK), {30108, 47843, 59172}) &&  // dots or seduce support
                HasclassSpell(nodePlayers.at(CLASS_SHAMAN), {974, 16190, 51514}))
                return AO_MLS;
        }

        // WLD (Warlock / Warrior / Druid)
        if (hasWarlock && hasWarrior && hasDruid)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_WARLOCK), {47843}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARRIOR), {12294, 7384}) &&
                HasclassSpell(nodePlayers.at(CLASS_DRUID), {33786, 33878, 48451}))
                return AO_WLD;
        }

        // TSG (Warrior / Death Knight / Paladin)
        if (hasWarrior && hasDK && hasPaladin)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_DEATH_KNIGHT), {49576}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARRIOR), {1715, 46924}) &&
                HasclassSpell(nodePlayers.at(CLASS_PALADIN), {10278, 1044, 1022}))
                return AO_TSG;
        }

        // Beastcleave (Hunter / Shaman / healer optional)
        if (hasHunter && hasShaman && (hasPaladin || hasPriest || hasDruid))
        {
            if (HasclassSpell(nodePlayers.at(CLASS_HUNTER), {34490, 53301, 53209}) &&
                HasclassSpell(nodePlayers.at(CLASS_SHAMAN), {51514, 57994}))
                return AO_BEAST;
        }

        // Shadowcleave (Death Knight / Warlock / healer)
        if (hasDK && hasWarlock && (hasPaladin || hasPriest || hasShaman || hasDruid))
        {
            if (HasclassSpell(nodePlayers.at(CLASS_DEATH_KNIGHT), {49576}) &&
                HasclassSpell(nodePlayers.at(CLASS_WARLOCK), {59172, 47843}))
                return AO_SHADOWCLEAVE;
        }

        // KFC (Warrior / Hunter / healer)
        if (hasWarrior && hasHunter && (hasPaladin || hasPriest || hasShaman || hasDruid))
        {
            if (HasclassSpell(nodePlayers.at(CLASS_WARRIOR), {12294, 23881}) &&
                HasclassSpell(nodePlayers.at(CLASS_HUNTER), {49050, 49045}))
                return AO_KFC;
        }

        // PHP (Ret / Hunter / Priest)
        if (hasHunter && hasPaladin && hasPriest)
        {
            if (HasclassSpell(nodePlayers.at(CLASS_HUNTER), {49050, 34490}) &&
                HasclassSpell(nodePlayers.at(CLASS_PALADIN), {35395, 53385}) &&
                HasclassSpell(nodePlayers.at(CLASS_PRIEST), {48158, 53007}))
                return AO_PHP;
        }

        return AO_NONE;
    }
}

ArenaOpenerInfo ArenaOpenerValue::Calculate()
{
    ArenaOpenerInfo info;
    if (!bot->InArena())
    {
        openerStates.clear();
        return info;
    }

    Battleground* bg = bot->GetBattleground();
    if (!bg)
    {
        openerStates.clear();
        return info;
    }

    uint32 instanceId = bg->GetInstanceID();
    if (bg->GetStatus() != STATUS_IN_PROGRESS)
    {
        openerStates.erase(instanceId);
        return info;
    }

    ArenaOpenerState& state = openerStates[instanceId];
    time_t now = time(nullptr);
    if (!state.startTime && bg->GetStartDelayTime() <= 0)
        state.startTime = now;

    if (!state.startTime)
        return info;

    float elapsed = static_cast<float>(now - state.startTime);
    info.active = elapsed <= ARENA_OPENER_WINDOW;
    info.shadowSightActive = elapsed >= 90.0f;

    info.mapType = bg->GetBgTypeID();
    info.mapId = bg->GetMapId();
    info.featureFlags = DetermineArenaMapFeatures(info.mapType, info.shadowSightActive);

    if (!info.active)
    {
         // LOG_DEBUG("playerbots", "ArenaOpenerValue: Not active (elapsed: {})", elapsed);
        state.logged = false;
        return info;
    }

    std::set<uint8> classes;
    std::unordered_map<uint8, std::vector<Player*>> classPlayers;
    Group* group = bot->GetGroup();
    uint8 playerCount = 0;

    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsAlive())
            {
                classes.insert(member->getClass());
                classPlayers[member->getClass()].push_back(member);
                ++playerCount;
            }
        }
    }

    classes.insert(bot->getClass());
    classPlayers[bot->getClass()].push_back(bot);
    if (playerCount == 0)
        playerCount = 1;

    info.teamSize = std::min<uint8>(playerCount, 5);

    info.combo = DetectCombo(classPlayers, classes);
    // Even if no named combo, keep opener active so bots move/coordinate
    if (info.combo == AO_NONE)
    {
        if (!state.logged)
        {
            LOG_DEBUG("playerbots", "Arena opener: no combo detected on map {} (teamSize={} shadowSight={})",
                      info.mapId, info.teamSize, info.shadowSightActive);
            state.logged = true;
        }
    }

    if (!state.logged)
    {
        LOG_DEBUG("playerbots", "Arena opener: {} detected combo {} on map {} (teamSize={} shadowSight={})",
                  bot->GetName(), info.combo, info.mapId, info.teamSize, info.shadowSightActive);
        state.logged = true;
    }

    Unit* target = AI_VALUE(Unit*, "enemy healer target");
    if (!target)
        target = AI_VALUE(Unit*, "enemy player target");
    if (!target)
        target = AI_VALUE(Unit*, "dps target");
    // Fallback: if still no target (e.g., no healer detected), grab any enemy player to avoid opener stall
    if (!target)
        target = AI_VALUE(Unit*, "enemy player target");

    if (target)
        info.focusTarget = target->GetGUID();

    return info;
}
