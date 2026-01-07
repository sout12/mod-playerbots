/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RogueTriggers.h"

#include "GenericTriggers.h"
#include "Playerbots.h"
#include "ServerFacade.h"

// bool AdrenalineRushTrigger::isPossible()
// {
//     return !botAI->HasAura("stealth", bot);
// }

bool UnstealthTrigger::IsActive()
{
    if (!botAI->HasAura("stealth", bot))
        return false;

    if (bot->InArena())
    {
        Unit* enemy = AI_VALUE(Unit*, "enemy player target");
        if (enemy && enemy->IsAlive() && sServerFacade->GetDistance2d(bot, enemy) < 40.0f)
            return false;
    }

    return botAI->HasAura("stealth", bot) && !AI_VALUE(uint8, "attacker count") &&
           (AI_VALUE2(bool, "moving", "self target") &&
            ((botAI->GetMaster() &&
              sServerFacade->IsDistanceGreaterThan(AI_VALUE2(float, "distance", "group leader"), 10.0f) &&
              AI_VALUE2(bool, "moving", "group leader")) ||
             !AI_VALUE(uint8, "attacker count")));
}

bool StealthTrigger::IsActive()
{
    if (botAI->HasAura("stealth", bot) || bot->IsInCombat() || bot->HasSpellCooldown(1784))
        return false;

    float distance = 30.f;

    Unit* target = AI_VALUE(Unit*, "enemy player target");
    if (target && !target->IsInWorld())
    {
        return false;
    }
    if (!target)
        target = AI_VALUE(Unit*, "grind target");

    if (!target)
        target = AI_VALUE(Unit*, "dps target");

    if (!target)
        return false;

    if (target && target->GetVictim())
        distance -= 10;

    if (target->isMoving() && target->GetVictim())
        distance -= 10;

    if (bot->InBattleground())
        distance += 15;

    if (bot->InArena())
        distance += 15;

    return target && sServerFacade->GetDistance2d(bot, target) < distance;
}

bool ShadowDanceTrigger::IsActive()
{
    if (!BoostTrigger::IsActive())
        return false;

    if (!bot->IsInCombat())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsPlayer())
        return false;

    // Only burst when target is vulnerable or already pressured
    if (target->HasUnitState(UNIT_STATE_STUNNED) || target->HasUnitState(UNIT_STATE_CONTROLLED))
        return true;

    return target->GetHealthPct() < 70.0f;
}

bool SapTrigger::IsPossible() { return bot->GetLevel() > 10 && bot->HasSpell(6770) && !bot->IsInCombat(); }

bool SprintTrigger::IsPossible() { return bot->HasSpell(2983); }

bool SprintTrigger::IsActive()
{
    if (bot->HasSpellCooldown(2983))
        return false;

    float distance = botAI->GetMaster() ? 45.0f : 35.0f;
    if (botAI->HasAura("stealth", bot))
        distance -= 10;

    bool targeted = false;

    Unit* dps = AI_VALUE(Unit*, "dps target");
    Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");

    if (enemyPlayer && !enemyPlayer->IsInWorld())
    {
        return false;
    }
    if (dps)
        targeted = (dps == AI_VALUE(Unit*, "current target"));

    if (enemyPlayer && !targeted)
        targeted = (enemyPlayer == AI_VALUE(Unit*, "current target"));

    if (!targeted)
        return false;

    if ((dps && dps->IsInCombat()) || enemyPlayer)
        distance -= 10;

    return AI_VALUE2(bool, "moving", "self target") &&
           (AI_VALUE2(bool, "moving", "dps target") || AI_VALUE2(bool, "moving", "enemy player target")) && targeted &&
           (sServerFacade->IsDistanceGreaterThan(AI_VALUE2(float, "distance", "dps target"), distance) ||
            sServerFacade->IsDistanceGreaterThan(AI_VALUE2(float, "distance", "enemy player target"), distance));
}

bool ExposeArmorTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target"); // Get the bot's current target
    return DebuffTrigger::IsActive() && !botAI->HasAura("sunder armor", target, false, false, -1, true) &&
           AI_VALUE2(uint8, "combo", "current target") <= 3;
}

bool MainHandWeaponNoEnchantTrigger::IsActive()
{
    Item* const itemForSpell = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!itemForSpell || itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return false;
    return true;
}

bool OffHandWeaponNoEnchantTrigger::IsActive()
{
    Item* const itemForSpell = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!itemForSpell || itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return false;
    return true;
}

bool SprintPvPTrigger::IsPossible()
{
    // Works in battlegrounds, arenas, AND world PvP combat with players
    return bot->HasSpell(2983) && (bot->InBattleground() || bot->InArena() || 
                                     (bot->IsInCombat() && bot->GetSelectedUnit() && bot->GetSelectedUnit()->IsPlayer()));
}

bool SprintPvPTrigger::IsActive()
{
    // Already has Sprint buff or on cooldown
    if (botAI->HasAura("sprint", bot) || bot->HasSpellCooldown(2983))
        return false;
    
    // PRIORITY 1: Use Sprint if we're a flag carrier (WSG: 23333/23335, EotS: 34976)
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return true;
    
    // PRIORITY 2: Use Sprint if chasing enemies (especially in combat when can't mount)
    Unit* target = bot->GetSelectedUnit();
    if (target && target->IsPlayer())
    {
        Player* enemy = target->ToPlayer();
        float dist = bot->GetDistance(enemy);
        bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
        bool isFleeing = enemy->isMoving() && !bot->IsWithinMeleeRange(enemy);
        
        // Chase flag carriers aggressively (top priority)
        if (isFlagCarrier && isFleeing && dist > 15.0f)
            return true;
        
        // Chase any fleeing enemy in combat when distance is increasing
        if (bot->IsInCombat() && isFleeing && dist > 20.0f && dist < 40.0f)
            return true;
        
        // Chase high-priority targets (healers) even outside combat
        if (isFleeing && dist > 15.0f && dist < 35.0f)
        {
            // Check if target is a healer
            if (botAI->IsHeal(enemy))
                return true;
        }
    }
    
    return false;
}

bool KidneyShotPvPTrigger::IsActive()
{
    if (!bot->HasSpell(408) || bot->HasSpellCooldown(408))
        return false;

    Unit* target = bot->GetSelectedUnit();
    if (!target || !target->IsPlayer())
        return false;

    if (!bot->InBattleground() && !bot->InArena() && !bot->IsInCombat())
        return false;

    if (!bot->IsWithinMeleeRange(target))
        return false;

    if (AI_VALUE2(uint8, "combo", "current target") < 5)
        return false;

    if (target->HasAuraType(SPELL_AURA_MOD_STUN) || botAI->HasAura("cheap shot", target) ||
        botAI->HasAura("kidney shot", target))
        return false;

    return true;
}

bool CheapShotPvPTrigger::IsActive()
{
    if (!bot->HasSpell(1833) || bot->HasSpellCooldown(1833))
        return false;

    if (!botAI->HasAura("stealth", bot))
        return false;

    Unit* target = bot->GetSelectedUnit();
    if (!target || !target->IsPlayer())
        return false;

    if (!bot->InBattleground() && !bot->InArena() && !bot->IsInCombat())
        return false;

    if (!bot->IsWithinMeleeRange(target))
        return false;

    if (target->HasAuraType(SPELL_AURA_MOD_STUN) || botAI->HasAura("cheap shot", target) ||
        botAI->HasAura("kidney shot", target))
        return false;

    return true;
}
