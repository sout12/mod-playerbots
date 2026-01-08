/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ARENA_OPENER_STRATEGY_H
#define _PLAYERBOT_ARENA_OPENER_STRATEGY_H

#include "Strategy.h"

class ArenaOpenerStrategy : public Strategy
{
public:
    ArenaOpenerStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::vector<NextAction> getDefaultActions() override
    {
        return {
            NextAction("arena opener move", ACTION_EMERGENCY)
        };
    }
    std::string const getName() override { return "arena opener"; }
};

#endif
