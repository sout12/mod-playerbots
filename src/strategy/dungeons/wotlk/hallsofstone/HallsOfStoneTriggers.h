#ifndef _PLAYERBOT_WOTLKDUNGEONHOSTRIGGERS_H
#define _PLAYERBOT_WOTLKDUNGEONHOSTRIGGERS_H

#include "Trigger.h"
#include "PlayerbotAIConfig.h"
#include "GenericTriggers.h"
#include "DungeonStrategyUtils.h"

enum HallsOfStoneIDs
{
    // Krystallus
    SPELL_GROUND_SLAM               = 50827,
    DEBUFF_GROUND_SLAM              = 50833,

    // Sjonnir The Ironshaper
    SPELL_LIGHTNING_RING_N          = 50840,
    SPELL_LIGHTNING_RING_H          = 59848,
    
    // Tribunal of Ages (Brann Escort Event)
    NPC_ABEDNEUM                    = 28234,
    NPC_TRIBUNAL_CONTROLLER         = 28234,  // Invisible controller for event
    SPELL_SEARING_GAZE_N            = 50865,  // Fire beam from statues
    SPELL_SEARING_GAZE_H            = 59867,
    DEBUFF_SEARING_GAZE_N           = 50865,
    DEBUFF_SEARING_GAZE_H           = 59867,
};

#define SPELL_LIGHTNING_RING        DUNGEON_MODE(bot, SPELL_LIGHTNING_RING_N, SPELL_LIGHTNING_RING_H)
#define SPELL_SEARING_GAZE          DUNGEON_MODE(bot, SPELL_SEARING_GAZE_N, SPELL_SEARING_GAZE_H)
#define DEBUFF_SEARING_GAZE         DUNGEON_MODE(bot, DEBUFF_SEARING_GAZE_N, DEBUFF_SEARING_GAZE_H)

class KrystallusGroundSlamTrigger : public Trigger
{
public:
    KrystallusGroundSlamTrigger(PlayerbotAI* ai) : Trigger(ai, "krystallus ground slam") {}
    bool IsActive() override;
};

class SjonnirLightningRingTrigger : public Trigger
{
public:
    SjonnirLightningRingTrigger(PlayerbotAI* ai) : Trigger(ai, "sjonnir lightning ring") {}
    bool IsActive() override;
};

class TribunalFireBeamTrigger : public Trigger
{
public:
    TribunalFireBeamTrigger(PlayerbotAI* ai) : Trigger(ai, "tribunal fire beam") {}
    bool IsActive() override;
};

#endif
