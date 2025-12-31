/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DruidTriggers.h"
#include "Player.h"
#include "Playerbots.h"

bool MarkOfTheWildOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && !botAI->HasAura("gift of the wild", GetTarget());
}

bool MarkOfTheWildTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("gift of the wild", GetTarget());
}

bool ThornsOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget());
}

bool EntanglingRootsKiteTrigger::IsActive()
{
    return DebuffTrigger::IsActive() && AI_VALUE(uint8, "attacker count") < 3 && !GetTarget()->GetPower(POWER_MANA);
}

bool ThornsTrigger::IsActive() { return BuffTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget()); }

bool BearFormTrigger::IsActive() { return !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr); }

bool TreeFormTrigger::IsActive() { return !botAI->HasAura(33891, bot); }

bool CatFormTrigger::IsActive() { return !botAI->HasAura("cat form", bot); }

const std::set<uint32> HurricaneChannelCheckTrigger::HURRICANE_SPELL_IDS = {
    16914,  // Hurricane Rank 1
    17401,  // Hurricane Rank 2
    17402,  // Hurricane Rank 3
    27012,  // Hurricane Rank 4
    48467   // Hurricane Rank 5
};

bool HurricaneChannelCheckTrigger::IsActive()
{
    Player* bot = botAI->GetBot();

    // Check if the bot is channeling a spell
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        // Only trigger if the spell being channeled is Hurricane
        if (HURRICANE_SPELL_IDS.count(spell->m_spellInfo->Id))
        {
            uint8 attackerCount = AI_VALUE(uint8, "attacker count");
            return attackerCount < minEnemies;
        }
    }

    // Not channeling Hurricane
    return false;
}

// Travel Form PvP - Shift to Travel Form when flag carrier for speed boost
bool TravelFormPvPTrigger::IsActive()
{
    // Must have Travel Form spell (783)
    if (!bot->HasSpell(783))
        return false;
    
    // Works in battlegrounds, arenas, AND world PvP
    if (!bot->InBattleground() && !bot->InArena())
        return false;
    
    // Already in travel form
    if (botAI->HasAura("travel form", bot))
        return false;
    
    // Shift to Travel Form if we're a flag carrier (WSG: 23333/23335, EotS: 34976)
    bool isFlagCarrier = bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976);
    if (isFlagCarrier)
    {
        // Only shift if HP > 30% (otherwise stay caster to heal)
        if (bot->GetHealthPct() > 30.0f)
            return true;
    }
    
    return false;
}

// Entangling Roots PvP - Root fleeing flag carriers
bool EntanglingRootsPvPTrigger::IsActive()
{
    // Must have Entangling Roots spell
    if (!bot->HasSpell(339))
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
    
    // Range check (~30 yards)
    if (dist > 30.0f)
        return false;
    
    // Already rooted
    if (enemy->HasAuraType(SPELL_AURA_MOD_ROOT))
        return false;
    
    // Priority 1: Flag carrier fleeing
    bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
    if (isFlagCarrier && enemy->isMoving())
        return true;
    
    // Priority 2: Any target fleeing at distance > 15y
    bool isFleeing = enemy->isMoving() && dist > 15.0f;
    if (isFleeing)
        return true;
    
    return false;
}

// Dash PvP - Use Dash in cat form when flag carrier or chasing
bool DashPvPTrigger::IsActive()
{
    // Must have Dash spell (1850) and be in cat form
    if (!bot->HasSpell(1850) || bot->HasSpellCooldown(1850))
        return false;
    
    if (!botAI->HasAura("cat form", bot))
        return false;
    
    // Works in battlegrounds, arenas, AND world PvP
    if (!bot->InBattleground() && !bot->InArena() && !bot->IsInCombat())
        return false;
    
    // Already has Dash buff
    if (botAI->HasAura("dash", bot))
        return false;
    
    // Use Dash if we're a flag carrier
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return true;
    
    // Use Dash if chasing a flag carrier
    Unit* target = bot->GetSelectedUnit();
    if (target && target->IsPlayer())
    {
        Player* enemy = target->ToPlayer();
        bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
        float dist = bot->GetDistance(enemy);
        
        if (isFlagCarrier && enemy->isMoving() && dist > 15.0f)
            return true;
        
        // Chase any fleeing enemy in combat
        if (bot->IsInCombat() && enemy->isMoving() && dist > 20.0f && dist < 40.0f)
            return true;
    }
    
    return false;
}
