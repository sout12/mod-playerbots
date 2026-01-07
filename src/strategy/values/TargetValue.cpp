/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TargetValue.h"

#include "LastMovementValue.h"
#include "ObjectGuid.h"
#include "Playerbots.h"
#include "RtiTargetValue.h"
#include "ScriptedCreature.h"
#include "ThreatMgr.h"

Unit* FindTargetStrategy::GetResult() { return result; }

Unit* TargetValue::FindTarget(FindTargetStrategy* strategy)
{
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        ThreatMgr& ThreatMgr = unit->GetThreatMgr();
        strategy->CheckAttacker(unit, &ThreatMgr);
    }

    return strategy->GetResult();
}

bool FindNonCcTargetStrategy::IsCcTarget(Unit* attacker)
{
    if (Group* group = botAI->GetBot()->GetGroup())
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = ObjectAccessor::FindPlayer(itr->guid);
            if (!member || !member->IsAlive())
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(member))
            {
                if (botAI->GetAiObjectContext()->GetValue<Unit*>("rti cc target")->Get() == attacker)
                    return true;

                std::string const rti = botAI->GetAiObjectContext()->GetValue<std::string>("rti cc")->Get();
                int32 index = RtiTargetValue::GetRtiIndex(rti);
                if (index != -1)
                {
                    if (ObjectGuid guid = group->GetTargetIcon(index))
                        if (attacker->GetGUID() == guid)
                            return true;
                }
            }
        }

        if (ObjectGuid guid = group->GetTargetIcon(4))
            if (attacker->GetGUID() == guid)
                return true;
    }

    return false;
}

void FindTargetStrategy::GetPlayerCount(Unit* creature, uint32* tankCount, uint32* dpsCount)
{
    Player* bot = botAI->GetBot();
    if (tankCountCache.find(creature) != tankCountCache.end())
    {
        *tankCount = tankCountCache[creature];
        *dpsCount = dpsCountCache[creature];
        return;
    }

    *tankCount = 0;
    *dpsCount = 0;

    Unit::AttackerSet attackers(creature->getAttackers());
    for (Unit* attacker : attackers)
    {
        if (!attacker || !attacker->IsAlive() || attacker == bot)
            continue;

        Player* player = attacker->ToPlayer();
        if (!player)
            continue;

        if (botAI->IsTank(player))
            ++(*tankCount);
        else
            ++(*dpsCount);
    }

    tankCountCache[creature] = *tankCount;
    dpsCountCache[creature] = *dpsCount;
}

bool FindTargetStrategy::IsHighPriority(Unit* attacker)
{
    // Check raid icon priority
    if (Group* group = botAI->GetBot()->GetGroup())
    {
        ObjectGuid guid = group->GetTargetIcon(7);
        if (guid && attacker->GetGUID() == guid)
        {
            return true;
        }
    }
    
    // Check manual prioritized targets
    GuidVector prioritizedTargets = botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Get();
    for (ObjectGuid targetGuid : prioritizedTargets)
    {
        if (targetGuid && attacker->GetGUID() == targetGuid)
        {
            return true;
        }
    }
    
    // PvP Priority System - use scoring for battlegrounds and arenas
    Player* bot = botAI->GetBot();
    if (bot->InBattleground() || bot->InArena())
    {
        float priority = CalculatePvPPriority(attacker);
        // High priority threshold: 100+ (healers, flag carriers, low HP)
        return priority >= 100.0f;
    }
    
    return false;
}

float FindTargetStrategy::CalculatePvPPriority(Unit* target)
{
    if (!target || !target->IsPlayer())
        return 0.0f;

    Player* player = target->ToPlayer();
    float priority = 0.0f;

    // PRIORITY 0: Targets actively capturing objectives (node/flag channel)
    if (IsCapturingObjective(player))
        priority += 250.0f;

    // PRIORITY 1: Flag Carrier (300 points)
    if (IsFlagCarrier(player))
        priority += 300.0f;

    // PRIORITY 2: Enemy Siege Engines in IoC (400 points - HIGHEST!)
    if (player->GetVehicle())  // Player is in a vehicle
    {
        // Check if in IoC battleground
        if (botAI->GetBot()->InBattleground())
        {
            Battleground* bg = botAI->GetBot()->GetBattleground();
            if (bg && bg->GetBgTypeID() == BATTLEGROUND_IC)
                priority += 400.0f;  // KILL SIEGE MACHINES in IoC!
        }
    }

    // PRIORITY 3: Healer (150 points)
    if (IsHealer(player))
        priority += 150.0f;

    // PRIORITY 4: Low HP (100 points)
    if (IsLowHealthPriority(player))
        priority += 100.0f;

    // Deprioritize temporarily immune targets to force a hard switch
    if (IsTemporarilyImmune(player))
        priority -= 250.0f;

    // Distance bonus - but reduce penalty for high-priority targets
    float distance = botAI->GetBot()->GetDistance(player);
    
    // High priority targets (healers, flag carriers) get extended range
    bool isHighPriorityTarget = (IsHealer(player) || IsFlagCarrier(player));
    
    if (isHighPriorityTarget)
    {
        // High priority targets: maintain good priority up to 60y range
        // This ensures healers/FCs are targeted even at distance
        if (distance < 60.0f)
        {
            float distBonus = (60.0f - distance) / 60.0f * 30.0f;
            priority += distBonus;
        }
    }
    else
    {
        // Normal targets: standard distance bonus up to 40y
        if (distance < 40.0f)
        {
            float distBonus = (40.0f - distance) / 40.0f * 50.0f;
            priority += distBonus;
        }
    }

    // Peel priority (attacking our healer or FC)
    Group* group = botAI->GetBot()->GetGroup();
    if (group)
    {
        bool botIsMelee = botAI->IsMelee(botAI->GetBot());

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;

            // Attacking our healer
            if (IsHealer(member) && target->GetVictim() == member)
            {
                priority += 100.0f;
                if (botIsMelee)
                    priority += 50.0f;
                else if (player->IsWithinMeleeRange(member))
                    priority += 50.0f;
            }

            // Attacking our FC
            if (IsFlagCarrier(member) && target->GetVictim() == member)
                priority += 150.0f;
        }
    }

    return priority;
}

bool FindTargetStrategy::IsFlagCarrier(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;
    
    Player* player = unit->ToPlayer();
    
    // Check for flag carrier auras
    // Warsong Gulch flags
    if (player->HasAura(23333) || player->HasAura(23335)) // WSG Horde/Alliance flag
        return true;
    
    // Eye of the Storm flag
    if (player->HasAura(34976)) // Netherstorm flag
        return true;
    
    return false;
}

bool FindTargetStrategy::IsHealer(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;
    
    Player* player = unit->ToPlayer();
    return botAI->IsHeal(player);
}

bool FindTargetStrategy::IsLowHealthPriority(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;
    
    Player* player = unit->ToPlayer();
    
    // Low HP threshold: 30% for quick kill priority
    return player->GetHealthPct() < 30;
}

bool FindTargetStrategy::IsCapturingObjective(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;

    if (!botAI->GetBot()->InBattleground())
        return false;

    Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == 21651)
        return true;

    spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == 21651)
        return true;

    return false;
}

bool FindTargetStrategy::IsTemporarilyImmune(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;

    // Divine Shield, Hand of Protection, Ice Block, Dispersion
    if (botAI->HasAura(642, unit) || botAI->HasAura(1022, unit) || botAI->HasAura(45438, unit) ||
        botAI->HasAura(47585, unit))
        return true;

    return false;
}

WorldPosition LastLongMoveValue::Calculate()
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
    if (lastMove.lastPath.empty())
        return WorldPosition();

    return lastMove.lastPath.getBack();
}

WorldPosition HomeBindValue::Calculate()
{
    return WorldPosition(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.f);
}

Unit* FindTargetValue::Calculate()
{
    if (qualifier == "")
    {
        return nullptr;
    }
    Group* group = bot->GetGroup();
    if (!group)
    {
        return nullptr;
    }
    HostileReference* ref = bot->getHostileRefMgr().getFirst();
    while (ref)
    {
        ThreatMgr* threatManager = ref->GetSource();
        Unit* unit = threatManager->GetOwner();
        std::wstring wnamepart;
        Utf8toWStr(unit->GetName(), wnamepart);
        wstrToLower(wnamepart);
        if (!qualifier.empty() && qualifier.length() == wnamepart.length() && Utf8FitTo(qualifier, wnamepart))
        {
            return unit;
        }
        ref = ref->next();
    }
    return nullptr;
}

void FindBossTargetStrategy::CheckAttacker(Unit* attacker, ThreatMgr* threatManager)
{
    UnitAI* unitAI = attacker->GetAI();
    BossAI* bossAI = dynamic_cast<BossAI*>(unitAI);
    if (bossAI)
    {
        result = attacker;
    }
}

Unit* BossTargetValue::Calculate()
{
    FindBossTargetStrategy strategy(botAI);
    return FindTarget(&strategy);
}
