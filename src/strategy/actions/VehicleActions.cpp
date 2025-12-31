/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "VehicleActions.h"

#include "BattlegroundIC.h"
#include "ItemVisitors.h"
#include "ObjectDefines.h"
#include "Playerbots.h"
#include "QuestValues.h"
#include "ServerFacade.h"
#include "Unit.h"
#include "Vehicle.h"

// TODO methods to enter/exit vehicle should be added to BGTactics or MovementAction (so that we can better control
// whether bot is in vehicle, eg: get out of vehicle to cap flag, if we're down to final boss, etc),
// right now they will enter vehicle based only what's available here, then they're stuck in vehicle until they die
// (LeaveVehicleAction doesnt do much seeing as they, or another bot, will get in immediately after exit)
bool EnterVehicleAction::Execute(Event event)
{
    // do not switch vehicles yet
    if (bot->GetVehicle())
        return false;

    Player* master = botAI->GetMaster();
    // Triggered by a chat command
    if (event.getOwner() && master && master->GetTarget())
    {
        Unit* vehicleBase = botAI->GetUnit(master->GetTarget());
        if (!vehicleBase)
            return false;
        Vehicle* veh = vehicleBase->GetVehicleKit();
        if (vehicleBase->IsVehicle() && veh && veh->GetAvailableSeatCount())
        {
            return EnterVehicle(vehicleBase, false);
        }
        return false;
    }

    GuidVector npcs = AI_VALUE(GuidVector, "nearest vehicles");
    for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* vehicleBase = botAI->GetUnit(*i);
        if (!vehicleBase)
            continue;

        if (vehicleBase->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            continue;

        // dont let them get in the cannons as they'll stay forever and do nothing useful
        // dont let them in catapult they cant use them at all
        if (NPC_KEEP_CANNON == vehicleBase->GetEntry() || NPC_CATAPULT == vehicleBase->GetEntry())
            continue;

        if (!vehicleBase->IsFriendlyTo(bot))
            continue;

        if (!vehicleBase->GetVehicleKit()->GetAvailableSeatCount())
            continue;

        // this will avoid adding passengers (which dont really do much for the IOC vehicles which is the only place
        // this code is used)
        if (vehicleBase->GetVehicleKit()->IsVehicleInUse())
            continue;

        if (EnterVehicle(vehicleBase, true))
            return true;
    }

    return false;
}

bool EnterVehicleAction::EnterVehicle(Unit* vehicleBase, bool moveIfFar)
{
    float dist = sServerFacade->GetDistance2d(bot, vehicleBase);
    if (dist > 40.0f)
        return false;

    if (dist > INTERACTION_DISTANCE && !moveIfFar)
        return false;

    if (dist > INTERACTION_DISTANCE)
        return MoveTo(vehicleBase);
    // Use HandleSpellClick instead of Unit::EnterVehicle to handle special vehicle script (ulduar)
    vehicleBase->HandleSpellClick(bot);

    if (!bot->IsOnVehicle(vehicleBase))
        return false;

    // dismount because bots can enter vehicle on mount
    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return true;
}

bool LeaveVehicleAction::Execute(Event event)
{
    Vehicle* myVehicle = bot->GetVehicle();
    if (!myVehicle)
        return false;

    VehicleSeatEntry const* seat = myVehicle->GetSeatForPassenger(bot);
    if (!seat || !seat->CanEnterOrExit())
        return false;

    WorldPacket p;
    bot->GetSession()->HandleRequestVehicleExit(p);

    return true;
}

// ==========================================
// IOC SIEGE ENGINE ACTIONS
// ==========================================

bool IoCDriveSiegeEngineAction::isUseful()
{
    // Only useful if driving a siege engine in IoC
    if (!bot->InBattleground())
        return false;
    
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_IC)
        return false;
    
    // Must be in vehicle
    if (!bot->GetVehicle())
        return false;
    
    // Check if it's a siege engine
    Unit* vehicle = bot->GetVehicle()->GetBase();
    if (!vehicle)
        return false;
    
    uint32 entry = vehicle->GetEntry();
    return (entry == 34776 || entry == 35273);  // Siege engines
}

bool IoCDriveSiegeEngineAction::Execute(Event event)
{
    if (!bot->GetVehicle())
        return false;
    
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;
    
    // Get enemy gate position
    Position gatePos;
    if (bot->GetTeamId() == TEAM_ALLIANCE)
        gatePos = Position(1270.0f, -765.0f, 48.0f, 0.0f);  // Horde gate
    else
        gatePos = Position(341.0f, -872.0f, 47.0f, 0.0f);   // Alliance gate
    
    // Drive to gate
    float dist = bot->GetDistance(gatePos.GetPositionX(), gatePos.GetPositionY(), gatePos.GetPositionZ());
    
    // If HP < 40%, retreat (per IoC guide)
    Unit* vehicle = bot->GetVehicle()->GetBase();
    if (vehicle && vehicle->GetHealthPct() < 40.0f)
    {
        // Retreat - move away from gate
        float retreatDist = 30.0f;
        float angle = vehicle->GetAngle(gatePos.GetPositionX(), gatePos.GetPositionY()) + M_PI;
        float retreatX = vehicle->GetPositionX() + retreatDist * cos(angle);
        float retreatY = vehicle->GetPositionY() + retreatDist * sin(angle);
        
        return MoveTo(bot->GetMapId(), retreatX, retreatY, vehicle->GetPositionZ());
    }
    
    // Drive to enemy gate
    if (dist > 5.0f)
        return MoveTo(bot->GetMapId(), gatePos.GetPositionX(), gatePos.GetPositionY(), gatePos.GetPositionZ());
    
    // At gate - ram it! (vehicle spell)
    // Ram spell: 49366
    botAI->CastSpell(49366, bot);
    
    return true;
}

bool IoCPassengerCombatAction::isUseful()
{
    // Only useful if passenger in siege engine
    if (!bot->GetVehicle())
        return false;
    
    if (!bot->InBattleground())
        return false;
    
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_IC)
        return false;
    
    // Check if in a vehicle
    return true;  // Simplified - assumes passenger if in vehicle
}

bool IoCPassengerCombatAction::Execute(Event event)
{
    if (!bot->GetVehicle())
        return false;
    
    Unit* vehicle = bot->GetVehicle()->GetBase();
    if (!vehicle)
        return false;
    
    // Find nearest melee attacker (within 20y)
    Unit* nearestAttacker = nullptr;
    float nearestDist = 20.0f;
    
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;
        
        float dist = vehicle->GetDistance(unit);
        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearestAttacker = unit;
        }
    }
    
    if (nearestAttacker)
    {
        // Use Steam Blast (passenger ability: 49549)
        botAI->CastSpell(49549, nearestAttacker);
        return true;
    }
    
    return false;
}

