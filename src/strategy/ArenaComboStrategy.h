/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ARENA_COMBO_STRATEGY_H
#define _PLAYERBOT_ARENA_COMBO_STRATEGY_H

#include "Playerbots.h"
#include "Strategy.h"

class ArenaComboStrategy : public Strategy
{
public:
    ArenaComboStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    NextAction** getDefaultActions() override
    {
        return NextAction::array(0, new NextAction("arena opener move", ACTION_EMERGENCY), nullptr);
    }
    std::string const getName() override { return "arena combo"; }
};

#endif
