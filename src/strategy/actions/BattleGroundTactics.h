/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BATTLEGROUNDTACTICSACTION_H
#define _PLAYERBOT_BATTLEGROUNDTACTICSACTION_H

#include "BattlegroundAV.h"
#include "MovementActions.h"
#include "ObjectGuid.h"

class ChatHandler;
class Battleground;
class PlayerbotAI;
struct Position;

#define SPELL_CAPTURE_BANNER 21651

enum WSBotStrategy : uint8
{
    WS_STRATEGY_BALANCED      = 0,
    WS_STRATEGY_OFFENSIVE     = 1,
    WS_STRATEGY_DEFENSIVE     = 2,
    WS_STRATEGY_MAX           = 3,
};

enum ABBotStrategy : uint8
{
    AB_STRATEGY_BALANCED      = 0,
    AB_STRATEGY_OFFENSIVE     = 1,
    AB_STRATEGY_DEFENSIVE     = 2,
    AB_STRATEGY_MAX           = 3,
};

enum AVBotStrategy : uint8
{
    AV_STRATEGY_BALANCED      = 0,
    AV_STRATEGY_OFFENSIVE     = 1,
    AV_STRATEGY_DEFENSIVE     = 2,
    AV_STRATEGY_MAX           = 3,
};

enum EYBotStrategy : uint8
{
    EY_STRATEGY_BALANCED      = 0,
    EY_STRATEGY_FRONT_FOCUS   = 1,
    EY_STRATEGY_BACK_FOCUS    = 2,
    EY_STRATEGY_FLAG_FOCUS    = 3,
    EY_STRATEGY_MAX           = 4
};

typedef void (*BattleBotWaypointFunc)();

struct BGStrategyData
{
    uint8 allianceStrategy = 0;
    uint8 hordeStrategy = 0;
};

extern std::unordered_map<uint32, BGStrategyData> bgStrategies;

// =============================================
// REACTIVE DEFENSE SYSTEM - Node State Tracking
// =============================================

enum NodeOwnerState : uint8
{
    NODE_STATE_NEUTRAL        = 0,
    NODE_STATE_ALLY_CONTROLLED = 1,
    NODE_STATE_ALLY_CONTESTED  = 2,  // Was ours, being captured
    NODE_STATE_ENEMY_CONTROLLED = 3,
    NODE_STATE_ENEMY_CONTESTED = 4,  // Was theirs, we're capturing
};

struct NodeStateInfo
{
    uint32 nodeId = 0;
    NodeOwnerState currentState = NODE_STATE_NEUTRAL;
    NodeOwnerState previousState = NODE_STATE_NEUTRAL;
    time_t stateChangeTime = 0;
    Position position;
    bool needsDefense = false;
    float defensivePriority = 0.0f;
};

// Global node state cache per battleground instance
extern std::unordered_map<uint32, std::unordered_map<uint32, NodeStateInfo>> bgNodeStates;

// Defensive recapture constants
constexpr float AB_CAPTURE_TIME = 60.0f;  // 60 seconds in AB
constexpr float DEFENSIVE_PRIORITY_MULTIPLIER_BASE = 2.0f;
constexpr float DEFENSIVE_RESPONSE_RADIUS = 200.0f;  // How far bots will travel to defend
constexpr uint32 MIN_DEFENDERS_RESPONSE = 2;
constexpr uint32 MAX_DEFENDERS_RESPONSE = 4;

struct BattleBotWaypoint
{
    BattleBotWaypoint(float x_, float y_, float z_, BattleBotWaypointFunc func) : x(x_), y(y_), z(z_), pFunc(func){};

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    BattleBotWaypointFunc pFunc = nullptr;
};

struct AVNodePositionData
{
    Position pos;
    float maxRadius;
};

// Added to fix bot stuck at objectives
static std::unordered_map<uint8, AVNodePositionData> AVNodeMovementTargets = {
    {BG_AV_NODES_FIRSTAID_STATION, {Position(640.364f, -36.535f, 45.625f), 15.0f}},
    {BG_AV_NODES_STORMPIKE_GRAVE, {Position(665.598f, -292.976f, 30.291f), 15.0f}},
    {BG_AV_NODES_STONEHEART_GRAVE, {Position(76.108f, -399.602f, 45.730f), 15.0f}},
    {BG_AV_NODES_SNOWFALL_GRAVE, {Position(-201.298f, -119.661f, 78.291f), 15.0f}},
    {BG_AV_NODES_ICEBLOOD_GRAVE, {Position(-617.858f, -400.654f, 59.692f), 15.0f}},
    {BG_AV_NODES_FROSTWOLF_GRAVE, {Position(-1083.803f, -341.520f, 55.304f), 15.0f}},
    {BG_AV_NODES_FROSTWOLF_HUT, {Position(-1405.678f, -309.108f, 89.377f, 0.392f), 10.0f}},
    {BG_AV_NODES_DUNBALDAR_SOUTH, {Position(556.551f, -77.240f, 51.931f), 0.0f}},
    {BG_AV_NODES_DUNBALDAR_NORTH, {Position(670.664f, -142.031f, 63.666f), 0.0f}},
    {BG_AV_NODES_ICEWING_BUNKER, {Position(200.310f, -361.232f, 56.387f), 0.0f}},
    {BG_AV_NODES_STONEHEART_BUNKER, {Position(-156.302f, -440.032f, 40.403f), 0.0f}},
    {BG_AV_NODES_ICEBLOOD_TOWER, {Position(-569.702f, -265.362f, 75.009f), 0.0f}},
    {BG_AV_NODES_TOWER_POINT, {Position(-767.439f, -360.200f, 90.895f), 0.0f}},
    {BG_AV_NODES_FROSTWOLF_ETOWER, {Position(-1303.737f, -314.070f, 113.868f), 0.0f}},
    {BG_AV_NODES_FROSTWOLF_WTOWER, {Position(-1300.648f, -267.356f, 114.151f), 0.0f}},
};

typedef std::vector<BattleBotWaypoint> BattleBotPath;

extern std::vector<BattleBotPath*> const vPaths_WS;
extern std::vector<BattleBotPath*> const vPaths_AB;
extern std::vector<BattleBotPath*> const vPaths_AV;
extern std::vector<BattleBotPath*> const vPaths_EY;
extern std::vector<BattleBotPath*> const vPaths_IC;

class BGTactics : public MovementAction
{
public:
    static bool HandleConsoleCommand(ChatHandler* handler, char const* args);
    uint8 static GetBotStrategyForTeam(Battleground* bg, TeamId teamId);

    BGTactics(PlayerbotAI* botAI, std::string const name = "bg tactics") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;

private:
    static std::string const HandleConsoleCommandPrivate(WorldSession* session, char const* args);
    bool moveToStart(bool force = false);
    bool selectObjective(bool reset = false);
    bool moveToObjective(bool ignoreDist);
    bool selectObjectiveWp(std::vector<BattleBotPath*> const& vPaths);
    bool moveToObjectiveWp(BattleBotPath* const& currentPath, uint32 currentPoint, bool reverse = false);
    bool startNewPathBegin(std::vector<BattleBotPath*> const& vPaths);
    bool startNewPathFree(std::vector<BattleBotPath*> const& vPaths);
    bool resetObjective();
    bool wsJumpDown();
    bool eyJumpDown();
    bool atFlag(std::vector<BattleBotPath*> const& vPaths, std::vector<uint32> const& vFlagIds);
    bool flagTaken();
    bool teamFlagTaken();
    bool protectFC();
    bool useBuff();
    uint32 getPlayersInArea(TeamId teamId, Position point, float range, bool combat = true);
    bool IsLockedInsideKeep();
    
    // Game State Awareness
    bool IsLosingBadly(Battleground* bg);
    bool IsWinning(Battleground* bg);
    bool ShouldPlayAggressive(Battleground* bg);
    bool ShouldPlayDefensive(Battleground* bg);
    uint32 GetTeamBasesControlled(Battleground* bg, TeamId teamId);
    
    // Opening Strategy (AB)
    bool IsGameOpening(Battleground* bg);
    uint32 GetAssignedOpeningNode(Battleground* bg);
    bool ShouldRushContestedObjectives();
    bool MoveToABNode(uint32 nodeIndex);
    
    // Objective Focus System
    bool ShouldEngageInCombat(Unit* target);
    bool IsNearObjective(float maxDistance = 40.0f);
    bool IsTargetThreateningObjective(Unit* target);
    bool IsDefendingObjective();
    bool IsAttackingObjective();
    Position GetNearestObjectivePosition();

    // Arena Intelligence Methods
    Unit* GetArenaFocusTarget();  // Get coordinated focus target (healer priority)
    bool ShouldFocusHealer();     // Check if should focus enemy healer
    bool IsUnderHeavyPressure();  // Check if bot is being heavily pressured
    bool ShouldUseDefensiveCooldown();  // Check if should use defensive CD
    bool IsBurstWindow();         // Check if it's a good time to burst
    bool ShouldUseBurstCooldown();  // Check if should use offensive CDs
    uint8 CountEnemyHealers();    // Count enemy healers in arena
    bool IsEnemyHealerCCd();      // Check if enemy healer is CC'd

    // Team Coordination Methods
    bool ShouldProtectHealer();   // Check if should peel for healer
    Unit* GetAllyHealer();        // Get allied healer to protect
    bool IsAllyHealerThreatened();  // Check if ally healer under attack
    bool ShouldEscortFlagCarrier();  // Check if should escort FC

    // Tactical Vision & Risk Assessment
    uint8 CountEnemiesNearPosition(Position pos, float radius = 40.0f);  // Count enemies near position
    uint8 CountAlliesNearPosition(Position pos, float radius = 40.0f);   // Count allies near position
    bool IsSafeToAttackObjective(Position objPos);  // Check if safe to attack (not outnumbered)
    Position GetSafestObjective();  // Get objective with best ally:enemy ratio
    bool ShouldRegroupBeforeAttack();  // Check if should wait for allies
    bool IsCriticalSituation();  // Check if situation demands aggressive play despite risk
    bool IsEnemyFCNearCap();  // Check if enemy FC is about to score

    // IoC Vehicle & Coordination Methods
    bool IsDrivingSiegeEngine();  // Check if driving siege engine
    bool IsVehiclePassenger();    // Check if passenger in vehicle
    Position GetEnemyGatePosition();  // Get enemy gate position
    Unit* FindNearestSiegeVehicle(float radius);  // Find nearest siege vehicle
    bool ShouldWaitForSiegeGroup();  // Wait for allies before assault
    bool ShouldProtectSiegeEngine();  // Protect allied siege engines
    Unit* FindAlliedSiegeEngine(float radius);  // Find allied siege to protect
    bool HasSeaforiumCharge();  // Check for seaforium charge in inventory
    bool TeamControlsWorkshop();  // Check workshop control
    bool TeamControlsHangar();    // Check hangar control

    // =============================================
    // REACTIVE DEFENSE SYSTEM
    // =============================================
    
    // Node State Tracking
    // React to node state changes
    void UpdateNodeStates(Battleground* bg);           // Check all nodes for state changes
    
    // Current Objective Tracking (Make accessible to helpers)
    WorldObject* BgObjective = nullptr;
    ObjectGuid lastCaptureInterruptGuid;

    void OnNodeContested(uint32 nodeId, Position pos); // Handler when our node is attacked
    void OnNodeLost(uint32 nodeId, Position pos);      // Handler when we lose a node
    void OnNodeRecaptured(uint32 nodeId);              // Handler when we recapture
    
    // Defensive Response
    bool IsDefensiveRecaptureTarget(uint32 nodeId);    // Check if node is defensive priority
    float GetDefensiveRecapturePriority(uint32 nodeId, Position nodePos);  // Priority calculation
    bool ShouldRespondToDefense(uint32 nodeId, Position nodePos);  // Should this bot respond?
    uint32 GetClosestContestedNode();                  // Find nearest contested node
    float GetCaptureTimeRemaining(uint32 nodeId);      // Time left before node fully captured
    
    // Priority Boost System
    float GetNodeStrategicValue(uint32 nodeId, BattlegroundTypeId bgType);  // Base strategic value
    float GetDefensivePriorityMultiplier(float timeRemaining);  // Urgency multiplier
    bool HasCriticalObjective();  // Check if bot has critical task (flag, etc)
    
    // Helper Methods
    Position GetNodePosition(uint32 nodeId, BattlegroundTypeId bgType);
    std::string GetNodeName(uint32 nodeId, BattlegroundTypeId bgType);
    uint8 GetEnemiesAtNode(uint32 nodeId, Position nodePos);
    uint8 GetAlliesAtNode(uint32 nodeId, Position nodePos);
};

class ArenaTactics : public MovementAction
{
public:
    ArenaTactics(PlayerbotAI* botAI, std::string const name = "arena tactics") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;

private:
    bool moveToCenter(Battleground* bg);
    bool moveToPillar(Battleground* bg, bool preferSafety, uint32 featureFlags);
    bool moveToReset(Battleground* bg, uint32 featureFlags);
    std::vector<Position> GetArenaPillarPositions(BattlegroundTypeId bgType) const;
    std::vector<Position> GetArenaResetPositions(BattlegroundTypeId bgType) const;
    Position SelectBestArenaPosition(BattlegroundTypeId bgType, std::vector<Position> const& positions, bool preferSafety,
                                     uint32 featureFlags) const;
    uint8 CountEnemiesNearPosition(Position const& pos, float radius) const;
    uint8 CountAlliesNearPosition(Position const& pos, float radius) const;
    bool IsUnderPressure() const;
    float GetManaPct() const;
};

#endif
