/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AUTOEQUIPACTION_H
#define _PLAYERBOT_AUTOEQUIPACTION_H

#include "OutfitAction.h"

class AutoEquipAction : public OutfitAction
{
public:
    AutoEquipAction(PlayerbotAI* botAI) : OutfitAction(botAI) {}

    bool Execute(Event event) override;

private:
    bool IsPvPActivity();
    void AutoCreateOutfits();
    bool HasResilience(uint32 itemId);
    void EquipOutfit(std::string const name);
};

#endif
