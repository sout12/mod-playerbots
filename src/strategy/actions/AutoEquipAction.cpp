/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AutoEquipAction.h"
#include "Playerbots.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"

bool AutoEquipAction::Execute(Event event)
{
    AutoCreateOutfits();

    if (IsPvPActivity())
    {
        EquipOutfit("pvp");
    }
    else
    {
        EquipOutfit("pve");
    }

    return true;
}

bool AutoEquipAction::IsPvPActivity()
{
    return bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue();
}

void AutoEquipAction::AutoCreateOutfits()
{
    ItemIds pvpItems = FindOutfitItems("pvp");
    ItemIds pveItems = FindOutfitItems("pve");

    // If pvp outfit is missing, we try to create it from resilience items
    if (pvpItems.empty())
    {
        ListItemsVisitor visitor;
        IterateItems(&visitor, ITERATE_ALL_ITEMS);

        for (std::map<uint32, uint32>::iterator i = visitor.items.begin(); i != visitor.items.end(); ++i)
        {
            if (HasResilience(i->first))
                pvpItems.insert(i->first);
        }

        if (!pvpItems.empty())
            Save("pvp", pvpItems);
    }

    // If pve outfit is missing, we create it from current equip (assuming we start in PVE)
    if (pveItems.empty())
    {
        Update("pve");
    }
}

bool AutoEquipAction::HasResilience(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    for (uint32 i = 0; i < 10; ++i)
    {
        if (proto->ItemStat[i].ItemStatType == 35) // ITEM_MOD_RESILIENCE_RATING = 35
            return true;
    }

    return false;
}

void AutoEquipAction::EquipOutfit(std::string const name)
{
    ItemIds outfit = FindOutfitItems(name);
    if (outfit.empty())
        return;

    EquipItems(outfit);
}
