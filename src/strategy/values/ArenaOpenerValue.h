/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ARENAOPENER_VALUE_H
#define _PLAYERBOT_ARENAOPENER_VALUE_H

#include "Value.h"
#include "Playerbots.h"
#include "ObjectGuid.h"
#include "Battleground.h"
#include "SharedDefines.h"

enum ArenaOpenerCombo
{
    AO_NONE,
    AO_RMP,
    AO_RLS,
    AO_RM,
    AO_ML,
    AO_WP,
    AO_RD,
    AO_HS,
    AO_WLD,
    AO_TSG,
    AO_BEAST,
    AO_SHADOWCLEAVE,
    AO_KFC,
    AO_MLS,
    AO_PHP
};

struct ArenaOpenerInfo
{
    ArenaOpenerCombo combo = AO_NONE;
    ObjectGuid focusTarget = ObjectGuid::Empty;
    bool active = false;
    BattlegroundTypeId mapType = BATTLEGROUND_TYPE_NONE;
    uint32 mapId = 0;
    uint32 featureFlags = 0;
    uint8 teamSize = 0;
    bool shadowSightActive = false;
};

enum ArenaMapFeatures : uint32
{
    AMF_NONE = 0,
    AMF_SHADOW_SIGHT = 1 << 0,
    AMF_WATER_FLUSH = 1 << 1,
    AMF_PILLAR_ROTATION = 1 << 2,
    AMF_FLAME_WALL = 1 << 3,
    AMF_WATERFALL = 1 << 4
};
class ArenaOpenerValue : public CalculatedValue<ArenaOpenerInfo>
{
public:
    ArenaOpenerValue(PlayerbotAI* botAI) : CalculatedValue<ArenaOpenerInfo>(botAI, "arena opener info", 1 * 1000) {}

    ArenaOpenerInfo Calculate() override;
};

#endif
