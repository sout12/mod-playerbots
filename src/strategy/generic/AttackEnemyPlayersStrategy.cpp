/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AttackEnemyPlayersStrategy.h"

#include "Playerbots.h"

void AttackEnemyPlayersStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("pvp cc break", NextAction::array(0, new NextAction("use trinket", ACTION_EMERGENCY + 5), nullptr)));
    triggers.push_back(new TriggerNode("enemy player near",
                                       NextAction::array(0, new NextAction("attack enemy player", 55.0f), nullptr)));
}
