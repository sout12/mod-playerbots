/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DKTriggers.h"

#include <string>

#include "GenericTriggers.h"
#include "Playerbots.h"
#include "SharedDefines.h"

bool DKPresenceTrigger::IsActive()
{
    Unit* target = GetTarget();
    return !botAI->HasAura("blood presence", target) && !botAI->HasAura("unholy presence", target) &&
           !botAI->HasAura("frost presence", target);
}

bool PestilenceGlyphTrigger::IsActive()
{
    if (!SpellTrigger::IsActive())
    {
        return false;
    }
    if (!bot->HasAura(63334))
    {
        return false;
    }
    Aura* blood_plague = botAI->GetAura("blood plague", GetTarget(), true, true);
    Aura* frost_fever = botAI->GetAura("frost fever", GetTarget(), true, true);
    if ((blood_plague && blood_plague->GetDuration() <= 3000) || (frost_fever && frost_fever->GetDuration() <= 3000))
    {
        return true;
    }
    return false;
}

// Based on runeSlotTypes
bool HighBloodRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(0) <= 2000 && bot->GetRuneCooldown(1) <= 2000;
}

bool HighFrostRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(4) <= 2000 && bot->GetRuneCooldown(5) <= 2000;
}

bool HighUnholyRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(2) <= 2000 && bot->GetRuneCooldown(3) <= 2000;
}

bool NoRuneTrigger::IsActive()
{
    for (uint32 i = 0; i < MAX_RUNES; ++i)
    {
        if (!bot->GetRuneCooldown(i))
            return false;
    }
    return true;
}

bool DesolationTrigger::IsActive()
{
    return bot->HasAura(66817) && BuffTrigger::IsActive();
}

bool DeathAndDecayCooldownTrigger::IsActive()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    if (!spellId)
        return true;

    return bot->GetSpellCooldownDelay(spellId) >= 2000;
}

// Death Grip PvP - Pull fleeing flag carriers, healers, or any fleeing player target
bool DeathGripPvPTrigger::IsActive()
{
    // Must have Death Grip spell (49576)
    if (!bot->HasSpell(49576) || bot->HasSpellCooldown(49576))
        return false;
    
    // Works in battlegrounds, arenas, AND world PvP
    Unit* target = bot->GetSelectedUnit();
    if (!target || !target->IsPlayer())
        return false;
    
    // Check if in PvP scenario
    if (!bot->InBattleground() && !bot->InArena() && !bot->IsInCombat())
        return false;
    
    Player* enemy = target->ToPlayer();
    float dist = bot->GetDistance(enemy);
    
    // Death Grip range: 8-30 yards (can't use too close or too far)
    if (dist < 8.0f || dist > 30.0f)
        return false;
    
    // Check if target is fleeing (moving away and not in melee range)
    bool isFleeing = enemy->isMoving() && !bot->IsWithinMeleeRange(enemy);
    if (!isFleeing)
        return false;
    
    // Priority 1: Flag carrier trying to escape (highest priority)
    bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
    if (isFlagCarrier)
        return true;
    
    // Priority 2: Healer fleeing
    if (botAI->IsHeal(enemy))
        return true;
    
    // Priority 3: Any fleeing target at distance > 20y
    if (dist > 20.0f)
        return true;
    
    return false;
}

// Chains of Ice PvP - Slow flag carriers and fleeing targets
bool ChainsOfIcePvPTrigger::IsActive()
{
    // Must have Chains of Ice spell (45524)
    if (!bot->HasSpell(45524) || bot->HasSpellCooldown(45524))
        return false;
    
    // Works in battlegrounds, arenas, AND world PvP
    Unit* target = bot->GetSelectedUnit();
    if (!target || !target->IsPlayer())
        return false;
    
    // Check if in PvP scenario
    if (!bot->InBattleground() && !bot->InArena() && !bot->IsInCombat())
        return false;
    
    Player* enemy = target->ToPlayer();
    float dist = bot->GetDistance(enemy);
    
    // Chains of Ice range: ~30 yards
    if (dist > 30.0f)
        return false;
    
    // Already slowed by Chains of Ice
    if (enemy->HasAura(45524))
        return false;
    
    // Priority 1: Flag carrier moving away
    bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
    if (isFlagCarrier && enemy->isMoving())
        return true;
    
    // Priority 2: Target fleeing at distance > 10y
    bool isFleeing = enemy->isMoving() && !bot->IsWithinMeleeRange(enemy);
    if (isFleeing && dist > 10.0f)
        return true;
    
    return false;
}

bool PathOfFrostPvPTrigger::IsActive()
{
    if (!bot->HasSpell(3714))
        return false;

    if (!bot->InBattleground())
        return false;

    if (bot->GetBattlegroundTypeId() != BATTLEGROUND_AB)
        return false;

    if (bot->IsInCombat() || !bot->IsMounted())
        return false;

    if (botAI->HasAura("path of frost", bot))
        return false;

    return bot->IsInWater();
}
