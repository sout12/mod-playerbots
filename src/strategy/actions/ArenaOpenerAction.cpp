/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ArenaOpenerAction.h"
#include "ArenaOpenerValue.h"
#include "Battleground.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Group.h"
#include "GroupReference.h"
#include <vector>

bool ArenaOpenerAction::Execute(Event event)
{
    // Hard safety: avoid spinning if opener conditions are not met
    Battleground* bg = bot->GetBattleground();
    if (!CanExecuteOpener(bg))
        return false;

    if (!bot->InArena())
    {
        // LOG_DEBUG("playerbots", "ArenaOpenerAction: Bot {} not in arena", bot->GetName());
        return false;
    }

    if (bot->IsInCombat())
    {
         // LOG_DEBUG("playerbots", "ArenaOpenerAction: Bot {} in combat", bot->GetName());
        return false;
    }

    ArenaOpenerInfo info = AI_VALUE(ArenaOpenerInfo, "arena opener info");
    if (!info.active)
    {
         // LOG_DEBUG("playerbots", "ArenaOpenerAction: Bot {} opener not active", bot->GetName());
        return false;
    }

    Unit* target = botAI->GetUnit(info.focusTarget);
    if (!target || !target->IsAlive())
        return false;

    Player* leader = FindLeader(info.combo);
    Position safe = GetMapSafePosition(info);
    if ((!leader || !leader->IsInWorld()) && (safe.GetPositionX() || safe.GetPositionY() || safe.GetPositionZ()) &&
        bot->GetDistance(safe) > 7.0f)
    {
        return MoveNear(bg->GetMapId(), safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ(), 6.0f,
                        MovementPriority::MOVEMENT_NORMAL);
    }

    if (leader && leader->IsInWorld() && bot->GetDistance(leader) > 7.0f)
        return MoveNear(leader, 6.0f, MovementPriority::MOVEMENT_NORMAL);

    if (!bot->IsWithinMeleeRange(target))
        return MoveNear(target, 5.0f, MovementPriority::MOVEMENT_NORMAL);

    return true;
}

bool ArenaOpenerAction::CanExecuteOpener(Battleground* bg) const
{
    if (!bg)
        return false;

    // Opener only during start window
    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    // If gates are still closed (start delay), skip
    if (bg->GetStartDelayTime() > 0)
        return false;

    // Avoid executing if already in combat
    if (bot->IsInCombat())
        return false;

    return true;
}

Player* ArenaOpenerAction::FindLeader(ArenaOpenerCombo combo) const
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::vector<uint8> priorities;
    switch (combo)
    {
        case AO_RMP:
            priorities = {CLASS_MAGE, CLASS_ROGUE, CLASS_PRIEST};
            break;
        case AO_RLS:
            priorities = {CLASS_WARLOCK, CLASS_ROGUE, CLASS_SHAMAN};
            break;
        case AO_RM:
            priorities = {CLASS_MAGE, CLASS_ROGUE};
            break;
        case AO_ML:
            priorities = {CLASS_MAGE, CLASS_WARLOCK};
            break;
        case AO_MLS:
            priorities = {CLASS_MAGE, CLASS_WARLOCK, CLASS_SHAMAN};
            break;
        case AO_WLD:
            priorities = {CLASS_WARLOCK, CLASS_WARRIOR, CLASS_DRUID};
            break;
        case AO_TSG:
            priorities = {CLASS_DEATH_KNIGHT, CLASS_WARRIOR, CLASS_PALADIN};
            break;
        case AO_BEAST:
            priorities = {CLASS_HUNTER, CLASS_SHAMAN};
            break;
        case AO_SHADOWCLEAVE:
            priorities = {CLASS_DEATH_KNIGHT, CLASS_WARLOCK};
            break;
        case AO_KFC:
            priorities = {CLASS_WARRIOR, CLASS_HUNTER};
            break;
        case AO_PHP:
            priorities = {CLASS_HUNTER, CLASS_PALADIN, CLASS_PRIEST};
            break;
        default:
            break;
    }

    if (!priorities.empty())
    {
        for (uint8 cls : priorities)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsAlive())
                    continue;

                if (member->getClass() == cls)
                    return member;
            }
        }
    }

    if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
        return leader;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsAlive())
            return member;
    }

    return nullptr;
}

Position ArenaOpenerAction::GetMapSafePosition(ArenaOpenerInfo const& info) const
{
    switch (info.mapType)
    {
        case BATTLEGROUND_DS:
            if (info.featureFlags & AMF_WATER_FLUSH)
                return Position(6240.0f, 268.0f, 1.5f);
            break;
        case BATTLEGROUND_RL:
            return Position(1265.0f, 1663.0f, 34.0f);
        case BATTLEGROUND_BE:
            return Position(6236.0f, 260.0f, 1.5f);
        case BATTLEGROUND_NA:
            return Position(4056.0f, 2919.0f, 13.5f);
        case BATTLEGROUND_RV:
            if (info.featureFlags & AMF_FLAME_WALL)
                return Position(1266.0f, 1663.0f, 34.0f);
            break;
        default:
            break;
    }

    return Position();
}
