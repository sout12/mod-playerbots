/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ARENA_OPENER_ACTION_H
#define _PLAYERBOT_ARENA_OPENER_ACTION_H

#include "../values/ArenaOpenerValue.h"
#include "MovementActions.h"

class ArenaOpenerAction : public MovementAction
{
public:
    ArenaOpenerAction(PlayerbotAI* botAI) : MovementAction(botAI, "arena opener move") {}

    bool Execute(Event event) override;

private:
    Player* FindLeader(ArenaOpenerCombo combo) const;
    Position GetMapSafePosition(ArenaOpenerInfo const& info) const;
    bool CanExecuteOpener(Battleground* bg) const;
};

#endif
