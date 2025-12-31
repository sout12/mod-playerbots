#include "Playerbots.h"
#include "HallsOfStoneActions.h"
#include "HallsOfStoneStrategy.h"

bool ShatterSpreadAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    if (!boss) { return false; }

    float radius = 40.0f;
    Unit* closestMember = nullptr;

    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (!unit || bot->GetGUID() == member)
        {
            continue;
        }
        if (!closestMember || bot->GetExactDist2d(unit) < bot->GetExactDist2d(closestMember))
        {
            closestMember = unit;
        }
    }

    if (closestMember && bot->GetExactDist2d(closestMember) < radius)
    {
        // Move in small increments so course can be corrected, otherwise two bots may
        // run in the same direction and not adjust until they reach the destination
        // return MoveAway(closestMember, radius - bot->GetExactDist2d(closestMember));
        return MoveAway(closestMember, 5.0f);
    }

    return false;
}

bool AvoidLightningRingAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sjonnir the ironshaper");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 10.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}

bool AvoidTribunalFireBeamAction::Execute(Event event)
{
    // Quickly move away from fire beam AOE during Tribunal of Ages
    // Fire beams are stationary but cover large areas
    
    // If we have the debuff, we're standing in fire - move immediately
    if (bot->HasAura(DEBUFF_SEARING_GAZE))
    {
        // Move perpendicular to current position to exit beam quickly
        float currentAngle = bot->GetOrientation();
        float moveAngle = currentAngle + (urand(0, 1) ? M_PI / 2 : -M_PI / 2);
        
        float moveDistance = 8.0f;  // Move 8 yards to exit beam
        float x = bot->GetPositionX() + cos(moveAngle) * moveDistance;
        float y = bot->GetPositionY() + sin(moveAngle) * moveDistance;
        float z = bot->GetPositionZ();
        
        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, 
                      MovementPriority::MOVEMENT_COMBAT);
    }
    
    return false;
}
