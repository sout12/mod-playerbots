#include "Playerbots.h"
#include "HallsOfStoneTriggers.h"
#include "AiObject.h"
#include "AiObjectContext.h"

bool KrystallusGroundSlamTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    if (!boss) { return false; }

    // Check both of these... the spell is applied first, debuff later.
    // Neither is active for the full duration so we need to trigger off both
    return bot->HasAura(SPELL_GROUND_SLAM) || bot->HasAura(DEBUFF_GROUND_SLAM);
}

bool SjonnirLightningRingTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sjonnir the ironshaper");
    if (!boss) { return false; }

    return boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_LIGHTNING_RING);
}

bool TribunalFireBeamTrigger::IsActive()
{
    // Check if bot is taking damage from Searing Gaze (fire beams from statues)
    if (bot->HasAura(DEBUFF_SEARING_GAZE))
        return true;
    
    // Also check if we're near the Tribunal controller NPC during the event
    // This helps bots be proactive about avoiding beam areas
    Unit* controller = AI_VALUE2(Unit*, "find target", "brann bronzebeard");
    if (controller && bot->GetDistance(controller) < 50.0f)
    {
        // If Brann is active and we detect the fire beam spell nearby, be ready to move
        if (bot->HasAura(SPELL_SEARING_GAZE) || bot->HasAura(DEBUFF_SEARING_GAZE))
            return true;
    }
    
    return false;
}
