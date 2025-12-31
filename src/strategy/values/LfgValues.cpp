/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgValues.h"

#include "LFGMgr.h"
#include "Playerbots.h"

using namespace lfg;

ObjectGuid DungeonGuideValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return ObjectGuid::Empty;

    // Check if we're in an LFG dungeon
    LfgState groupState = sLFGMgr->GetState(group->GetGUID());
    if (groupState != LFG_STATE_DUNGEON)
        return ObjectGuid::Empty;

    // Priority 1: Current Group Leader
    // The user expects that when they are promoted to leader, the bots follow them.
    // LFG Roles (PLAYER_ROLE_LEADER) might not update dynamically when leadership changes.
    ObjectGuid leaderGuid = group->GetLeaderGUID();
    if (leaderGuid && !leaderGuid.IsEmpty())
    {
        return leaderGuid;
    }

    // Priority 2: Fallback to LFG Role (if for some reason leader isn't set)
    // Find the player with the dungeon guide flag
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member)
            continue;

        // Check if this player is the dungeon guide
        // The guide flag is stored in the LFG roles
        uint8 roles = sLFGMgr->GetRoles(member->GetGUID());
        if (roles & PLAYER_ROLE_LEADER)
        {
            // Log when bot detects a dungeon guide (helps debug guide following)
            static ObjectGuid lastGuide;
            if (lastGuide != member->GetGUID())
            {
                LOG_DEBUG("playerbots", "Bot {} detected dungeon guide: {} (roles: {})", 
                         bot->GetName(), member->GetName(), (uint32)roles);
                lastGuide = member->GetGUID();
            }
            
            return member->GetGUID();
        }
    }

    return ObjectGuid::Empty;
}
