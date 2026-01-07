/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "HunterTriggers.h"
#include "GenericSpellActions.h"
#include "GenericTriggers.h"
#include "HunterActions.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Player.h"

bool KillCommandTrigger::IsActive()
{
    Unit* target = GetTarget();
    return !botAI->HasAura("kill command", target);
}

bool BlackArrowTrigger::IsActive()
{
    if (botAI->HasStrategy("trap weave", BOT_STATE_COMBAT))
        return false;

    return DebuffTrigger::IsActive();
    return BuffTrigger::IsActive();
}

bool HunterAspectOfTheHawkTrigger::IsActive()
{
    Unit* target = GetTarget();
    return SpellTrigger::IsActive() && !botAI->HasAura("aspect of the hawk", target) &&
           !botAI->HasAura("aspect of the dragonhawk", target) &&
           (!AI_VALUE2(bool, "has mana", "self target") || AI_VALUE2(uint8, "mana", "self target") > 70);
}

bool HunterNoStingsActiveTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return DebuffTrigger::IsActive() && target && !botAI->HasAura("serpent sting", target, false, true) &&
           !botAI->HasAura("scorpid sting", target, false, true) && !botAI->HasAura("viper sting", target, false, true);
    return BuffTrigger::IsActive();
}

bool FreezingTrapEnemyHealerTrigger::IsActive()
{
    Unit* healer = AI_VALUE(Unit*, "enemy healer target");
    if (!healer || !healer->IsAlive())
        return false;
    if (!bot->InArena() && !bot->InBattleground())
        return false;
    if (!bot->IsWithinLOSInMap(healer))
        return false;
    if (healer->HasAuraType(SPELL_AURA_MOD_CONFUSE))
        return false;
    return true;
}

bool HuntersPetDeadTrigger::IsActive()
{
    // Unit* pet = AI_VALUE(Unit*, "pet target");
    // return pet && AI_VALUE2(bool, "dead", "pet target") && !AI_VALUE2(bool, "mounted", "self target");
    return AI_VALUE(bool, "pet dead") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HuntersPetLowHealthTrigger::IsActive()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    return pet && AI_VALUE2(uint8, "health", "pet target") < 40 && !AI_VALUE2(bool, "dead", "pet target") &&
           !AI_VALUE2(bool, "mounted", "self target");
}

bool HuntersPetMediumHealthTrigger::IsActive()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    return pet && AI_VALUE2(uint8, "health", "pet target") < sPlayerbotAIConfig->mediumHealth &&
           !AI_VALUE2(bool, "dead", "pet target") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterPetNotHappy::IsActive()
{
    return !AI_VALUE(bool, "pet happy") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterAspectOfTheViperTrigger::IsActive()
{
    return SpellTrigger::IsActive() && !botAI->HasAura(spell, GetTarget()) &&
           AI_VALUE2(uint8, "mana", "self target") < (sPlayerbotAIConfig->lowMana / 2);
    ;
}

bool HunterAspectOfThePackTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("aspect of the cheetah", GetTarget());
};

bool HunterLowAmmoTrigger::IsActive()
{
    return bot->GetGroup() && (AI_VALUE2(uint32, "item count", "ammo") < 100) &&
           (AI_VALUE2(uint32, "item count", "ammo") > 0);
}

bool HunterHasAmmoTrigger::IsActive() { return !AmmoCountTrigger::IsActive(); }

bool SwitchToRangedTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return botAI->HasStrategy("close", BOT_STATE_COMBAT) && target &&
           (target->GetVictim() != bot &&
            sServerFacade->IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"), 8.0f));
}

bool SwitchToMeleeTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return botAI->HasStrategy("ranged", BOT_STATE_COMBAT) && target &&
           (target->GetVictim() == bot &&
            sServerFacade->IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "current target"), 8.0f));
}

bool NoTrackTrigger::IsActive()
{
    std::vector<std::string> track_list = {
        "track beasts",
        "track demons",
        "track dragonkin",
        "track elementals",
        "track giants",
        "track hidden",
        "track humanoids"
    };

    for (auto &track: track_list)
    {
        if (botAI->HasAura(track, bot))
            return false;
    }
    return true;
}

bool SerpentStingOnAttackerTrigger::IsActive()
{
    if (!DebuffOnAttackerTrigger::IsActive())
        return false;
    Unit* target = GetTarget();
    if (!target)
    {
        return false;
    }
    return !botAI->HasAura("scorpid sting", target, false, true) &&
           !botAI->HasAura("viper sting", target, false, true);
    return BuffTrigger::IsActive();
}

const std::set<uint32> VolleyChannelCheckTrigger::VOLLEY_SPELL_IDS = {
    1510,   // Volley Rank 1
    14294,  // Volley Rank 2
    14295,  // Volley Rank 3
    27022,  // Volley Rank 4
    58431,  // Volley Rank 5
    58434   // Volley Rank 6
};

bool VolleyChannelCheckTrigger::IsActive()
{
    Player* bot = botAI->GetBot();

    // Check if the bot is channeling a spell
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        // Only trigger if the spell being channeled is Volley
        if (VOLLEY_SPELL_IDS.count(spell->m_spellInfo->Id))
        {
            uint8 attackerCount = AI_VALUE(uint8, "attacker count");
            return attackerCount < minEnemies;
        }
    }

    // Not channeling Volley
    return false;
}

// Aspect of the Cheetah PvP - Use for flag carrier mobility when not in combat
bool AspectOfCheetahPvPTrigger::IsActive()
{
    // Must have Aspect of the Cheetah spell (5118)
    if (!bot->HasSpell(5118))
        return false;
    
    // Works in battlegrounds and arenas
    if (!bot->InBattleground() && !bot->InArena())
        return false;
    
    // Already has Cheetah aspect
    if (botAI->HasAura("aspect of the cheetah", bot))
        return false;
    
    // Already has Pack aspect
    if (botAI->HasAura("aspect of the pack", bot))
        return false;
    
    // Don't use in combat (daze risk)
    if (bot->IsInCombat())
        return false;
    
    // Use Cheetah if we're a flag carrier and out of combat
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return true;
    
    return false;
}

// Concussive Shot PvP - Slow fleeing flag carriers and high-priority targets
bool ConcussiveShotPvPTrigger::IsActive()
{
    // Must have Concussive Shot spell (5116)
    if (!bot->HasSpell(5116) || bot->HasSpellCooldown(5116))
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
    
    // Range check (~35 yards)
    if (dist > 35.0f || dist < 5.0f)
        return false;
    
    // Already slowed by Concussive Shot
    if (enemy->HasAura(5116))
        return false;
    
    // Priority 1: Flag carrier moving
    bool isFlagCarrier = enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976);
    if (isFlagCarrier && enemy->isMoving())
        return true;
    
    // Priority 2: Target fleeing at distance > 15y
    bool isFleeing = enemy->isMoving() && !bot->IsWithinMeleeRange(enemy);
    if (isFleeing && dist > 15.0f)
        return true;
    
    return false;
}
