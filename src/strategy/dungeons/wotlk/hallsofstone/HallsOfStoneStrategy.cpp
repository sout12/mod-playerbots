#include "HallsOfStoneStrategy.h"
#include "HallsOfStoneMultipliers.h"

void WotlkDungeonHoSStrategy::InitTriggers(std::vector<TriggerNode*> &triggers)
{
    // Maiden of Grief
    // TODO: Jump into damage during shock of sorrow?

    // Krystallus
    // TODO: I think bots need to dismiss pets on this, or they nuke players they are standing close to
    triggers.push_back(new TriggerNode("ground slam",
        { NextAction("shatter spread", ACTION_RAID + 5) }));

    // Tribunal of Ages (Brann Escort Event)
    // Fire beams from statues during escort - bots need to move out quickly
    triggers.push_back(new TriggerNode("tribunal fire beam",
        { NextAction("avoid tribunal fire beam", ACTION_EMERGENCY) }));

    // Sjonnir The Ironshaper
    // Possibly tank in place in the middle of the room, assign a dps to adds?
    triggers.push_back(new TriggerNode("lightning ring",
        { NextAction("avoid lightning ring", ACTION_RAID + 5) }));
}

void WotlkDungeonHoSStrategy::InitMultipliers(std::vector<Multiplier*> &multipliers)
{
    multipliers.push_back(new KrystallusMultiplier(botAI));
    multipliers.push_back(new SjonnirMultiplier(botAI));
}
