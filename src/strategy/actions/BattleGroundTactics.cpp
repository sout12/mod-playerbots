/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BattleGroundTactics.h"

#include <algorithm>

#include "ArenaTeam.h"
#include <mutex>
#include "ArenaTeamMgr.h"
#include "BattleGroundJoinAction.h"
#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundBE.h"
#include "BattlegroundDS.h"
#include "BattlegroundEY.h"
#include "BattlegroundIC.h"
#include "BattlegroundMgr.h"
#include "BattlegroundNA.h"
#include "BattlegroundRL.h"
#include "BattlegroundRV.h"
#include "BattlegroundSA.h"
#include "BattlegroundWS.h"
#include "Event.h"
#include "Bag.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "IVMapMgr.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PvpTriggers.h"
#include "ServerFacade.h"
#include "UseItemAction.h"
#include "../values/ArenaOpenerValue.h"
#include "Vehicle.h"
#include "World.h"
#include "Pet.h"
#include <unordered_map>
#include <ctime>

// Arena helper predicates
static bool IsImmuneOrInvulnerable(Unit* u)
{
    if (!u)
        return true;
    // bubble, ice block, dispersion, deterrence
    return u->HasAura(642) || u->HasAura(45438) || u->HasAura(47585) || u->HasAura(19263);
}

static bool IsHardCC(Unit* u)
{
    if (!u)
        return false;

    uint32 hardCcSpells[] = {33786, 118, 28272, 61305, 61721, 61780, 12826, 51514, 6358, 20066, 2094,
                             6770, 22570, 1833, 1776, 5246, 8122, 10890, 408,   17928, 6215, 5484};
    for (uint32 spellId : hardCcSpells)
    {
        if (u->HasAura(spellId))
            return true;
    }

    if (u->HasAuraType(SPELL_AURA_MOD_STUN) || u->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        u->HasAuraType(SPELL_AURA_MOD_FEAR) || u->HasAuraType(SPELL_AURA_MOD_CHARM) ||
        u->HasAuraType(SPELL_AURA_MOD_ROOT))
        return true;

    return false;
}

struct ArenaBurstState
{
    ObjectGuid focusTarget = ObjectGuid::Empty;
    time_t expire = 0;
};

static std::unordered_map<uint32, ArenaBurstState> sBurstStates;
static std::unordered_map<uint32, std::pair<ObjectGuid, time_t>> sPeelAssignments;
static std::unordered_map<ObjectGuid::LowType, time_t> sLastHardCC;
static std::unordered_map<ObjectGuid::LowType, time_t> sLastSoftCC;
static std::unordered_map<ObjectGuid::LowType, time_t> sLastDefensive;
static std::unordered_map<ObjectGuid::LowType, time_t> sIoCBannerThrottle;

static bool WasRecentlyHardCC(ObjectGuid const& guid, time_t now)
{
    auto it = sLastHardCC.find(guid.GetCounter());
    return it != sLastHardCC.end() && (now - it->second) < 3;
}

static bool WasRecentlySoftCC(ObjectGuid const& guid, time_t now)
{
    auto it = sLastSoftCC.find(guid.GetCounter());
    return it != sLastSoftCC.end() && (now - it->second) < 4;
}

static bool WasRecentlyDefensive(ObjectGuid const& guid, time_t now)
{
    auto it = sLastDefensive.find(guid.GetCounter());
    return it != sLastDefensive.end() && (now - it->second) < 6;
}

static ObjectGuid GetBurstTarget(uint32 instanceId, time_t now)
{
    auto it = sBurstStates.find(instanceId);
    if (it != sBurstStates.end() && it->second.focusTarget != ObjectGuid::Empty &&
        it->second.expire > now)
        return it->second.focusTarget;
    return ObjectGuid::Empty;
}

static void StartBurstWindow(uint32 instanceId, ObjectGuid target, time_t now, float duration = 5.0f)
{
    sBurstStates[instanceId] = {target, now + static_cast<time_t>(duration)};
}

static bool RescueFromRLSlime(Player* bot, Battleground* bg)
{
    if (!bot || !bg || bg->GetBgTypeID() != BATTLEGROUND_RL)
        return false;

    if (!bot->IsInWater())
        return false;

    Position safe(1266.8f, 1663.5f, 34.0f);
    if (bot->GetDistance(safe) > 3.0f)
    {
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ());
        return true;
    }

    return false;
}

static bool RescueFromRLSpawnTunnel(Player* bot, Battleground* bg)
{
    if (!bot || !bg || bg->GetBgTypeID() != BATTLEGROUND_RL)
        return false;

    // Detect spawn tunnel stuck positions and push toward mid
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();

    if (z < 33.0f && x > 1240.0f && x < 1325.0f && (y > 1710.0f || y < 1610.0f))
    {
        Position mid(1266.8f, 1663.5f, 34.0f);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, mid.GetPositionX(), mid.GetPositionY(), mid.GetPositionZ());
        return true;
    }

    return false;
}

static bool RescueFromBladeEdgeStart(Player* bot, Battleground* bg)
{
    if (!bot || !bg || bg->GetBgTypeID() != BATTLEGROUND_BE)
        return false;

    // If we’re still up on a start platform and not near mid after gates open, push to mid walkway.
    Position mid(6239.89f, 261.11f, 0.89f);
    float distMid = bot->GetDistance(mid);
    if (distMid < 6.0f)
        return false;

    // Only after start delay is over
    if (bg->GetStatus() == STATUS_IN_PROGRESS && bg->GetStartDelayTime() <= 0)
    {
        // Avoid forcing mid if already fighting near a target; only rescue if stationary at higher elevation.
        if (!bot->IsInCombat() || bot->GetPositionZ() > 10.0f)
        {
            bot->GetMotionMaster()->Clear();
            bot->GetMotionMaster()->MovePoint(0, mid.GetPositionX(), mid.GetPositionY(), mid.GetPositionZ());
            return true;
        }
    }

    return false;
}

static Player* FindNearestArenaEnemy(Player* bot, Battleground* bg)
{
    if (!bot || !bg)
        return nullptr;

    Player* best = nullptr;
    float bestDist = 250.0f;

    for (auto const& kv : bg->GetPlayers())
    {
        Player* enemy = ObjectAccessor::FindConnectedPlayer(kv.first);
        if (!enemy || !enemy->IsAlive() || enemy->GetTeamId() == bot->GetTeamId())
            continue;

        float d = bot->GetDistance(enemy);
        if (d < bestDist)
        {
            bestDist = d;
            best = enemy;
        }
    }

    return best;
}

static void ManagePetDiscipline(Player* bot, Unit* target)
{
    if (!bot)
        return;

    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive())
        return;

    pet->SetReactState(REACT_DEFENSIVE);

    if (!target || IsImmuneOrInvulnerable(target) || IsHardCC(target))
    {
        pet->AttackStop();
        pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        return;
    }

    if (pet->GetVictim() != target)
        pet->Attack(target, true);
}

// common bg positions
Position const WS_WAITING_POS_HORDE_1 = {944.981f, 1423.478f, 345.434f, 6.18f};
Position const WS_WAITING_POS_HORDE_2 = {948.488f, 1459.834f, 343.066f, 6.27f};
Position const WS_WAITING_POS_HORDE_3 = {933.484f, 1433.726f, 345.535f, 0.08f};
Position const WS_WAITING_POS_ALLIANCE_1 = {1510.502f, 1493.385f, 351.995f, 3.1f};
Position const WS_WAITING_POS_ALLIANCE_2 = {1496.578f, 1457.900f, 344.442f, 3.1f};
Position const WS_WAITING_POS_ALLIANCE_3 = {1521.235f, 1480.951f, 352.007f, 3.2f};
Position const WS_FLAG_POS_HORDE = {915.958f, 1433.925f, 346.193f, 0.0f};
Position const WS_FLAG_POS_ALLIANCE = {1539.219f, 1481.747f, 352.458f, 0.0f};
Position const WS_FLAG_HIDE_HORDE_1 = {926.142f, 1460.641f, 346.116f, 4.84f};
Position const WS_FLAG_HIDE_HORDE_2 = {925.166f, 1458.084f, 355.966f, 0.00f};
Position const WS_FLAG_HIDE_HORDE_3 = {924.922f, 1423.672f, 345.524f, 0.82f};
Position const WS_FLAG_HIDE_ALLIANCE_1 = {1529.249f, 1456.470f, 353.04f, 1.25f};
Position const WS_FLAG_HIDE_ALLIANCE_2 = {1540.286f, 1476.026f, 352.692f, 2.91f};
Position const WS_FLAG_HIDE_ALLIANCE_3 = {1495.807f, 1466.774f, 352.350f, 1.50f};
Position const WS_ROAM_POS = {1227.446f, 1476.235f, 307.484f, 1.50f};
Position const WS_GY_CAMPING_HORDE = {1039.819, 1388.759f, 340.703f, 0.0f};
Position const WS_GY_CAMPING_ALLIANCE = {1422.320f, 1551.978f, 342.834f, 0.0f};
std::vector<Position> const WS_FLAG_HIDE_HORDE = {WS_FLAG_HIDE_HORDE_1, WS_FLAG_HIDE_HORDE_2, WS_FLAG_HIDE_HORDE_3};
std::vector<Position> const WS_FLAG_HIDE_ALLIANCE = {WS_FLAG_HIDE_ALLIANCE_1, WS_FLAG_HIDE_ALLIANCE_2,
                                                     WS_FLAG_HIDE_ALLIANCE_3};

Position const AB_WAITING_POS_HORDE = {702.884f, 703.045f, -16.115f, 0.77f};
Position const AB_WAITING_POS_ALLIANCE = {1286.054f, 1282.500f, -15.697f, 3.95f};
Position const AB_GY_CAMPING_HORDE = {723.513f, 725.924f, -28.265f, 3.99f};
Position const AB_GY_CAMPING_ALLIANCE = {1262.627f, 1256.341f, -27.289f, 0.64f};

// the captains aren't the actual creatures but invisible trigger creatures - they still have correct death state and
// location (unless they move)
uint32 const AV_CREATURE_A_CAPTAIN = AV_CPLACE_TRIGGER16;
uint32 const AV_CREATURE_A_BOSS = AV_CPLACE_A_BOSS;
uint32 const AV_CREATURE_H_CAPTAIN = AV_CPLACE_TRIGGER18;
uint32 const AV_CREATURE_H_BOSS = AV_CPLACE_H_BOSS;

Position const AV_CAVE_SPAWN_ALLIANCE = {872.460f, -491.571f, 96.546f, 0.0f};
Position const AV_CAVE_SPAWN_HORDE = {-1437.127f, -608.382f, 51.185f, 0.0f};
Position const AV_WAITING_POS_ALLIANCE = {793.627f, -493.814f, 99.689f, 3.09f};
Position const AV_WAITING_POS_HORDE = {-1381.865f, -544.872f, 54.773f, 0.76f};
Position const AV_ICEBLOOD_GARRISON_WAITING_ALLIANCE = {-523.105f, -182.178f, 57.956f, 0.0f};
Position const AV_ICEBLOOD_GARRISON_ATTACKING_ALLIANCE = {-545.288f, -167.932f, 57.012f, 0.0f};
Position const AV_STONEHEARTH_WAITING_HORDE = {-36.399f, -306.403f, 15.565f, 0.0f};
Position const AV_STONEHEARTH_ATTACKING_HORDE = {-55.210f, -288.546f, 15.578f, 0.0f};
Position const AV_BOSS_WAIT_H = {689.210f, -20.173f, 50.620f, 0.0f};
Position const AV_BOSS_WAIT_A = {-1361.773f, -248.977f, 99.369f, 0.0f};
Position const AV_MINE_SOUTH_1 = {-860.159f, -12.780f, 70.817f, 0.0f};
Position const AV_MINE_SOUTH_2 = {-827.639f, -147.314f, 62.565f, 0.0f};
Position const AV_MINE_SOUTH_3 = {-920.228f, -134.594f, 61.478f, 0.0f};
Position const AV_MINE_NORTH_1 = {822.477f, -456.782f, 48.569f, 0.0f};
Position const AV_MINE_NORTH_2 = {966.362f, -446.570f, 56.641f, 0.0f};
Position const AV_MINE_NORTH_3 = {952.081f, -335.073f, 63.579f, 0.0f};
Position const EY_WAITING_POS_HORDE = {1809.102f, 1540.854f, 1267.142f, 0.0f};
Position const EY_WAITING_POS_ALLIANCE = {2523.827f, 1596.915f, 1270.204f, 0.0f};
Position const EY_FLAG_RETURN_POS_RETREAT_HORDE = {1885.529f, 1532.157f, 1200.635f, 0.0f};
Position const EY_FLAG_RETURN_POS_RETREAT_ALLIANCE = {2452.253f, 1602.356f, 1203.617f, 0.0f};
Position const EY_GY_CAMPING_HORDE = {1874.854f, 1530.405f, 1207.432f, 0.0f};
Position const EY_GY_CAMPING_ALLIANCE = {2456.887f, 1599.025f, 1206.280f, 0.0f};
Position const EY_MID_FLAG_POS = {2174.782f, 1569.054f, 1160.361f, 0.0f};
Position const EY_BRIDGE_NORTH_ENTRANCE = {2275.622f, 1586.123f, 1164.469f, 0.0f};
Position const EY_BRIDGE_NORTH_MID = {2221.334f, 1575.123f, 1158.277f, 0.0f};
Position const EY_BRIDGE_SOUTH_ENTRANCE = {2059.276f, 1546.143f, 1162.394f, 0.0f};
Position const EY_BRIDGE_SOUTH_MID = {2115.978f, 1559.244f, 1156.362f, 0.0f};

Position const IC_WAITING_POS_HORDE = {1166.322f, -762.402f, 48.628f};
Position const IC_WEST_WAITING_POS_HORDE = {1217.666f, -685.449f, 48.915f};
Position const IC_EAST_WAITING_POS_HORDE = {1219.068f, -838.791f, 48.916f};

Position const IC_WAITING_POS_ALLIANCE = {387.893f, -833.384f, 48.714f};
Position const IC_WEST_WAITING_POS_ALLIANCE = {352.129f, -788.029f, 48.916f};
Position const IC_EAST_WAITING_POS_ALLIANCE = {351.517f, -882.477f, 48.916f};

Position const IC_CANNON_POS_HORDE1 = {1140.938f, -838.685f, 88.124f, 2.30f};
Position const IC_CANNON_POS_HORDE2 = {1139.695f, -686.574f, 88.173f, 3.95f};
Position const IC_CANNON_POS_ALLIANCE1 = {424.860f, -855.795f, 87.96f, 0.44f};
Position const IC_CANNON_POS_ALLIANCE2 = {425.525f, -779.538f, 87.717f, 5.88f};

Position const IC_GATE_ATTACK_POS_HORDE = {506.782f, -828.594f, 24.313f, 0.0f};
Position const IC_GATE_ATTACK_POS_ALLIANCE = {1091.273f, -763.619f, 42.352f, 0.0f};

// AB Node Indices (for opening strategy)
constexpr uint32 AB_NODE_STABLES = 0;
constexpr uint32 AB_NODE_BLACKSMITH = 1;
constexpr uint32 AB_NODE_FARM = 2;
constexpr uint32 AB_NODE_LUMBER_MILL = 3;
constexpr uint32 AB_NODE_GOLD_MINE = 4;

// AB Node Positions (for opening rush)
Position const AB_NODE_POS_STABLES = {1166.785f, 1200.132f, -56.70f, 0.0f};
Position const AB_NODE_POS_BLACKSMITH = {977.047f, 1046.819f, -44.80f, 0.0f};
Position const AB_NODE_POS_FARM = {806.205f, 874.005f, -55.99f, 0.0f};
Position const AB_NODE_POS_LUMBER_MILL = {1146.923f, 816.811f, -98.39f, 0.0f};
Position const AB_NODE_POS_GOLD_MINE = {735.551f, 646.016f, -12.91f, 0.0f};

enum BattleBotWsgWaitSpot
{
    BB_WSG_WAIT_SPOT_SPAWN,
    BB_WSG_WAIT_SPOT_LEFT,
    BB_WSG_WAIT_SPOT_RIGHT
};

std::unordered_map<uint32, BGStrategyData> bgStrategies;

std::vector<uint32> const vFlagsAV = {
    BG_AV_OBJECTID_BANNER_H_B,      BG_AV_OBJECTID_BANNER_H,      BG_AV_OBJECTID_BANNER_A_B,
    BG_AV_OBJECTID_BANNER_A,        BG_AV_OBJECTID_BANNER_CONT_A, BG_AV_OBJECTID_BANNER_CONT_A_B,
    BG_AV_OBJECTID_BANNER_CONT_H_B, BG_AV_OBJECTID_BANNER_CONT_H, BG_AV_OBJECTID_BANNER_SNOWFALL_N};

std::vector<uint32> const vFlagsAB = {
    BG_AB_OBJECTID_BANNER_A,
    BG_AB_OBJECTID_BANNER_CONT_A,
    BG_AB_OBJECTID_BANNER_H,
    BG_AB_OBJECTID_BANNER_CONT_H,
    BG_AB_OBJECTID_NODE_BANNER_0,
    BG_AB_OBJECTID_NODE_BANNER_1,
    BG_AB_OBJECTID_NODE_BANNER_2,
    BG_AB_OBJECTID_NODE_BANNER_3,
    BG_AB_OBJECTID_NODE_BANNER_4
};

std::vector<uint32> const vFlagsWS = {BG_WS_OBJECT_A_FLAG, BG_WS_OBJECT_H_FLAG};

std::vector<uint32> const vFlagsEY = {BG_EY_OBJECT_FLAG_NETHERSTORM, BG_EY_OBJECT_FLAG_FEL_REAVER,
                                      BG_EY_OBJECT_FLAG_BLOOD_ELF, BG_EY_OBJECT_FLAG_DRAENEI_RUINS,
                                      BG_EY_OBJECT_FLAG_MAGE_TOWER};

std::vector<uint32> const vFlagsIC = {BG_IC_GO_HORDE_BANNER,
                                      BG_IC_GO_ALLIANCE_BANNER,
                                      BG_IC_GO_WORKSHOP_BANNER,
                                      BG_IC_GO_DOCKS_BANNER,
                                      BG_IC_GO_HANGAR_BANNER,
                                      BG_IC_GO_QUARRY_BANNER,
                                      BG_IC_GO_REFINERY_BANNER};

// BG Waypoints (vmangos)

// Horde Flag Room to Horde Graveyard
BattleBotPath vPath_WSG_HordeFlagRoom_to_HordeGraveyard = {
    {933.331f, 1433.72f, 345.536f, nullptr}, {944.859f, 1423.05f, 345.437f, nullptr},
    {966.691f, 1422.53f, 345.223f, nullptr}, {979.588f, 1422.84f, 345.46f, nullptr},
    {997.806f, 1422.52f, 344.623f, nullptr}, {1008.53f, 1417.02f, 343.206f, nullptr},
    {1016.42f, 1402.33f, 341.352f, nullptr}, {1029.14f, 1387.49f, 340.836f, nullptr},
};

// Horde Graveyard to Horde Tunnel
BattleBotPath vPath_WSG_HordeGraveyard_to_HordeTunnel = {
    {1029.14f, 1387.49f, 340.836f, nullptr}, {1034.95f, 1392.62f, 340.856f, nullptr},
    {1038.21f, 1406.43f, 341.562f, nullptr}, {1043.87f, 1426.9f, 339.197f, nullptr},
    {1054.53f, 1441.47f, 339.725f, nullptr}, {1056.33f, 1456.03f, 341.463f, nullptr},
    {1057.39f, 1469.98f, 342.148f, nullptr}, {1057.67f, 1487.55f, 342.537f, nullptr},
    {1048.7f, 1505.37f, 341.117f, nullptr},  {1042.19f, 1521.69f, 338.003f, nullptr},
    {1050.01f, 1538.22f, 332.43f, nullptr},  {1068.15f, 1548.1f, 321.446f, nullptr},
    {1088.14f, 1538.45f, 316.398f, nullptr}, {1101.26f, 1522.79f, 314.918f, nullptr},
    {1114.67f, 1503.18f, 312.947f, nullptr}, {1126.45f, 1487.4f, 314.136f, nullptr},
    {1124.37f, 1462.28f, 315.853f, nullptr},
};

// Horde Tunnel to Horde Flag Room
BattleBotPath vPath_WSG_HordeTunnel_to_HordeFlagRoom = {
    {1124.37f, 1462.28f, 315.853f, nullptr}, {1106.87f, 1462.13f, 316.558f, nullptr},
    {1089.44f, 1461.04f, 316.332f, nullptr}, {1072.07f, 1459.46f, 317.449f, nullptr},
    {1051.09f, 1459.89f, 323.126f, nullptr}, {1030.1f, 1459.58f, 330.204f, nullptr},
    {1010.76f, 1457.49f, 334.896f, nullptr}, {1005.47f, 1448.19f, 335.864f, nullptr},
    {999.974f, 1458.49f, 335.632f, nullptr}, {982.632f, 1459.18f, 336.127f, nullptr},
    {965.049f, 1459.15f, 338.076f, nullptr}, {944.526f, 1459.0f, 344.207f, nullptr},
    {937.479f, 1451.12f, 345.553f, nullptr}, {933.331f, 1433.72f, 345.536f, nullptr},
};

// Alliance Flag Room to Alliance Graveyard
BattleBotPath vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard = {
    {1519.53f, 1481.87f, 352.024f, nullptr}, {1508.27f, 1493.17f, 352.005f, nullptr},
    {1490.78f, 1493.51f, 352.141f, nullptr}, {1469.79f, 1494.13f, 351.774f, nullptr},
    {1453.65f, 1494.39f, 350.614f, nullptr}, {1443.51f, 1501.75f, 348.317f, nullptr},
    {1443.33f, 1517.78f, 345.534f, nullptr}, {1443.55f, 1533.4f, 343.148f, nullptr},
    {1441.47f, 1548.12f, 342.752f, nullptr}, {1433.79f, 1552.67f, 342.763f, nullptr},
    {1422.88f, 1552.37f, 342.751f, nullptr}, {1415.33f, 1554.79f, 343.156f, nullptr},
};

// Alliance Graveyard to Alliance Tunnel
BattleBotPath vPath_WSG_AllianceGraveyard_to_AllianceTunnel = {
    {1415.33f, 1554.79f, 343.156f, nullptr}, {1428.29f, 1551.79f, 342.751f, nullptr},
    {1441.51f, 1545.79f, 342.757f, nullptr}, {1441.15f, 1530.35f, 343.712f, nullptr},
    {1435.53f, 1517.29f, 346.698f, nullptr}, {1424.81f, 1499.24f, 349.486f, nullptr},
    {1416.31f, 1483.94f, 348.536f, nullptr}, {1408.83f, 1468.4f, 347.648f, nullptr},
    {1404.64f, 1449.79f, 347.279f, nullptr}, {1405.34f, 1432.33f, 345.792f, nullptr},
    {1406.38f, 1416.18f, 344.755f, nullptr}, {1400.22f, 1401.87f, 340.496f, nullptr},
    {1385.96f, 1394.15f, 333.829f, nullptr}, {1372.38f, 1390.75f, 328.722f, nullptr},
    {1362.93f, 1390.02f, 327.034f, nullptr}, {1357.91f, 1398.07f, 325.674f, nullptr},
    {1354.17f, 1411.56f, 324.327f, nullptr}, {1351.44f, 1430.38f, 323.506f, nullptr},
    {1350.36f, 1444.43f, 323.388f, nullptr}, {1348.02f, 1461.06f, 323.167f, nullptr},
};

// Alliance Tunnel to Alliance Flag Room
BattleBotPath vPath_WSG_AllianceTunnel_to_AllianceFlagRoom = {
    {1348.02f, 1461.06f, 323.167f, nullptr}, {1359.8f, 1461.49f, 324.527f, nullptr},
    {1372.47f, 1461.61f, 324.354f, nullptr}, {1389.08f, 1461.12f, 325.913f, nullptr},
    {1406.57f, 1460.48f, 330.615f, nullptr}, {1424.04f, 1459.57f, 336.029f, nullptr},
    {1442.5f, 1459.7f, 342.024f, nullptr},   {1449.59f, 1469.14f, 342.65f, nullptr},
    {1458.03f, 1458.43f, 342.746f, nullptr}, {1469.4f, 1458.14f, 342.794f, nullptr},
    {1489.06f, 1457.86f, 342.794f, nullptr}, {1502.27f, 1457.52f, 347.589f, nullptr},
    {1512.87f, 1457.81f, 352.039f, nullptr}, {1517.53f, 1468.79f, 352.033f, nullptr},
    {1519.53f, 1481.87f, 352.024f, nullptr},
};

// Horde Tunnel to Horde Base Roof
BattleBotPath vPath_WSG_HordeTunnel_to_HordeBaseRoof = {
    {1124.37f, 1462.28f, 315.853f, nullptr}, {1106.87f, 1462.13f, 316.558f, nullptr},
    {1089.44f, 1461.04f, 316.332f, nullptr}, {1072.07f, 1459.46f, 317.449f, nullptr},
    {1051.09f, 1459.89f, 323.126f, nullptr}, {1030.1f, 1459.58f, 330.204f, nullptr},
    {1010.76f, 1457.49f, 334.896f, nullptr}, {981.948f, 1459.07f, 336.154f, nullptr},
    {981.768f, 1480.46f, 335.976f, nullptr}, {974.664f, 1495.9f, 340.837f, nullptr},
    {964.661f, 1510.21f, 347.509f, nullptr}, {951.188f, 1520.99f, 356.377f, nullptr},
    {937.37f, 1513.27f, 362.589f, nullptr},  {935.947f, 1499.58f, 364.199f, nullptr},
    {935.9f, 1482.08f, 366.396f, nullptr},   {937.564f, 1462.81f, 367.287f, nullptr},
    {945.871f, 1458.65f, 367.287f, nullptr}, {956.972f, 1459.48f, 367.291f, nullptr},
    {968.317f, 1459.71f, 367.291f, nullptr}, {979.934f, 1454.58f, 367.078f, nullptr},
    {979.99f, 1442.87f, 367.093f, nullptr},  {978.632f, 1430.71f, 367.125f, nullptr},
    {970.395f, 1422.32f, 367.289f, nullptr}, {956.338f, 1425.09f, 367.293f, nullptr},
    {952.778f, 1433.0f, 367.604f, nullptr},  {952.708f, 1445.01f, 367.604f, nullptr},
};

// Alliance Tunnel to Alliance Base Roof
BattleBotPath vPath_WSG_AllianceTunnel_to_AllianceBaseRoof = {
    {1348.02f, 1461.06f, 323.167f, nullptr}, {1359.8f, 1461.49f, 324.527f, nullptr},
    {1372.47f, 1461.61f, 324.354f, nullptr}, {1389.08f, 1461.12f, 325.913f, nullptr},
    {1406.57f, 1460.48f, 330.615f, nullptr}, {1424.04f, 1459.57f, 336.029f, nullptr},
    {1442.5f, 1459.7f, 342.024f, nullptr},   {1471.86f, 1456.65f, 342.794f, nullptr},
    {1470.93f, 1440.5f, 342.794f, nullptr},  {1472.24f, 1427.49f, 342.06f, nullptr},
    {1476.86f, 1412.46f, 341.426f, nullptr}, {1484.42f, 1396.69f, 346.117f, nullptr},
    {1490.7f, 1387.59f, 351.861f, nullptr},  {1500.79f, 1382.98f, 357.784f, nullptr},
    {1511.08f, 1391.29f, 364.444f, nullptr}, {1517.85f, 1403.18f, 370.336f, nullptr},
    {1517.99f, 1417.59f, 371.636f, nullptr}, {1517.07f, 1431.56f, 372.106f, nullptr},
    {1516.66f, 1445.55f, 372.939f, nullptr}, {1514.23f, 1457.37f, 373.689f, nullptr},
    {1503.73f, 1457.67f, 373.684f, nullptr}, {1486.24f, 1457.8f, 373.718f, nullptr},
    {1476.78f, 1460.35f, 373.711f, nullptr}, {1477.37f, 1470.83f, 373.709f, nullptr},
    {1477.5f, 1484.83f, 373.715f, nullptr},  {1480.53f, 1495.26f, 373.721f, nullptr},
    {1492.61f, 1494.72f, 373.721f, nullptr}, {1499.37f, 1489.02f, 373.718f, nullptr},
    {1500.63f, 1472.89f, 373.707f, nullptr},
};

// Alliance Tunnel to Horde Tunnel
BattleBotPath vPath_WSG_AllianceTunnel_to_HordeTunnel = {
    {1129.30f, 1461.03f, 315.206f, nullptr},
    {1149.55f, 1466.57f, 311.031f, nullptr},
    {1196.39f, 1477.25f, 305.697f, nullptr},
    {1227.73f, 1478.38f, 307.418f, nullptr},
    {1267.06f, 1463.40f, 312.227f, nullptr},
    {1303.42f, 1460.18f, 317.257f, nullptr},
};

// Alliance Graveyard (lower) to Horde Flag room
BattleBotPath vPath_WSG_AllianceGraveyardLower_to_HordeFlagRoom = {
    //{1370.71f, 1543.55f, 321.585f, nullptr},
    //{1339.41f, 1533.42f, 313.336f, nullptr},
    {1316.07f, 1533.53f, 315.700f, nullptr},
    {1276.17f, 1533.72f, 311.722f, nullptr},
    {1246.25f, 1533.86f, 307.072f, nullptr},
    {1206.84f, 1528.22f, 307.677f, nullptr},
    {1172.28f, 1523.28f, 301.958f, nullptr},
    {1135.93f, 1505.27f, 308.085f, nullptr},
    {1103.54f, 1521.89f, 314.583f, nullptr},
    {1073.49f, 1551.19f, 319.418f, nullptr},
    {1042.92f, 1530.49f, 336.667f, nullptr},
    {1052.11f, 1493.52f, 342.176f, nullptr},
    {1057.42f, 1452.75f, 341.131f, nullptr},
    {1037.96f, 1422.27f, 339.919f, nullptr},
    {966.01f, 1422.84f, 345.223f, nullptr},
    {942.74f, 1423.10f, 345.467f, nullptr},
    {929.39f, 1434.75f, 345.535f, nullptr},
};

// Horde Graveyard (lower) to Alliance Flag room
BattleBotPath vPath_WSG_HordeGraveyardLower_to_AllianceFlagRoom = {
    //{1096.59f, 1395.07f, 317.016f, nullptr},
    //{1134.38f, 1370.13f, 312.741f, nullptr},
    {1164.96f, 1356.91f, 313.884f, nullptr},
    {1208.32f, 1346.27f, 313.816f, nullptr},
    {1245.06f, 1346.12f, 312.112f, nullptr},
    {1291.43f, 1394.60f, 314.359f, nullptr},
    {1329.33f, 1411.13f, 318.399f, nullptr},
    {1361.57f, 1391.21f, 326.756f, nullptr},
    {1382.19f, 1381.03f, 332.314f, nullptr},
    {1408.06f, 1412.93f, 344.565f, nullptr},
    {1407.88f, 1458.76f, 347.346f, nullptr},
    {1430.40f, 1489.34f, 348.658f, nullptr},
    {1466.64f, 1493.50f, 351.869f, nullptr},
    {1511.51f, 1493.75f, 352.009f, nullptr},
    {1531.44f, 1481.79f, 351.959f, nullptr},
};

// Alliance Graveyard Jump
BattleBotPath vPath_WSG_AllianceGraveyardJump = {
    {1420.08f, 1552.84f, 342.878f, nullptr},
    {1404.06f, 1551.37f, 342.850f, nullptr},
    {1386.24f, 1543.37f, 321.944f, nullptr},
};

// Horde Graveyard Jump
BattleBotPath vPath_WSG_HordeGraveyardJump = {
    {1045.70f, 1389.15f, 340.638f, nullptr},
    {1057.43f, 1390.12f, 339.869f, nullptr},
    {1074.59f, 1400.67f, 323.811f, nullptr},
    {1092.85f, 1399.38f, 317.429f, nullptr},
};

// Alliance Base to Stables
BattleBotPath vPath_AB_AllianceBase_to_Stables = {
    {1285.67f, 1282.14f, -15.8466f, nullptr}, {1272.52f, 1267.83f, -21.7811f, nullptr},
    {1250.44f, 1248.09f, -33.3028f, nullptr}, {1232.56f, 1233.05f, -41.5241f, nullptr},
    {1213.25f, 1224.93f, -47.5513f, nullptr}, {1189.29f, 1219.49f, -53.119f, nullptr},
    {1177.17f, 1210.21f, -56.4593f, nullptr}, {1167.98f, 1202.9f, -56.4743f, nullptr},
};

// Blacksmith to Lumber Mill
BattleBotPath vPath_AB_Blacksmith_to_LumberMill = {
    {967.04f, 1039.03f, -45.091f, nullptr},  {933.67f, 1016.49f, -50.5154f, nullptr},
    {904.02f, 996.63f, -62.3461f, nullptr},  {841.74f, 985.23f, -58.8920f, nullptr},
    {796.25f, 1009.93f, -44.3286f, nullptr}, {781.29f, 1034.49f, -32.887f, nullptr},
    {793.17f, 1107.21f, 5.5663f, nullptr},   {848.98f, 1155.9f, 11.3453f, nullptr},
};

// Blacksmith to GoldMine
BattleBotPath vPath_AB_Blacksmith_to_GoldMine = {
    {1035.98f, 1015.66f, -46.0278f, nullptr}, {1096.86f, 1002.05f, -60.8013f, nullptr},
    {1159.93f, 1003.69f, -63.8378f, nullptr}, {1198.03f, 1064.09f, -65.8385f, nullptr},
    {1218.58f, 1016.96f, -76.9848f, nullptr}, {1192.83f, 956.25f, -93.6974f, nullptr},
    {1162.93f, 908.92f, -108.6703f, nullptr}, {1144.94f, 860.09f, -111.2100f, nullptr},
};

// Farm to Stables
BattleBotPath vPath_AB_Farm_to_Stable = {
    {749.88f, 878.23f, -55.1523f, nullptr},   {819.77f, 931.13f, -57.5882f, nullptr},
    {842.34f, 984.76f, -59.0333f, nullptr},   {863.03f, 1051.47f, -58.0495f, nullptr},
    {899.28f, 1098.27f, -57.4149f, nullptr},  {949.22f, 1153.27f, -54.4464f, nullptr},
    {999.07f, 1189.47f, -49.9125f, nullptr},  {1063.11f, 1211.55f, -53.4164f, nullptr},
    {1098.45f, 1225.47f, -53.1301f, nullptr}, {1146.02f, 1226.34f, -53.8979f, nullptr},
    {1167.10f, 1204.31f, -56.55f, nullptr},
};

// Alliance Base to Gold Mine
BattleBotPath vPath_AB_AllianceBase_to_GoldMine = {
    {1285.67f, 1282.14f, -15.8466f, nullptr}, {1276.41f, 1267.11f, -20.775f, nullptr},
    {1261.34f, 1241.52f, -31.2971f, nullptr}, {1244.91f, 1219.03f, -41.9658f, nullptr},
    {1232.25f, 1184.41f, -50.3348f, nullptr}, {1226.89f, 1150.82f, -55.7935f, nullptr},
    {1224.09f, 1120.38f, -57.0633f, nullptr}, {1220.03f, 1092.72f, -59.1744f, nullptr},
    {1216.05f, 1060.86f, -67.2771f, nullptr}, {1213.77f, 1027.96f, -74.429f, nullptr},
    {1208.56f, 998.394f, -81.9493f, nullptr}, {1197.42f, 969.73f, -89.9385f, nullptr},
    {1185.23f, 944.531f, -97.2433f, nullptr}, {1166.29f, 913.945f, -107.214f, nullptr},
    {1153.17f, 887.863f, -112.34f, nullptr},  {1148.89f, 871.391f, -111.96f, nullptr},
    {1145.24f, 850.82f, -110.514f, nullptr},
};

// Alliance Base to Lumber Mill
BattleBotPath vPath_AB_AllianceBase_to_LumberMill = {
    {1285.67f, 1282.14f, -15.8466f, nullptr}, {1269.13f, 1267.89f, -22.7764f, nullptr},
    {1247.79f, 1249.77f, -33.2518f, nullptr}, {1226.29f, 1232.02f, -43.9193f, nullptr},
    {1196.68f, 1230.15f, -50.4644f, nullptr}, {1168.72f, 1228.98f, -53.9329f, nullptr},
    {1140.82f, 1226.7f, -53.6318f, nullptr},  {1126.85f, 1225.77f, -47.98f, nullptr},
    {1096.5f, 1226.57f, -53.1769f, nullptr},  {1054.52f, 1226.14f, -49.2011f, nullptr},
    {1033.52f, 1226.08f, -45.5968f, nullptr}, {1005.52f, 1226.08f, -43.2912f, nullptr},
    {977.53f, 1226.68f, -40.16f, nullptr},    {957.242f, 1227.94f, -34.1487f, nullptr},
    {930.689f, 1221.57f, -18.9588f, nullptr}, {918.202f, 1211.98f, -12.2494f, nullptr},
    {880.329f, 1192.63f, 7.61168f, nullptr},  {869.965f, 1178.52f, 10.9678f, nullptr},
    {864.74f, 1163.78f, 12.385f, nullptr},    {859.165f, 1148.84f, 11.5289f, nullptr},
};

// Stables to Blacksmith
BattleBotPath vPath_AB_Stables_to_Blacksmith = {
    {1169.52f, 1198.71f, -56.2742f, nullptr}, {1166.93f, 1185.2f, -56.3634f, nullptr},
    {1173.84f, 1170.6f, -56.4094f, nullptr},  {1186.7f, 1163.92f, -56.3961f, nullptr},
    {1189.7f, 1150.68f, -55.8664f, nullptr},  {1185.18f, 1129.31f, -58.1044f, nullptr},
    {1181.7f, 1108.6f, -62.1797f, nullptr},   {1177.92f, 1087.95f, -63.5768f, nullptr},
    {1174.52f, 1067.23f, -64.402f, nullptr},  {1171.27f, 1051.09f, -65.0833f, nullptr},
    {1163.22f, 1031.7f, -64.954f, nullptr},   {1154.25f, 1010.25f, -63.5299f, nullptr},
    {1141.07f, 999.479f, -63.3713f, nullptr}, {1127.12f, 1000.37f, -60.628f, nullptr},
    {1106.17f, 1001.66f, -61.7457f, nullptr}, {1085.64f, 1005.62f, -58.5932f, nullptr},
    {1064.88f, 1008.65f, -52.3547f, nullptr}, {1044.16f, 1011.96f, -47.2647f, nullptr},
    {1029.72f, 1014.88f, -45.3546f, nullptr}, {1013.94f, 1028.7f, -43.9786f, nullptr},
    {990.89f, 1039.3f, -42.7659f, nullptr},   {978.269f, 1043.84f, -44.4588f, nullptr},
};

// Horde Base to Farm
BattleBotPath vPath_AB_HordeBase_to_Farm = {
    {707.259f, 707.839f, -17.5318f, nullptr}, {712.063f, 712.928f, -20.1802f, nullptr},
    {725.941f, 728.682f, -29.7536f, nullptr}, {734.715f, 739.591f, -35.2144f, nullptr},
    {747.607f, 756.161f, -40.899f, nullptr},  {753.994f, 766.668f, -43.3049f, nullptr},
    {758.715f, 787.106f, -46.7014f, nullptr}, {762.077f, 807.831f, -48.4721f, nullptr},
    {764.132f, 821.68f, -49.656f, nullptr},   {767.947f, 839.274f, -50.8574f, nullptr},
    {773.745f, 852.013f, -52.6226f, nullptr}, {785.123f, 869.103f, -54.2089f, nullptr},
    {804.429f, 874.961f, -55.2691f, nullptr},
};

// Horde Base to Gold Mine
BattleBotPath vPath_AB_HordeBase_to_GoldMine = {
    {707.259f, 707.839f, -17.5318f, nullptr}, {717.935f, 716.874f, -23.3941f, nullptr},
    {739.195f, 732.483f, -34.5791f, nullptr}, {757.087f, 742.008f, -38.1123f, nullptr},
    {776.946f, 748.775f, -42.7346f, nullptr}, {797.138f, 754.539f, -46.3237f, nullptr},
    {817.37f, 760.167f, -48.9235f, nullptr},  {837.638f, 765.664f, -49.7374f, nullptr},
    {865.092f, 774.738f, -51.9831f, nullptr}, {878.86f, 777.149f, -47.2361f, nullptr},
    {903.911f, 780.212f, -53.1424f, nullptr}, {923.454f, 787.888f, -54.7937f, nullptr},
    {946.218f, 798.93f, -59.0904f, nullptr},  {978.1f, 813.321f, -66.7268f, nullptr},
    {1002.94f, 817.895f, -77.3119f, nullptr}, {1030.77f, 820.92f, -88.7717f, nullptr},
    {1058.61f, 823.889f, -94.1623f, nullptr}, {1081.6f, 828.32f, -99.4137f, nullptr},
    {1104.64f, 844.773f, -106.387f, nullptr}, {1117.56f, 853.686f, -110.716f, nullptr},
    {1144.9f, 850.049f, -110.522f, nullptr},
};

// Horde Base to Lumber Mill
BattleBotPath vPath_AB_HordeBase_to_LumberMill = {
    {707.259f, 707.839f, -17.5318f, nullptr}, {721.611f, 726.507f, -27.9646f, nullptr},
    {733.846f, 743.573f, -35.8633f, nullptr}, {746.201f, 760.547f, -40.838f, nullptr},
    {758.937f, 787.565f, -46.741f, nullptr},  {761.289f, 801.357f, -48.0037f, nullptr},
    {764.341f, 822.128f, -49.6908f, nullptr}, {769.766f, 842.244f, -51.1239f, nullptr},
    {775.322f, 855.093f, -53.1161f, nullptr}, {783.995f, 874.216f, -55.0822f, nullptr},
    {789.917f, 886.902f, -56.2935f, nullptr}, {798.03f, 906.259f, -57.1162f, nullptr},
    {803.183f, 919.266f, -57.6692f, nullptr}, {813.248f, 937.688f, -57.7106f, nullptr},
    {820.412f, 958.712f, -56.1492f, nullptr}, {814.247f, 973.692f, -50.4602f, nullptr},
    {807.697f, 985.502f, -47.2383f, nullptr}, {795.672f, 1002.69f, -44.9382f, nullptr},
    {784.653f, 1020.77f, -38.6278f, nullptr}, {784.826f, 1037.34f, -31.5719f, nullptr},
    {786.083f, 1051.28f, -24.0793f, nullptr}, {787.314f, 1065.23f, -16.8918f, nullptr},
    {788.892f, 1086.17f, -6.42608f, nullptr}, {792.077f, 1106.53f, 4.81124f, nullptr},
    {800.398f, 1119.48f, 8.5814f, nullptr},   {812.476f, 1131.1f, 10.439f, nullptr},
    {829.704f, 1142.52f, 10.738f, nullptr},   {842.646f, 1143.51f, 11.9984f, nullptr},
    {857.674f, 1146.16f, 11.529f, nullptr},
};

// Farm to Blacksmith
BattleBotPath vPath_AB_Farm_to_Blacksmith = {
    {803.826f, 874.909f, -55.2547f, nullptr}, {808.763f, 887.991f, -57.4437f, nullptr},
    {818.33f, 906.674f, -59.3554f, nullptr},  {828.634f, 924.972f, -60.5664f, nullptr},
    {835.255f, 937.308f, -60.2915f, nullptr}, {845.244f, 955.78f, -60.4208f, nullptr},
    {852.125f, 967.965f, -61.3135f, nullptr}, {863.232f, 983.109f, -62.6402f, nullptr},
    {875.413f, 989.245f, -61.2916f, nullptr}, {895.765f, 994.41f, -63.6287f, nullptr},
    {914.16f, 1001.09f, -58.37f, nullptr},    {932.418f, 1011.44f, -51.9225f, nullptr},
    {944.244f, 1018.92f, -49.1438f, nullptr}, {961.55f, 1030.81f, -45.814f, nullptr},
    {978.122f, 1043.87f, -44.4682f, nullptr},
};

// Stables to Gold Mine
BattleBotPath vPath_AB_Stables_to_GoldMine = {
    {1169.52f, 1198.71f, -56.2742f, nullptr}, {1166.72f, 1183.58f, -56.3633f, nullptr},
    {1172.14f, 1170.99f, -56.4735f, nullptr}, {1185.18f, 1164.02f, -56.4269f, nullptr},
    {1193.98f, 1155.85f, -55.924f, nullptr},  {1201.51f, 1145.65f, -56.4733f, nullptr},
    {1205.39f, 1134.81f, -56.2366f, nullptr}, {1207.57f, 1106.9f, -58.4748f, nullptr},
    {1209.4f, 1085.98f, -63.4022f, nullptr},  {1212.68f, 1065.25f, -66.514f, nullptr},
    {1216.42f, 1037.52f, -72.0457f, nullptr}, {1215.4f, 1011.56f, -78.3338f, nullptr},
    {1209.8f, 992.293f, -83.2433f, nullptr},  {1201.23f, 973.121f, -88.5661f, nullptr},
    {1192.16f, 954.183f, -94.2209f, nullptr}, {1181.88f, 935.894f, -99.5239f, nullptr},
    {1169.86f, 918.68f, -105.588f, nullptr},  {1159.36f, 900.497f, -110.461f, nullptr},
    {1149.32f, 874.429f, -112.142f, nullptr}, {1145.34f, 849.824f, -110.523f, nullptr},
};

// Stables to Lumber Mill
BattleBotPath vPath_AB_Stables_to_LumberMill = {
    {1169.52f, 1198.71f, -56.2742f, nullptr},  {1169.33f, 1203.43f, -56.5457f, nullptr},
    {1164.77f, 1208.73f, -56.1907f, nullptr},  {1141.52f, 1224.99f, -53.8204f, nullptr},
    {1127.54f, 1224.82f, -48.2081f, nullptr},  {1106.56f, 1225.58f, -50.5154f, nullptr},
    {1085.6f, 1226.54f, -53.1863f, nullptr},   {1064.6f, 1226.82f, -50.4381f, nullptr},
    {1043.6f, 1227.27f, -46.5439f, nullptr},   {1022.61f, 1227.72f, -44.7157f, nullptr},
    {1001.61f, 1227.62f, -42.6876f, nullptr},  {980.623f, 1226.93f, -40.4687f, nullptr},
    {959.628f, 1227.1f, -35.3838f, nullptr},   {938.776f, 1226.34f, -23.5399f, nullptr},
    {926.138f, 1217.21f, -16.2176f, nullptr},  {911.966f, 1205.99f, -9.69655f, nullptr},
    {895.135f, 1198.85f, -0.546275f, nullptr}, {873.419f, 1189.27f, 9.3466f, nullptr},
    {863.821f, 1181.72f, 9.76912f, nullptr},   {851.803f, 1166.3f, 10.4423f, nullptr},
    {853.921f, 1150.92f, 11.543f, nullptr},
};

// Farm to Gold Mine
BattleBotPath vPath_AB_Farm_to_GoldMine = {
    {803.826f, 874.909f, -55.2547f, nullptr}, {801.662f, 865.689f, -56.9445f, nullptr},
    {806.433f, 860.776f, -57.5899f, nullptr}, {816.236f, 857.397f, -57.7029f, nullptr},
    {826.717f, 855.846f, -57.9914f, nullptr}, {836.128f, 851.257f, -57.8321f, nullptr},
    {847.933f, 843.837f, -58.1296f, nullptr}, {855.08f, 832.688f, -57.7373f, nullptr},
    {864.513f, 813.663f, -57.574f, nullptr},  {864.229f, 797.762f, -54.2057f, nullptr},
    {862.967f, 787.372f, -53.0276f, nullptr}, {864.163f, 776.33f, -52.0372f, nullptr},
    {872.583f, 777.391f, -48.5342f, nullptr}, {893.575f, 777.922f, -49.1826f, nullptr},
    {915.941f, 783.534f, -53.6598f, nullptr}, {928.105f, 789.929f, -55.4802f, nullptr},
    {946.263f, 800.46f, -59.166f, nullptr},   {958.715f, 806.845f, -62.1494f, nullptr},
    {975.79f, 811.913f, -65.9648f, nullptr},  {989.468f, 814.883f, -71.3089f, nullptr},
    {1010.13f, 818.643f, -80.0817f, nullptr}, {1023.97f, 820.667f, -86.1114f, nullptr},
    {1044.84f, 823.011f, -92.0583f, nullptr}, {1058.77f, 824.482f, -94.1937f, nullptr},
    {1079.13f, 829.402f, -99.1207f, nullptr}, {1092.85f, 836.986f, -102.755f, nullptr},
    {1114.75f, 851.21f, -109.782f, nullptr},  {1128.22f, 851.928f, -111.078f, nullptr},
    {1145.14f, 849.895f, -110.523f, nullptr},
};

// Farm to Lumber Mill
BattleBotPath vPath_AB_Farm_to_LumberMill = {
    {803.826f, 874.909f, -55.2547f, nullptr}, {802.874f, 894.28f, -56.4661f, nullptr},
    {806.844f, 920.39f, -57.3157f, nullptr},  {814.003f, 934.161f, -57.6065f, nullptr},
    {824.594f, 958.47f, -58.4916f, nullptr},  {820.434f, 971.184f, -53.201f, nullptr},
    {808.339f, 987.79f, -47.5705f, nullptr},  {795.98f, 1004.76f, -44.9189f, nullptr},
    {785.497f, 1019.18f, -39.2806f, nullptr}, {783.94f, 1032.46f, -33.5692f, nullptr},
    {784.956f, 1053.41f, -22.8368f, nullptr}, {787.499f, 1074.25f, -12.4232f, nullptr},
    {789.406f, 1088.11f, -5.28606f, nullptr}, {794.617f, 1109.17f, 6.1966f, nullptr},
    {801.514f, 1120.77f, 8.81455f, nullptr},  {817.3f, 1134.59f, 10.6064f, nullptr},
    {828.961f, 1142.98f, 10.7354f, nullptr},  {841.63f, 1147.75f, 11.6916f, nullptr},
    {854.326f, 1150.55f, 11.537f, nullptr},
};

//* ALLIANCE SIDE WAYPOINTS *//
BattleBotPath vPath_AV_AllianceSpawn_To_AllianceCrossroad1 = {
    {768.306f, -492.228f, 97.862f, nullptr}, {758.694f, -489.560f, 96.219f, nullptr},
    {718.949f, -473.185f, 75.285f, nullptr}, {701.759f, -457.199f, 64.398f, nullptr},
    {691.888f, -424.726f, 63.641f, nullptr}, {663.458f, -408.930f, 67.525f, nullptr},
    {650.696f, -400.027f, 67.948f, nullptr}, {621.638f, -383.551f, 67.511f, nullptr},
    {584.176f, -395.038f, 65.210f, nullptr}, {554.333f, -392.831f, 55.495f, nullptr},
    {518.020f, -415.663f, 43.470f, nullptr}, {444.974f, -429.032f, 27.276f, nullptr},
    {418.297f, -409.409f, 9.173f, nullptr},  {400.182f, -392.450f, -1.197f, nullptr}
};

BattleBotPath vPath_AV_AllianceFortress_To_AllianceCrossroad1 = {
    {654.750f, -32.779f, 48.611f, nullptr},  {639.731f, -46.509f, 44.072f, nullptr},
    {630.953f, -64.360f, 41.341f, nullptr},  {634.789f, -85.583f, 41.495f, nullptr},
    {628.271f, -100.943f, 40.328f, nullptr}, {621.330f, -129.900f, 33.687f, nullptr},
    {620.615f, -153.156f, 33.606f, nullptr}, {622.186f, -171.443f, 36.848f, nullptr},
    {624.456f, -188.047f, 38.569f, nullptr}, {629.185f, -222.637f, 38.362f, nullptr},
    {633.239f, -252.286f, 34.275f, nullptr}, {635.941f, -272.053f, 30.130f, nullptr},
    {632.884f, -294.946f, 30.267f, nullptr}, {625.348f, -322.407f, 30.133f, nullptr},
    {608.386f, -335.968f, 30.347f, nullptr}, {588.856f, -331.991f, 30.485f, nullptr},
    {560.140f, -327.872f, 17.396f, nullptr}, {530.523f, -324.481f, 1.718f, nullptr},
    {511.340f, -329.767f, -1.084f, nullptr}, {497.221f, -341.494f, -1.184f, nullptr},
    {465.281f, -365.577f, -1.243f, nullptr}, {437.839f, -376.473f, -1.243f, nullptr},
    {414.292f, -384.557f, -1.243f, nullptr}
};

BattleBotPath vPath_AV_AllianceCrossroad1_To_AllianceCrossroad2 = {
    {391.160f, -391.667f, -1.244f, nullptr}, {369.595f, -388.874f, -0.153f, nullptr},
    {334.986f, -384.282f, -0.726f, nullptr}, {308.373f, -383.026f, -0.336f, nullptr},
    {287.175f, -386.773f, 5.531f, nullptr},  {276.595f, -394.487f, 12.126f, nullptr},
    {264.248f, -405.109f, 23.509f, nullptr}, {253.142f, -412.069f, 31.665f, nullptr},
    {237.243f, -416.570f, 37.103f, nullptr}, {216.805f, -416.987f, 40.497f, nullptr},
    {195.369f, -407.750f, 42.876f, nullptr}, {179.619f, -402.642f, 42.793f, nullptr},
    {157.154f, -391.535f, 43.616f, nullptr}, {137.928f, -380.956f, 43.018f, nullptr},
    {117.990f, -380.243f, 43.571f, nullptr}
};

BattleBotPath vPath_AV_StoneheartGrave_To_AllianceCrossroad2 = {
    {73.141f, -481.863f, 48.479f, nullptr}, {72.974f, -461.923f, 48.498f, nullptr},
    {73.498f, -443.377f, 48.989f, nullptr}, {74.061f, -423.435f, 49.235f, nullptr},
    {83.086f, -403.871f, 47.412f, nullptr}, {102.003f, -388.023f, 45.095f, nullptr}
};

BattleBotPath vPath_AV_AllianceCrossroad2_To_StoneheartBunker = {
    {113.412f, -381.154f, 44.324f, nullptr},  {87.868f, -389.039f, 45.025f, nullptr},
    {62.170f, -388.790f, 45.012f, nullptr},   {30.645f, -390.873f, 45.644f, nullptr},
    {6.239f, -410.405f, 45.157f, nullptr},    {-11.185f, -423.994f, 45.222f, nullptr},
    {-19.595f, -437.877f, 46.383f, nullptr},  {-31.747f, -451.316f, 45.500f, nullptr},
    {-49.279f, -466.706f, 41.101f, nullptr},  {-78.427f, -459.899f, 27.648f, nullptr},
    {-103.623f, -467.766f, 24.806f, nullptr}, {-113.718f, -476.527f, 25.802f, nullptr},
    {-129.793f, -481.160f, 27.735f, nullptr}
};

BattleBotPath vPath_AV_AllianceCrossroad2_To_AllianceCaptain = {
    {117.948f, -363.836f, 43.485f, nullptr}, {110.096f, -342.635f, 41.331f, nullptr},
    {92.672f, -320.463f, 35.312f, nullptr},  {75.147f, -303.007f, 29.411f, nullptr},
    {59.694f, -293.137f, 24.670f, nullptr},  {41.342f, -293.374f, 17.193f, nullptr},
    {22.388f, -299.599f, 14.204f, nullptr}
};

BattleBotPath vPath_AV_AllianceCrossroads3_To_SnowfallGraveyard = {
    {-141.090f, -248.599f, 6.746f, nullptr},  {-147.551f, -233.354f, 9.675f, nullptr},
    {-153.353f, -219.665f, 17.267f, nullptr}, {-159.091f, -205.990f, 26.091f, nullptr},
    {-164.041f, -188.716f, 35.813f, nullptr}, {-170.634f, -166.838f, 51.540f, nullptr},
    {-183.338f, -154.159f, 64.252f, nullptr}, {-193.333f, -139.166f, 74.581f, nullptr},
    {-199.853f, -124.194f, 78.247f, nullptr}
};

BattleBotPath vPath_AV_AllianceCaptain_To_AllianceCrossroad3 = {
    {31.023f, -290.783f, 15.966f, nullptr},   {31.857f, -270.165f, 16.040f, nullptr},
    {26.531f, -242.488f, 14.158f, nullptr},   {3.448f, -241.318f, 11.900f, nullptr},
    {-24.158f, -233.744f, 9.802f, nullptr},   {-55.556f, -235.327f, 10.038f, nullptr},
    {-93.670f, -255.273f, 6.264f, nullptr},   {-117.164f, -263.636f, 6.363f, nullptr}
};

BattleBotPath vPath_AV_AllianceCaptain_To_HordeCrossroad3 = {
    {31.023f, -290.783f, 15.966f, nullptr}, {31.857f, -270.165f, 16.040f, nullptr},
    {26.531f, -242.488f, 14.158f, nullptr}, {3.448f, -241.318f, 11.900f, nullptr},
    {-24.158f, -233.744f, 9.802f, nullptr}, {-55.556f, -235.327f, 10.038f, nullptr},
    {-93.670f, -255.273f, 6.264f, nullptr}, {-117.164f, -263.636f, 6.363f, nullptr},
    {-154.433f, -272.428f, 8.016f, nullptr},  {-165.219f, -277.141f, 9.138f, nullptr},
    {-174.154f, -281.561f, 7.062f, nullptr},  {-189.765f, -290.755f, 6.668f, nullptr},
    {-213.121f, -303.227f, 6.668f, nullptr},  {-240.497f, -315.013f, 6.668f, nullptr},
    {-260.829f, -329.400f, 6.677f, nullptr},  {-280.652f, -344.530f, 6.668f, nullptr},
    {-305.708f, -363.655f, 6.668f, nullptr},  {-326.363f, -377.530f, 6.668f, nullptr},
    {-347.706f, -377.546f, 11.493f, nullptr}, {-361.965f, -373.012f, 13.904f, nullptr},
    {-379.646f, -367.389f, 16.907f, nullptr}, {-399.610f, -353.045f, 17.793f, nullptr},
    {-411.324f, -335.019f, 17.564f, nullptr}, {-421.800f, -316.128f, 17.843f, nullptr},
    {-435.855f, -293.863f, 19.553f, nullptr}, {-454.279f, -277.457f, 21.943f, nullptr},
    {-473.868f, -280.536f, 24.837f, nullptr}, {-492.305f, -289.846f, 29.787f, nullptr},
    {-504.724f, -313.745f, 31.938f, nullptr}, {-518.431f, -333.087f, 34.017f, nullptr}
};

BattleBotPath vPath_AV_StoneheartBunker_To_HordeCrossroad3 = {
    {-507.656f, -342.031f, 33.079f, nullptr}, {-490.580f, -348.193f, 29.170f, nullptr},
    {-458.194f, -343.101f, 31.685f, nullptr}, {-441.632f, -329.773f, 19.349f, nullptr},
    {-429.951f, -333.153f, 18.078f, nullptr}, {-415.858f, -341.834f, 17.865f, nullptr},
    {-406.698f, -361.322f, 17.764f, nullptr}, {-394.097f, -376.775f, 16.309f, nullptr},
    {-380.107f, -393.161f, 10.662f, nullptr}, {-359.474f, -405.379f, 10.437f, nullptr},
    {-343.166f, -415.036f, 10.177f, nullptr}, {-328.328f, -423.823f, 11.057f, nullptr},
    {-315.454f, -431.447f, 17.709f, nullptr}, {-296.180f, -449.228f, 22.316f, nullptr},
    {-272.700f, -459.303f, 28.764f, nullptr}, {-262.987f, -457.030f, 30.248f, nullptr},
    {-244.145f, -452.620f, 25.754f, nullptr}, {-221.311f, -448.195f, 25.166f, nullptr},
    {-201.762f, -457.441f, 27.413f, nullptr}, {-172.130f, -474.654f, 28.884f, nullptr},
    {-153.135f, -480.692f, 30.459f, nullptr}, {-137.077f, -478.766f, 28.798f, nullptr}
};

BattleBotPath vPath_AV_AllianceCrossroad1_To_AllianceMine = {
    {414.329f, -386.483f, -1.244f, nullptr}, {428.252f, -380.010f, -1.244f, nullptr},
    {446.203f, -372.263f, -1.244f, nullptr}, {468.352f, -360.936f, -1.244f, nullptr},
    {490.502f, -344.252f, -1.228f, nullptr}, {515.565f, -328.078f, -1.085f, nullptr},
    {531.431f, -323.435f, 1.906f, nullptr},  {548.612f, -324.795f, 10.162f, nullptr},
    {565.412f, -326.485f, 20.217f, nullptr}, {579.313f, -331.417f, 28.435f, nullptr},
    {593.026f, -336.281f, 30.215f, nullptr}, {614.580f, -331.387f, 30.236f, nullptr},
    {626.524f, -309.520f, 30.375f, nullptr}, {642.822f, -290.896f, 30.201f, nullptr},
    {661.428f, -285.810f, 29.862f, nullptr}, {670.488f, -281.637f, 27.951f, nullptr},
    {687.522f, -273.793f, 23.510f, nullptr}, {706.938f, -272.231f, 31.122f, nullptr},
    {725.715f, -281.845f, 40.529f, nullptr}, {733.208f, -296.224f, 47.418f, nullptr},
    {742.182f, -308.196f, 52.947f, nullptr}, {749.742f, -319.208f, 56.275f, nullptr},
    {760.992f, -335.660f, 60.279f, nullptr}, {783.734f, -342.468f, 61.410f, nullptr},
    {791.843f, -330.986f, 63.040f, nullptr}, {800.683f, -338.452f, 63.286f, nullptr},
    {812.383f, -337.218f, 64.698f, nullptr}, {826.326f, -331.789f, 64.492f, nullptr},
    {839.641f, -338.601f, 65.461f, nullptr}, {851.763f, -345.123f, 65.935f, nullptr},
    {871.511f, -345.464f, 64.947f, nullptr}, {882.320f, -340.819f, 66.864f, nullptr},
    {898.911f, -333.166f, 67.532f, nullptr}, {910.825f, -334.185f, 66.889f, nullptr},
    {922.109f, -336.746f, 66.120f, nullptr}, {929.915f, -353.314f, 65.902f, nullptr},
    {918.736f, -367.359f, 66.307f, nullptr}, {920.196f, -380.444f, 62.599f, nullptr},
    {920.625f, -400.310f, 59.454f, nullptr}, {920.521f, -410.085f, 57.002f, nullptr},
    {917.319f, -424.261f, 56.972f, nullptr}, {909.302f, -429.930f, 58.459f, nullptr},
    {893.164f, -430.083f, 55.782f, nullptr}, {873.846f, -425.108f, 51.387f, nullptr},
    {860.920f, -421.705f, 51.032f, nullptr}, {839.651f, -412.104f, 47.572f, nullptr},
    {831.515f, -410.477f, 47.778f, nullptr}
};
//* ALLIANCE SIDE WAYPOINTS *//

//* HORDE SIDE WAYPOINTS *//
BattleBotPath vPath_AV_HordeSpawn_To_MainRoad = {
    {-1366.182f, -532.170f, 53.354f, nullptr}, {-1325.007f, -516.055f, 51.554f, nullptr},
    {-1273.492f, -514.574f, 50.360f, nullptr}, {-1232.875f, -515.097f, 51.085f, nullptr},
    {-1201.299f, -500.807f, 51.665f, nullptr}, {-1161.199f, -476.367f, 54.956f, nullptr},
    {-1135.679f, -469.551f, 56.934f, nullptr}, {-1113.439f, -458.271f, 52.196f, nullptr},
    {-1086.750f, -444.735f, 52.903f, nullptr}, {-1061.950f, -434.380f, 51.396f, nullptr},
    {-1031.777f, -424.596f, 51.262f, nullptr}, {-967.556f, -399.110f, 49.213f, nullptr}
};

BattleBotPath vPath_AV_SnowfallGraveyard_To_HordeCaptain = {
    {-213.992f, -103.451f, 79.389f, nullptr}, {-222.690f, -95.820f, 77.588f, nullptr},
    {-237.377f, -88.173f, 65.871f, nullptr},  {-253.605f, -85.059f, 56.342f, nullptr},
    {-272.886f, -94.676f, 42.306f, nullptr},  {-289.627f, -108.123f, 26.978f, nullptr},
    {-304.265f, -105.120f, 20.341f, nullptr}, {-316.857f, -92.162f, 22.999f, nullptr},
    {-332.066f, -75.537f, 27.062f, nullptr},  {-358.184f, -76.459f, 27.212f, nullptr},
    {-388.166f, -81.693f, 23.836f, nullptr},  {-407.733f, -90.216f, 23.385f, nullptr},
    {-420.646f, -122.109f, 23.955f, nullptr}, {-418.916f, -143.640f, 24.135f, nullptr},
    {-419.211f, -171.836f, 24.088f, nullptr}, {-425.798f, -195.843f, 26.290f, nullptr},
    {-445.352f, -195.483f, 35.300f, nullptr}, {-464.614f, -194.387f, 49.409f, nullptr},
    {-477.910f, -193.219f, 54.985f, nullptr}, {-488.230f, -187.985f, 56.729f, nullptr}
};

BattleBotPath vPath_AV_HordeCrossroad3_To_IcebloodTower = {
    {-533.792f, -341.435f, 35.860f, nullptr}, {-551.527f, -332.298f, 38.432f, nullptr},
    {-574.093f, -312.653f, 44.791f, nullptr}
};

BattleBotPath vPath_AV_IcebloodTower_To_HordeCaptain = {
    {-569.690f, -295.928f, 49.096f, nullptr}, {-559.809f, -282.641f, 52.074f, nullptr},
    {-546.890f, -261.488f, 53.194f, nullptr}, {-529.471f, -236.931f, 56.746f, nullptr},
    {-518.182f, -222.736f, 56.922f, nullptr}, {-500.372f, -205.938f, 57.364f, nullptr},
    {-494.455f, -190.473f, 57.190f, nullptr}
};

BattleBotPath vPath_AV_IcebloodTower_To_IcebloodGrave = {
    {-584.305f, -313.025f, 47.651f, nullptr}, {-600.831f, -327.032f, 51.026f, nullptr},
    {-613.276f, -343.187f, 54.958f, nullptr}, {-625.873f, -364.812f, 56.829f, nullptr},
    {-625.494f, -390.816f, 58.781f, nullptr}
};

BattleBotPath vPath_AV_IcebloodGrave_To_TowerBottom = {
    {-635.524f, -393.738f, 59.527f, nullptr}, {-659.484f, -386.214f, 63.131f, nullptr},
    {-679.221f, -374.851f, 65.710f, nullptr}, {-694.579f, -368.145f, 66.017f, nullptr},
    {-726.698f, -346.235f, 66.804f, nullptr}, {-743.446f, -345.899f, 66.566f, nullptr},
    {-754.564f, -344.804f, 67.422f, nullptr}
};

BattleBotPath vPath_AV_TowerBottom_To_HordeCrossroad1 = {
    {-764.722f, -339.262f, 67.669f, nullptr},  {-777.559f, -338.964f, 66.287f, nullptr},
    {-796.674f, -341.982f, 61.848f, nullptr},  {-812.721f, -346.317f, 53.286f, nullptr},
    {-826.586f, -350.765f, 50.140f, nullptr},  {-842.410f, -355.642f, 49.750f, nullptr},
    {-861.475f, -361.517f, 50.514f, nullptr},  {-878.634f, -366.805f, 49.987f, nullptr},
    {-897.097f, -374.362f, 48.931f, nullptr},  {-920.177f, -383.808f, 49.487f, nullptr},
    {-947.087f, -392.660f, 48.533f, nullptr},  {-977.268f, -395.606f, 49.426f, nullptr},
    {-993.685f, -394.251f, 50.180f, nullptr},  {-1016.760f, -390.774f, 50.955f, nullptr},
    {-1042.994f, -383.854f, 50.904f, nullptr}, {-1066.925f, -377.541f, 52.535f, nullptr},
    {-1103.309f, -365.939f, 51.502f, nullptr}, {-1127.469f, -354.968f, 51.502f, nullptr}
};

BattleBotPath vPath_AV_HordeCrossroad1_To_FrostwolfGrave = {
    {-1127.565f, -340.254f, 51.753f, nullptr}, {-1112.843f, -337.645f, 53.368f, nullptr},
    {-1089.873f, -334.993f, 54.580f, nullptr}
};

BattleBotPath vPath_AV_HordeCrossroad1_To_HordeFortress = {
    {-1140.070f, -349.834f, 51.090f, nullptr}, {-1161.807f, -352.447f, 51.782f, nullptr},
    {-1182.047f, -361.856f, 52.458f, nullptr}, {-1203.318f, -365.951f, 54.427f, nullptr},
    {-1228.105f, -367.462f, 58.155f, nullptr}, {-1243.013f, -357.409f, 59.866f, nullptr},
    {-1245.382f, -337.206f, 59.322f, nullptr}, {-1236.790f, -323.051f, 60.500f, nullptr},
    {-1227.642f, -311.981f, 63.269f, nullptr}, {-1217.120f, -299.546f, 69.058f, nullptr},
    {-1207.536f, -288.218f, 71.742f, nullptr}, {-1198.808f, -270.859f, 72.431f, nullptr},
    {-1203.695f, -255.038f, 72.498f, nullptr}, {-1220.292f, -252.715f, 73.243f, nullptr},
    {-1236.539f, -252.215f, 73.326f, nullptr}, {-1246.512f, -257.577f, 73.326f, nullptr},
    {-1257.051f, -272.847f, 73.018f, nullptr}, {-1265.600f, -284.769f, 77.939f, nullptr},
    {-1282.656f, -290.696f, 88.334f, nullptr}, {-1292.589f, -290.809f, 90.446f, nullptr},
    {-1307.385f, -290.950f, 90.681f, nullptr}, {-1318.955f, -291.061f, 90.451f, nullptr},
    {-1332.717f, -291.117f, 90.806f, nullptr}, {-1346.880f, -287.015f, 91.066f, nullptr}
};

BattleBotPath vPath_AV_FrostwolfGrave_To_HordeMine = {
    {-1075.070f, -332.081f, 55.758f, nullptr}, {-1055.926f, -326.469f, 57.026f, nullptr},
    {-1036.115f, -328.239f, 59.368f, nullptr}, {-1018.217f, -332.306f, 59.335f, nullptr},
    {-990.396f, -337.397f, 58.342f, nullptr},  {-963.987f, -335.529f, 60.945f, nullptr},
    {-954.477f, -321.258f, 63.429f, nullptr},  {-951.052f, -301.476f, 64.761f, nullptr},
    {-955.257f, -282.794f, 63.683f, nullptr},  {-960.355f, -261.040f, 64.498f, nullptr},
    {-967.410f, -230.933f, 67.408f, nullptr},  {-967.187f, -207.444f, 68.924f, nullptr},
};
//* HORDE SIDE WAYPOINTS *//

BattleBotPath vPath_EY_Horde_Spawn_to_Crossroad1Horde = {
    {1809.102f, 1540.854f, 1267.142f, nullptr}, {1832.335f, 1539.495f, 1256.417f, nullptr},
    {1846.995f, 1539.792f, 1243.077f, nullptr}, {1846.243f, 1530.716f, 1238.477f, nullptr},
    {1883.154f, 1532.143f, 1202.143f, nullptr}, {1941.452f, 1549.086f, 1176.700f, nullptr}};

BattleBotPath vPath_EY_Horde_Crossroad1Horde_to_Crossroad2Horde = {{1951.647f, 1545.187f, 1174.831f, nullptr},
                                                                   {1992.266f, 1546.962f, 1169.816f, nullptr},
                                                                   {2045.865f, 1543.925f, 1163.759f, nullptr}};

BattleBotPath vPath_EY_Crossroad1Horde_to_Blood_Elf_Tower = {{1952.907f, 1539.857f, 1174.638f, nullptr},
                                                             {2000.130f, 1508.182f, 1169.778f, nullptr},
                                                             {2044.239f, 1483.860f, 1166.165f, nullptr},
                                                             {2048.773f, 1389.578f, 1193.903f, nullptr}};

BattleBotPath vPath_EY_Crossroad1Horde_to_Fel_Reaver_Ruins = {{1944.301f, 1557.170f, 1176.370f, nullptr},
                                                              {1992.953f, 1625.188f, 1173.616f, nullptr},
                                                              {2040.421f, 1676.989f, 1177.079f, nullptr},
                                                              {2045.527f, 1736.398f, 1189.661f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Horde_to_Blood_Elf_Tower = {{2049.363f, 1532.337f, 1163.178f, nullptr},
                                                             {2050.149f, 1484.721f, 1165.099f, nullptr},
                                                             {2046.865f, 1423.937f, 1188.882f, nullptr},
                                                             {2048.478f, 1389.491f, 1193.878f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Horde_to_Fel_Reaver_Ruins = {{2052.267f, 1555.692f, 1163.147f, nullptr},
                                                              {2047.684f, 1614.272f, 1165.397f, nullptr},
                                                              {2045.993f, 1668.937f, 1174.978f, nullptr},
                                                              {2044.286f, 1733.128f, 1189.739f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Horde_to_Flag = {{2059.276f, 1546.143f, 1162.394f, nullptr},
                                                  {2115.978f, 1559.244f, 1156.362f, nullptr},
                                                  {2149.140f, 1556.570f, 1158.412f, nullptr},
                                                  {2170.601f, 1567.113f, 1159.456f, nullptr}};

BattleBotPath vPath_EY_Alliance_Spawn_to_Crossroad1Alliance = {
    {2502.110f, 1604.330f, 1260.750f, nullptr}, {2497.077f, 1596.198f, 1257.302f, nullptr},
    {2483.930f, 1597.062f, 1244.660f, nullptr}, {2486.549f, 1617.651f, 1225.837f, nullptr},
    {2449.150f, 1601.792f, 1201.552f, nullptr}, {2395.737f, 1588.287f, 1176.570f, nullptr}};

BattleBotPath vPath_EY_Alliance_Crossroad1Alliance_to_Crossroad2Alliance = {
    {2380.262f, 1586.757f, 1173.567f, nullptr},
    {2333.956f, 1586.052f, 1169.873f, nullptr},
    {2291.210f, 1591.435f, 1166.048f, nullptr},
};

BattleBotPath vPath_EY_Crossroad1Alliance_to_Mage_Tower = {{2380.973f, 1593.445f, 1173.189f, nullptr},
                                                           {2335.762f, 1621.922f, 1169.007f, nullptr},
                                                           {2293.526f, 1643.972f, 1166.501f, nullptr},
                                                           {2288.198f, 1688.568f, 1172.790f, nullptr},
                                                           {2284.286f, 1737.889f, 1189.708f, nullptr}};

BattleBotPath vPath_EY_Crossroad1Alliance_to_Draenei_Ruins = {{2388.687f, 1576.089f, 1175.975f, nullptr},
                                                              {2354.921f, 1522.763f, 1176.060f, nullptr},
                                                              {2300.056f, 1459.208f, 1184.181f, nullptr},
                                                              {2289.880f, 1415.640f, 1196.755f, nullptr},
                                                              {2279.870f, 1387.461f, 1195.003f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Alliance_to_Mage_Tower = {{2282.525f, 1597.721f, 1164.553f, nullptr},
                                                           {2281.028f, 1651.310f, 1165.426f, nullptr},
                                                           {2284.633f, 1736.082f, 1189.708f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Alliance_to_Draenei_Ruins = {{2282.487f, 1581.630f, 1165.318f, nullptr},
                                                              {2284.728f, 1525.618f, 1170.812f, nullptr},
                                                              {2287.697f, 1461.228f, 1183.450f, nullptr},
                                                              {2290.861f, 1413.606f, 1197.115f, nullptr}};

BattleBotPath vPath_EY_Crossroad2Alliance_to_Flag = {{2275.622f, 1586.123f, 1164.469f, nullptr},
                                                     {2221.334f, 1575.123f, 1158.277f, nullptr},
                                                     {2178.372f, 1572.144f, 1159.462f, nullptr}};

BattleBotPath vPath_EY_Draenei_Ruins_to_Blood_Elf_Tower = {
    {2287.925f, 1406.976f, 1197.004f, nullptr}, {2283.283f, 1454.769f, 1184.243f, nullptr},
    {2237.519f, 1398.161f, 1178.191f, nullptr}, {2173.150f, 1388.084f, 1170.185f, nullptr},
    {2105.039f, 1381.507f, 1162.911f, nullptr}, {2074.315f, 1404.387f, 1178.141f, nullptr},
    {2047.649f, 1411.681f, 1192.032f, nullptr}, {2049.197f, 1387.392f, 1193.799f, nullptr}};

BattleBotPath vPath_EY_Fel_Reaver_to_Mage_Tower = {
    {2044.519f, 1726.113f, 1189.395f, nullptr}, {2045.408f, 1682.986f, 1177.574f, nullptr},
    {2097.595f, 1736.117f, 1170.419f, nullptr}, {2158.866f, 1746.998f, 1161.184f, nullptr},
    {2220.635f, 1757.837f, 1151.886f, nullptr}, {2249.922f, 1721.807f, 1161.550f, nullptr},
    {2281.021f, 1694.735f, 1174.020f, nullptr}, {2284.522f, 1728.234f, 1189.015f, nullptr}};

BattleBotPath vPath_IC_Ally_Keep_to_Ally_Front_Crossroad = {
    //{ 351.652f, -834.837f, 48.916f, nullptr },
    {434.768f, -833.976f, 46.090f, nullptr},
    {506.782f, -828.594f, 24.313f, nullptr},
    {524.955f, -799.002f, 19.498f, nullptr}};

BattleBotPath vPath_IC_Ally_Front_Crossroad_to_Workshop = {
    {524.955f, -799.002f, 19.498f, nullptr}, {573.557f, -804.838f, 9.6291f, nullptr},
    {627.977f, -810.197f, 3.5154f, nullptr}, {681.501f, -805.208f, 3.1464f, nullptr},
    {721.905f, -797.917f, 4.5112f, nullptr}, {774.466f, -801.058f, 6.3428f, nullptr}};

BattleBotPath vPath_IC_Ally_Keep_to_Ally_Dock_Crossroad = {{434.768f, -833.976f, 46.090f, nullptr},
                                                           {446.710f, -776.008f, 48.783f, nullptr},
                                                           {463.745f, -742.368f, 48.584f, nullptr},
                                                           {488.201f, -714.563f, 36.564f, nullptr},
                                                           {525.923f, -666.880f, 25.425f, nullptr}};

BattleBotPath vPath_IC_Ally_Front_Crossroad_to_Ally_Dock_Crossroad = {{524.955f, -799.002f, 19.498f, nullptr},
                                                                      {542.225f, -745.142f, 18.348f, nullptr},
                                                                      {545.309f, -712.497f, 22.005f, nullptr},
                                                                      {538.678f, -748.361f, 18.261f, nullptr},
                                                                      {525.923f, -666.880f, 25.425f, nullptr}};

BattleBotPath vPath_IC_Lower_Graveyard_to_Lower_Graveyard_Crossroad = {{443.095f, -310.797f, 51.749f, nullptr},
                                                                       {462.733f, -323.587f, 48.706f, nullptr},
                                                                       {471.540f, -343.914f, 40.706f, nullptr},
                                                                       {475.622f, -360.728f, 34.384f, nullptr},
                                                                       {484.458f, -379.796f, 33.122f, nullptr}};

BattleBotPath vPath_IC_Lower_Graveyard_Crossroad_to_Ally_Docks_Crossroad = {
    {484.458f, -379.796f, 33.122f, nullptr}, {509.786f, -380.592f, 33.122f, nullptr},
    {532.549f, -381.576f, 33.122f, nullptr}, {553.506f, -386.102f, 33.507f, nullptr},
    {580.533f, -398.536f, 33.416f, nullptr}, {605.112f, -409.843f, 33.121f, nullptr},
    {619.212f, -419.169f, 33.121f, nullptr}, {631.702f, -428.763f, 33.070f, nullptr},
    {648.483f, -444.714f, 28.629f, nullptr}};

BattleBotPath vPath_IC_Lower_Graveyard_Crossroad_to_Ally_Docks_Second_Crossroad = {
    {484.458f, -379.796f, 33.122f, nullptr}, {470.771f, -394.789f, 33.112f, nullptr},
    {461.191f, -409.475f, 33.120f, nullptr}, {452.794f, -431.842f, 33.120f, nullptr},
    {452.794f, -456.896f, 33.658f, nullptr}, {453.279f, -481.742f, 33.052f, nullptr},
    {453.621f, -504.979f, 32.956f, nullptr}, {452.006f, -526.792f, 32.221f, nullptr},
    {453.150f, -548.212f, 29.133f, nullptr}, {455.224f, -571.323f, 26.119f, nullptr},
    {465.486f, -585.424f, 25.756f, nullptr}, {475.366f, -598.414f, 25.784f, nullptr},
    {477.702f, -605.757f, 25.714f, nullptr}};

BattleBotPath vPath_IC_Ally_Dock_Crossroad_to_Ally_Docks_Second_Crossroad = {{525.923f, -666.880f, 25.425f, nullptr},
                                                                             {497.190f, -630.709f, 25.626f, nullptr},
                                                                             {477.702f, -605.757f, 25.714f, nullptr}};

BattleBotPath vPath_IC_Ally_Docks_Second_Crossroad_to_Ally_Docks_Crossroad = {{477.702f, -605.757f, 25.714f, nullptr},
                                                                              {493.697f, -555.838f, 26.014f, nullptr},
                                                                              {522.939f, -525.199f, 26.014f, nullptr},
                                                                              {580.398f, -486.274f, 26.013f, nullptr},
                                                                              {650.132f, -445.811f, 28.503f, nullptr}};

BattleBotPath vPath_IC_Ally_Docks_Crossroad_to_Docks_Flag = {{650.132f, -445.811f, 28.503f, nullptr},
                                                             {690.527f, -452.961f, 18.039f, nullptr},
                                                             {706.813f, -430.003f, 13.797f, nullptr},
                                                             {726.427f, -364.849f, 17.815f, nullptr}};

BattleBotPath vPath_IC_Docks_Graveyard_to_Docks_Flag = {
    {638.142f, -283.782f, 11.512f, nullptr}, {655.760f, -284.433f, 13.220f, nullptr},
    {661.656f, -299.912f, 12.756f, nullptr}, {675.068f, -317.192f, 12.627f, nullptr},
    {692.712f, -323.866f, 12.686f, nullptr}, {712.968f, -341.285f, 13.350f, nullptr},
    {726.427f, -364.849f, 17.815f, nullptr}};

BattleBotPath vPath_IC_Ally_Keep_to_Quarry_Crossroad = {
    {320.547f, -919.896f, 48.481f, nullptr}, {335.384f, -922.371f, 49.518f, nullptr},
    {353.471f, -920.316f, 48.660f, nullptr}, {353.305f, -958.823f, 47.665f, nullptr},
    {369.196f, -989.960f, 37.719f, nullptr}, {380.671f, -1023.51f, 29.369f, nullptr}};

BattleBotPath vPath_IC_Quarry_Crossroad_to_Quarry_Flag = {
    {380.671f, -1023.51f, 29.369f, nullptr}, {361.584f, -1052.89f, 27.445f, nullptr},
    {341.853f, -1070.17f, 24.024f, nullptr}, {295.845f, -1075.22f, 16.164f, nullptr},
    {253.030f, -1094.06f, 4.1517f, nullptr}, {211.391f, -1121.80f, 1.9591f, nullptr},
    {187.836f, -1155.66f, 1.9749f, nullptr}, {221.193f, -1186.87f, 8.0247f, nullptr},
    {249.181f, -1162.09f, 16.687f, nullptr}};

BattleBotPath vPath_IC_Ally_Front_Crossroad_to_Hangar_First_Crossroad = {{524.955f, -799.002f, 19.498f, nullptr},
                                                                         {512.563f, -840.166f, 23.913f, nullptr},
                                                                         {513.418f, -877.726f, 26.333f, nullptr},
                                                                         {512.962f, -945.951f, 39.382f, nullptr}};

BattleBotPath vPath_IC_Ally_Keep_to_Hangar_First_Crossroad = {{434.768f, -833.976f, 46.090f, nullptr},
                                                              {486.355f, -909.736f, 26.112f, nullptr},
                                                              {512.962f, -945.951f, 39.382f, nullptr}};

BattleBotPath vPath_IC_Hangar_First_Crossroad_to_Hangar_Second_Crossroad = {{512.962f, -945.951f, 39.382f, nullptr},
                                                                            {499.525f, -985.850f, 47.659f, nullptr},
                                                                            {492.794f, -1016.36f, 49.834f, nullptr},
                                                                            {481.738f, -1052.67f, 60.190f, nullptr}};

BattleBotPath vPath_IC_Quarry_Crossroad_to_Hangar_Second_Crossroad = {{380.671f, -1023.51f, 29.369f, nullptr},
                                                                      {430.997f, -1021.72f, 31.021f, nullptr},
                                                                      {439.528f, -1044.88f, 41.827f, nullptr},
                                                                      {455.062f, -1060.67f, 67.209f, nullptr},
                                                                      {481.738f, -1052.67f, 60.190f, nullptr}};

BattleBotPath vPath_IC_Hangar_Second_Crossroad_to_Hangar_Flag = {
    {508.945f, -1103.30f, 79.054f, nullptr}, {536.397f, -1145.79f, 95.478f, nullptr},
    {573.242f, -1138.19f, 109.26f, nullptr}, {609.051f, -1112.93f, 128.31f, nullptr},
    {645.569f, -1094.58f, 132.13f, nullptr}, {689.621f, -1068.33f, 132.87f, nullptr},
    {730.045f, -1042.67f, 133.03f, nullptr}, {755.322f, -1030.28f, 133.30f, nullptr},
    {801.685f, -1005.46f, 132.39f, nullptr}, {806.404f, -1001.709f, 132.382f, nullptr}};

BattleBotPath vPath_IC_Horde_Keep_to_Horde_Front_Crossroad = {{1128.646f, -763.221f, 48.385f, nullptr},
                                                              {1091.273f, -763.619f, 42.352f, nullptr},
                                                              {1032.825f, -763.024f, 30.420f, nullptr},
                                                              {991.4235f, -807.672f, 21.788f, nullptr}};

BattleBotPath vPath_IC_Horde_Front_Crossroad_to_Horde_Hangar_Crossroad = {{991.4235f, -807.672f, 21.788f, nullptr},
                                                                          {999.1844f, -855.182f, 21.484f, nullptr},
                                                                          {1012.089f, -923.098f, 19.296f, nullptr}};

BattleBotPath vPath_IC_Horde_Keep_to_Horde_Hangar_Crossroad = {{1128.646f, -763.221f, 48.385f, nullptr},
                                                               {1121.090f, -816.666f, 49.008f, nullptr},
                                                               {1107.106f, -851.459f, 48.804f, nullptr},
                                                               {1072.313f, -888.355f, 30.853f, nullptr},
                                                               {1012.089f, -923.098f, 19.296f, nullptr}};

BattleBotPath vPath_IC_Horde_Hangar_Crossroad_to_Hangar_Flag = {
    {1001.745f, -973.174f, 15.784f, nullptr},  {1015.437f, -1019.47f, 15.578f, nullptr},
    {1009.622f, -1067.78f, 15.777f, nullptr},  {988.0692f, -1113.32f, 18.254f, nullptr},
    {943.7221f, -1134.50f, 32.296f, nullptr},  {892.2205f, -1115.16f, 63.319f, nullptr},
    {849.6576f, -1090.88f, 91.943f, nullptr},  {814.9168f, -1056.42f, 117.275f, nullptr},
    {799.0856f, -1034.62f, 129.000f, nullptr}, {801.685f, -1005.46f, 132.39f, nullptr}};

BattleBotPath vPath_IC_Horde_Keep_to_Horde_Dock_Crossroad = {{1128.646f, -763.221f, 48.385f, nullptr},
                                                             {1116.203f, -723.328f, 48.655f, nullptr},
                                                             {1093.246f, -696.880f, 37.041f, nullptr},
                                                             {1034.226f, -653.581f, 24.432f, nullptr}};

BattleBotPath vPath_IC_Horde_Front_Crossroad_to_Horde_Dock_Crossroad = {{991.4235f, -807.672f, 21.788f, nullptr},
                                                                        {1025.305f, -757.165f, 29.241f, nullptr},
                                                                        {1029.308f, -710.366f, 26.366f, nullptr},
                                                                        {1034.226f, -653.581f, 24.432f, nullptr}};

BattleBotPath vPath_IC_Horde_Dock_Crossroad_to_Refinery_Crossroad = {{1034.226f, -653.581f, 24.432f, nullptr},
                                                                     {1102.358f, -617.505f, 5.4963f, nullptr},
                                                                     {1116.255f, -580.956f, 18.184f, nullptr},
                                                                     {1114.414f, -546.731f, 23.422f, nullptr},
                                                                     {1148.358f, -503.947f, 23.423f, nullptr}};

BattleBotPath vPath_IC_Refinery_Crossroad_to_Refinery_Base = {{1148.358f, -503.947f, 23.423f, nullptr},
                                                              {1201.885f, -500.425f, 4.7262f, nullptr},
                                                              {1240.595f, -471.971f, 0.8933f, nullptr},
                                                              {1265.993f, -435.419f, 10.669f, nullptr}};

BattleBotPath vPath_IC_Horde_Side_Gate_to_Refinery_Base = {
    {1218.676f, -660.487f, 47.870f, nullptr}, {1211.677f, -626.181f, 46.085f, nullptr},
    {1212.720f, -562.300f, 19.514f, nullptr}, {1238.803f, -538.997f, 3.9892f, nullptr},
    {1248.875f, -482.852f, 0.8933f, nullptr}, {1265.993f, -435.419f, 10.669f, nullptr}};

BattleBotPath vPath_IC_Refinery_Crossroad_to_Docks_Crossroad = {
    {1148.358f, -503.947f, 23.423f, nullptr}, {1127.010f, -469.451f, 23.422f, nullptr},
    {1100.976f, -431.146f, 21.312f, nullptr}, {1053.812f, -405.457f, 12.749f, nullptr},
    {1005.570f, -375.439f, 12.695f, nullptr}, {963.4349f, -353.282f, 12.356f, nullptr},
    {907.1394f, -380.470f, 11.912f, nullptr}};

BattleBotPath vPath_IC_Horde_Dock_Crossroad_to_Docks_Crossroad = {
    {1034.226f, -653.581f, 24.432f, nullptr}, {1013.435f, -622.066f, 24.486f, nullptr},
    {988.1990f, -547.937f, 24.424f, nullptr}, {982.4955f, -508.332f, 24.524f, nullptr},
    {982.5065f, -462.920f, 16.833f, nullptr}, {948.8842f, -421.200f, 16.877f, nullptr},
    {907.1394f, -380.470f, 11.912f, nullptr}};

BattleBotPath vPath_IC_Docks_Crossroad_to_Docks_Flag = {{907.1394f, -380.470f, 11.912f, nullptr},
                                                        {851.5726f, -382.503f, 11.906f, nullptr},
                                                        {808.1441f, -381.199f, 11.906f, nullptr},
                                                        {761.1740f, -381.854f, 14.504f, nullptr},
                                                        {726.427f, -364.849f, 17.815f, nullptr}};

BattleBotPath vPath_IC_Horde_Front_Crossroad_to_Workshop = {
    {991.4235f, -807.672f, 21.788f, nullptr}, {944.5518f, -800.344f, 13.155f, nullptr},
    {907.1300f, -798.892f, 8.3237f, nullptr}, {842.9721f, -795.224f, 5.2007f, nullptr},
    {804.5959f, -794.269f, 5.9836f, nullptr}, {774.466f, -801.058f, 6.3428f, nullptr}};

BattleBotPath vPath_IC_Central_Graveyard_to_Workshop = {
    {775.377f, -664.151f, 8.388f, nullptr}, {776.299f, -684.079f, 5.036f, nullptr},
    {777.451f, -707.525f, 0.051f, nullptr}, {779.059f, -734.611f, 1.695f, nullptr},
    {779.643f, -767.010f, 4.843f, nullptr}, {774.466f, -801.058f, 6.3428f, nullptr}};

BattleBotPath vPath_IC_Horde_East_Gate_to_Horde_Keep = {
    {1216.1918f, -864.922f, 48.852f, nullptr}, {1197.3117f, -866.054f, 48.916f, nullptr},
    {1174.195f, -867.931f, 48.621f, nullptr},  {1149.671f, -869.240f, 48.096f, nullptr},
    {1128.257f, -860.087f, 49.562f, nullptr},  {1118.730f, -829.959f, 49.074f, nullptr},
    {1123.201f, -806.498f, 48.896f, nullptr},  {1129.685f, -787.156f, 48.680f, nullptr},
    {1128.646f, -763.221f, 48.385f, nullptr}};

BattleBotPath vPath_IC_Workshop_to_Workshop_Keep = {{773.792f, -825.637f, 8.127f, nullptr},
                                                    {772.706f, -841.881f, 11.622f, nullptr},
                                                    {773.057f, -859.936f, 12.418f, nullptr}};

BattleBotPath vPath_IC_Alliance_Base = {
    {402.020f, -832.289f, 48.627f, nullptr}, {384.784f, -832.551f, 48.830f, nullptr},
    {369.413f, -832.480f, 48.916f, nullptr}, {346.677f, -832.355f, 48.916f, nullptr},
    {326.296f, -832.189f, 48.916f, nullptr}, {311.174f, -832.204f, 48.916f, nullptr},
};

BattleBotPath vPath_IC_Horde_Base = {
    {1158.652f, -762.680f, 48.628f, nullptr}, {1171.598f, -762.628f, 48.649f, nullptr},
    {1189.102f, -763.484f, 48.915f, nullptr}, {1208.599f, -764.332f, 48.915f, nullptr},
    {1227.592f, -764.782f, 48.915f, nullptr}, {1253.676f, -765.441f, 48.915f, nullptr},
};

BattleBotPath vPath_IC_Workshop_to_North_West = {
    {786.051f, -801.798f, 5.968f, nullptr},   {801.767f, -800.845f, 6.201f, nullptr},
    {830.190f, -800.059f, 5.163f, nullptr},   {858.827f, -797.691f, 5.602f, nullptr},
    {881.579f, -795.190f, 6.441f, nullptr},   {905.796f, -792.252f, 8.008f, nullptr},
    {929.874f, -788.912f, 10.674f, nullptr},  {960.001f, -784.233f, 15.521f, nullptr},
    {991.890f, -780.605f, 22.402f, nullptr},  {1014.062f, -761.863f, 28.672f, nullptr},
    {1019.925f, -730.822f, 27.421f, nullptr}, {1022.320f, -698.581f, 25.993f, nullptr},
    {1022.539f, -665.405f, 24.574f, nullptr}, {1016.279f, -632.780f, 24.487f, nullptr},
    {1004.839f, -602.680f, 24.501f, nullptr}, {992.826f, -567.833f, 24.558f, nullptr},
    {984.998f, -535.303f, 24.485f, nullptr},
};

BattleBotPath vPath_IC_South_West_Crossroads = {
    {528.932f, -667.953f, 25.413f, nullptr}, {514.800f, -650.381f, 26.171f, nullptr},
    {488.367f, -621.869f, 25.820f, nullptr}, {479.491f, -594.284f, 26.095f, nullptr},
    {498.094f, -557.031f, 26.015f, nullptr}, {528.272f, -528.761f, 26.015f, nullptr},
    {595.746f, -480.009f, 26.007f, nullptr}, {632.156f, -458.182f, 27.416f, nullptr},
    {656.013f, -446.685f, 28.003f, nullptr},
};

BattleBotPath vPath_IC_Hanger_to_Workshop = {
    {808.923f, -1003.441f, 132.380f, nullptr}, {804.031f, -1002.785f, 132.382f, nullptr},
    {798.466f, -1021.472f, 132.292f, nullptr}, {795.472f, -1031.693f, 130.232f, nullptr},
    {804.631f, -1042.672f, 125.310f, nullptr}, {827.549f, -1061.778f, 111.276f, nullptr},
    {847.424f, -1077.930f, 98.061f, nullptr},  {868.225f, -1091.563f, 83.794f, nullptr},
    {894.100f, -1104.828f, 66.570f, nullptr},  {923.615f, -1117.689f, 47.391f, nullptr},
    {954.614f, -1119.716f, 27.880f, nullptr},  {970.397f, -1116.281f, 22.124f, nullptr},
    {985.906f, -1105.714f, 18.572f, nullptr},  {996.308f, -1090.605f, 17.669f, nullptr},
    {1003.693f, -1072.916f, 16.583f, nullptr}, {1008.660f, -1051.310f, 15.970f, nullptr},
    {1008.437f, -1026.678f, 15.623f, nullptr}, {1000.103f, -999.047f, 16.487f, nullptr},
    {988.412f, -971.096f, 17.796f, nullptr},   {971.503f, -945.742f, 14.438f, nullptr},
    {947.420f, -931.306f, 13.209f, nullptr},   {922.920f, -916.960f, 10.859f, nullptr},
    {901.607f, -902.203f, 9.854f, nullptr},    {881.932f, -882.226f, 7.848f, nullptr},
    {861.795f, -862.477f, 6.731f, nullptr},    {844.186f, -845.952f, 6.192f, nullptr},
    {826.565f, -826.551f, 5.171f, nullptr},    {809.497f, -815.351f, 6.179f, nullptr},
    {790.787f, -809.678f, 6.450f, nullptr},
};

std::vector<BattleBotPath*> const vPaths_WS = {
    &vPath_WSG_HordeFlagRoom_to_HordeGraveyard,
    &vPath_WSG_HordeGraveyard_to_HordeTunnel,
    &vPath_WSG_HordeTunnel_to_HordeFlagRoom,
    &vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard,
    &vPath_WSG_AllianceGraveyard_to_AllianceTunnel,
    &vPath_WSG_AllianceTunnel_to_AllianceFlagRoom,
    &vPath_WSG_HordeTunnel_to_HordeBaseRoof,
    &vPath_WSG_AllianceTunnel_to_AllianceBaseRoof,
    &vPath_WSG_AllianceTunnel_to_HordeTunnel,
    &vPath_WSG_AllianceGraveyardLower_to_HordeFlagRoom,
    &vPath_WSG_HordeGraveyardLower_to_AllianceFlagRoom,
    &vPath_WSG_AllianceGraveyardJump,
    &vPath_WSG_HordeGraveyardJump
};

std::vector<BattleBotPath*> const vPaths_AB = {
    &vPath_AB_AllianceBase_to_Stables,  &vPath_AB_AllianceBase_to_GoldMine, &vPath_AB_AllianceBase_to_LumberMill,
    &vPath_AB_Stables_to_Blacksmith,    &vPath_AB_HordeBase_to_Farm,        &vPath_AB_HordeBase_to_GoldMine,
    &vPath_AB_HordeBase_to_LumberMill,  &vPath_AB_Farm_to_Blacksmith,       &vPath_AB_Stables_to_GoldMine,
    &vPath_AB_Stables_to_LumberMill,    &vPath_AB_Farm_to_GoldMine,         &vPath_AB_Farm_to_LumberMill,
    &vPath_AB_Blacksmith_to_LumberMill, &vPath_AB_Blacksmith_to_GoldMine,   &vPath_AB_Farm_to_Stable,
};

std::vector<BattleBotPath*> const vPaths_AV = {
    &vPath_AV_AllianceSpawn_To_AllianceCrossroad1,
    &vPath_AV_AllianceFortress_To_AllianceCrossroad1,
    &vPath_AV_AllianceCrossroad1_To_AllianceCrossroad2,
    &vPath_AV_StoneheartGrave_To_AllianceCrossroad2,
    &vPath_AV_AllianceCrossroad2_To_StoneheartBunker,
    &vPath_AV_AllianceCrossroad2_To_AllianceCaptain,
    &vPath_AV_AllianceCaptain_To_HordeCrossroad3,
    &vPath_AV_AllianceCrossroads3_To_SnowfallGraveyard,
    //&vPath_AV_AllianceCaptain_To_AllianceCrossroad3,
    &vPath_AV_StoneheartBunker_To_HordeCrossroad3,
    &vPath_AV_AllianceCrossroad1_To_AllianceMine,
    &vPath_AV_HordeSpawn_To_MainRoad,
    &vPath_AV_SnowfallGraveyard_To_HordeCaptain,
    &vPath_AV_HordeCrossroad3_To_IcebloodTower,
    &vPath_AV_IcebloodTower_To_HordeCaptain,
    &vPath_AV_IcebloodTower_To_IcebloodGrave,
    &vPath_AV_IcebloodGrave_To_TowerBottom,
    &vPath_AV_TowerBottom_To_HordeCrossroad1,
    &vPath_AV_HordeCrossroad1_To_FrostwolfGrave,
    &vPath_AV_HordeCrossroad1_To_HordeFortress,
    &vPath_AV_FrostwolfGrave_To_HordeMine
};

std::vector<BattleBotPath*> const vPaths_EY = {
    &vPath_EY_Horde_Spawn_to_Crossroad1Horde,
    &vPath_EY_Horde_Crossroad1Horde_to_Crossroad2Horde,
    &vPath_EY_Crossroad1Horde_to_Blood_Elf_Tower,
    &vPath_EY_Crossroad1Horde_to_Fel_Reaver_Ruins,
    &vPath_EY_Crossroad2Horde_to_Blood_Elf_Tower,
    &vPath_EY_Crossroad2Horde_to_Fel_Reaver_Ruins,
    &vPath_EY_Crossroad2Horde_to_Flag,
    &vPath_EY_Alliance_Spawn_to_Crossroad1Alliance,
    &vPath_EY_Alliance_Crossroad1Alliance_to_Crossroad2Alliance,
    &vPath_EY_Crossroad1Alliance_to_Mage_Tower,
    &vPath_EY_Crossroad1Alliance_to_Draenei_Ruins,
    &vPath_EY_Crossroad2Alliance_to_Mage_Tower,
    &vPath_EY_Crossroad2Alliance_to_Draenei_Ruins,
    &vPath_EY_Crossroad2Alliance_to_Flag,
    &vPath_EY_Draenei_Ruins_to_Blood_Elf_Tower,
    &vPath_EY_Fel_Reaver_to_Mage_Tower,
};

std::vector<BattleBotPath*> const vPaths_IC = {
    &vPath_IC_Ally_Dock_Crossroad_to_Ally_Docks_Second_Crossroad,
    &vPath_IC_Ally_Docks_Crossroad_to_Docks_Flag,
    &vPath_IC_Ally_Docks_Second_Crossroad_to_Ally_Docks_Crossroad,
    &vPath_IC_Lower_Graveyard_Crossroad_to_Ally_Docks_Crossroad,
    &vPath_IC_Lower_Graveyard_Crossroad_to_Ally_Docks_Second_Crossroad,
    &vPath_IC_Lower_Graveyard_to_Lower_Graveyard_Crossroad,
    &vPath_IC_Ally_Front_Crossroad_to_Ally_Dock_Crossroad,
    &vPath_IC_Ally_Front_Crossroad_to_Hangar_First_Crossroad,
    &vPath_IC_Ally_Front_Crossroad_to_Workshop,
    &vPath_IC_Ally_Keep_to_Ally_Dock_Crossroad,
    &vPath_IC_Ally_Keep_to_Ally_Front_Crossroad,
    &vPath_IC_Ally_Keep_to_Hangar_First_Crossroad,
    &vPath_IC_Ally_Keep_to_Quarry_Crossroad,
    &vPath_IC_Docks_Crossroad_to_Docks_Flag,
    &vPath_IC_Docks_Graveyard_to_Docks_Flag,
    &vPath_IC_Hangar_First_Crossroad_to_Hangar_Second_Crossroad,
    &vPath_IC_Hangar_Second_Crossroad_to_Hangar_Flag,
    &vPath_IC_Horde_Dock_Crossroad_to_Docks_Crossroad,
    &vPath_IC_Horde_Dock_Crossroad_to_Refinery_Crossroad,
    &vPath_IC_Horde_Front_Crossroad_to_Horde_Dock_Crossroad,
    &vPath_IC_Horde_Front_Crossroad_to_Horde_Hangar_Crossroad,
    &vPath_IC_Horde_Front_Crossroad_to_Workshop,
    &vPath_IC_Horde_Hangar_Crossroad_to_Hangar_Flag,
    &vPath_IC_Horde_Keep_to_Horde_Dock_Crossroad,
    &vPath_IC_Horde_Keep_to_Horde_Front_Crossroad,
    &vPath_IC_Horde_Keep_to_Horde_Hangar_Crossroad,
    &vPath_IC_Horde_Side_Gate_to_Refinery_Base,
    &vPath_IC_Quarry_Crossroad_to_Hangar_Second_Crossroad,
    &vPath_IC_Quarry_Crossroad_to_Quarry_Flag,
    &vPath_IC_Refinery_Crossroad_to_Docks_Crossroad,
    &vPath_IC_Refinery_Crossroad_to_Refinery_Base,
    &vPath_IC_Central_Graveyard_to_Workshop,
    &vPath_IC_Horde_East_Gate_to_Horde_Keep,
    &vPath_IC_Workshop_to_Workshop_Keep,
    &vPath_IC_Alliance_Base,
    &vPath_IC_Horde_Base,
    &vPath_IC_Workshop_to_North_West,
    &vPath_IC_South_West_Crossroads,
    &vPath_IC_Hanger_to_Workshop,
};

std::vector<BattleBotPath*> const vPaths_NoReverseAllowed = {
    &vPath_WSG_AllianceGraveyardJump,
    &vPath_WSG_HordeGraveyardJump,
    &vPath_IC_Central_Graveyard_to_Workshop,
    &vPath_IC_Docks_Graveyard_to_Docks_Flag,
};

static std::vector<std::pair<uint8, uint32>> AV_AttackObjectives_Horde = {
    // Attack - these are in order they should be attacked
    {BG_AV_NODES_STONEHEART_GRAVE, BG_AV_OBJECT_FLAG_A_STONEHEART_GRAVE},
    {BG_AV_NODES_STONEHEART_BUNKER, BG_AV_OBJECT_FLAG_A_STONEHEART_BUNKER},
    {BG_AV_NODES_ICEWING_BUNKER, BG_AV_OBJECT_FLAG_A_ICEWING_BUNKER},
    {BG_AV_NODES_STORMPIKE_GRAVE, BG_AV_OBJECT_FLAG_A_STORMPIKE_GRAVE},
    {BG_AV_NODES_DUNBALDAR_SOUTH, BG_AV_OBJECT_FLAG_A_DUNBALDAR_SOUTH},
    {BG_AV_NODES_DUNBALDAR_NORTH, BG_AV_OBJECT_FLAG_A_DUNBALDAR_NORTH},
    {BG_AV_NODES_FIRSTAID_STATION, BG_AV_OBJECT_FLAG_A_FIRSTAID_STATION},
};

static std::vector<std::pair<uint8, uint32>> AV_AttackObjectives_Alliance = {
    // Attack - these are in order they should be attacked
    {BG_AV_NODES_ICEBLOOD_GRAVE, BG_AV_OBJECT_FLAG_H_ICEBLOOD_GRAVE},
    {BG_AV_NODES_ICEBLOOD_TOWER, BG_AV_OBJECT_FLAG_H_ICEBLOOD_TOWER},
    {BG_AV_NODES_TOWER_POINT, BG_AV_OBJECT_FLAG_H_TOWER_POINT},
    {BG_AV_NODES_FROSTWOLF_GRAVE, BG_AV_OBJECT_FLAG_H_FROSTWOLF_GRAVE},
    {BG_AV_NODES_FROSTWOLF_ETOWER, BG_AV_OBJECT_FLAG_H_FROSTWOLF_ETOWER},
    {BG_AV_NODES_FROSTWOLF_WTOWER, BG_AV_OBJECT_FLAG_H_FROSTWOLF_WTOWER},
    {BG_AV_NODES_FROSTWOLF_HUT, BG_AV_OBJECT_FLAG_H_FROSTWOLF_HUT},
};

static std::vector<std::pair<uint8, uint32>> AV_DefendObjectives_Horde = {
    {BG_AV_NODES_FROSTWOLF_HUT, BG_AV_OBJECT_FLAG_H_FROSTWOLF_HUT},
    {BG_AV_NODES_FROSTWOLF_WTOWER, BG_AV_OBJECT_FLAG_H_FROSTWOLF_WTOWER},
    {BG_AV_NODES_FROSTWOLF_ETOWER, BG_AV_OBJECT_FLAG_H_FROSTWOLF_ETOWER},
    {BG_AV_NODES_FROSTWOLF_GRAVE, BG_AV_OBJECT_FLAG_H_FROSTWOLF_GRAVE},
    {BG_AV_NODES_TOWER_POINT, BG_AV_OBJECT_FLAG_H_TOWER_POINT},
    {BG_AV_NODES_ICEBLOOD_TOWER, BG_AV_OBJECT_FLAG_H_ICEBLOOD_TOWER},
    {BG_AV_NODES_ICEBLOOD_GRAVE, BG_AV_OBJECT_FLAG_H_ICEBLOOD_GRAVE},
};

static std::vector<std::pair<uint8, uint32>> AV_DefendObjectives_Alliance = {
    {BG_AV_NODES_FIRSTAID_STATION, BG_AV_OBJECT_FLAG_A_FIRSTAID_STATION},
    {BG_AV_NODES_DUNBALDAR_SOUTH, BG_AV_OBJECT_FLAG_A_DUNBALDAR_SOUTH},
    {BG_AV_NODES_DUNBALDAR_NORTH, BG_AV_OBJECT_FLAG_A_DUNBALDAR_NORTH},
    {BG_AV_NODES_STORMPIKE_GRAVE, BG_AV_OBJECT_FLAG_A_STORMPIKE_GRAVE},
    {BG_AV_NODES_ICEWING_BUNKER, BG_AV_OBJECT_FLAG_A_ICEWING_BUNKER},
    {BG_AV_NODES_STONEHEART_BUNKER, BG_AV_OBJECT_FLAG_A_STONEHEART_BUNKER},
    {BG_AV_NODES_STONEHEART_GRAVE, BG_AV_OBJECT_FLAG_A_STONEHEART_GRAVE},
};

static uint32 AB_AttackObjectives[] = {
    BG_AB_NODE_STABLES,
    BG_AB_NODE_BLACKSMITH,
    BG_AB_NODE_FARM, BG_AB_NODE_LUMBER_MILL,
    BG_AB_NODE_GOLD_MINE
};

static std::tuple<uint32, uint32, uint32> EY_AttackObjectives[] = {
    {POINT_FEL_REAVER, BG_EY_OBJECT_FLAG_FEL_REAVER, AT_FEL_REAVER_POINT},
    {POINT_BLOOD_ELF, BG_EY_OBJECT_FLAG_BLOOD_ELF, AT_BLOOD_ELF_POINT},
    {POINT_DRAENEI_RUINS, BG_EY_OBJECT_FLAG_DRAENEI_RUINS, AT_DRAENEI_RUINS_POINT},
    {POINT_MAGE_TOWER, BG_EY_OBJECT_FLAG_MAGE_TOWER, AT_MAGE_TOWER_POINT}
};

static std::unordered_map<uint32, Position> EY_NodePositions = {
    {POINT_FEL_REAVER, Position(2044.173f, 1727.503f, 1189.505f)},
    {POINT_BLOOD_ELF, Position(2048.277f, 1395.093f, 1194.255f)},
    {POINT_DRAENEI_RUINS, Position(2286.245f, 1404.683f, 1196.991f)},
    {POINT_MAGE_TOWER, Position(2284.720f, 1728.457f, 1189.153f)}
};

static std::pair<uint32, uint32> IC_AttackObjectives[] = {
    {NODE_TYPE_WORKSHOP, BG_IC_GO_WORKSHOP_BANNER},
    {NODE_TYPE_DOCKS, BG_IC_GO_DOCKS_BANNER},
    {NODE_TYPE_HANGAR, BG_IC_GO_HANGAR_BANNER},
};

// useful commands for fixing BG bugs and checking waypoints/paths
bool BGTactics::HandleConsoleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig->enabled)
    {
        handler->PSendSysMessage("|cffff0000Playerbot system is currently disabled!");
        return true;
    }
    WorldSession* session = handler->GetSession();
    if (!session)
    {
        handler->PSendSysMessage("Command can only be used from an active session");
        return true;
    }
    std::string const commandOutput = HandleConsoleCommandPrivate(session, args);
    if (!commandOutput.empty())
        handler->PSendSysMessage(commandOutput.c_str());
    return true;
}

// has different name to above as some compilers get confused
std::string const BGTactics::HandleConsoleCommandPrivate(WorldSession* session, char const* args)
{
    Player* player = session->GetPlayer();
    if (!player)
        return "Error - session player not found";
    if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
        return "Command can only be used by a GM";
    Battleground* bg = player->GetBattleground();
    if (!bg)
        return "Command can only be used within a battleground";
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    char* cmd = strtok((char*)args, " ");
    // char* charname = strtok(nullptr, " ");

    if (!strncmp(cmd, "showpath", 8))
    {
        int num = -1;
        if (!strncmp(cmd, "showpath=all", 12))
        {
            num = -2;
        }
        else if (!strncmp(cmd, "showpath=", 9))
        {
            if (sscanf(cmd, "showpath=%d", &num) == -1 || num < 0)
                return "Bad showpath parameter";
        }
        std::vector<BattleBotPath*> const* vPaths;
        switch (bgType)
        {
            case BATTLEGROUND_AB:
                vPaths = &vPaths_AB;
                break;
            case BATTLEGROUND_AV:
                vPaths = &vPaths_AV;
                break;
            case BATTLEGROUND_WS:
                vPaths = &vPaths_WS;
                break;
            case BATTLEGROUND_EY:
                vPaths = &vPaths_EY;
                break;
            case BATTLEGROUND_IC:
                vPaths = &vPaths_IC;
                break;
            default:
                vPaths = nullptr;
                break;
        }
        if (!vPaths)
            return "This battleground has no paths and is unsupported";
        if (num == -1)
        {
            float closestPoint = FLT_MAX;
            for (uint32 j = 0; j < vPaths->size(); j++)
            {
                auto const& path = (*vPaths)[j];
                for (uint32 i = 0; i < path->size(); i++)
                {
                    BattleBotWaypoint& waypoint = ((*path)[i]);
                    float dist = player->GetDistance(waypoint.x, waypoint.y, waypoint.z);
                    if (closestPoint > dist)
                    {
                        closestPoint = dist;
                        num = j;
                    }
                }
            }
        }
        uint32 min = 0u;
        uint32 max = vPaths->size() - 1;
        if (num >= 0)  // num specified or found
        {
            if (num > max)
                return fmt::format("Path {} of range of 0 - {}", num, max);
            min = num;
            max = num;
        }
        for (uint32 j = min; j <= max; j++)
        {
            auto const& path = (*vPaths)[j];
            for (uint32 i = 0; i < path->size(); i++)
            {
                BattleBotWaypoint& waypoint = ((*path)[i]);
                Creature* wpCreature = player->SummonCreature(15631, waypoint.x, waypoint.y, waypoint.z, 0,
                                                              TEMPSUMMON_TIMED_DESPAWN, 15000u);
                wpCreature->SetOwnerGUID(player->GetGUID());
            }
        }
        if (num >= 0)
            return fmt::format("Showing path {}", num);
        return fmt::format("Showing paths 0 - {}", max);
    }

    if (!strncmp(cmd, "showcreature=", 13))
    {
        uint32 num;
        if (sscanf(cmd, "showcreature=%u", &num) == -1)
            return "Bad showcreature parameter";
        if (num >= bg->BgCreatures.size())
            return fmt::format("Creature out of range of 0 - {}", bg->BgCreatures.size() - 1);
        Creature* c = bg->GetBGCreature(num);
        if (!c)
            return "Creature not found";
        Creature* wpCreature = player->SummonCreature(15631, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), 0,
                                                      TEMPSUMMON_TIMED_DESPAWN, 15000u);
        wpCreature->SetOwnerGUID(player->GetGUID());
        float distance = player->GetDistance(c);
        float exactDistance = player->GetExactDist(c);
        return fmt::format("Showing Creature {} location={:.3f},{:.3f},{:.3f} distance={} exactDistance={}",
            num, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), distance, exactDistance);
    }

    if (!strncmp(cmd, "showobject=", 11))
    {
        uint32 num;
        if (sscanf(cmd, "showobject=%u", &num) == -1)
            return "Bad showobject parameter";
        if (num >= bg->BgObjects.size())
            return fmt::format("Object out of range of 0 - {}", bg->BgObjects.size() - 1);
        GameObject* o = bg->GetBGObject(num);
        if (!o)
            return "GameObject not found";
        Creature* wpCreature = player->SummonCreature(15631, o->GetPositionX(), o->GetPositionY(), o->GetPositionZ(), 0,
                                                      TEMPSUMMON_TIMED_DESPAWN, 15000u);
        wpCreature->SetOwnerGUID(player->GetGUID());
        float distance = player->GetDistance(o);
        float exactDistance = player->GetExactDist(o);
        return fmt::format("Showing GameObject {} location={:.3f},{:.3f},{:.3f} distance={} exactDistance={}",
            num, o->GetPositionX(), o->GetPositionY(), o->GetPositionZ(), distance, exactDistance);
    }

    return "usage: showpath(=[num]) / showcreature=[num] / showobject=[num]";
}

// Depends on OnBattlegroundStart in playerbots.cpp
uint8 BGTactics::GetBotStrategyForTeam(Battleground* bg, TeamId teamId)
{
    auto itr = bgStrategies.find(bg->GetInstanceID());
    if (itr == bgStrategies.end())
        return 0;

    return teamId == TEAM_ALLIANCE ? itr->second.allianceStrategy : itr->second.hordeStrategy;
}

bool BGTactics::wsJumpDown()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    ArenaOpenerInfo opener = AI_VALUE(ArenaOpenerInfo, "arena opener info");
    if (opener.active)
        botAI->ChangeStrategy("arena opener", BOT_STATE_NON_COMBAT);
    else
        botAI->ChangeStrategy("-arena opener", BOT_STATE_NON_COMBAT);

    TeamId team = bot->GetTeamId();
    uint32 mapId = bg->GetMapId();

    if (team == TEAM_HORDE)
    {
        if (bot->GetDistance({1038.220f, 1420.197f, 340.099f}) < 4.0f)
        {
            MoveTo(mapId, 1029.242f, 1387.024f, 340.866f);
            return true;
        }
        if (bot->GetDistance({1029.242f, 1387.024f, 340.866f}) < 4.0f)
        {
            MoveTo(mapId, 1045.764f, 1389.831f, 340.825f);
            return true;
        }
        if (bot->GetDistance({1045.764f, 1389.831f, 340.825f}) < 4.0f)
        {
            MoveTo(mapId, 1057.076f, 1393.081f, 339.505f);
            return true;
        }
        if (bot->GetDistance({1057.076f, 1393.081f, 339.505f}) < 4.0f)
        {
            JumpTo(mapId, 1075.233f, 1398.645f, 323.669f);
            return true;
        }
        if (bot->GetDistance({1075.233f, 1398.645f, 323.669f}) < 4.0f)
        {
            MoveTo(mapId, 1096.590f, 1395.070f, 317.016f);
            return true;
        }
        if (bot->GetDistance({1096.590f, 1395.070f, 317.016f}) < 4.0f)
        {
            MoveTo(mapId, 1134.380f, 1370.130f, 312.741f);
            return true;
        }
    }
    else if (team == TEAM_ALLIANCE)
    {
        if (bot->GetDistance({1415.548f, 1554.538f, 343.164f}) < 4.0f)
        {
            MoveTo(mapId, 1407.234f, 1551.658f, 343.432f);
            return true;
        }
        if (bot->GetDistance({1407.234f, 1551.658f, 343.432f}) < 4.0f)
        {
            JumpTo(mapId, 1385.325f, 1544.592f, 322.047f);
            return true;
        }
        if (bot->GetDistance({1385.325f, 1544.592f, 322.047f}) < 4.0f)
        {
            MoveTo(mapId, 1370.710f, 1543.550f, 321.585f);
            return true;
        }
        if (bot->GetDistance({1370.710f, 1543.550f, 321.585f}) < 4.0f)
        {
            MoveTo(mapId, 1339.410f, 1533.420f, 313.336f);
            return true;
        }
    }

    return false;
}

bool BGTactics::eyJumpDown()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;
    Position const hordeJumpPositions[] = {
        EY_WAITING_POS_HORDE,
        {1838.007f, 1539.856f, 1253.383f},
        {1846.264f, 1535.062f, 1240.796f},
        {1849.813f, 1527.303f, 1237.262f},
        {1849.041f, 1518.884f, 1223.624f},
    };
    Position const allianceJumpPositions[] = {
        EY_WAITING_POS_ALLIANCE,
        {2492.955f, 1597.769f, 1254.828f},
        {2484.601f, 1598.209f, 1244.344f},
        {2478.424f, 1609.539f, 1238.651f},
        {2475.926f, 1619.658f, 1218.706f},
    };
    Position const* positons = bot->GetTeamId() == TEAM_HORDE ? hordeJumpPositions : allianceJumpPositions;
    {
        if (bot->GetDistance(positons[0]) < 16.0f)
        {
            MoveTo(bg->GetMapId(), positons[1].GetPositionX(), positons[1].GetPositionY(), positons[1].GetPositionZ());
            return true;
        }
        if (bot->GetDistance(positons[1]) < 4.0f)
        {
            JumpTo(bg->GetMapId(), positons[2].GetPositionX(), positons[2].GetPositionY(), positons[2].GetPositionZ());
            return true;
        }
        if (bot->GetDistance(positons[2]) < 4.0f)
        {
            MoveTo(bg->GetMapId(), positons[3].GetPositionX(), positons[3].GetPositionY(), positons[3].GetPositionZ());
            return true;
        }
        if (bot->GetDistance(positons[3]) < 4.0f)
        {
            JumpTo(bg->GetMapId(), positons[4].GetPositionX(), positons[4].GetPositionY(), positons[4].GetPositionZ());
            return true;
        }
    }
    return false;
}

//
// actual bg tactics below
//
bool BGTactics::Execute(Event event)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
    {
        botAI->ResetStrategies();
        return false;
    }

    if (bg->GetStatus() == STATUS_WAIT_LEAVE)
        return BGStatusAction::LeaveBG(botAI);

    if (bg->isArena())
    {
        // can't use this in arena - no vPaths/vFlagIds (will crash server)
        botAI->ResetStrategies();
        return false;
    }

    if (bg->GetStatus() == STATUS_IN_PROGRESS)
        botAI->ChangeStrategy("-buff", BOT_STATE_NON_COMBAT);

    std::vector<BattleBotPath*> const* vPaths;
    std::vector<uint32> const* vFlagIds;
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bot->GetBattleground()->GetBgTypeID(true);

    switch (bgType)
    {
        case BATTLEGROUND_AB:
        {
            vPaths = &vPaths_AB;
            vFlagIds = &vFlagsAB;
            break;
        }
        case BATTLEGROUND_AV:
        {
            vPaths = &vPaths_AV;
            vFlagIds = &vFlagsAV;
            break;
        }
        case BATTLEGROUND_WS:
        {
            vPaths = &vPaths_WS;
            vFlagIds = &vFlagsWS;
            break;
        }
        case BATTLEGROUND_EY:
        {
            vPaths = &vPaths_EY;
            vFlagIds = &vFlagsEY;
            break;
        }
        case BATTLEGROUND_IC:
        {
            vPaths = &vPaths_IC;
            vFlagIds = &vFlagsIC;
            break;
        }
        default:
            // can't use this in this BG - no vPaths/vFlagIds (will crash server)
            botAI->ResetStrategies();
            return false;
    }

    if (getName() == "move to start")
        return moveToStart();

    // =============================================
    // REACTIVE DEFENSE SYSTEM - STATE TRACKING
    // =============================================
    // Only track state here. Decisions are made in selectObjective()
    if (bg && bg->GetStatus() == STATUS_IN_PROGRESS && !bg->isArena())
    {
        // Safe thread-safe update called here, throttling handled inside
        UpdateNodeStates(bg);
    }

    if (getName() == "reset objective force")
    {
        bool isCarryingFlag =
            bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) ||
            bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
            bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL);

        if (!isCarryingFlag)
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            return resetObjective();  // Reset objective to not use "old" data
        }
    }

    if (getName() == "select objective")
        return selectObjective();

    if (getName() == "protect fc")
    {
        if (protectFC())
            return true;
    }

    if (getName() == "move to objective")
    {
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
            return false;

        if (bot->isMoving())
            return false;

        if (!bot->IsStopped())
            return false;

        switch (bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            // TODO: should ESCORT_MOTION_TYPE be here seeing as bots use it by default?
            case IDLE_MOTION_TYPE:
            case CHASE_MOTION_TYPE:
            case POINT_MOTION_TYPE:
                break;
            default:
                return true;
        }

        if (vFlagIds && vPaths && atFlag(*vPaths, *vFlagIds))
            return true;

        // Don't pick up buffs during opening rush (distraction)
        if (!IsGameOpening(bg) && useBuff())
            return true;

        // NOTE: can't use IsInCombat() when in vehicle as player is stuck in combat forever while in vehicle (ac bug?)
        bool inCombat = bot->GetVehicle() ? (bool)AI_VALUE(Unit*, "enemy player target") : bot->IsInCombat();
        bool isCarryingFlag = PlayerHasFlag::IsCapturingFlag(bot);
        
        if (inCombat && !isCarryingFlag)
        {
            // bot->GetMotionMaster()->MovementExpired();
            return false;
        }
        
        // Flag carriers: prioritize movement over combat
        // Only fight if being melee attacked and can't escape
        if (isCarryingFlag && inCombat)
        {
            Unit* attacker = AI_VALUE(Unit*, "current target");
            if (attacker && attacker->IsAlive() && bot->GetDistance(attacker) > 5.0f)
            {
                // Attacker is ranged - keep running, don't stop to fight
                // Safety check: only call AttackStop if not in vehicle (prevents crash)
                if (!bot->GetVehicle())
                    bot->AttackStop();
                // Continue movement to objective
            }
            // If melee attacker in range, combat is unavoidable - let normal targeting handle it
        }

        if (!moveToObjective(false))
            if (!selectObjectiveWp(*vPaths))
                return moveToObjective(true);

        // bot with flag should only move to objective
        if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
            bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
            return false;

        if (!startNewPathBegin(*vPaths))
            return moveToObjective(true);

        if (!startNewPathFree(*vPaths))
            return moveToObjective(true);
    }

    if (getName() == "use buff")
        return useBuff();

    if (getName() == "check flag")
    {
        if (vFlagIds && vPaths && atFlag(*vPaths, *vFlagIds))
            return true;
    }

    if (getName() == "check objective")
        return resetObjective();

    return false;
}

bool BGTactics::moveToStart(bool force)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    if (!force && bg->GetStatus() != STATUS_WAIT_JOIN)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_WS)
    {
        uint32 role = context->GetValue<uint32>("bg role")->Get();

        int startSpot = role < 4 ? BB_WSG_WAIT_SPOT_LEFT : role > 6 ? BB_WSG_WAIT_SPOT_RIGHT : BB_WSG_WAIT_SPOT_SPAWN;
        if (startSpot == BB_WSG_WAIT_SPOT_RIGHT)
        {
            if (bot->GetTeamId() == TEAM_HORDE)
                MoveTo(bg->GetMapId(), WS_WAITING_POS_HORDE_1.GetPositionX() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_HORDE_1.GetPositionY() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_HORDE_1.GetPositionZ());
            else
                MoveTo(bg->GetMapId(), WS_WAITING_POS_ALLIANCE_1.GetPositionX() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_ALLIANCE_1.GetPositionY() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_ALLIANCE_1.GetPositionZ());
        }
        else if (startSpot == BB_WSG_WAIT_SPOT_LEFT)
        {
            if (bot->GetTeamId() == TEAM_HORDE)
                MoveTo(bg->GetMapId(), WS_WAITING_POS_HORDE_2.GetPositionX() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_HORDE_2.GetPositionY() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_HORDE_2.GetPositionZ());
            else
                MoveTo(bg->GetMapId(), WS_WAITING_POS_ALLIANCE_2.GetPositionX() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_ALLIANCE_2.GetPositionY() + frand(-4.0f, 4.0f),
                       WS_WAITING_POS_ALLIANCE_2.GetPositionZ());
        }
        else  // BB_WSG_WAIT_SPOT_SPAWN
        {
            if (bot->GetTeamId() == TEAM_HORDE)
                MoveTo(bg->GetMapId(), WS_WAITING_POS_HORDE_3.GetPositionX() + frand(-10.0f, 10.0f),
                       WS_WAITING_POS_HORDE_3.GetPositionY() + frand(-10.0f, 10.0f),
                       WS_WAITING_POS_HORDE_3.GetPositionZ());
            else
                MoveTo(bg->GetMapId(), WS_WAITING_POS_ALLIANCE_3.GetPositionX() + frand(-10.0f, 10.0f),
                       WS_WAITING_POS_ALLIANCE_3.GetPositionY() + frand(-10.0f, 10.0f),
                       WS_WAITING_POS_ALLIANCE_3.GetPositionZ());
        }
    }
    else if (bgType == BATTLEGROUND_AB)
    {
        if (bot->GetTeamId() == TEAM_HORDE)
            MoveTo(bg->GetMapId(), AB_WAITING_POS_HORDE.GetPositionX() + frand(-7.0f, 7.0f),
                   AB_WAITING_POS_HORDE.GetPositionY() + frand(-2.0f, 2.0f), AB_WAITING_POS_HORDE.GetPositionZ());
        else
            MoveTo(bg->GetMapId(), AB_WAITING_POS_ALLIANCE.GetPositionX() + frand(-7.0f, 7.0f),
                   AB_WAITING_POS_ALLIANCE.GetPositionY() + frand(-2.0f, 2.0f), AB_WAITING_POS_ALLIANCE.GetPositionZ());
    }
    else if (bgType == BATTLEGROUND_AV)
    {
        if (bot->GetTeamId() == TEAM_HORDE)
            MoveTo(bg->GetMapId(), AV_WAITING_POS_HORDE.GetPositionX() + frand(-5.0f, 5.0f),
                   AV_WAITING_POS_HORDE.GetPositionY() + frand(-3.0f, 3.0f), AV_WAITING_POS_HORDE.GetPositionZ());
        else
            MoveTo(bg->GetMapId(), AV_WAITING_POS_ALLIANCE.GetPositionX() + frand(-5.0f, 5.0f),
                   AV_WAITING_POS_ALLIANCE.GetPositionY() + frand(-3.0f, 3.0f), AV_WAITING_POS_ALLIANCE.GetPositionZ());
    }
    else if (bgType == BATTLEGROUND_EY)
    {
        /* Disabled: Not needed here and sometimes the bots can go out of map (at least with my map files)
        if (bot->GetTeamId() == TEAM_HORDE)
        {
            MoveTo(bg->GetMapId(), EY_WAITING_POS_HORDE.GetPositionX(), EY_WAITING_POS_HORDE.GetPositionY(), EY_WAITING_POS_HORDE.GetPositionZ());
        }
        else
        {
            MoveTo(bg->GetMapId(), EY_WAITING_POS_ALLIANCE.GetPositionX(), EY_WAITING_POS_ALLIANCE.GetPositionZ(), EY_WAITING_POS_ALLIANCE.GetPositionZ());
        }
        */
    }
    else if (bgType == BATTLEGROUND_IC)
    {
        uint32 role = context->GetValue<uint32>("bg role")->Get();

        if (bot->GetTeamId() == TEAM_HORDE)
        {
            if (role == 9)  // refinery
                MoveTo(bg->GetMapId(), IC_WEST_WAITING_POS_HORDE.GetPositionX() + frand(-5.0f, 5.0f),
                       IC_WEST_WAITING_POS_HORDE.GetPositionY() + frand(-5.0f, 5.0f),
                       IC_WEST_WAITING_POS_HORDE.GetPositionZ());
            else if (role >= 3 && role < 6)  // hanger
                MoveTo(bg->GetMapId(), IC_EAST_WAITING_POS_HORDE.GetPositionX() + frand(-5.0f, 5.0f),
                       IC_EAST_WAITING_POS_HORDE.GetPositionY() + frand(-5.0f, 5.0f),
                       IC_EAST_WAITING_POS_HORDE.GetPositionZ());
            else  // everything else
                MoveTo(bg->GetMapId(), IC_WAITING_POS_HORDE.GetPositionX() + frand(-5.0f, 5.0f),
                       IC_WAITING_POS_HORDE.GetPositionY() + frand(-5.0f, 5.0f), IC_WAITING_POS_HORDE.GetPositionZ());
        }
        else
        {
            if (role < 3)  // docks
                MoveTo(
                    bg->GetMapId(), IC_WAITING_POS_ALLIANCE.GetPositionX() + frand(-5.0f, 5.0f),
                    IC_WAITING_POS_ALLIANCE.GetPositionY() + frand(-5.0f, 5.0f),
                    IC_WAITING_POS_ALLIANCE.GetPositionZ());  // dont bother using west, there's no paths to use anyway
            else if (role == 9 || (role >= 3 && role < 6))    // quarry and hanger
                MoveTo(bg->GetMapId(), IC_EAST_WAITING_POS_ALLIANCE.GetPositionX() + frand(-5.0f, 5.0f),
                       IC_EAST_WAITING_POS_ALLIANCE.GetPositionY() + frand(-5.0f, 5.0f),
                       IC_EAST_WAITING_POS_ALLIANCE.GetPositionZ());
            else  // everything else
                MoveTo(bg->GetMapId(), IC_WAITING_POS_ALLIANCE.GetPositionX() + frand(-5.0f, 5.0f),
                       IC_WAITING_POS_ALLIANCE.GetPositionY() + frand(-5.0f, 5.0f),
                       IC_WAITING_POS_ALLIANCE.GetPositionZ());
        }
    }

    return true;
}

bool BGTactics::selectObjective(bool reset)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    // WorldObject* BgObjective = nullptr; // Use member variable

    // =============================================
    // PRIORITY 0: Combat Engagement (High Priority)
    // =============================================
    // Check this BEFORE checking if position is already set. 
    // This allows us to break off from a move-to-flag command if we see an enemy.
    if (Unit* enemy = AI_VALUE(Unit*, "enemy player target"))
    {
        if (ShouldEngageInCombat(enemy))
        {
             // Engage if close enough or they are attacking us
             float dist = bot->GetDistance(enemy);
             bool isAttackingMe = (enemy->GetVictim() == bot);
             
             // If we are already near target, let CombatStrategy take over by returning false?
             // No, BGTactics normally returns false if in combat.
             // But if we are "stuck" moving to an objective, we need to override the position.
             
             if (dist < 40.0f || isAttackingMe)
             {
                 // Interrupt any channel/cast (like flag capture)
                 bot->CastStop();
                 
                 // Override current objective
                 pos.Set(enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), bot->GetMapId());
                 posMap["bg objective"] = pos;
                 LOG_DEBUG("playerbots", "BGTactics: {} interrupting objective to engage {}", bot->GetName(), enemy->GetName());
                 return true;
             }
        }
    }

    // Check if we already have a valid objective position set
    if (pos.isSet() && !reset)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    switch (bgType)
    {
        case BATTLEGROUND_AV:
        {
            BattlegroundAV* av = static_cast<BattlegroundAV*>(bg);
            TeamId team = bot->GetTeamId();
            uint8 role = context->GetValue<uint32>("bg role")->Get();
            AVBotStrategy strategyHorde = static_cast<AVBotStrategy>(GetBotStrategyForTeam(bg, TEAM_HORDE));
            AVBotStrategy strategyAlliance = static_cast<AVBotStrategy>(GetBotStrategyForTeam(bg, TEAM_ALLIANCE));
            AVBotStrategy strategy = (team == TEAM_ALLIANCE) ? strategyAlliance : strategyHorde;
            AVBotStrategy enemyStrategy = (team == TEAM_ALLIANCE) ? strategyHorde : strategyAlliance;

            uint8 defendersProhab = 4;
            bool enableMineCapture = true;
            bool enableSnowfall = true;
            uint32 elapsed = GameTime::GetGameTime().count() - bg->GetStartTime();
            uint32 myReinforcements = bg->GetTeamScore(team);
            uint32 enemyReinforcements = bg->GetTeamScore(bg->GetOtherTeamId(team));
            bool lowReinforcements = myReinforcements < 120;
            bool criticalReinforcements = myReinforcements < 60;
            bool enemyLowReinforcements = enemyReinforcements < 120;

            switch (strategy)
            {
                case AV_STRATEGY_BALANCED:
                    defendersProhab = 4;
                    break;
                case AV_STRATEGY_OFFENSIVE:
                    defendersProhab = 1;
                    enableMineCapture = false;
                    break;
                case AV_STRATEGY_DEFENSIVE:
                    defendersProhab = 9;
                    enableSnowfall = false;
                    break;
                default:
                    break;
            }

            if (enemyStrategy == AV_STRATEGY_DEFENSIVE)
                defendersProhab = 0;

            bool isDefender = role < defendersProhab;
            bool isAdvanced = !isDefender && role > 8;

            auto const& attackObjectives =
                (team == TEAM_HORDE) ? AV_AttackObjectives_Horde : AV_AttackObjectives_Alliance;
            auto const& defendObjectives =
                (team == TEAM_HORDE) ? AV_DefendObjectives_Horde : AV_DefendObjectives_Alliance;

            uint32 destroyedNodes = 0;
            for (auto const& [nodeId, _] : defendObjectives)
                if (av->GetAVNodeInfo(nodeId).State == POINT_DESTROYED)
                    destroyedNodes++;

            float botX = bot->GetPositionX();
            if (isDefender)
            {
                if ((team == TEAM_HORDE && botX >= -62.0f) || (team == TEAM_ALLIANCE && botX <= -462.0f))
                    isDefender = false;
            }

            if (isDefender && destroyedNodes > 0)
            {
                if (destroyedNodes >= 2)
                    isDefender = false;
            }

            // DYNAMIC STRATEGY OVERRIDE (AV)
            if (ShouldPlayDefensive(bg))
            {
                strategy = AV_STRATEGY_DEFENSIVE;
                isDefender = true;
            }
            else if (ShouldPlayAggressive(bg))
            {
                strategy = AV_STRATEGY_OFFENSIVE;
                isDefender = false;
            }
            if (criticalReinforcements)
            {
                strategy = AV_STRATEGY_DEFENSIVE;
                isDefender = true;
                enableSnowfall = false;
                enableMineCapture = false;
            }
            else if (lowReinforcements && !enemyLowReinforcements)
            {
                strategy = AV_STRATEGY_DEFENSIVE;
                enableMineCapture = false;
            }

            // Early rush to enemy captain
            if (!BgObjective && !isDefender && elapsed < 120)
            {
                if (av->IsCaptainAlive(team == TEAM_HORDE ? TEAM_ALLIANCE : TEAM_HORDE))
                {
                    uint32 creatureId = (team == TEAM_HORDE) ? AV_CREATURE_A_CAPTAIN : AV_CREATURE_H_CAPTAIN;
                    if (Creature* captain = bg->GetBGCreature(creatureId))
                    {
                        if (captain->IsAlive())
                            BgObjective = captain;
                    }
                }
            }

            // --- Mine Capture (rarely works, needs some improvement) ---
            if (!BgObjective && enableMineCapture && role == 0)
            {
                BG_AV_OTHER_VALUES mineType = (team == TEAM_HORDE) ? AV_SOUTH_MINE : AV_NORTH_MINE;
                if (av->GetMineOwner(mineType) != team)
                {
                    uint32 bossEntry = (team == TEAM_HORDE) ? AV_CPLACE_MINE_S_3 : AV_CPLACE_MINE_N_3;
                    Creature* mBossNeutral = bg->GetBGCreature(bossEntry);
                    const Position* minePositions[] = {(team == TEAM_HORDE) ? &AV_MINE_SOUTH_1 : &AV_MINE_NORTH_1,
                                                       (team == TEAM_HORDE) ? &AV_MINE_SOUTH_2 : &AV_MINE_NORTH_2,
                                                       (team == TEAM_HORDE) ? &AV_MINE_SOUTH_3 : &AV_MINE_NORTH_3};

                    const Position* chosen = minePositions[0];
                    float bestDist = FLT_MAX;
                    for (auto const& p : minePositions)
                    {
                        float dist = bot->GetDistance(*p);
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            chosen = p;
                        }
                    }
                    pos.Set(chosen->GetPositionX(), chosen->GetPositionY(), chosen->GetPositionZ(), bot->GetMapId());
                    posMap["bg objective"] = pos;
                    BgObjective = mBossNeutral;
                }
            }

            // --- Nearby Enemy (deterministic intercept if close) ---
            if (!BgObjective)
            {
                if (Unit* enemy = AI_VALUE(Unit*, "enemy player target"))
                {
                    if (bot->GetDistance(enemy) < 60.0f && ShouldEngageInCombat(enemy))
                    {
                        pos.Set(enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), bot->GetMapId());
                        posMap["bg objective"] = pos;
                        BgObjective = enemy;
                    }
                }
            }

            // --- Snowfall ---
            bool hasSnowfallRole = enableSnowfall && ((team == TEAM_ALLIANCE && role < 6) || (team == TEAM_HORDE && role < 5));

            if (!BgObjective && hasSnowfallRole)
            {
                const BG_AV_NodeInfo& snowfallNode = av->GetAVNodeInfo(BG_AV_NODES_SNOWFALL_GRAVE);
                if (snowfallNode.OwnerId == TEAM_NEUTRAL)
                {
                    if (GameObject* go = bg->GetBGObject(BG_AV_OBJECT_FLAG_N_SNOWFALL_GRAVE))
                    {
                        Position objPos = go->GetPosition();
                        float rx, ry, rz;
                        bot->GetRandomPoint(objPos, frand(5.0f, 15.0f), rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }

                        pos.Set(rx, ry, rz, go->GetMapId());
                        posMap["bg objective"] = pos;
                        BgObjective = go;
                    }
                }
            }

            // --- Captain ---
            if (!BgObjective)
            {
                bool shouldTargetCaptain = (elapsed < 300) || (strategy == AV_STRATEGY_OFFENSIVE) || enemyLowReinforcements;
                if (shouldTargetCaptain && av->IsCaptainAlive(team == TEAM_HORDE ? TEAM_ALLIANCE : TEAM_HORDE))
                {
                    uint32 creatureId = (team == TEAM_HORDE) ? AV_CREATURE_A_CAPTAIN : AV_CREATURE_H_CAPTAIN;
                    if (Creature* captain = bg->GetBGCreature(creatureId))
                    {
                        if (captain->IsAlive())
                        {
                            BgObjective = captain;
                        }
                    }
                }
            }

            // --- PRIORITY 0.5: Secure Nearby Nodes/Bunkers (Any role) ---
            if (!BgObjective)
            {
                // Check for attackable objectives (Bunkers/Graveyards) nearby
                for (auto const& [nodeId, goId] : attackObjectives)
                {
                    const BG_AV_NodeInfo& node = av->GetAVNodeInfo(nodeId);
                    if (node.State == POINT_DESTROYED || node.State == POINT_ASSAULTED)
                        continue;

                    GameObject* go = bg->GetBGObject(goId);
                    if (go && go->IsWithinDist3d(bot, 20.0f))
                    {
                        LOG_DEBUG("playerbots", "BGTactics: {} prioritizing nearby AV attack objective: {}", bot->GetName(), nodeId);
                        BgObjective = go;
                        
                        // Force precise movement
                        float rx, ry, rz;
                        Position objPos = BgObjective->GetPosition();
                        bot->GetRandomPoint(objPos, frand(3.0f, 5.0f), rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }
                        pos.Set(rx, ry, rz, BgObjective->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
                
                // Check for defenses (Recap captured bunkers/graveyards)
                for (auto const& [nodeId, goId] : defendObjectives)
                {
                    const BG_AV_NodeInfo& node = av->GetAVNodeInfo(nodeId);
                    if (node.State != POINT_ASSAULTED) // We only recap if it is currently assaulted by enemy
                        continue;

                    GameObject* go = bg->GetBGObject(goId);
                    if (go && go->IsWithinDist3d(bot, 20.0f))
                    {
                        LOG_DEBUG("playerbots", "BGTactics: {} prioritizing nearby AV defend objective: {}", bot->GetName(), nodeId);
                        BgObjective = go;
                        
                        // Force precise movement
                        float rx, ry, rz;
                        Position objPos = BgObjective->GetPosition();
                        bot->GetRandomPoint(objPos, frand(3.0f, 5.0f), rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }
                        pos.Set(rx, ry, rz, BgObjective->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
            }

            // --- Defender Logic ---
            if (!BgObjective && isDefender)
            {
                float bestScore = -99999.0f;
                GameObject* bestGo = nullptr;

                for (auto const& [nodeId, goId] : defendObjectives)
                {
                    const BG_AV_NodeInfo& node = av->GetAVNodeInfo(nodeId);
                    if (node.State == POINT_DESTROYED)
                        continue;

                    GameObject* go = bg->GetBGObject(goId);
                    if (!go)
                        continue;

                    Position nodePos = go->GetPosition();
                    uint8 enemies = CountEnemiesNearPosition(nodePos, 40.0f);
                    uint8 allies = CountAlliesNearPosition(nodePos, 40.0f);
                    float dist = bot->GetDistance(go);

                    float score = (node.State == POINT_ASSAULTED ? 20.0f : 6.0f);
                    score += (static_cast<float>(allies) - static_cast<float>(enemies)) * 3.0f;
                    score -= (dist / 80.0f);

                    if (enemies >= allies + 1)
                        score += 6.0f;
                    if (lowReinforcements && node.State == POINT_ASSAULTED)
                        score += 6.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestGo = go;
                    }
                }

                if (bestGo)
                    BgObjective = bestGo;
            }

            // --- Enemy Boss ---
            if (!BgObjective)
            {
                uint32 towersDown = 0;
                for (auto const& [nodeId, _] : attackObjectives)
                    if (av->GetAVNodeInfo(nodeId).State == POINT_DESTROYED)
                        towersDown++;

                if ((towersDown >= 2) || (strategy == AV_STRATEGY_OFFENSIVE))
                {
                    uint8 lastGY = (team == TEAM_HORDE) ? BG_AV_NODES_FIRSTAID_STATION : BG_AV_NODES_FROSTWOLF_HUT;
                    bool ownsFinalGY = av->GetAVNodeInfo(lastGY).OwnerId == team;

                    uint32 bossId = (team == TEAM_HORDE) ? AV_CREATURE_A_BOSS : AV_CREATURE_H_BOSS;
                    if (Creature* boss = bg->GetBGCreature(bossId))
                    {
                        if (boss->IsAlive())
                        {
                            uint32 nearbyCount = getPlayersInArea(team, boss->GetPosition(), 200.0f, false);
                            if ((ownsFinalGY || nearbyCount >= 20) && !(lowReinforcements && !enemyLowReinforcements))
                                BgObjective = boss;
                        }
                    }
                }
            }

            // --- Attacker Logic (scored) ---
            if (!BgObjective)
            {
                float bestScore = -99999.0f;
                GameObject* bestGo = nullptr;

                for (auto const& [nodeId, goId] : attackObjectives)
                {
                    const BG_AV_NodeInfo& node = av->GetAVNodeInfo(nodeId);
                    GameObject* go = bg->GetBGObject(goId);
                    if (!go || node.State == POINT_DESTROYED || node.TotalOwnerId == team)
                        continue;

                    Position nodePos = go->GetPosition();
                    uint8 enemies = CountEnemiesNearPosition(nodePos, 40.0f);
                    uint8 allies = CountAlliesNearPosition(nodePos, 40.0f);
                    float dist = bot->GetDistance(go);

                    float score = 6.0f;
                    if (node.State == POINT_ASSAULTED)
                        score -= 6.0f;
                    if (node.OwnerId == TEAM_NEUTRAL)
                        score += 4.0f;

                    score += (static_cast<float>(allies) - static_cast<float>(enemies)) * 3.0f;
                    score -= (dist / 70.0f);

                    if (strategy == AV_STRATEGY_OFFENSIVE)
                        score += 3.0f;
                    if (lowReinforcements && !enemyLowReinforcements)
                        score -= 6.0f;

                    if (!IsCriticalSituation() && enemies > allies + 2)
                        score -= 20.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestGo = go;
                    }
                }

                if (bestGo)
                {
                    BgObjective = bestGo;
                }
                else
                {
                    // Fallback: move to boss wait position
                    const Position& waitPos = (team == TEAM_HORDE) ? AV_BOSS_WAIT_H : AV_BOSS_WAIT_A;

                    float rx, ry, rz;
                    bot->GetRandomPoint(waitPos, 5.0f, rx, ry, rz);

                    if (Map* map = bot->GetMap())
                    {
                        float groundZ = map->GetHeight(rx, ry, rz);
                        if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                            rz = groundZ;
                    }

                    pos.Set(rx, ry, rz, bot->GetMapId());
                    posMap["bg objective"] = pos;

                    uint32 bossId = (team == TEAM_HORDE) ? AV_CREATURE_A_BOSS : AV_CREATURE_H_BOSS;
                    if (Creature* boss = bg->GetBGCreature(bossId))
                        if (boss->IsAlive())
                            BgObjective = boss;
                }
            }

            // --- Movement logic for any valid objective ---
            if (BgObjective)
            {
                Position objPos = BgObjective->GetPosition();

                Optional<uint8> linkedNodeId;
                for (auto const& [nodeId, goId] : attackObjectives)
                    if (bg->GetBGObject(goId) == BgObjective)
                        linkedNodeId = nodeId;

                if (!linkedNodeId)
                {
                    for (auto const& [nodeId, goId] : defendObjectives)
                        if (bg->GetBGObject(goId) == BgObjective)
                            linkedNodeId = nodeId;
                }

                float rx, ry, rz;
                if (linkedNodeId && AVNodeMovementTargets.count(*linkedNodeId))
                {
                    const AVNodePositionData& data = AVNodeMovementTargets[*linkedNodeId];
                    bot->GetRandomPoint(data.pos, frand(-data.maxRadius, data.maxRadius), rx, ry, rz);
                }
                else
                    bot->GetRandomPoint(objPos, frand(-2.0f, 2.0f), rx, ry, rz);

                if (Map* map = bot->GetMap())
                {
                    float groundZ = map->GetHeight(rx, ry, rz);
                    if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                        rz = groundZ;
                }

                pos.Set(rx, ry, rz, BgObjective->GetMapId());
                posMap["bg objective"] = pos;
                return true;
            }

            break;
        }
        case BATTLEGROUND_WS:
        {
            Position target;
            TeamId team = bot->GetTeamId();

            // Utility to safely relocate a position with optional random radius
            auto SetSafePos = [&](Position const& origin, float radius = 0.0f) -> void
            {
                float rx, ry, rz;
                if (radius > 0.0f)
                {
                    bot->GetRandomPoint(origin, radius, rx, ry, rz);
                    if (rz == VMAP_INVALID_HEIGHT_VALUE)
                        target.Relocate(origin);
                    else
                        target.Relocate(rx, ry, rz);
                }
                else
                {
                    target.Relocate(origin);
                }
            };
            auto ChooseBestHide = [&](std::vector<Position> const& spots, Unit* enemyCarrier) -> Position
            {
                float bestScore = -99999.0f;
                Position best = spots.front();

                for (auto const& p : spots)
                {
                    uint8 allies = CountAlliesNearPosition(p, 30.0f);
                    uint8 enemies = CountEnemiesNearPosition(p, 30.0f);
                    float dist = bot->GetDistance(p);
                    float score = (static_cast<float>(allies) * 2.0f) -
                                  (static_cast<float>(enemies) * 3.0f) -
                                  (dist / 25.0f);

                    if (enemyCarrier)
                        score += (enemyCarrier->GetDistance(p) / 40.0f);

                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = p;
                    }
                }

                return best;
            };

            // Check if the bot is carrying the flag
            bool hasFlag = bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG);

            // Retrieve role
            uint8 role = context->GetValue<uint32>("bg role")->Get();
            WSBotStrategy strategyHorde = static_cast<WSBotStrategy>(GetBotStrategyForTeam(bg, TEAM_HORDE));
            WSBotStrategy strategyAlliance = static_cast<WSBotStrategy>(GetBotStrategyForTeam(bg, TEAM_ALLIANCE));
            WSBotStrategy strategy = (team == TEAM_ALLIANCE) ? strategyAlliance : strategyHorde;
            WSBotStrategy enemyStrategy = (team == TEAM_ALLIANCE) ? strategyHorde : strategyAlliance;

            uint8 defendersProhab = 3;  // Default balanced

            switch (strategy)
            {
                case 0:
                case 1:
                case 2:
                case 3:  // Balanced
                    defendersProhab = 3;
                    break;
                case 4:
                case 5:
                case 6:
                case 7:  // Heavy Offense
                    defendersProhab = 1;
                    break;
                case 8:
                case 9:  // Heavy Defense
                    defendersProhab = 6;
                    break;
            }

            if (enemyStrategy == WS_STRATEGY_DEFENSIVE)
                defendersProhab = 2;

            // DYNAMIC STRATEGY OVERRIDE (WSG)
            if (ShouldPlayDefensive(bg))
            {
                strategy = WS_STRATEGY_DEFENSIVE;
                defendersProhab = 6; // Force heavy defense
            }
            else if (ShouldPlayAggressive(bg))
            {
                strategy = WS_STRATEGY_OFFENSIVE;
                defendersProhab = 1; // Force heavy offense
            }

            // Role check
            bool isDefender = role < defendersProhab;

            // --- PRIORITY 0: Secure Dropped Flags ---
            if (!hasFlag)
            {
                 BattlegroundWS* ws = static_cast<BattlegroundWS*>(bg);
                 
                 // 1. Return Friendly Flag (High Priority)
                 ObjectGuid friendlyFlagGUID = ws->GetDroppedFlagGUID(team);
                 GameObject* fFlag = friendlyFlagGUID ? bot->GetMap()->GetGameObject(friendlyFlagGUID) : nullptr;
                 if (fFlag && fFlag->isSpawned()) 
                 {
                     LOG_DEBUG("playerbots", "BGTactics: {} rushing to return friendly flag", bot->GetName());
                     pos.Set(fFlag->GetPositionX(), fFlag->GetPositionY(), fFlag->GetPositionZ(), fFlag->GetMapId());
                     posMap["bg objective"] = pos;
                     return true;
                 }
                 
                 // 2. Pick up Enemy Flag
                 TeamId enemyTeam = (team == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
                 ObjectGuid enemyFlagGUID = ws->GetDroppedFlagGUID(enemyTeam);
                 GameObject* eFlag = enemyFlagGUID ? bot->GetMap()->GetGameObject(enemyFlagGUID) : nullptr;
                 if (eFlag && eFlag->isSpawned()) 
                 {
                     LOG_DEBUG("playerbots", "BGTactics: {} rushing to pick up enemy flag", bot->GetName());
                     pos.Set(eFlag->GetPositionX(), eFlag->GetPositionY(), eFlag->GetPositionZ(), eFlag->GetMapId());
                     posMap["bg objective"] = pos;
                     return true;
                 }
            }

            // Retrieve flag carriers
            Unit* enemyFC = AI_VALUE(Unit*, "enemy flag carrier");
            Unit* teamFC = AI_VALUE(Unit*, "team flag carrier");
            TeamId enemyTeam = bg->GetOtherTeamId(team);
            Position ownFlagPos = (team == TEAM_ALLIANCE) ? WS_FLAG_POS_ALLIANCE : WS_FLAG_POS_HORDE;
            uint8 ownBaseEnemies = CountEnemiesNearPosition(ownFlagPos, 35.0f);
            uint8 ownBaseAllies = CountAlliesNearPosition(ownFlagPos, 35.0f);
            bool baseUnderAttack = ownBaseEnemies >= ownBaseAllies + 1;
            bool baseThreatHigh = ownBaseEnemies >= ownBaseAllies + 2;
            bool enemyFCNearCap = IsEnemyFCNearCap();
            bool shouldEscort = ShouldEscortFlagCarrier() ||
                                (teamFC && teamFC->IsAlive() && teamFC->GetHealthPct() < 60.0f);

            // Retrieve current score
            uint8 allianceScore = bg->GetTeamScore(TEAM_ALLIANCE);
            uint8 hordeScore = bg->GetTeamScore(TEAM_HORDE);

            // Check if both teams currently have the flag
            bool bothFlagsTaken = enemyFC && teamFC;
            if (!hasFlag && bothFlagsTaken)
            {
                if (enemyFCNearCap)
                {
                    target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                }
                // If both flags taken: escort if needed, otherwise attack enemy FC
                else if (teamFC && shouldEscort)
                {
                    target.Relocate(teamFC->GetPositionX(), teamFC->GetPositionY(), teamFC->GetPositionZ());
                    if (sServerFacade->GetDistance2d(bot, teamFC) < 33.0f)
                        Follow(teamFC);
                }
                else if (enemyFC)
                    target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                else if (teamFC)
                    target.Relocate(teamFC->GetPositionX(), teamFC->GetPositionY(), teamFC->GetPositionZ());
            }
            // Graveyard Camping if in lead
            else if (!hasFlag && role < 8 &&
                (team == TEAM_ALLIANCE && allianceScore == 2 && hordeScore == 0) ||
                (team == TEAM_HORDE && hordeScore == 2 && allianceScore == 0))
            {
                if (team == TEAM_ALLIANCE)
                    SetSafePos(WS_GY_CAMPING_HORDE, 10.0f);
                else
                    SetSafePos(WS_GY_CAMPING_ALLIANCE, 10.0f);
            }
            else if (hasFlag)
            {
                // If carrying the flag, either hide or return to base
                if (team == TEAM_ALLIANCE)
                {
                    if (teamFlagTaken() && baseUnderAttack)
                        SetSafePos(ChooseBestHide(WS_FLAG_HIDE_ALLIANCE, enemyFC));
                    else
                        SetSafePos(teamFlagTaken() ? ChooseBestHide(WS_FLAG_HIDE_ALLIANCE, enemyFC) : WS_FLAG_POS_ALLIANCE);
                }
                else
                {
                    if (teamFlagTaken() && baseUnderAttack)
                        SetSafePos(ChooseBestHide(WS_FLAG_HIDE_HORDE, enemyFC));
                    else
                        SetSafePos(teamFlagTaken() ? ChooseBestHide(WS_FLAG_HIDE_HORDE, enemyFC) : WS_FLAG_POS_HORDE);
                }
            }
            else
            {
                if (isDefender)
                {
                    if (enemyFCNearCap && enemyFC)
                    {
                        // Defenders attack enemy FC if found
                        target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                    }
                    else if (baseUnderAttack)
                    {
                        SetSafePos(ownFlagPos, 8.0f);
                    }
                    else if (teamFC && shouldEscort)
                    {
                        // 70% chance to support own FC
                        target.Relocate(teamFC->GetPositionX(), teamFC->GetPositionY(), teamFC->GetPositionZ());
                        if (sServerFacade->GetDistance2d(bot, teamFC) < 33.0f)
                            Follow(teamFC);
                    }
                    else if (enemyFC)
                    {
                        target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                    }
                    else
                    {
                        // Roam around central area
                        SetSafePos(WS_ROAM_POS, 75.0f);
                    }
                }
                else  // attacker logic
                {
                    if (enemyFCNearCap && enemyFC)
                    {
                        target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                    }
                    else if (enemyFC)
                    {
                        target.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());
                    }
                    else if (teamFC && shouldEscort)
                    {
                        // Assist own FC if not pursuing enemy FC
                        target.Relocate(teamFC->GetPositionX(), teamFC->GetPositionY(), teamFC->GetPositionZ());
                        if (sServerFacade->GetDistance2d(bot, teamFC) < 33.0f)
                            Follow(teamFC);
                    }
                    else if (baseThreatHigh)
                    {
                        SetSafePos(ownFlagPos, 10.0f);
                    }
                    else
                    {
                        // Push toward enemy flag base
                        SetSafePos(team == TEAM_ALLIANCE ? WS_FLAG_POS_HORDE : WS_FLAG_POS_ALLIANCE);
                    }
                }
            }

            // Save the final target position
            if (target.IsPositionValid())
            {
                pos.Set(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(), bot->GetMapId());
                posMap["bg objective"] = pos;
                return true;
            }

            break;
        }
        case BATTLEGROUND_AB:
        {
            if (!bg)
                break;

            BattlegroundAB* ab = static_cast<BattlegroundAB*>(bg);
            UpdateNodeStates(bg); // Update reactive defense tracking
            
            TeamId team = bot->GetTeamId();
            TeamId enemyTeam = bg->GetOtherTeamId(team);

            uint32 basesOwned = 0;
            uint32 basesEnemy = 0;
            uint32 basesNeutral = 0;
            for (uint32 nodeId : AB_AttackObjectives)
            {
                TeamId owner = ab->GetCapturePointInfo(nodeId)._ownerTeamId;
                if (owner == TEAM_NEUTRAL)
                    ++basesNeutral;
                else if (owner == team)
                    ++basesOwned;
                else
                    ++basesEnemy;
            }

            uint32 myScore = bg->GetTeamScore(team);
            uint32 enemyScore = bg->GetTeamScore(enemyTeam);
            bool isBehind = enemyScore >= myScore + 200;
            bool isAhead = myScore >= enemyScore + 200;

            uint8 role = context->GetValue<uint32>("bg role")->Get();
            ABBotStrategy strategyHorde = static_cast<ABBotStrategy>(GetBotStrategyForTeam(bg, TEAM_HORDE));
            ABBotStrategy strategyAlliance = static_cast<ABBotStrategy>(GetBotStrategyForTeam(bg, TEAM_ALLIANCE));
            ABBotStrategy strategy = (team == TEAM_ALLIANCE) ? strategyAlliance : strategyHorde;
            ABBotStrategy enemyStrategy = (team == TEAM_ALLIANCE) ? strategyHorde : strategyAlliance;
            
            bool isDefender = role == 0 || role == 1 || role == 2; // Default defensive roles

            // DYNAMIC STRATEGY OVERRIDE
            if (ShouldPlayDefensive(bg))
            {
                strategy = AB_STRATEGY_DEFENSIVE;
            }
            else if (ShouldPlayAggressive(bg))
            {
                strategy = AB_STRATEGY_OFFENSIVE;
            }
            // "Turtle Mode" override if winning by a lot
            if (IsWinning(bg) && GetTeamBasesControlled(bg, team) >= 3)
            {
                 // Heavy protection
                 strategy = AB_STRATEGY_DEFENSIVE;
            }

            uint8 defendersProhab = 3;
            // Adjusted prohab based on strategy override
            if (strategy == AB_STRATEGY_OFFENSIVE)
                defendersProhab = 1;
            else if (strategy == AB_STRATEGY_DEFENSIVE)
                defendersProhab = 6;

            if (enemyStrategy == AB_STRATEGY_DEFENSIVE)
                defendersProhab = 2;

            if (isAhead && basesOwned >= 3)
                defendersProhab = 6;
            else if (isBehind && basesOwned <= 1)
                defendersProhab = 1;

            // Recalculate isDefender based on final prohab
            isDefender = role < defendersProhab;

            BgObjective = nullptr;

            // =============================================
            // PRIORITY 0: Emergency Reactive Defense
            // =============================================
            // Check if any node is critical and needs immediate defense
            // This is prioritized above all other objectives (except manual commands)
            {
                uint32 bgInstanceId = bg->GetInstanceID();
                auto& nodes = bgNodeStates[bgInstanceId];
                
                uint32 bestNode = UINT32_MAX;
                float highestPriority = 0.0f;
                
                for (auto const& [nodeId, info] : nodes)
                {
                    if (info.needsDefense && info.defensivePriority > highestPriority)
                    {
                        bestNode = nodeId;
                        highestPriority = info.defensivePriority;
                    }
                }
                
                if (bestNode != UINT32_MAX)
                {
                    // Find the GameObject for this node (flag/banner)
                    GameObject* go = bg->GetBGObject(bestNode * BG_AB_OBJECTS_PER_NODE);
                    if (go)
                    {
                        // Throttle defensive-emergency to avoid tight loops/spam
                        static std::unordered_map<ObjectGuid::LowType, time_t> sDefEmergencyThrottle;
                        ObjectGuid::LowType key = go->GetGUID().GetCounter();
                        time_t now = time(nullptr);
                        auto it = sDefEmergencyThrottle.find(key);
                        if (it == sDefEmergencyThrottle.end() || (now - it->second) > 5)
                        {
                            LOG_DEBUG("playerbots", "BGTactics: {} prioritizing defensive emergency at {}",
                                      bot->GetName(), GetNodeName(bestNode, bgType));
                            sDefEmergencyThrottle[key] = now;
                        }
                        BgObjective = go;
                    }
                }
            }

            // =============================================
            // PRIORITY 0.5: Emergency Blacksmith contest
            // =============================================
            if (!BgObjective && !isDefender)
            {
                uint8 state = ab->GetCapturePointInfo(AB_NODE_BLACKSMITH)._state;
                bool bsOwned = (team == TEAM_ALLIANCE && state == BG_AB_NODE_STATE_ALLY_OCCUPIED) ||
                               (team == TEAM_HORDE && state == BG_AB_NODE_STATE_HORDE_OCCUPIED);
                if (!bsOwned)
                {
                    uint8 bsEnemies = CountEnemiesNearPosition(AB_NODE_POS_BLACKSMITH, 40.0f);
                    if (bsEnemies >= 4)
                    {
                        if (GameObject* go = bg->GetBGObject(AB_NODE_BLACKSMITH * BG_AB_OBJECTS_PER_NODE))
                            BgObjective = go;
                    }
                }
            }

            // =============================================
            // PRIORITY 1: AB Opening Rush (First 45s)
            // =============================================
            if (IsGameOpening(bg))
            {
                uint32 openingNode = GetAssignedOpeningNode(bg);
                if (openingNode != UINT32_MAX)
                {
                    // Find actual game object for this node
                    GameObject* go = bg->GetBGObject(openingNode * BG_AB_OBJECTS_PER_NODE);
                    if (go)
                    {
                        LOG_DEBUG("playerbots", "BGTactics: {} performing opening rush to {}", 
                                  bot->GetName(), GetNodeName(openingNode, bgType));
                        BgObjective = go;
                        
                        // Set explicit position for movement immediately
                        float rx, ry, rz;
                        Position objPos = BgObjective->GetPosition();
                        bot->GetRandomPoint(objPos, frand(3.0f, 8.0f), rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }
                        pos.Set(rx, ry, rz, BgObjective->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
            }

            // =============================================
            // PRIORITY 2: Combat Engagement (Secondary Check)
            // =============================================
            // This block is redundant if the Priority 0 check at the start works, 
            // but kept as a fallback for when 'reset' is true.
            if (Unit* enemy = AI_VALUE(Unit*, "enemy player target"))
            {
                 // Logic handled at function start
            }

            // --- PRIORITY 2: No valid nodes? Camp or attack visible enemy
            bool hasValidTarget = false;
            for (uint32 nodeId : AB_AttackObjectives)
            {
                uint8 state = ab->GetCapturePointInfo(nodeId)._state;
                if (state == BG_AB_NODE_STATE_NEUTRAL ||
                    (team == TEAM_ALLIANCE &&
                     (state == BG_AB_NODE_STATE_HORDE_OCCUPIED || state == BG_AB_NODE_STATE_HORDE_CONTESTED)) ||
                    (team == TEAM_HORDE &&
                     (state == BG_AB_NODE_STATE_ALLY_OCCUPIED || state == BG_AB_NODE_STATE_ALLY_CONTESTED)))
                {
                    hasValidTarget = true;
                    break;
                }
            }

            if (!hasValidTarget)
            {
                if (Unit* enemy = AI_VALUE(Unit*, "enemy player target"))
                {
                    if (bot->GetDistance(enemy) < 500.0f)
                    {
                        pos.Set(enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), bot->GetMapId());
                        posMap["bg objective"] = pos;
                        break;
                    }
                }

                // Camp enemy GY fallback
                Position camp = (team == TEAM_ALLIANCE) ? AB_GY_CAMPING_HORDE : AB_GY_CAMPING_ALLIANCE;
                float rx, ry, rz;
                bot->GetRandomPoint(camp, 10.0f, rx, ry, rz);
                if (Map* map = bot->GetMap())
                {
                    float groundZ = map->GetHeight(rx, ry, rz);
                    if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                        rz = groundZ;
                }
                pos.Set(rx, ry, rz, bot->GetMapId());
                posMap["bg objective"] = pos;
                break;
            }

            // --- PRIORITY 3: Defender logic ---
            if (isDefender)
            {
                float closestDist = FLT_MAX;
                for (uint32 nodeId : AB_AttackObjectives)
                {
                    uint8 state = ab->GetCapturePointInfo(nodeId)._state;

                    bool isContested = (team == TEAM_ALLIANCE && state == BG_AB_NODE_STATE_HORDE_CONTESTED) ||
                                       (team == TEAM_HORDE && state == BG_AB_NODE_STATE_ALLY_CONTESTED);
                    bool isOwned = (team == TEAM_ALLIANCE && state == BG_AB_NODE_STATE_ALLY_OCCUPIED) ||
                                   (team == TEAM_HORDE && state == BG_AB_NODE_STATE_HORDE_OCCUPIED);

                    if (!isContested && !isOwned)
                        continue;

                    GameObject* go = bg->GetBGObject(nodeId * BG_AB_OBJECTS_PER_NODE);
                    if (!go)
                        continue;

                    float dist = bot->GetDistance(go);
                    if (dist < closestDist)
                    {
                        closestDist = dist;
                        BgObjective = go;
                    }
                }
            }

            // --- PRIORITY 3.5: Secure Nearby Objectives (Cap if close) ---
            if (!BgObjective)
            {
                for (uint32 nodeId : AB_AttackObjectives)
                {
                    uint8 state = ab->GetCapturePointInfo(nodeId)._state;

                    // We want to capture if it is Unfriendly
                    bool isFriendly = (team == TEAM_ALLIANCE && (state == BG_AB_NODE_STATE_ALLY_OCCUPIED || state == BG_AB_NODE_STATE_ALLY_CONTESTED)) ||
                                      (team == TEAM_HORDE && (state == BG_AB_NODE_STATE_HORDE_OCCUPIED || state == BG_AB_NODE_STATE_HORDE_CONTESTED));

                    if (isFriendly)
                        continue;

                    GameObject* go = bg->GetBGObject(nodeId * BG_AB_OBJECTS_PER_NODE);
                    // Use a slightly larger radius to ensure we catch it if we're "in the area"
                    if (go && go->IsWithinDist3d(bot, 20.0f))
                    {
                        LOG_DEBUG("playerbots", "BGTactics: {} prioritizing nearby capturable node: {}", bot->GetName(), nodeId);
                        BgObjective = go;

                        // Force precise movement to tag it
                        float rx, ry, rz;
                        Position objPos = BgObjective->GetPosition();
                        // Move very close (3-5 yards) to ensure interaction logic triggers
                        bot->GetRandomPoint(objPos, frand(3.0f, 5.0f), rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }
                        pos.Set(rx, ry, rz, BgObjective->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
            }

            // --- PRIORITY 3.75: Smart objective selection / regroup ---
            if (!BgObjective)
            {
                uint32 bestNode = UINT32_MAX;
                float bestScore = -99999.0f;
                uint8 bestAllies = 0;
                uint8 bestEnemies = 0;

                for (uint32 nodeId : AB_AttackObjectives)
                {
                    CaptureABPointInfo const& captureInfo = ab->GetCapturePointInfo(nodeId);
                    uint8 state = captureInfo._state;
                    TeamId owner = captureInfo._ownerTeamId;

                    bool canTarget = (state == BG_AB_NODE_STATE_NEUTRAL) ||
                                     (owner == enemyTeam) ||
                                     (owner == team && (state == BG_AB_NODE_STATE_ALLY_CONTESTED ||
                                                        state == BG_AB_NODE_STATE_HORDE_CONTESTED));
                    if (!canTarget)
                        continue;

                    Position nodePos = GetNodePosition(nodeId, bgType);
                    if (nodePos.GetPositionX() == 0.0f && nodePos.GetPositionY() == 0.0f)
                        continue;

                    uint8 allies = CountAlliesNearPosition(nodePos, 40.0f);
                    uint8 enemies = CountEnemiesNearPosition(nodePos, 40.0f);
                    float dist = bot->GetDistance(nodePos);

                    float strategic = GetNodeStrategicValue(nodeId, bgType);
                    float score = (strategic * 10.0f) +
                                  (static_cast<float>(allies) - static_cast<float>(enemies)) * 4.0f -
                                  (dist / 60.0f);

                    bool isTriangle = (nodeId == AB_NODE_BLACKSMITH || nodeId == AB_NODE_LUMBER_MILL ||
                                       (team == TEAM_ALLIANCE && nodeId == AB_NODE_STABLES) ||
                                       (team == TEAM_HORDE && nodeId == AB_NODE_FARM));
                    if (isTriangle)
                        score += 4.0f;

                    if (nodeId == AB_NODE_BLACKSMITH)
                        score += 3.0f;

                    if (nodeId == AB_NODE_GOLD_MINE && (isBehind || basesOwned <= 1))
                        score -= 6.0f;

                    if (IsGameOpening(bg))
                    {
                        if (nodeId == AB_NODE_BLACKSMITH || nodeId == AB_NODE_LUMBER_MILL)
                            score += 3.0f;
                        if (nodeId == AB_NODE_GOLD_MINE)
                            score -= 4.0f;
                    }

                    if (!IsCriticalSituation() && enemies > allies + 2)
                        score -= 20.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestNode = nodeId;
                        bestAllies = allies;
                        bestEnemies = enemies;
                    }
                }

                bool shouldRegroup = false;
                if (bestNode != UINT32_MAX && !IsCriticalSituation())
                {
                    if (bestEnemies >= bestAllies + 2 && basesOwned >= 2 && !IsGameOpening(bg))
                        shouldRegroup = true;
                }

                if (bestNode != UINT32_MAX && !shouldRegroup)
                {
                    if (GameObject* go = bg->GetBGObject(bestNode * BG_AB_OBJECTS_PER_NODE))
                        BgObjective = go;
                }
                else if (shouldRegroup)
                {
                    Position regroupPos;
                    bool hasRegroup = false;

                    float bestSupport = -9999.0f;
                    for (uint32 nodeId : AB_AttackObjectives)
                    {
                        CaptureABPointInfo const& captureInfo = ab->GetCapturePointInfo(nodeId);
                        if (captureInfo._ownerTeamId != team)
                            continue;

                        Position nodePos = GetNodePosition(nodeId, bgType);
                        uint8 allies = CountAlliesNearPosition(nodePos, 40.0f);
                        uint8 enemies = CountEnemiesNearPosition(nodePos, 40.0f);
                        float support = static_cast<float>(allies) * 2.0f - static_cast<float>(enemies) -
                                        (bot->GetDistance(nodePos) / 80.0f);

                        if (support > bestSupport)
                        {
                            bestSupport = support;
                            regroupPos = nodePos;
                            hasRegroup = true;
                        }
                    }

                    if (hasRegroup)
                    {
                        float rx, ry, rz;
                        bot->GetRandomPoint(regroupPos, 8.0f, rx, ry, rz);
                        if (Map* map = bot->GetMap())
                        {
                            float groundZ = map->GetHeight(rx, ry, rz);
                            if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                rz = groundZ;
                        }
                        pos.Set(rx, ry, rz, bot->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
            }

            // --- PRIORITY 4: Attack objectives ---
            if (!BgObjective)
            {
                float bestScore = -99999.0f;
                GameObject* bestGo = nullptr;

                for (uint32 nodeId : AB_AttackObjectives)
                {
                    uint8 state = ab->GetCapturePointInfo(nodeId)._state;

                    bool isNeutral = state == BG_AB_NODE_STATE_NEUTRAL;
                    bool isEnemyOccupied = (team == TEAM_ALLIANCE && state == BG_AB_NODE_STATE_HORDE_OCCUPIED) ||
                                           (team == TEAM_HORDE && state == BG_AB_NODE_STATE_ALLY_OCCUPIED);
                    bool isFriendlyContested =
                        (team == TEAM_ALLIANCE && state == BG_AB_NODE_STATE_ALLY_CONTESTED) ||
                        (team == TEAM_HORDE && state == BG_AB_NODE_STATE_HORDE_CONTESTED);

                    if (!(isNeutral || isEnemyOccupied || isFriendlyContested))
                        continue;

                    GameObject* go = bg->GetBGObject(nodeId * BG_AB_OBJECTS_PER_NODE);
                    if (!go)
                        continue;

                    float dist = bot->GetDistance(go);
                    float strategic = GetNodeStrategicValue(nodeId, bgType);
                    float score = (strategic * 10.0f) - (dist / 70.0f);

                    if (nodeId == AB_NODE_BLACKSMITH)
                        score += 3.0f;
                    if (nodeId == AB_NODE_GOLD_MINE && (isBehind || basesOwned <= 1))
                        score -= 6.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestGo = go;
                    }
                }

                if (bestGo)
                    BgObjective = bestGo;
            }

            // --- Final move ---
            if (BgObjective)
            {
                float rx, ry, rz;
                Position objPos = BgObjective->GetPosition();
                bot->GetRandomPoint(objPos, frand(5.0f, 15.0f), rx, ry, rz);

                if (Map* map = bot->GetMap())
                {
                    float groundZ = map->GetHeight(rx, ry, rz);
                    if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                        rz = groundZ;
                }

                pos.Set(rx, ry, rz, BgObjective->GetMapId());
                posMap["bg objective"] = pos;
            }

            return true;
        }
        case BATTLEGROUND_EY:
        {
            BattlegroundEY* eyeOfTheStormBG = (BattlegroundEY*)bg;
            TeamId team = bot->GetTeamId();
            uint8 role = context->GetValue<uint32>("bg role")->Get();

            EYBotStrategy strategyHorde = static_cast<EYBotStrategy>(GetBotStrategyForTeam(bg, TEAM_HORDE));
            EYBotStrategy strategyAlliance = static_cast<EYBotStrategy>(GetBotStrategyForTeam(bg, TEAM_ALLIANCE));
            EYBotStrategy strategy = (team == TEAM_ALLIANCE) ? strategyAlliance : strategyHorde;
            EYBotStrategy enemyStrategy = (team == TEAM_ALLIANCE) ? strategyHorde : strategyAlliance;

            auto IsOwned = [&](uint32 nodeId) -> bool
            { return eyeOfTheStormBG->GetCapturePointInfo(nodeId)._ownerTeamId == team; };

            uint32 basesOwned = 0;
            for (auto const& [nodeId, _, __] : EY_AttackObjectives)
            {
                if (IsOwned(nodeId))
                    ++basesOwned;
            }
            TeamId enemyTeam = bg->GetOtherTeamId(team);
            uint32 enemyBases = 0;
            uint32 neutralBases = 0;
            for (auto const& [nodeId, _, __] : EY_AttackObjectives)
            {
                TeamId owner = eyeOfTheStormBG->GetCapturePointInfo(nodeId)._ownerTeamId;
                if (owner == TEAM_NEUTRAL)
                    ++neutralBases;
                else if (owner == enemyTeam)
                    ++enemyBases;
            }

            uint32 elapsed = GameTime::GetGameTime().count() - bg->GetStartTime();
            bool openingPhase = elapsed < 90;
            uint32 myScore = bg->GetTeamScore(team);
            uint32 enemyScore = bg->GetTeamScore(enemyTeam);
            bool isBehind = enemyScore >= myScore + 150;
            bool isAhead = myScore >= enemyScore + 150;

            // =============================================
            // EotS DYNAMIC UPDATE & STRATEGY
            // =============================================
            UpdateNodeStates(bg); // Critical: Update node states for reactive defense

            uint8 defendersProhab = 4;
            // DYNAMIC STRATEGY OVERRIDE (EotS)
            if (ShouldPlayDefensive(bg))
            {
                strategy = EY_STRATEGY_FLAG_FOCUS; // Defend implies controlling points/flag
                defendersProhab = 6;
            }
            else if (ShouldPlayAggressive(bg))
            {
                strategy = EY_STRATEGY_BALANCED; // Push everywhere
                defendersProhab = 2;
            }

            if (isAhead && basesOwned >= 3)
                defendersProhab = 6;
            else if (isBehind && basesOwned <= 1)
                defendersProhab = 2;
            
            switch (strategy)
            {
                case EY_STRATEGY_FLAG_FOCUS:
                    defendersProhab = 2;
                    break;
                case EY_STRATEGY_FRONT_FOCUS:
                case EY_STRATEGY_BACK_FOCUS:
                    defendersProhab = 3;
                    break;
                default:
                    // defense prohab already set above for balanced/default
                    break;
            }

            // =============================================
            // PRIORITY 0: Emergency Reactive Defense
            // =============================================
            // Check if any node is critical and needs immediate defense
            {
                uint32 bgInstanceId = bg->GetInstanceID();
                auto& nodes = bgNodeStates[bgInstanceId];
                
                uint32 bestNode = UINT32_MAX;
                float highestPriority = 0.0f;
                
                for (auto const& [nodeId, info] : nodes)
                {
                    if (info.needsDefense && info.defensivePriority > highestPriority)
                    {
                        bestNode = nodeId;
                        highestPriority = info.defensivePriority;
                    }
                }
                
                if (bestNode != UINT32_MAX && EY_NodePositions.find(bestNode) != EY_NodePositions.end())
                {
                    const Position& p = EY_NodePositions[bestNode];
                    float rx, ry, rz;
                    bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                    if (bot->GetMap()) 
                        rz = bot->GetMap()->GetHeight(rx, ry, rz);
                    
                    if (rz == VMAP_INVALID_HEIGHT_VALUE)
                        rz = p.GetPositionZ();

                    pos.Set(rx, ry, rz, bot->GetMapId());
                    
                    LOG_DEBUG("playerbots", "BGTactics: {} prioritizing defensive emergency at {}", 
                              bot->GetName(), GetNodeName(bestNode, bgType));
                              
                    posMap["bg objective"] = pos;
                    return true;
                }
            }

            bool isDefender = role < defendersProhab;

            std::tuple<uint32, uint32, uint32> front[2];
            std::tuple<uint32, uint32, uint32> back[2];

            if (team == TEAM_HORDE)
            {
                front[0] = EY_AttackObjectives[0];
                front[1] = EY_AttackObjectives[1];
                back[0] = EY_AttackObjectives[2];
                back[1] = EY_AttackObjectives[3];
            }
            else
            {
                front[0] = EY_AttackObjectives[2];
                front[1] = EY_AttackObjectives[3];
                back[0] = EY_AttackObjectives[0];
                back[1] = EY_AttackObjectives[1];
            }

            bool foundObjective = false;

            // --- PRIORITY 0.25: Opening base split (faction biased) ---
            if (!foundObjective && openingPhase)
            {
                std::vector<uint32> openingBases;
                if (team == TEAM_ALLIANCE)
                {
                    openingBases = {POINT_MAGE_TOWER, POINT_DRAENEI_RUINS};
                }
                else
                {
                    openingBases = {POINT_FEL_REAVER, POINT_BLOOD_ELF};
                }

                uint32 bestNode = UINT32_MAX;
                float bestScore = -99999.0f;

                for (uint32 nodeId : openingBases)
                {
                    auto it = EY_NodePositions.find(nodeId);
                    if (it == EY_NodePositions.end())
                        continue;

                    const Position& p = it->second;
                    uint8 allies = CountAlliesNearPosition(p, 40.0f);
                    uint8 enemies = CountEnemiesNearPosition(p, 40.0f);
                    float dist = bot->GetDistance(p);

                    float score = 12.0f - (static_cast<float>(allies) * 2.0f) -
                                  (static_cast<float>(enemies) * 3.0f) -
                                  (dist / 120.0f);

                    if (!IsCriticalSituation() && enemies > allies + 2)
                        score -= 30.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestNode = nodeId;
                    }
                }

                if (bestNode != UINT32_MAX)
                {
                    const Position& p = EY_NodePositions[bestNode];
                    float rx, ry, rz;
                    bot->GetRandomPoint(p, 6.0f, rx, ry, rz);
                    rz = bot->GetMap()->GetHeight(rx, ry, rz);
                    pos.Set(rx, ry, rz, bot->GetMapId());
                    foundObjective = true;
                }
            }

            // --- PRIORITY 0.5: Secure Nearby Nodes ---
            if (!foundObjective && !isDefender) // Defenders usually stay put, but roamers should cap if close
            {
                for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                {
                    if (IsOwned(nodeId))
                        continue;

                    if (EY_NodePositions.contains(nodeId))
                    {
                        const Position& p = EY_NodePositions[nodeId];
                        // If we are very close to the center of the node
                        if (bot->IsWithinDist2d(p.GetPositionX(), p.GetPositionY(), 20.0f))
                        {
                             // Find the banner logic or just stand on point (EY captures by standing)
                             // Actually EY is radius based (standing near tower). 
                             // So we just need to Ensure we stay there if we are "capping"
                             // Logic: If state is not ours, and we are close, STAY.
                             
                             // However, the "Secure Nearby" logic for AB/AV was about clicking banners.
                             // For EY, we just need to be in the zone. 
                             // If we are already in the zone (20y), just picking the random point in the zone (below) is fine?
                             // No, current logic picks random point.
                             // We want to force it to be THE objective if we are close.
                             
                             LOG_DEBUG("playerbots", "BGTactics: {} holding nearby EY node: {}", bot->GetName(), nodeId);
                             
                             float rx, ry, rz;
                             bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                             rz = bot->GetMap()->GetHeight(rx, ry, rz);
                             pos.Set(rx, ry, rz, bot->GetMapId());
                             foundObjective = true;
                             break;
                        }
                    }
                }
            }

            // --- PRIORITY 1: FLAG CARRIER ---
            if (bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
            {
                uint32 bestNodeId = 0;
                float bestDist = FLT_MAX;
                uint32 bestTrigger = 0;

                for (auto const& [nodeId, _, areaTrigger] : EY_AttackObjectives)
                {
                    if (!IsOwned(nodeId))
                        continue;

                    auto it = EY_NodePositions.find(nodeId);
                    if (it == EY_NodePositions.end())
                        continue;

                    float dist = bot->GetDistance(it->second);
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestNodeId = nodeId;
                        bestTrigger = areaTrigger;
                    }
                }

                if (bestNodeId != 0 && EY_NodePositions.contains(bestNodeId))
                {
                    const Position& targetPos = EY_NodePositions[bestNodeId];
                    float rx, ry, rz;
                    bot->GetRandomPoint(targetPos, 5.0f, rx, ry, rz);

                    if (Map* map = bot->GetMap())
                    {
                        float groundZ = map->GetHeight(rx, ry, rz);
                        if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                            rz = groundZ;
                    }

                    pos.Set(rx, ry, rz, bot->GetMapId());

                    // Check AreaTrigger activation range
                    if (bestTrigger && bot->IsWithinDist3d(pos.x, pos.y, pos.z, INTERACTION_DISTANCE))
                    {
                        WorldPacket data(CMSG_AREATRIGGER);
                        data << uint32(bestTrigger);
                        bot->GetSession()->HandleAreaTriggerOpcode(data);
                        pos.Reset();
                    }

                    foundObjective = true;
                }
                else
                {
                    // No bases controlled - regroup with team at center instead of running to spawn
                    // This allows team to coordinate and capture a base together
                    const Position& fallback = (team == TEAM_ALLIANCE) ? EY_FLAG_RETURN_POS_RETREAT_ALLIANCE
                                                                       : EY_FLAG_RETURN_POS_RETREAT_HORDE;

                    float rx, ry, rz;
                    bot->GetRandomPoint(fallback, 5.0f, rx, ry, rz);

                    if (Map* map = bot->GetMap())
                    {
                        float groundZ = map->GetHeight(rx, ry, rz);
                        if (groundZ == VMAP_INVALID_HEIGHT_VALUE)
                            rz = groundZ;
                    }

                    pos.Set(rx, ry, rz, bot->GetMapId());
                    foundObjective = true;
                }
            }

            // --- PRIORITY 2: Nearby unowned contested node ---
            if (!foundObjective)
            {
                for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                {
                    if (IsOwned(nodeId))
                        continue;

                    auto it = EY_NodePositions.find(nodeId);
                    if (it == EY_NodePositions.end())
                        continue;

                    const Position& p = it->second;
                    if (bot->IsWithinDist2d(p.GetPositionX(), p.GetPositionY(), 125.0f))
                    {
                        float rx, ry, rz;
                        bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                        rz = bot->GetMap()->GetHeight(rx, ry, rz);
                        pos.Set(rx, ry, rz, bot->GetMapId());
                        foundObjective = true;
                    }
                }
            }

            // --- PRIORITY 2.5: Smart base selection / regroup ---
            if (!foundObjective && !isDefender)
            {
                uint32 bestNode = UINT32_MAX;
                float bestScore = -99999.0f;
                uint8 bestEnemies = 0;
                uint8 bestAllies = 0;

                for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                {
                    if (IsOwned(nodeId))
                        continue;

                    auto it = EY_NodePositions.find(nodeId);
                    if (it == EY_NodePositions.end())
                        continue;

                    const Position& p = it->second;
                    uint8 allies = CountAlliesNearPosition(p, 40.0f);
                    uint8 enemies = CountEnemiesNearPosition(p, 40.0f);
                    float dist = bot->GetDistance(p);
                    float strategic = GetNodeStrategicValue(nodeId, bgType);

                    float score = (strategic * 10.0f) +
                                  (static_cast<float>(allies) - static_cast<float>(enemies)) * 4.0f -
                                  (dist / 60.0f);

                    if (enemies == 0)
                        score += 6.0f;
                    if (allies >= enemies + 2)
                        score += 4.0f;
                    if (!IsCriticalSituation() && enemies > allies + 1)
                        score -= 20.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestNode = nodeId;
                        bestEnemies = enemies;
                        bestAllies = allies;
                    }
                }

                bool shouldRegroup = false;
                if (bestNode != UINT32_MAX && !IsCriticalSituation())
                {
                    if (bestEnemies >= bestAllies + 2 && bestScore < 0.0f)
                        shouldRegroup = true;
                }

                if (bestNode != UINT32_MAX && !shouldRegroup)
                {
                    const Position& p = EY_NodePositions[bestNode];
                    float rx, ry, rz;
                    bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                    rz = bot->GetMap()->GetHeight(rx, ry, rz);
                    pos.Set(rx, ry, rz, bot->GetMapId());
                    foundObjective = true;
                }
                else if (shouldRegroup)
                {
                    Position regroupPos;
                    bool hasRegroup = false;

                    if (basesOwned > 0)
                    {
                        float bestSupport = -9999.0f;
                        for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                        {
                            if (!IsOwned(nodeId))
                                continue;

                            auto it = EY_NodePositions.find(nodeId);
                            if (it == EY_NodePositions.end())
                                continue;

                            const Position& p = it->second;
                            uint8 allies = CountAlliesNearPosition(p, 40.0f);
                            uint8 enemies = CountEnemiesNearPosition(p, 40.0f);
                            float support = static_cast<float>(allies) * 2.0f - static_cast<float>(enemies) -
                                            (bot->GetDistance(p) / 80.0f);

                            if (support > bestSupport)
                            {
                                bestSupport = support;
                                regroupPos = p;
                                hasRegroup = true;
                            }
                        }
                    }

                    if (!hasRegroup)
                    {
                        if (Group* group = bot->GetGroup())
                        {
                            float bestDist = FLT_MAX;
                            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            {
                                Player* member = ref->GetSource();
                                if (!member || member == bot || !member->IsAlive())
                                    continue;

                                float dist = bot->GetDistance(member);
                                if (dist < bestDist)
                                {
                                    bestDist = dist;
                                    regroupPos = member->GetPosition();
                                    hasRegroup = true;
                                }
                            }
                        }
                    }

                    if (!hasRegroup)
                    {
                        regroupPos = (team == TEAM_ALLIANCE) ? EY_FLAG_RETURN_POS_RETREAT_ALLIANCE
                                                             : EY_FLAG_RETURN_POS_RETREAT_HORDE;
                        hasRegroup = true;
                    }

                    if (hasRegroup)
                    {
                        float rx, ry, rz;
                        bot->GetRandomPoint(regroupPos, 8.0f, rx, ry, rz);
                        rz = bot->GetMap()->GetHeight(rx, ry, rz);
                        pos.Set(rx, ry, rz, bot->GetMapId());
                        foundObjective = true;
                    }
                }
            }

            // --- PRIORITY 2.6: Comeback base push (when behind) ---
            if (!foundObjective && (isBehind || enemyBases >= 3) && basesOwned <= 1)
            {
                uint32 bestNode = UINT32_MAX;
                float bestScore = -99999.0f;

                for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                {
                    auto it = EY_NodePositions.find(nodeId);
                    if (it == EY_NodePositions.end())
                        continue;

                    TeamId owner = eyeOfTheStormBG->GetCapturePointInfo(nodeId)._ownerTeamId;
                    if (owner == team)
                        continue;

                    const Position& p = it->second;
                    uint8 allies = CountAlliesNearPosition(p, 40.0f);
                    uint8 enemies = CountEnemiesNearPosition(p, 40.0f);
                    float dist = bot->GetDistance(p);
                    float strategic = GetNodeStrategicValue(nodeId, bgType);

                    float score = (strategic * 12.0f) +
                                  (static_cast<float>(allies) - static_cast<float>(enemies)) * 5.0f -
                                  (dist / 60.0f);

                    if (owner == TEAM_NEUTRAL)
                        score += 8.0f;
                    if (!IsCriticalSituation() && enemies > allies + 2)
                        score -= 20.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestNode = nodeId;
                    }
                }

                if (bestNode != UINT32_MAX)
                {
                    const Position& p = EY_NodePositions[bestNode];
                    float rx, ry, rz;
                    bot->GetRandomPoint(p, 6.0f, rx, ry, rz);
                    rz = bot->GetMap()->GetHeight(rx, ry, rz);
                    pos.Set(rx, ry, rz, bot->GetMapId());
                    foundObjective = true;
                }
            }

            // --- PRIORITY 2.75: Bridge control / mid denial ---
            if (!foundObjective)
            {
                GameObject* midFlag = bg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM);
                bool flagUp = midFlag && midFlag->isSpawned();
                uint8 midAllies = CountAlliesNearPosition(EY_MID_FLAG_POS, 40.0f);
                uint8 midEnemies = CountEnemiesNearPosition(EY_MID_FLAG_POS, 40.0f);
                bool shouldControlMid = (basesOwned >= 2) || (openingPhase && !isBehind) || IsCriticalSituation();

                if (shouldControlMid && (flagUp || midEnemies > 0))
                {
                    std::vector<Position> bridges = (team == TEAM_ALLIANCE)
                        ? std::vector<Position>{EY_BRIDGE_NORTH_ENTRANCE, EY_BRIDGE_NORTH_MID}
                        : std::vector<Position>{EY_BRIDGE_SOUTH_ENTRANCE, EY_BRIDGE_SOUTH_MID};

                    float bestScore = -99999.0f;
                    Position bestPos;
                    bool hasBest = false;

                    for (auto const& p : bridges)
                    {
                        uint8 allies = CountAlliesNearPosition(p, 30.0f);
                        uint8 enemies = CountEnemiesNearPosition(p, 30.0f);
                        float dist = bot->GetDistance(p);

                        float score = (static_cast<float>(allies) * 2.0f) -
                                      (static_cast<float>(enemies) * 3.0f) -
                                      (dist / 80.0f);

                        if (!IsCriticalSituation() && enemies > allies + 2 && basesOwned < 2)
                            score -= 25.0f;

                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestPos = p;
                            hasBest = true;
                        }
                    }

                    if (hasBest && !(midEnemies > midAllies + 2 && basesOwned < 2 && !IsCriticalSituation()))
                    {
                        float rx, ry, rz;
                        bot->GetRandomPoint(bestPos, 6.0f, rx, ry, rz);
                        rz = bot->GetMap()->GetHeight(rx, ry, rz);
                        pos.Set(rx, ry, rz, bot->GetMapId());
                        foundObjective = true;
                    }
                }
            }

            // --- PRIORITY 4: Defender Logic ---
            if (!foundObjective && isDefender)
            {
                // 1. Chase enemy flag carrier
                if (Unit* enemyFC = AI_VALUE(Unit*, "enemy flag carrier"))
                {
                    if (bot->CanSeeOrDetect(enemyFC, false, false, true) && enemyFC->IsAlive())
                    {
                        pos.Set(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ(), bot->GetMapId());
                        foundObjective = true;
                    }
                }

                // 2. Support friendly flag carrier
                if (!foundObjective)
                {
                    if (Unit* friendlyFC = AI_VALUE(Unit*, "team flag carrier"))
                    {
                        if (friendlyFC->IsAlive())
                        {
                            pos.Set(friendlyFC->GetPositionX(), friendlyFC->GetPositionY(), friendlyFC->GetPositionZ(), bot->GetMapId());
                            foundObjective = true;
                        }
                    }
                }

                // 3. Pick up the flag
                if (!foundObjective)
                {
                    if (GameObject* flag = bg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM); flag && flag->isSpawned())
                    {
                        if (basesOwned >= 2 || IsCriticalSituation())
                        {
                            pos.Set(flag->GetPositionX(), flag->GetPositionY(), flag->GetPositionZ(), flag->GetMapId());
                            foundObjective = true;
                        }
                    }
                }

                // 4. Default: defend owned node
                if (!foundObjective)
                {
                    std::vector<uint32> owned;
                    for (auto const& obj : front)
                    {
                        uint32 nodeId = std::get<0>(obj);
                        if (IsOwned(nodeId))
                            owned.push_back(nodeId);
                    }

                    if (!owned.empty())
                    {
                        float bestScore = -99999.0f;
                        Optional<uint32> bestNode;
                        for (uint32 nodeId : owned)
                        {
                            const Position& p = EY_NodePositions[nodeId];
                            uint8 allies = CountAlliesNearPosition(p, 35.0f);
                            uint8 enemies = CountEnemiesNearPosition(p, 35.0f);
                            float dist = bot->GetDistance(p);
                            float score = (static_cast<float>(enemies) * 2.5f) -
                                          (static_cast<float>(allies) * 1.5f) -
                                          (dist / 80.0f);

                            if (score > bestScore)
                            {
                                bestScore = score;
                                bestNode = nodeId;
                            }
                        }

                        if (bestNode && EY_NodePositions.contains(*bestNode))
                        {
                            const Position& p = EY_NodePositions[*bestNode];
                            float rx, ry, rz;
                            bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                            rz = bot->GetMap()->GetHeight(rx, ry, rz);
                            pos.Set(rx, ry, rz, bot->GetMapId());
                            foundObjective = true;
                        }
                    }
                }
            }

            // --- PRIORITY 5: Flag Strategy ---
            if (!foundObjective && strategy == EY_STRATEGY_FLAG_FOCUS)
            {
                bool canChaseFlag = (basesOwned >= 2) || IsCriticalSituation();

                if (basesOwned > 0 && canChaseFlag && (!isBehind || basesOwned >= 2))
                {
                    if (Unit* fc = AI_VALUE(Unit*, "enemy flag carrier"))
                    {
                        pos.Set(fc->GetPositionX(), fc->GetPositionY(), fc->GetPositionZ(), bot->GetMapId());
                        foundObjective = true;
                    }
                    else if (GameObject* flag = bg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM);
                             flag && flag->isSpawned())
                    {
                        pos.Set(flag->GetPositionX(), flag->GetPositionY(), flag->GetPositionZ(), flag->GetMapId());
                        foundObjective = true;
                    }
                }
                else
                {
                    float bestDist = FLT_MAX;
                    Optional<uint32> bestNode;

                    for (auto const& [nodeId, _, __] : EY_AttackObjectives)
                    {
                        if (IsOwned(nodeId))
                            continue;

                        auto it = EY_NodePositions.find(nodeId);
                        if (it == EY_NodePositions.end())
                            continue;

                        float dist = bot->GetDistance(it->second);
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            bestNode = nodeId;
                        }
                    }

                    if (bestNode && EY_NodePositions.contains(*bestNode))
                    {
                        const Position& p = EY_NodePositions[*bestNode];
                        float rx, ry, rz;
                        bot->GetRandomPoint(p, 5.0f, rx, ry, rz);
                        rz = bot->GetMap()->GetHeight(rx, ry, rz);
                        pos.Set(rx, ry, rz, bot->GetMapId());
                        foundObjective = true;
                    }
                }
            }

            // --- PRIORITY 6: Strategy Objectives ---
            if (!foundObjective)
            {
                std::vector<std::tuple<uint32, uint32, uint32>> priority, secondary;

                switch (strategy)
                {
                    case EY_STRATEGY_FRONT_FOCUS:
                        priority = {front[0], front[1]};
                        secondary = {back[0], back[1]};
                        break;
                    case EY_STRATEGY_BACK_FOCUS:
                        priority = {back[0], back[1]};
                        secondary = {front[0], front[1]};
                        break;
                    case EY_STRATEGY_BALANCED:
                    default:
                        priority = {front[0], front[1], back[0], back[1]};
                        break;
                }

                std::vector<uint32> candidates;

                for (auto const& obj : priority)
                {
                    uint32 nodeId = std::get<0>(obj);
                    if (!IsOwned(nodeId))
                        candidates.push_back(nodeId);
                }

                if (candidates.empty())
                {
                    for (auto const& obj : secondary)
                    {
                        uint32 nodeId = std::get<0>(obj);
                        if (!IsOwned(nodeId))
                            candidates.push_back(nodeId);
                    }
                }

                if (!candidates.empty())
                {
                    float bestScore = -99999.0f;
                    Optional<uint32> bestNode;
                    for (uint32 nodeId : candidates)
                    {
                        if (!EY_NodePositions.contains(nodeId))
                            continue;

                        const Position& p = EY_NodePositions[nodeId];
                        uint8 allies = CountAlliesNearPosition(p, 40.0f);
                        uint8 enemies = CountEnemiesNearPosition(p, 40.0f);
                        float dist = bot->GetDistance(p);
                        float strategic = GetNodeStrategicValue(nodeId, bgType);
                        float score = (strategic * 10.0f) +
                                      (static_cast<float>(allies) - static_cast<float>(enemies)) * 4.0f -
                                      (dist / 60.0f);

                        if (!IsCriticalSituation() && enemies > allies + 2)
                            score -= 20.0f;

                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestNode = nodeId;
                        }
                    }

                    if (bestNode && EY_NodePositions.contains(*bestNode))
                    {
                        const Position& p = EY_NodePositions[*bestNode];
                        pos.Set(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(), bot->GetMapId());
                        foundObjective = true;
                    }
                }
            }

            // --- PRIORITY 7: Camp GY if everything is owned ---
            bool ownsAll = IsOwned(std::get<0>(front[0])) && IsOwned(std::get<0>(front[1])) &&
                           IsOwned(std::get<0>(back[0])) && IsOwned(std::get<0>(back[1]));

            if (!foundObjective && ownsAll)
            {
                Position camp = (team == TEAM_HORDE) ? EY_GY_CAMPING_ALLIANCE : EY_GY_CAMPING_HORDE;
                float rx, ry, rz;
                bot->GetRandomPoint(camp, 10.0f, rx, ry, rz);
                rz = bot->GetMap()->GetHeight(rx, ry, rz);
                pos.Set(rx, ry, rz, bot->GetMapId());
                foundObjective = true;
            }

            if (foundObjective)
                posMap["bg objective"] = pos;

            return true;
        }
        case BATTLEGROUND_IC:
        {
            BattlegroundIC* isleOfConquestBG = (BattlegroundIC*)bg;

            uint32 role = context->GetValue<uint32>("bg role")->Get();
            bool inVehicle = botAI->IsInVehicle();
            bool controlsVehicle = botAI->IsInVehicle(true);
            uint32 vehicleId = inVehicle ? bot->GetVehicleBase()->GetEntry() : 0;

            // skip if not the driver
            if (inVehicle && !controlsVehicle)
                return false;

            // DYNAMIC STRATEGY OVERRIDE (IoC)
            // IoC doesn't have an Enum, so we bias role interpretation
            bool forceDefend = ShouldPlayDefensive(bg);
            bool forceAttack = ShouldPlayAggressive(bg);
            
            if (forceDefend) role = 0; // Force role 0 (often defensive/passive logic)
            if (forceAttack) role = 9; // Force role 9 (often offensive logic)

            // IoC Reactive Update (if we implement IoC support later, place it here)
            // UpdateNodeStates(bg);

            auto IsNodeControlledByTeam = [&](ICNodeState state) -> bool
            {
                return (bot->GetTeamId() == TEAM_ALLIANCE && state == NODE_STATE_CONTROLLED_A) ||
                       (bot->GetTeamId() == TEAM_HORDE && state == NODE_STATE_CONTROLLED_H);
            };
            auto IsNodeControlledByEnemy = [&](ICNodeState state) -> bool
            {
                return (bot->GetTeamId() == TEAM_ALLIANCE && state == NODE_STATE_CONTROLLED_H) ||
                       (bot->GetTeamId() == TEAM_HORDE && state == NODE_STATE_CONTROLLED_A);
            };
            auto IsNodeContestedByEnemy = [&](ICNodeState state) -> bool
            {
                return (bot->GetTeamId() == TEAM_ALLIANCE && state == NODE_STATE_CONFLICT_H) ||
                       (bot->GetTeamId() == TEAM_HORDE && state == NODE_STATE_CONFLICT_A);
            };
            auto FindNearestIoCVehicle = [&](float radius) -> Unit*
            {
                GuidVector targets = AI_VALUE(GuidVector, "all targets");
                Unit* nearestVehicle = nullptr;
                float nearestDist = radius;

                for (auto guid : targets)
                {
                    Unit* unit = botAI->GetUnit(guid);
                    if (!unit || !unit->IsVehicle())
                        continue;

                    uint32 entry = unit->GetEntry();
                    bool isSiege = (entry == NPC_SIEGE_ENGINE_A || entry == NPC_SIEGE_ENGINE_H);
                    bool isGlaive = (entry == NPC_GLAIVE_THROWER_A || entry == NPC_GLAIVE_THROWER_H);
                    bool isDemo = (entry == NPC_DEMOLISHER);
                    if (!isSiege && !isGlaive && !isDemo)
                        continue;

                    if (unit->GetFaction() != bot->GetFaction())
                        continue;

                    float dist = bot->GetDistance(unit);
                    if (dist < nearestDist)
                    {
                        nearestDist = dist;
                        nearestVehicle = unit;
                    }
                }

                return nearestVehicle;
            };
            auto FindItemInInventory = [&](uint32 itemId) -> Item*
            {
                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                {
                    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                    if (item && item->GetEntry() == itemId)
                        return item;
                }

                for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                {
                    Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
                    if (!bag)
                        continue;

                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    {
                        Item* item = bag->GetItemByPos(slot);
                        if (item && item->GetEntry() == itemId)
                            return item;
                    }
                }

                return nullptr;
            };
            auto GetWeakestEnemyGate = [&]() -> GameObject*
            {
                std::vector<uint32> gates = (bot->GetTeamId() == TEAM_ALLIANCE)
                    ? std::vector<uint32>{BG_IC_GO_HORDE_GATE_1, BG_IC_GO_HORDE_GATE_2, BG_IC_GO_HORDE_GATE_3}
                    : std::vector<uint32>{BG_IC_GO_ALLIANCE_GATE_1, BG_IC_GO_ALLIANCE_GATE_2, BG_IC_GO_ALLIANCE_GATE_3};

                float bestHealthPct = FLT_MAX;
                GameObject* bestGate = nullptr;

                for (uint32 gateId : gates)
                {
                    GameObject* gate = bg->GetBGObject(gateId);
                    if (!gate || !gate->IsDestructibleBuilding())
                        continue;

                    auto const* value = gate->GetGOValue();
                    if (!value || value->Building.MaxHealth == 0 || value->Building.Health == 0)
                        continue;

                    float healthPct = static_cast<float>(value->Building.Health) /
                                      static_cast<float>(value->Building.MaxHealth);
                    if (healthPct < bestHealthPct)
                    {
                        bestHealthPct = healthPct;
                        bestGate = gate;
                    }
                }

                return bestGate;
            };

            // Vehicle priority: drive/guard vehicles first
            if (!inVehicle)
            {
                if (Unit* vehicle = FindNearestIoCVehicle(60.0f))
                {
                    pos.Set(vehicle->GetPositionX(), vehicle->GetPositionY(), vehicle->GetPositionZ(), bot->GetMapId());
                    posMap["bg objective"] = pos;
                    return true;
                }

                if (ShouldProtectSiegeEngine())
                {
                    if (Unit* siegeEngine = FindAlliedSiegeEngine(60.0f))
                    {
                        pos.Set(siegeEngine->GetPositionX(), siegeEngine->GetPositionY(), siegeEngine->GetPositionZ(),
                                bot->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
            }

            bool gateOpen = false;
            if (bot->GetTeamId() == TEAM_HORDE)
            {
                if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_DOODAD_PORTCULLISACTIVE02))
                    gateOpen = pGO->isSpawned() && pGO->getLootState() == GO_ACTIVATED;
            }
            else
            {
                if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_HORDE_KEEP_PORTCULLIS))
                    gateOpen = pGO->isSpawned() && pGO->getLootState() == GO_ACTIVATED;
            }

            if (controlsVehicle && !gateOpen)
            {
                Position gatePos = GetEnemyGatePosition();
                pos.Set(gatePos.GetPositionX(), gatePos.GetPositionY(), gatePos.GetPositionZ(), bot->GetMapId());
                posMap["bg objective"] = pos;
                PositionInfo siegePos = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
                siegePos.Set(gatePos.GetPositionX(), gatePos.GetPositionY(), gatePos.GetPositionZ(), bot->GetMapId());
                posMap["bg siege"] = siegePos;
                return true;
            }

            /* TACTICS */
            // --- PRIORITY 0: Secure Nearby Banners (IoC) ---
            if (!inVehicle)
            {
                for (uint32 bannerId : vFlagsIC)
                {
                     GameObject* banner = bg->GetBGObject(bannerId);
                     if (banner && banner->IsWithinDist3d(bot, 20.0f))
                     {
                         time_t now = time(nullptr);
                         ObjectGuid::LowType guidLow = banner->GetGUID().GetCounter();
                         auto it = sIoCBannerThrottle.find(guidLow);
                         if (it != sIoCBannerThrottle.end() && (now - it->second) < 5)
                             continue; // recently tried, skip to avoid thrashing
                         sIoCBannerThrottle[guidLow] = now;

                         // Check if we can interact (capturable)
                         // We don't easily know the node state from the GO ID here without mapping, 
                         // but if it's a banner and we are close, we should probably check it.
                         // Simplification: Go to it. Interaction logic handles the rest.
                         // Optimization: Only if not already ours? 
                         // IoC banners change ID or appearance? IoC banners are usually standard.
                         // Let's rely on the fact that if we are 20y close to a banner, we probably want to click it.
                         
                         LOG_DEBUG("playerbots", "BGTactics: {} prioritizing nearby IoC banner", bot->GetName());
                         BgObjective = banner;
                         
                         float rx, ry, rz;
                         Position objPos = BgObjective->GetPosition();
                         bot->GetRandomPoint(objPos, frand(3.0f, 5.0f), rx, ry, rz);
                         if (Map* map = bot->GetMap())
                         {
                              float groundZ = map->GetHeight(rx, ry, rz);
                              if (groundZ != VMAP_INVALID_HEIGHT_VALUE)
                                  rz = groundZ;
                         }
                         pos.Set(rx, ry, rz, BgObjective->GetMapId());
                         posMap["bg objective"] = pos;
                         return true;
                     }
                }
            }

            // --- PRIORITY 0.2: Hangar teleport (gunship) ---
            if (!inVehicle && !BgObjective && TeamControlsHangar() && !gateOpen)
            {
                std::vector<uint32> hangarTeleporters = {
                    BG_IC_GO_HANGAR_TELEPORTER_1,
                    BG_IC_GO_HANGAR_TELEPORTER_2,
                    BG_IC_GO_HANGAR_TELEPORTER_3
                };

                GameObject* bestTeleporter = nullptr;
                float bestDist = FLT_MAX;

                for (uint32 teleporterId : hangarTeleporters)
                {
                    GameObject* teleporter = bg->GetBGObject(teleporterId);
                    if (!teleporter || !teleporter->isSpawned())
                        continue;

                    float dist = bot->GetDistance(teleporter);
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestTeleporter = teleporter;
                    }
                }

                if (bestTeleporter)
                {
                    pos.Set(bestTeleporter->GetPositionX(), bestTeleporter->GetPositionY(),
                            bestTeleporter->GetPositionZ(), bot->GetMapId());
                    posMap["bg objective"] = pos;
                    return true;
                }
            }

            // --- PRIORITY 0.25: Bomb runner ---
            if (!inVehicle && !BgObjective)
            {
                const uint32 SEAFORIUM_BOMBS_ITEM = 46847;
                const uint32 HUGE_SEAFORIUM_BOMBS_ITEM = 47030;
                Item* bombItem = FindItemInInventory(HUGE_SEAFORIUM_BOMBS_ITEM);
                if (!bombItem)
                    bombItem = FindItemInInventory(SEAFORIUM_BOMBS_ITEM);

                if (bombItem)
                {
                    if (GameObject* gate = GetWeakestEnemyGate())
                    {
                        float gateDist = bot->GetDistance(gate);
                        if (gateDist <= 6.0f && !bot->IsInCombat())
                        {
                            UseItemAction useBomb(botAI, "use", true);
                            useBomb.UseItemOnGameObjectDirect(bombItem, gate->GetGUID());
                            return true;
                        }

                        pos.Set(gate->GetPositionX(), gate->GetPositionY(), gate->GetPositionZ(), bot->GetMapId());
                        posMap["bg objective"] = pos;
                        return true;
                    }
                }
                else
                {
                    if (gateOpen)
                    {
                        std::vector<uint32> hugeBombGos = (bot->GetTeamId() == TEAM_ALLIANCE)
                            ? std::vector<uint32>{BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_1, BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_2,
                                                  BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_3, BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_4}
                            : std::vector<uint32>{BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_1, BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_2,
                                                  BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_3, BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_4};

                        GameObject* bestHugeBomb = nullptr;
                        float bestDist = FLT_MAX;

                        for (uint32 bombId : hugeBombGos)
                        {
                            GameObject* bomb = bg->GetBGObject(bombId);
                            if (!bomb || !bomb->isSpawned())
                                continue;

                            float dist = bot->GetDistance(bomb);
                            if (dist < bestDist)
                            {
                                bestDist = dist;
                                bestHugeBomb = bomb;
                            }
                        }

                        if (bestHugeBomb)
                        {
                            pos.Set(bestHugeBomb->GetPositionX(), bestHugeBomb->GetPositionY(),
                                    bestHugeBomb->GetPositionZ(), bot->GetMapId());
                            posMap["bg objective"] = pos;
                            return true;
                        }
                    }

                    if (TeamControlsWorkshop())
                    {
                        std::vector<uint32> bombGos = {BG_IC_GO_SEAFORIUM_BOMBS_1, BG_IC_GO_SEAFORIUM_BOMBS_2};
                        GameObject* bestBomb = nullptr;
                        float bestDist = FLT_MAX;

                        for (uint32 bombId : bombGos)
                        {
                            GameObject* bomb = bg->GetBGObject(bombId);
                            if (!bomb || !bomb->isSpawned())
                                continue;

                            float dist = bot->GetDistance(bomb);
                            if (dist < bestDist)
                            {
                                bestDist = dist;
                                bestBomb = bomb;
                            }
                        }

                        if (bestBomb)
                        {
                            pos.Set(bestBomb->GetPositionX(), bestBomb->GetPositionY(), bestBomb->GetPositionZ(),
                                    bot->GetMapId());
                            posMap["bg objective"] = pos;
                            return true;
                        }
                    }
                }
            }

            // --- PRIORITY 0.5: Objective scoring (IoC) ---
            if (!BgObjective && !inVehicle)
            {
                struct IoCNode
                {
                    uint32 nodeType;
                    uint32 bannerId;
                    float basePriority;
                };

                std::vector<IoCNode> nodes;
                if (bot->GetTeamId() == TEAM_ALLIANCE)
                {
                    nodes = {
                        {NODE_TYPE_DOCKS, BG_IC_GO_DOCKS_BANNER, 100.0f},
                        {NODE_TYPE_WORKSHOP, BG_IC_GO_WORKSHOP_BANNER, 85.0f},
                        {NODE_TYPE_HANGAR, BG_IC_GO_HANGAR_BANNER, 70.0f},
                        {NODE_TYPE_REFINERY, BG_IC_GO_REFINERY_BANNER, 30.0f},
                        {NODE_TYPE_QUARRY, BG_IC_GO_QUARRY_BANNER, 30.0f},
                    };
                }
                else
                {
                    nodes = {
                        {NODE_TYPE_WORKSHOP, BG_IC_GO_WORKSHOP_BANNER, 100.0f},
                        {NODE_TYPE_DOCKS, BG_IC_GO_DOCKS_BANNER, 85.0f},
                        {NODE_TYPE_HANGAR, BG_IC_GO_HANGAR_BANNER, 70.0f},
                        {NODE_TYPE_REFINERY, BG_IC_GO_REFINERY_BANNER, 30.0f},
                        {NODE_TYPE_QUARRY, BG_IC_GO_QUARRY_BANNER, 30.0f},
                    };
                }

                float bestScore = -99999.0f;
                GameObject* bestGo = nullptr;

                for (auto const& node : nodes)
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(node.nodeType);
                    GameObject* go = bg->GetBGObject(node.bannerId);
                    if (!go)
                        continue;

                    float score = node.basePriority;
                    if (IsNodeControlledByTeam(nodePoint.nodeState))
                        score -= 40.0f;
                    if (IsNodeControlledByEnemy(nodePoint.nodeState))
                        score += 20.0f;
                    if (IsNodeContestedByEnemy(nodePoint.nodeState))
                        score += 15.0f;
                    if (nodePoint.nodeState == NODE_STATE_UNCONTROLLED)
                        score += 10.0f;

                    float dist = bot->GetDistance(go);
                    score -= dist / 60.0f;

                    uint8 enemies = CountEnemiesNearPosition(go->GetPosition(), 40.0f);
                    uint8 allies = CountAlliesNearPosition(go->GetPosition(), 40.0f);
                    if (!IsCriticalSituation() && enemies > allies + 2)
                        score -= 20.0f;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestGo = go;
                    }
                }

                if (bestGo)
                    BgObjective = bestGo;
            }

            if (bot->GetTeamId() == TEAM_HORDE)  // HORDE
            {
                gateOpen = false;
                if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_DOODAD_PORTCULLISACTIVE02))
                {
                    if (pGO->isSpawned() && pGO->getLootState() == GO_ACTIVATED)
                    {
                        gateOpen = true;
                        PositionInfo siegePos = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
                        siegePos.Reset();
                        posMap["bg siege"] = siegePos;
                    }
                }
                if (gateOpen && !controlsVehicle &&
                    isleOfConquestBG->GetICNodePoint(NODE_TYPE_GRAVEYARD_A).nodeState ==
                        NODE_STATE_CONTROLLED_H)  // target enemy boss
                {
                    if (Creature* enemyBoss = bg->GetBGCreature(BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE))
                    {
                        if (enemyBoss->IsVisible())
                        {
                            BgObjective = enemyBoss;
                            // LOG_INFO("playerbots", "bot={} attack boss", bot->GetName());
                        }
                    }
                }

                if (!BgObjective && gateOpen && !controlsVehicle)
                {
                    if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_ALLIANCE_BANNER))  // capture flag within keep
                    {
                        BgObjective = pGO;
                        // LOG_INFO("playerbots", "bot={} attack keep", bot->GetName());
                    }
                }

                if (!BgObjective && !gateOpen && controlsVehicle)  // attack gates
                {
                    // TODO: check for free vehicles
                    if (GameObject* gate = bg->GetBGObject(BG_IC_GO_ALLIANCE_GATE_3))
                    {
                        if (vehicleId == NPC_SIEGE_ENGINE_H)  // target gate directly if siege engine
                        {
                            BgObjective = gate;
                            // LOG_INFO("playerbots", "bot={} (in siege-engine) attack gate", bot->GetName());
                        }
                        else  // target gate directly at range if other vehicle
                        {
                            // just make bot stay where it is if already close
                            // (stops them shifting around between the random spots)
                            if (bot->GetDistance(IC_GATE_ATTACK_POS_HORDE) < 8.0f)
                                pos.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
                            else
                                pos.Set(IC_GATE_ATTACK_POS_HORDE.GetPositionX() + frand(-5.0f, +5.0f),
                                        IC_GATE_ATTACK_POS_HORDE.GetPositionY() + frand(-5.0f, +5.0f),
                                        IC_GATE_ATTACK_POS_HORDE.GetPositionZ(), bot->GetMapId());
                            posMap["bg objective"] = pos;
                            // set siege position
                            PositionInfo siegePos = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
                            siegePos.Set(gate->GetPositionX(), gate->GetPositionY(), gate->GetPositionZ(),
                                         bot->GetMapId());
                            posMap["bg siege"] = siegePos;
                            // LOG_INFO("playerbots", "bot={} (in vehicle={}) attack gate", bot->GetName(), vehicleId);
                            return true;
                        }
                    }
                }

                // If gates arent down and not in vehicle, split tasks
                if (!BgObjective && !controlsVehicle && role == 9)  // Capture Side base
                {
                    // Capture Refinery
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_REFINERY);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_H && nodePoint.nodeState != NODE_STATE_CONTROLLED_H)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_REFINERY_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack refinery", bot->GetName());
                        }
                    }
                }

                if (!BgObjective && !controlsVehicle && role < 3)  // Capture Docks
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_DOCKS);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_H && nodePoint.nodeState != NODE_STATE_CONTROLLED_H)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_DOCKS_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack docks", bot->GetName());
                        }
                    }
                }
                if (!BgObjective && !controlsVehicle && role < 6)  // Capture Hangar
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_HANGAR);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_H && nodePoint.nodeState != NODE_STATE_CONTROLLED_H)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_HANGAR_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack hangar", bot->GetName());
                        }
                    }
                }
                if (!BgObjective && !controlsVehicle)  // Capture Workshop
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_WORKSHOP);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_H && nodePoint.nodeState != NODE_STATE_CONTROLLED_H)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_WORKSHOP_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack workshop", bot->GetName());
                        }
                    }
                }
                if (!BgObjective)  // Guard point that's not fully capped (also gets them in place to board vehicle)
                {
                    uint32 len = end(IC_AttackObjectives) - begin(IC_AttackObjectives);
                    for (uint32 i = 0; i < len; i++)
                    {
                        auto const& objective =
                            IC_AttackObjectives[(i + role) %
                                                len];  // use role to determine which objective checked first
                        if (isleOfConquestBG->GetICNodePoint(objective.first).nodeState != NODE_STATE_CONTROLLED_H)
                        {
                            if (GameObject* pGO = bg->GetBGObject(objective.second))
                            {
                                BgObjective = pGO;
                                // LOG_INFO("playerbots", "bot={} guard point while it captures", bot->GetName());
                                break;
                            }
                        }
                    }
                }
                if (!BgObjective)  // guard vehicles as they seige
                {
                    // just make bot stay where it is if already close
                    // (stops them shifting around between the random spots)
                    if (bot->GetDistance(IC_GATE_ATTACK_POS_HORDE) < 8.0f)
                        pos.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
                    else
                        pos.Set(IC_GATE_ATTACK_POS_HORDE.GetPositionX() + frand(-5.0f, +5.0f),
                                IC_GATE_ATTACK_POS_HORDE.GetPositionY() + frand(-5.0f, +5.0f),
                                IC_GATE_ATTACK_POS_HORDE.GetPositionZ(), bot->GetMapId());
                    posMap["bg objective"] = pos;
                    // LOG_INFO("playerbots", "bot={} guard vehicles as they attack gate", bot->GetName());
                    return true;
                }
            }

            if (bot->GetTeamId() == TEAM_ALLIANCE)  // ALLIANCE
            {
                gateOpen = false;
                if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_HORDE_KEEP_PORTCULLIS))
                {
                    if (pGO->isSpawned() && pGO->getLootState() == GO_ACTIVATED)
                    {
                        gateOpen = true;
                        PositionInfo siegePos = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
                        siegePos.Reset();
                        posMap["bg siege"] = siegePos;
                    }
                }

                if (gateOpen && !controlsVehicle &&
                    isleOfConquestBG->GetICNodePoint(NODE_TYPE_GRAVEYARD_H).nodeState ==
                        NODE_STATE_CONTROLLED_A)  // target enemy boss
                {
                    if (Creature* enemyBoss = bg->GetBGCreature(BG_IC_NPC_OVERLORD_AGMAR))
                    {
                        if (enemyBoss->IsVisible())
                        {
                            BgObjective = enemyBoss;
                            // LOG_INFO("playerbots", "bot={} attack boss", bot->GetName());
                        }
                    }
                }

                if (!BgObjective && gateOpen && !controlsVehicle)
                {
                    if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_HORDE_BANNER))  // capture flag within keep
                    {
                        BgObjective = pGO;
                        // LOG_INFO("playerbots", "bot={} attack keep", bot->GetName());
                    }
                }

                if (!BgObjective && !gateOpen && controlsVehicle)  // attack gates
                {
                    // TODO: check for free vehicles
                    if (GameObject* gate = bg->GetBGObject(BG_IC_GO_HORDE_GATE_1))
                    {
                        if (vehicleId == NPC_SIEGE_ENGINE_A)  // target gate directly if siege engine
                        {
                            BgObjective = gate;
                            // LOG_INFO("playerbots", "bot={} (in siege-engine) attack gate", bot->GetName());
                        }
                        else  // target gate directly at range if other vehicle
                        {
                            // just make bot stay where it is if already close
                            // (stops them shifting around between the random spots)
                            if (bot->GetDistance(IC_GATE_ATTACK_POS_ALLIANCE) < 8.0f)
                                pos.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
                            else
                                pos.Set(IC_GATE_ATTACK_POS_ALLIANCE.GetPositionX() + frand(-5.0f, +5.0f),
                                        IC_GATE_ATTACK_POS_ALLIANCE.GetPositionY() + frand(-5.0f, +5.0f),
                                        IC_GATE_ATTACK_POS_ALLIANCE.GetPositionZ(), bot->GetMapId());
                            posMap["bg objective"] = pos;
                            // set siege position
                            PositionInfo siegePos = context->GetValue<PositionMap&>("position")->Get()["bg siege"];
                            siegePos.Set(gate->GetPositionX(), gate->GetPositionY(), gate->GetPositionZ(),
                                         bot->GetMapId());
                            posMap["bg siege"] = siegePos;
                            // LOG_INFO("playerbots", "bot={} (in vehicle={}) attack gate", bot->GetName(), vehicleId);
                            return true;
                        }
                    }
                }

                // If gates arent down and not in vehicle, split tasks
                if (!BgObjective && !controlsVehicle && role == 9)  // Capture Side base
                {
                    // Capture Refinery
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_QUARRY);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_A && nodePoint.nodeState != NODE_STATE_CONTROLLED_A)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_QUARRY_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack quarry", bot->GetName());
                        }
                    }
                }

                if (!BgObjective && !controlsVehicle && role < 3)  // Capture Docks
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_DOCKS);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_A && nodePoint.nodeState != NODE_STATE_CONTROLLED_A)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_DOCKS_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack docks", bot->GetName());
                        }
                    }
                }
                if (!BgObjective && !controlsVehicle && role < 6)  // Capture Hangar
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_HANGAR);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_A && nodePoint.nodeState != NODE_STATE_CONTROLLED_A)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_HANGAR_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack hangar", bot->GetName());
                        }
                    }
                }
                if (!BgObjective && !controlsVehicle)  // Capture Workshop
                {
                    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_WORKSHOP);
                    if (nodePoint.nodeState != NODE_STATE_CONFLICT_A && nodePoint.nodeState != NODE_STATE_CONTROLLED_A)
                    {
                        if (GameObject* pGO = bg->GetBGObject(BG_IC_GO_WORKSHOP_BANNER))
                        {
                            BgObjective = pGO;
                            // LOG_INFO("playerbots", "bot={} attack workshop", bot->GetName());
                        }
                    }
                }
                if (!BgObjective)  // Guard point that's not fully capped (also gets them in place to board vehicle)
                {
                    uint32 len = end(IC_AttackObjectives) - begin(IC_AttackObjectives);
                    for (uint32 i = 0; i < len; i++)
                    {
                        auto const& objective =
                            IC_AttackObjectives[(i + role) %
                                                len];  // use role to determine which objective checked first
                        if (isleOfConquestBG->GetICNodePoint(objective.first).nodeState != NODE_STATE_CONTROLLED_H)
                        {
                            if (GameObject* pGO = bg->GetBGObject(objective.second))
                            {
                                BgObjective = pGO;
                                // LOG_INFO("playerbots", "bot={} guard point while it captures", bot->GetName());
                                break;
                            }
                        }
                    }
                }
                if (!BgObjective)  // guard vehicles as they seige
                {
                    // just make bot stay where it is if already close
                    // (stops them shifting around between the random spots)
                    if (bot->GetDistance(IC_GATE_ATTACK_POS_ALLIANCE) < 8.0f)
                        pos.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
                    else
                        pos.Set(IC_GATE_ATTACK_POS_ALLIANCE.GetPositionX() + frand(-5.0f, +5.0f),
                                IC_GATE_ATTACK_POS_ALLIANCE.GetPositionY() + frand(-5.0f, +5.0f),
                                IC_GATE_ATTACK_POS_ALLIANCE.GetPositionZ(), bot->GetMapId());
                    posMap["bg objective"] = pos;
                    // LOG_INFO("playerbots", "bot={} guard vehicles as they attack gate", bot->GetName());
                    return true;
                }
            }

            if (BgObjective)
            {
                pos.Set(BgObjective->GetPositionX(), BgObjective->GetPositionY(), BgObjective->GetPositionZ(),
                        bot->GetMapId());
                posMap["bg objective"] = pos;
                return true;
            }
            break;
        }
        default:
            break;
    }

    return false;
}

bool BGTactics::moveToObjective(bool ignoreDist)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    if (!pos.isSet())
        return selectObjective();
    else
    {
        // Use portals in Isle of Conquest Base
        if (bgType == BATTLEGROUND_IC)
        {
            if (IsLockedInsideKeep())
                return true;
        }

        if (!ignoreDist && sServerFacade->IsDistanceGreaterThan(sServerFacade->GetDistance2d(bot, pos.x, pos.y), 100.0f))
        {
            // std::ostringstream out;
            // out << "It is too far away! " << pos.x << ", " << pos.y << ", Distance: " <<
            // sServerFacade->GetDistance2d(bot, pos.x, pos.y); bot->Say(out.str(), LANG_UNIVERSAL);
            return false;
        }

        // don't try to move if already close
        if (bot->GetDistance(pos.x, pos.y, pos.z) < 4.0f)
        {
            resetObjective();
            return true;
        }

        // std::ostringstream out; out << "Moving to objective " << pos.x << ", " << pos.y << ", Distance: " <<
        // sServerFacade->GetDistance2d(bot, pos.x, pos.y); bot->Say(out.str(), LANG_UNIVERSAL);

        // dont increase from 1.5 will cause bugs with horde capping AV towers
        return MoveNear(bot->GetMapId(), pos.x, pos.y, pos.z, 1.5f);
    }
    return false;
}

bool BGTactics::selectObjectiveWp(std::vector<BattleBotPath*> const& vPaths)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    if (!pos.isSet())
        return false;

    if (bgType == BATTLEGROUND_WS)
    {
        if (wsJumpDown())
            return true;
    }
    else if (bgType == BATTLEGROUND_AV)
    {
        // get bots out of cave when they respawn there (otherwise path selection happens while they're deep within cave
        // and the results arent good)
        Position const caveSpawn = bot->GetTeamId() == TEAM_ALLIANCE ? AV_CAVE_SPAWN_ALLIANCE : AV_CAVE_SPAWN_HORDE;
        if (bot->GetDistance(caveSpawn) < 16.0f)
        {
            return moveToStart(true);
        }
    }
    else if (bgType == BATTLEGROUND_EY)
    {
        if (eyJumpDown())
            return true;
    }

    float chosenPathScore = FLT_MAX;  // lower score is better
    BattleBotPath* chosenPath = nullptr;
    uint32 chosenPathPoint = 0;
    bool chosenPathReverse = false;

    float botDistanceLimit = 50.0f;         // limit for how far path can be from bot
    float botDistanceScoreSubtract = 8.0f;  // path score modifier - lower = less likely to chose a further path (it's
                                            // basically the distance from bot that's ignored)
    float botDistanceScoreMultiply =
        3.0f;  // path score modifier - higher = less likely to chose a further path (it's basically a multiplier on
               // distance from bot - makes distance from bot more signifcant than distance from destination)

    if (bgType == BATTLEGROUND_IC)
    {
        // botDistanceLimit = 80.0f;
        botDistanceScoreMultiply = 8.0f;
        botDistanceScoreSubtract = 2.0f;
    }
    else if (bgType == BATTLEGROUND_AV)
    {
        botDistanceLimit = 80.0f;
        botDistanceScoreMultiply = 8.0f;
    }
    else if (bgType == BATTLEGROUND_AB || bgType == BATTLEGROUND_EY || bgType == BATTLEGROUND_WS)
    {
        botDistanceScoreSubtract = 2.0f;
        botDistanceScoreMultiply = 4.0f;
    }

    // uint32 index = -1;
    // uint32 chosenPathIndex = -1;
    for (auto const& path : vPaths)
    {
        // index++;
        // TODO need to remove sqrt from these two and distToBot but it totally throws path scoring out of
        // whack if you do that without changing how its implemented (I'm amazed it works as well as it does
        // using sqrt'ed distances)
        // In a reworked version maybe compare the differences of path distances to point (ie: against best path)
        // or maybe ratio's (where if a path end is twice the difference in distance from destination we basically
        // use that to multiply the total score?
        BattleBotWaypoint& startPoint = ((*path)[0]);
        float const startPointDistToDestination =
            sqrt(Position(pos.x, pos.y, pos.z, 0.f).GetExactDist(startPoint.x, startPoint.y, startPoint.z));
        BattleBotWaypoint& endPoint = ((*path)[path->size() - 1]);
        float const endPointDistToDestination =
            sqrt(Position(pos.x, pos.y, pos.z, 0.f).GetExactDist(endPoint.x, endPoint.y, endPoint.z));

        bool reverse = startPointDistToDestination < endPointDistToDestination;

        // dont travel reverse if it's a reverse paths
        if (reverse && std::find(vPaths_NoReverseAllowed.begin(), vPaths_NoReverseAllowed.end(), path) !=
                           vPaths_NoReverseAllowed.end())
            continue;

        int closestPointIndex = -1;
        float closestPointDistToBot = FLT_MAX;
        for (uint32 i = 0; i < path->size(); i++)
        {
            BattleBotWaypoint& waypoint = ((*path)[i]);
            float const distToBot = sqrt(bot->GetDistance(waypoint.x, waypoint.y, waypoint.z));
            if (closestPointDistToBot > distToBot)
            {
                closestPointDistToBot = distToBot;
                closestPointIndex = i;
            }
        }

        // don't pick path where bot is already closest to the paths closest point to target (it means path cant lead it
        // anywhere) don't pick path where closest point is too far away
        if (closestPointIndex == (reverse ? 0 : path->size() - 1) || closestPointDistToBot > botDistanceLimit)
            continue;

        // creates a score based on dist-to-bot and dist-to-destination, where lower is better, and dist-to-bot is more
        // important (when its beyond a certain distance) dist-to-bot is more important because otherwise they cant
        // reach it at all (or will fly through air with MM::MovePoint()), also bot may need to use multiple paths (one
        // after another) anyway
        float distToDestination = reverse ? startPointDistToDestination : endPointDistToDestination;
        float pathScore = (closestPointDistToBot < botDistanceScoreSubtract
                               ? 0.0f
                               : ((closestPointDistToBot - botDistanceScoreSubtract) * botDistanceScoreMultiply)) +
                          distToDestination;

        // LOG_INFO("playerbots", "bot={}\t{:6.1f}\t{:4.1f}\t{:4.1f}\t{}", bot->GetName(), pathScore,
        // closestPointDistToBot, distToDestination, vPaths_AB_name[pathNum]);

        if (chosenPathScore > pathScore)
        {
            chosenPathScore = pathScore;
            chosenPath = path;
            chosenPathPoint = closestPointIndex;
            chosenPathReverse = reverse;
            // chosenPathIndex = index;
        }
    }

    if (!chosenPath)
        return false;

    // LOG_INFO("playerbots", "{} bot={} path={}", (bot->GetTeamId() == TEAM_HORDE ? "HORDE" : "ALLIANCE"),
    // bot->GetName(), chosenPathIndex);

    return moveToObjectiveWp(chosenPath, chosenPathPoint, chosenPathReverse);
}

bool BGTactics::resetObjective()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // Adjust role-change chance based on battleground type
    uint32 oddsToChangeRole = 1;  // default low
    BattlegroundTypeId bgType = bg->GetBgTypeID();

    if (bgType == BATTLEGROUND_WS)
        oddsToChangeRole = 2;
    else if (bgType == BATTLEGROUND_EY || bgType == BATTLEGROUND_IC || bgType == BATTLEGROUND_AB)
        oddsToChangeRole = 1;
    else if (bgType == BATTLEGROUND_AV)
        oddsToChangeRole = 0;

    bool isCarryingFlag =
        bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) ||
        bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL);

    // Keep current role unless changed by strategy logic (no randomness)

    // Reset objective position
    PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    pos.Reset();
    posMap["bg objective"] = pos;
    lastCaptureInterruptGuid.Clear();

    return selectObjective(true);
}

bool BGTactics::moveToObjectiveWp(BattleBotPath* const& currentPath, uint32 currentPoint, bool reverse)
{
    if (!currentPath)
        return false;

    uint32 const lastPointInPath = reverse ? 0 : ((*currentPath).size() - 1);

    // NOTE: can't use IsInCombat() when in vehicle as player is stuck in combat forever while in vehicle (ac bug?)
    bool inCombat = bot->GetVehicle() ? (bool)AI_VALUE(Unit*, "enemy player target") : bot->IsInCombat();
    if (currentPoint == lastPointInPath || (inCombat && !PlayerHasFlag::IsCapturingFlag(bot)) || !bot->IsAlive())
    {
        // Path is over.
        // std::ostringstream out;
        // out << "Reached path end!";
        // bot->Say(out.str(), LANG_UNIVERSAL);
        resetObjective();
        return false;
    }

    uint32 currPoint = currentPoint;

    if (reverse)
        currPoint--;
    else
        currPoint++;

    uint32 nPoint = reverse ? std::max((int)(currPoint - 1), 0)
                            : std::min((uint32)(currPoint + 1), lastPointInPath);
    if (reverse && nPoint < 0)
        nPoint = 0;

    BattleBotWaypoint& nextPoint = currentPath->at(nPoint);

    // std::ostringstream out;
    // out << "WP: ";
    // reverse ? out << currPoint << " <<< -> " << nPoint : out << currPoint << ">>> ->" << nPoint;
    // out << ", " << nextPoint.x << ", " << nextPoint.y << " Path Size: " << currentPath->size() << ", Dist: " <<
    // sServerFacade->GetDistance2d(bot, nextPoint.x, nextPoint.y); bot->Say(out.str(), LANG_UNIVERSAL);

    return MoveTo(bot->GetMapId(), nextPoint.x, nextPoint.y, nextPoint.z);
}

bool BGTactics::startNewPathBegin(std::vector<BattleBotPath*> const& vPaths)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
        return false;

    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    if (!pos.isSet())
        return false;

    // Important for eye of the storm bg, so that bots stay near bg objective (see selectObjective for more Info)
    if (bot->GetDistance(pos.x, pos.y, pos.z) < 25.0f)
        return false;

    struct AvailablePath
    {
        AvailablePath(BattleBotPath* pPath_, bool reverse_) : pPath(pPath_), reverse(reverse_) {}

        BattleBotPath* pPath = nullptr;
        bool reverse = false;
    };

    std::vector<AvailablePath> availablePaths;
    for (auto const& pPath : vPaths)
    {
        // TODO remove sqrt
        BattleBotWaypoint* pStart = &((*pPath)[0]);
        if (sqrt(bot->GetDistance(pStart->x, pStart->y, pStart->z)) < INTERACTION_DISTANCE)
            availablePaths.emplace_back(AvailablePath(pPath, false));

        // Some paths are not allowed backwards.
        if (std::find(vPaths_NoReverseAllowed.begin(), vPaths_NoReverseAllowed.end(), pPath) !=
            vPaths_NoReverseAllowed.end())
            continue;

        // TODO remove sqrt
        BattleBotWaypoint* pEnd = &((*pPath)[(*pPath).size() - 1]);
        if (sqrt(bot->GetDistance(pEnd->x, pEnd->y, pEnd->z)) < INTERACTION_DISTANCE)
            availablePaths.emplace_back(AvailablePath(pPath, true));
    }

    if (availablePaths.empty())
        return false;

    float bestScore = FLT_MAX;
    AvailablePath const* chosenPath = nullptr;
    for (auto const& path : availablePaths)
    {
        BattleBotWaypoint* pNode = path.reverse ? &((*path.pPath)[(*path.pPath).size() - 1]) : &((*path.pPath)[0]);
        float dx = pos.x - pNode->x;
        float dy = pos.y - pNode->y;
        float distToObjective = sqrt(dx * dx + dy * dy);
        if (distToObjective < bestScore)
        {
            bestScore = distToObjective;
            chosenPath = &path;
        }
    }

    if (!chosenPath)
        return false;

    BattleBotPath* currentPath = chosenPath->pPath;
    bool reverse = chosenPath->reverse;
    uint32 currentPoint = reverse ? currentPath->size() - 1 : 0;

    return moveToObjectiveWp(currentPath, currentPoint, reverse);
}

bool BGTactics::startNewPathFree(std::vector<BattleBotPath*> const& vPaths)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
        return false;

    PositionInfo pos = context->GetValue<PositionMap&>("position")->Get()["bg objective"];
    if (!pos.isSet())
        return false;

    // Important for eye of the storm bg, so that bots stay near bg objective (see selectObjective for more Info)
    if (bot->GetDistance(pos.x, pos.y, pos.z) < 25.0f)
        return false;

    BattleBotPath* pClosestPath = nullptr;
    uint32 closestPoint = 0;
    float closestDistance = FLT_MAX;

    for (auto const& pPath : vPaths)
    {
        for (uint32 i = 0; i < pPath->size(); i++)
        {
            BattleBotWaypoint& waypoint = ((*pPath)[i]);
            // TODO remove sqrt
            float const distanceToPoint = sqrt(bot->GetDistance(waypoint.x, waypoint.y, waypoint.z));
            if (distanceToPoint < closestDistance)
            {
                pClosestPath = pPath;
                closestPoint = i;
                closestDistance = distanceToPoint;
            }
        }
    }

    if (!pClosestPath)
        return false;

    BattleBotPath* currentPath = pClosestPath;
    bool reverse = false;
    uint32 currentPoint = closestPoint - 1;

    return moveToObjectiveWp(currentPath, currentPoint, reverse);
}

/**
 * @brief Handles flag/base capturing gameplay in battlegrounds
 *
 * This function manages the logic for capturing flags and bases in various battlegrounds.
 * It handles:
 * - Enemy detection and combat near objectives
 * - Coordination with friendly players who are capturing
 * - Different capture mechanics for each battleground type
 * - Proper positioning and movement
 *
 * @param vPaths Vector of possible paths the bot can take
 * @param vFlagIds Vector of flag/base GameObjects that can be captured
 * @return true if handling a flag/base action, false otherwise
 */
bool BGTactics::atFlag(std::vector<BattleBotPath*> const& vPaths, std::vector<uint32> const& vFlagIds)
{
    // Basic sanity checks
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // Get the actual BG type (in case of random BG)
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    // Initialize vectors for nearby objects and players
    GuidVector closeObjects;
    GuidVector closePlayers;
    float flagRange = 0.0f;

    // Eye of the Storm helpers used later when handling capture positioning
    BattlegroundEY* eyeBg = nullptr;
    GameObject* eyCenterFlag = nullptr;
    if (bgType == BATTLEGROUND_EY)
    {
        eyeBg = static_cast<BattlegroundEY*>(bg);
        if (eyeBg)
            eyCenterFlag = eyeBg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM);
    }

    // Set up appropriate search ranges and object lists based on BG type
    switch (bgType)
    {
        case BATTLEGROUND_AV:
        case BATTLEGROUND_AB:
        case BATTLEGROUND_IC:
        {
            // For territory control BGs, use standard interaction range
            closeObjects = *context->GetValue<GuidVector>("closest game objects");
            closePlayers = *context->GetValue<GuidVector>("closest friendly players");
            flagRange = INTERACTION_DISTANCE;
            break;
        }
        case BATTLEGROUND_WS:
        case BATTLEGROUND_EY:
        {
            // For flag carrying BGs, use wider range and ignore LOS
            closeObjects = *context->GetValue<GuidVector>("nearest game objects no los");
            closePlayers = *context->GetValue<GuidVector>("closest friendly players");
            flagRange = 25.0f;
            break;
        }
        default:
            break;
    }

    if (closeObjects.empty())
        return false;

    bool abortedCapture = false;
    auto isCaptureBlockedByAttacker = [&]()
    {
        if (lastCaptureInterruptGuid.IsEmpty())
            return false;

        Unit* attacker = ObjectAccessor::GetUnit(*bot, lastCaptureInterruptGuid);
        if (!attacker || !attacker->IsAlive())
        {
            lastCaptureInterruptGuid.Clear();
            return false;
        }

        bool attackerEngaged = attacker->GetVictim() == bot || bot->GetVictim() == attacker;
        if (!attackerEngaged && bot->GetDistance(attacker) > 80.0f)
        {
            lastCaptureInterruptGuid.Clear();
            return false;
        }

        return true;
    };
    auto keepStationaryWhileCapturing = [&](CurrentSpellTypes spellType)
    {
        Spell* currentSpell = bot->GetCurrentSpell(spellType);
        if (!currentSpell || !currentSpell->m_spellInfo || currentSpell->m_spellInfo->Id != SPELL_CAPTURE_BANNER)
            return false;

        // If the capture target is no longer available (another bot already captured it), stop channeling
        if (GameObject* targetFlag = currentSpell->m_targets.GetGOTarget())
        {
            if (!targetFlag->isSpawned() || targetFlag->GetGoState() != GO_STATE_READY)
            {
                bot->InterruptNonMeleeSpells(true);
                resetObjective();
                return false;
            }

            Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");
            if (enemyPlayer && enemyPlayer->IsAlive())
            {
                bool enemyThreat = enemyPlayer->GetVictim() == bot ||
                                   bot->GetVictim() == enemyPlayer ||
                                   enemyPlayer->IsWithinDistInMap(bot, 20.0f);
                if (enemyThreat && ShouldEngageInCombat(enemyPlayer))
                {
                    TeamId enemyTeam = bot->GetTeamId() == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
                    uint32 enemyCount = getPlayersInArea(enemyTeam, targetFlag->GetPosition(), 40.0f, true);
                    uint32 allyCount = getPlayersInArea(bot->GetTeamId(), targetFlag->GetPosition(), 40.0f, true);
                    bool underPressure =
                        enemyPlayer->GetVictim() == bot || bot->GetHealthPct() < sPlayerbotAIConfig->mediumHealth;

                    if (enemyCount > allyCount || underPressure)
                    {
                        bot->InterruptNonMeleeSpells(true);
                        resetObjective();
                        context->GetValue<Unit*>("current target")->Set(enemyPlayer);
                        lastCaptureInterruptGuid = enemyPlayer->GetGUID();
                        abortedCapture = true;
                        return false;
                    }
                }
            }
        }
        else
        {
            bot->InterruptNonMeleeSpells(true);
            resetObjective();
            return false;
        }

        if (bot->IsMounted())
        {
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }

        if (bot->IsInDisallowedMountForm())
        {
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        }

        if (bot->isMoving())
        {
            bot->StopMoving();
        }

        return true;
    };

    // If we are already channeling the capture spell, keep the bot stationary and dismounted
    bool isCapturing = keepStationaryWhileCapturing(CURRENT_CHANNELED_SPELL);
    if (!abortedCapture)
        isCapturing = isCapturing || keepStationaryWhileCapturing(CURRENT_GENERIC_SPELL);
    if (abortedCapture)
        return false;
    if (isCapturing)
        return true;

    if (isCaptureBlockedByAttacker())
        return false;

    // First identify which flag/base we're trying to interact with
    GameObject* targetFlag = nullptr;
    for (ObjectGuid const guid : closeObjects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
        {
            continue;
        }

        bool const isEyCenterFlag = eyeBg && eyCenterFlag && eyCenterFlag->GetGUID() == go->GetGUID();

        // Check if this object is a valid capture target
        std::vector<uint32>::const_iterator f = std::find(vFlagIds.begin(), vFlagIds.end(), go->GetEntry());
        if (f == vFlagIds.end() && !isEyCenterFlag)
        {
            continue;
        }

        // Verify the object is active and ready
        if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY)
        {
            continue;
        }

        // Check if we're in range (using double range for enemy detection)
        float const dist = bot->GetDistance(go);
        if (flagRange && dist > flagRange * 2.0f)
        {
            continue;
        }

        targetFlag = go;
        break;
    }

    // If we found a valid flag/base to interact with
    if (bgType != BATTLEGROUND_WS && bgType != BATTLEGROUND_AV)
    {
        if (targetFlag)
        {
            // Check for enemy players near the flag using bot's targeting system
            Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");
            if (enemyPlayer && enemyPlayer->IsAlive())
            {
                // If enemy is near the flag, assess the situation before engaging or capturing
                float enemyDist = enemyPlayer->GetDistance(targetFlag);
                if (enemyDist < flagRange * 2.0f)
                {
                    // Count players to determine if we should fight or try to capture
                    TeamId enemyTeam = bot->GetTeamId() == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
                    uint32 enemyCount = getPlayersInArea(enemyTeam, targetFlag->GetPosition(), 40.0f, true);
                    uint32 allyCount = getPlayersInArea(bot->GetTeamId(), targetFlag->GetPosition(), 40.0f, true);
                    
                    // If outnumbered, FIGHT instead of attempting capture
                    // This prevents bots from getting stuck in capture->interrupt loop
                    bool enemyThreat = enemyPlayer->GetVictim() == bot || bot->GetVictim() == enemyPlayer;
                    bool botInCombat = bot->IsInCombat();
                    if (enemyCount > allyCount && (enemyThreat || botInCombat))
                    {
                        // Set enemy as current target and let combat AI handle it
                        context->GetValue<Unit*>("current target")->Set(enemyPlayer);
                        if (enemyThreat)
                            lastCaptureInterruptGuid = enemyPlayer->GetGUID();
                        return false;
                    }
                    // If equal numbers or advantage, bot can try to capture with ally support
                    // (allies will defend while this bot captures)
                }
            }
        }
    }

    // Check if friendly players are already capturing
    if (!closePlayers.empty() && bgType != BATTLEGROUND_EY)
    {
        // Track number of friendly players capturing and the closest one
        uint32 numCapturing = 0;
        Unit* capturingPlayer = nullptr;
        for (auto& guid : closePlayers)
        {
            if (Unit* pFriend = botAI->GetUnit(guid))
            {
                // Check if they're casting or channeling the capture spell
                Spell* spell = pFriend->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (!spell)
                {
                    spell = pFriend->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                }

                if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == SPELL_CAPTURE_BANNER)
                {
                    numCapturing++;
                    capturingPlayer = pFriend;
                }
            }
        }

        // If friendlies are capturing, stay to defend but don't capture
        if (numCapturing > 0 && capturingPlayer && bot->GetGUID() != capturingPlayer->GetGUID())
        {
            // Move away if too close to avoid crowding
            if (bot->GetDistance2d(capturingPlayer) < 3.0f)
            {
                float angle = bot->GetAngle(capturingPlayer);
                float x = bot->GetPositionX() + 5.0f * cos(angle);
                float y = bot->GetPositionY() + 5.0f * sin(angle);
                MoveTo(bot->GetMapId(), x, y, bot->GetPositionZ());
            }

            // Reset objective and take new path for defending
            resetObjective();
            if (!startNewPathBegin(vPaths))
                moveToObjective(true);
            return true;
        }
    }

    // Area is clear of enemies and no friendlies are capturing
    // Proceed with capture mechanics
    for (ObjectGuid const guid : closeObjects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
            continue;

        bool const isEyCenterFlag = eyeBg && eyCenterFlag && eyCenterFlag->GetGUID() == go->GetGUID();

        // Validate this is a capture target
        std::vector<uint32>::const_iterator f = std::find(vFlagIds.begin(), vFlagIds.end(), go->GetEntry());
        if (f == vFlagIds.end() && !isEyCenterFlag)
            continue;

        // Check object is active
        if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY)
            continue;

        // Verify we can interact with it
        if (!bot->CanUseBattlegroundObject(go) && bgType != BATTLEGROUND_WS)
            continue;

        float const dist = bot->GetDistance(go);
        if (flagRange && dist > flagRange)
            continue;

        // Special handling for WSG and EY base flags
        bool isWsBaseFlag = bgType == BATTLEGROUND_WS && go->GetEntry() == vFlagsWS[bot->GetTeamId()];
        bool isEyBaseFlag = bgType == BATTLEGROUND_EY && go->GetEntry() == vFlagsEY[0];

        // Ensure bots are inside the Eye of the Storm capture circle before casting
        if (bgType == BATTLEGROUND_EY)
        {
            GameObject* captureFlag = (isEyBaseFlag && eyCenterFlag) ? eyCenterFlag : go;
            float const requiredRange = 2.5f;
            if (!bot->IsWithinDistInMap(captureFlag, requiredRange))
            {
                // Stay mounted while relocating to avoid mount/dismount loops
                return MoveTo(bot->GetMapId(), captureFlag->GetPositionX(), captureFlag->GetPositionY(),
                              captureFlag->GetPositionZ());
            }

            // Once inside the circle, dismount and stop before starting the channel
            if (bot->IsMounted())
            {
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            }

            if (bot->IsInDisallowedMountForm())
            {
                bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            }

            if (bot->isMoving())
            {
                bot->StopMoving();
            }
        }

        // Don't capture own flag in WSG unless carrying enemy flag
        if (isWsBaseFlag && bgType == BATTLEGROUND_WS &&
            !(bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG)))
            continue;

        // Handle capture mechanics based on BG type
        switch (bgType)
        {
            case BATTLEGROUND_AV:
            case BATTLEGROUND_AB:
            case BATTLEGROUND_IC:
            {
                // Prevent capturing from inside flag pole
                if (dist == 0.0f)
                {
                    float const moveDist = bot->GetObjectSize() + go->GetObjectSize() + 0.1f;
                    bool const moveLeft = (bot->GetGUID().GetCounter() % 2) == 0;
                    float const offset = moveLeft ? -moveDist : moveDist;
                    return MoveTo(bot->GetMapId(), go->GetPositionX() + offset,
                                  go->GetPositionY() + offset, go->GetPositionZ());
                }

                // Dismount before capturing
                if (bot->IsMounted())
                    bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

                if (bot->IsInDisallowedMountForm())
                    bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

                // Cast the capture spell
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(SPELL_CAPTURE_BANNER);
                if (!spellInfo)
                    return false;

                Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);
                spell->m_targets.SetGOTarget(go);
                spell->prepare(&spell->m_targets);

                botAI->WaitForSpellCast(spell);

                resetObjective();
                return true;
            }
            case BATTLEGROUND_WS:
            {
                if (dist < INTERACTION_DISTANCE)
                {
                    // Handle flag capture at base
                    if (isWsBaseFlag)
                    {
                        if (bot->GetTeamId() == TEAM_HORDE)
                        {
                            WorldPacket data(CMSG_AREATRIGGER);
                            data << uint32(BG_WS_TRIGGER_HORDE_FLAG_SPAWN);
                            bot->GetSession()->HandleAreaTriggerOpcode(data);
                        }
                        else
                        {
                            WorldPacket data(CMSG_AREATRIGGER);
                            data << uint32(BG_WS_TRIGGER_ALLIANCE_FLAG_SPAWN);
                            bot->GetSession()->HandleAreaTriggerOpcode(data);
                        }
                        return true;
                    }

                    // Dismount before picking up flag
                    if (bot->IsMounted())
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

                    if (bot->IsInDisallowedMountForm())
                        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

                    // Pick up the flag
                    WorldPacket data(CMSG_GAMEOBJ_USE);
                    data << go->GetGUID();
                    bot->GetSession()->HandleGameObjectUseOpcode(data);

                    resetObjective();
                    return true;
                }
                else
                {
                    // Move to flag if not in range
                    return MoveTo(bot->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
                }
            }
            case BATTLEGROUND_EY:
            {  // Handle Netherstorm flag capture requiring a channel
                if (dist < INTERACTION_DISTANCE)
                {
                    // Dismount before interacting
                    if (bot->IsMounted())
                    {
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    }

                    if (bot->IsInDisallowedMountForm())
                    {
                        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
                    }

                    // Handle center flag differently (requires spell cast)
                    if (isEyCenterFlag)
                    {
                        for (uint8 type = CURRENT_MELEE_SPELL; type <= CURRENT_CHANNELED_SPELL; ++type)
                        {
                            if (Spell* currentSpell = bot->GetCurrentSpell(static_cast<CurrentSpellTypes>(type)))
                            {
                                // m_spellInfo may be null in some states: protect access
                                if (currentSpell->m_spellInfo && currentSpell->m_spellInfo->Id == SPELL_CAPTURE_BANNER)
                                {
                                    bot->StopMoving();
                                    botAI->SetNextCheckDelay(500);
                                    return true;
                                }
                            }
                        }

                        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(SPELL_CAPTURE_BANNER);
                        if (!spellInfo)
                            return false;

                        Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);
                        spell->m_targets.SetGOTarget(go);

                        bot->StopMoving();
                        spell->prepare(&spell->m_targets);

                        botAI->WaitForSpellCast(spell);
                        resetObjective();
                        return true;
                    }

                    // Pick up dropped flag
                    WorldPacket data(CMSG_GAMEOBJ_USE);
                    data << go->GetGUID();
                    bot->GetSession()->HandleGameObjectUseOpcode(data);

                    resetObjective();
                    return true;
                }
                else
                {
                    // Move to flag if not in range
                    return MoveTo(bot->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
                }
            }
            default:
                break;
        }
    }

    return false;
}

bool BGTactics::flagTaken()
{
    BattlegroundWS* bg = (BattlegroundWS*)bot->GetBattleground();
    if (!bg)
        return false;

    return !bg->GetFlagPickerGUID(bg->GetOtherTeamId(bot->GetTeamId())).IsEmpty();
}

bool BGTactics::teamFlagTaken()
{
    BattlegroundWS* bg = (BattlegroundWS*)bot->GetBattleground();
    if (!bg)
        return false;

    return !bg->GetFlagPickerGUID(bot->GetTeamId()).IsEmpty();
}

bool BGTactics::protectFC()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    Unit* teamFC = AI_VALUE(Unit*, "team flag carrier");

    if (!teamFC || teamFC == bot)
    {
        return false;
    }

    if (!bot->IsInCombat() && !bot->IsWithinDistInMap(teamFC, 20.0f))
    {
        // Get the flag carrier's position
        float fcX = teamFC->GetPositionX();
        float fcY = teamFC->GetPositionY();
        float fcZ = teamFC->GetPositionZ();
        uint32 mapId = bot->GetMapId();

        return MoveNear(mapId, fcX, fcY, fcZ, 5.0f, MovementPriority::MOVEMENT_NORMAL);
    }

    return false;
}

bool BGTactics::useBuff()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    GuidVector closeObjects = AI_VALUE(GuidVector, "nearest game objects no los");
    if (closeObjects.empty())
        return false;

    bool needRegen = bot->GetHealthPct() < sPlayerbotAIConfig->mediumHealth ||
                     (AI_VALUE2(bool, "has mana", "self target") &&
                      AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig->mediumMana);
    bool needSpeed = (bgType != BATTLEGROUND_WS || bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) ||
                      bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) || bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL)) ||
                     !(teamFlagTaken() || flagTaken());
    bool foundBuff = false;

    for (ObjectGuid const guid : closeObjects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
            continue;

        if (!go->isSpawned())
            continue;

        // use speed buff only if close
        if (sServerFacade->IsDistanceGreaterThan(sServerFacade->GetDistance2d(bot, go),
                                                 go->GetEntry() == Buff_Entries[0] ? 20.0f : 50.0f))
            continue;

        if (needSpeed && go->GetEntry() == Buff_Entries[0])
            foundBuff = true;

        if (needRegen && go->GetEntry() == Buff_Entries[1])
            foundBuff = true;

        // do not move to Berserk buff if bot is healer or has flag
        if (!(bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
              bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL)) &&
            !botAI->IsHeal(bot) && go->GetEntry() == Buff_Entries[2])
            foundBuff = true;

        if (foundBuff)
        {
            // std::ostringstream out;
            // out << "Moving to buff...";
            // bot->Say(out.str(), LANG_UNIVERSAL);
            return MoveTo(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
        }
    }

    return false;
}

uint32 BGTactics::getPlayersInArea(TeamId teamId, Position point, float range, bool combat)
{
    uint32 defCount = 0;

    if (!bot->InBattleground())
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 0;

    for (auto& guid : bg->GetBgMap()->GetPlayers())
    {
        Player* player = guid.GetSource();
        if (!player)
            continue;

        if (player->IsAlive() && (teamId == TEAM_NEUTRAL || teamId == player->GetTeamId()))
        {
            if (!combat && player->IsInCombat())
                continue;

            if (sServerFacade->GetDistance2d(player, point.GetPositionX(), point.GetPositionY()) < range)
                ++defCount;
        }
    }

    return defCount;
}

// check Isle of Conquest Keep position
bool BGTactics::IsLockedInsideKeep()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType != BATTLEGROUND_IC)
        return false;

    bool isInside = false;
    if (bot->GetTeamId() == TEAM_ALLIANCE && bot->GetPositionX() < 410.0f && bot->GetPositionY() > -900.0f &&
        bot->GetPositionY() < -765.0f)
        isInside = true;
    if (bot->GetTeamId() == TEAM_HORDE && bot->GetPositionX() > 1153.0f && bot->GetPositionY() > -849.0f &&
        bot->GetPositionY() < -679.0f)
        isInside = true;

    if (!isInside)
        return false;

    GuidVector closeObjects;
    closeObjects = *context->GetValue<GuidVector>("nearest game objects no los");
    if (closeObjects.empty())
        return moveToStart(true);

    GameObject* closestPortal = nullptr;
    float closestDistance = 100.0f;
    bool gateLock = false;

    // check inner gates status
    // ALLIANCE
    if (bot->GetTeamId() == TEAM_ALLIANCE)
    {
        if (GameObject* go = bg->GetBGObject(BG_IC_GO_DOODAD_PORTCULLISACTIVE02))
        {
            if (go->isSpawned())
            {
                gateLock = go->getLootState() != GO_ACTIVATED;
            }
            else
            {
                gateLock = false;
            }
        }
    }
    // HORDE
    if (bot->GetTeamId() == TEAM_HORDE)
    {
        if (GameObject* go = bg->GetBGObject(BG_IC_GO_HORDE_KEEP_PORTCULLIS))
        {
            if (go->isSpawned())
            {
                gateLock = go->getLootState() != GO_ACTIVATED;
            }
            else
            {
                gateLock = false;
            }
        }
    }

    for (ObjectGuid const& guid : closeObjects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
            continue;

        // ALLIANCE
        // get closest portal
        if (bot->GetTeamId() == TEAM_ALLIANCE && go->GetEntry() == GO_TELEPORTER_4)
        {
            float tempDist = sServerFacade->GetDistance2d(bot, go->GetPositionX(), go->GetPositionY());

            if (sServerFacade->IsDistanceLessThan(tempDist, closestDistance))
            {
                closestDistance = tempDist;
                closestPortal = go;
            }
        }

        // HORDE
        // get closest portal
        if (bot->GetTeamId() == TEAM_HORDE && go->GetEntry() == GO_TELEPORTER_2)
        {
            float tempDist = sServerFacade->GetDistance2d(bot, go->GetPositionX(), go->GetPositionY());

            if (sServerFacade->IsDistanceLessThan(tempDist, closestDistance))
            {
                closestDistance = tempDist;
                closestPortal = go;
            }
        }
    }

    // portal not found, move closer
    if (gateLock && !closestPortal)
        return moveToStart(true);

    // portal not found, move closer
    if (!gateLock && !closestPortal)
        return moveToStart(true);

    // nothing found, allow move through
    if (!gateLock || !closestPortal)
        return false;

    // portal found
    if (closestPortal)
    {
        // if close
        if (bot->IsWithinDistInMap(closestPortal, INTERACTION_DISTANCE))
        {
            WorldPacket data(CMSG_GAMEOBJ_USE);
            data << closestPortal->GetGUID();
            bot->GetSession()->HandleGameObjectUseOpcode(data);
            return true;
        }
        else
        {
            return MoveTo(bot->GetMapId(), closestPortal->GetPositionX(), closestPortal->GetPositionY(),
                          closestPortal->GetPositionZ());
        }
    }

    return moveToStart(true);

    return false;
}

// Game State Awareness Implementation
bool BGTactics::IsLosingBadly(Battleground* bg)
{
    if (!bg)
        return false;
    
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    TeamId myTeam = bot->GetTeamId();
    
    switch (bgType)
    {
        case BATTLEGROUND_AB:
        case BATTLEGROUND_EY:
        {
            // Check score differential
            uint32 myScore = bg->GetTeamScore(myTeam);
            uint32 enemyScore = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Losing badly if behind by 500+ points
            if (enemyScore > myScore + 500)
                return true;
            
            // Or if enemy has significantly more bases
            uint32 myBases = GetTeamBasesControlled(bg, myTeam);
            uint32 enemyBases = GetTeamBasesControlled(bg, bg->GetOtherTeamId(myTeam));
            if (enemyBases >= 4 && myBases <= 1)
                return true;
            
            break;
        }
        case BATTLEGROUND_WS:
        {
            // Check flag captures
            uint32 myScore = bg->GetTeamScore(myTeam);
            uint32 enemyScore = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Losing badly if behind by 2 captures
            if (enemyScore >= myScore + 2)
                return true;
            
            break;
        }
        case BATTLEGROUND_AV:
        {
            // Check reinforcements
            uint32 myReinforcements = bg->GetTeamScore(myTeam);
            uint32 enemyReinforcements = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Losing badly if down by 200+ reinforcements
            if (myReinforcements + 200 < enemyReinforcements)
                return true;
            
            break;
        }
        default:
            break;
    }
    
    return false;
}

bool BGTactics::IsWinning(Battleground* bg)
{
    if (!bg)
        return false;
    
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    TeamId myTeam = bot->GetTeamId();
    
    switch (bgType)
    {
        case BATTLEGROUND_AB:
        case BATTLEGROUND_EY:
        {
            uint32 myScore = bg->GetTeamScore(myTeam);
            uint32 enemyScore = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Winning if ahead by 300+ points
            if (myScore > enemyScore + 300)
                return true;
            
            // Or controlling majority of bases
            uint32 myBases = GetTeamBasesControlled(bg, myTeam);
            if (myBases >= 4)
                return true;
            
            break;
        }
        case BATTLEGROUND_WS:
        {
            uint32 myScore = bg->GetTeamScore(myTeam);
            uint32 enemyScore = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Winning if ahead in captures
            if (myScore >= enemyScore + 2)
                return true;
            
            break;
        }
        case BATTLEGROUND_AV:
        {
            uint32 myReinforcements = bg->GetTeamScore(myTeam);
            uint32 enemyReinforcements = bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
            
            // Winning if up by 150+ reinforcements
            if (myReinforcements > enemyReinforcements + 150)
                return true;
            
            break;
        }
        default:
            break;
    }
    
    return false;
}

bool BGTactics::ShouldPlayAggressive(Battleground* bg)
{
    if (!bg)
        return false;
    
    // Play aggressive if losing badly (need to make comeback)
    if (IsLosingBadly(bg))
        return true;
    
    // Play aggressive early game (first 3 minutes)
    uint32 elapsed = GameTime::GetGameTime().count() - bg->GetStartTime();
    if (elapsed < 180) // First 3 minutes
        return true;
    
    // Don't play aggressive if winning significantly
    if (IsWinning(bg))
        return false;
    
    // Default: balanced approach
    return false;
}

bool BGTactics::ShouldPlayDefensive(Battleground* bg)
{
    if (!bg)
        return false;
    
    // Play defensive if winning
    if (IsWinning(bg))
        return true;
    
    // Play defensive late game with lead
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    TeamId myTeam = bot->GetTeamId();
    uint32 myScore = bg->GetTeamScore(myTeam);
    uint32 enemyScore =bg->GetTeamScore(bg->GetOtherTeamId(myTeam));
    
    uint32 elapsed = GameTime::GetGameTime().count() - bg->GetStartTime();
    if (elapsed > 600 && myScore > enemyScore) // Last 10+ min with lead
        return true;
    
    return false;
}

uint32 BGTactics::GetTeamBasesControlled(Battleground* bg, TeamId teamId)
{
    if (!bg)
        return 0;
    
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    uint32 basesControlled = 0;
    
    // Currently only implemented for Arathi Basin
    // EotS requires access to BattlegroundEY-specific methods which is complex
    if (bgType == BATTLEGROUND_AB)
    {
        // Count controlled bases in Arathi Basin by checking banner spawns
        // This is a simplified check - actual implementation may need refinement
        for (uint8 node = 0; node < 5; ++node)
        {
            // Check if base has alliance or horde banner spawned
            // Note: This is approximate - may need adjustment based on actual AB implementation
            GameObject* banner = bg->GetBGObject(node * 8 + (teamId == TEAM_ALLIANCE ? 0 : 1));
            if (banner && banner->isSpawned())
                basesControlled++;
        }
    }
    
    return basesControlled;
}

// Opening Strategy Implementation
bool BGTactics::IsGameOpening(Battleground* bg)
{
    if (!bg)
        return false;
    
    // Check if BG is started
    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    // Use sWorld->GetGameTime() if GameTime::GetGameTime() is problematic, 
    // but existing code used GameTime::GetGameTime().count().
    // We stick to the existing method that compiled before, but change threshold.
    uint32 elapsed = GameTime::GetGameTime().count() - bg->GetStartTime();
    return elapsed < 45; // Reduced from 120s to 45s for tighter opening rush
}

uint32 BGTactics::GetAssignedOpeningNode(Battleground* bg)
{
    // Distribution:
    // Alliance: 10% Stables, 30% Lumber Mill, 45% Blacksmith, 15% Gold Mine (stealth favored)
    // Horde: 10% Farm, 10% Lumber Mill, 75% Blacksmith, 5% Gold Mine (stealth favored)
    
    uint32 roll = (bot->GetGUID().GetCounter() + (bg ? bg->GetInstanceID() : 0)) % 100;
    TeamId team = bot->GetTeamId();
    bool isStealth = (bot->getClass() == CLASS_ROGUE || bot->getClass() == CLASS_DRUID);
    
    if (team == TEAM_ALLIANCE)
    {
        if (isStealth && roll < 40)
            return AB_NODE_GOLD_MINE;               // Stealth ninja
        if (roll < 10) return AB_NODE_STABLES;      // 10%
        if (roll < 40) return AB_NODE_LUMBER_MILL;  // 30%
        if (roll < 85) return AB_NODE_BLACKSMITH;   // 45%
        return AB_NODE_GOLD_MINE;                   // 15%
    }
    else // HORDE
    {
        if (isStealth && roll < 20)
            return AB_NODE_GOLD_MINE;               // Stealth ninja
        if (roll < 10) return AB_NODE_FARM;         // 10%
        if (roll < 20) return AB_NODE_LUMBER_MILL;  // 10%
        if (roll < 95) return AB_NODE_BLACKSMITH;   // 75%
        return AB_NODE_GOLD_MINE;                   // 5%
    }
}

bool BGTactics::ShouldRushContestedObjectives()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;
    
    // During opening (first 2 min), rush contested objectives
    if (IsGameOpening(bg))
    {
        uint32 assignedNode = GetAssignedOpeningNode(bg);
        TeamId myTeam = bot->GetTeamId();
        
        // Skip home base defense unless specifically assigned
        if (myTeam == TEAM_ALLIANCE && assignedNode != AB_NODE_STABLES)
            return true; // Don't defend Stables unless assigned
        else if (myTeam == TEAM_HORDE && assignedNode != AB_NODE_FARM)
            return true; // Don't defend Farm unless assigned
    }
    
    return false;
}

bool BGTactics::MoveToABNode(uint32 nodeIndex)
{
    Position targetPos;
    
    switch (nodeIndex)
    {
        case AB_NODE_STABLES:
            targetPos = AB_NODE_POS_STABLES;
            break;
        case AB_NODE_BLACKSMITH:
            targetPos = AB_NODE_POS_BLACKSMITH;
            break;
        case AB_NODE_FARM:
            targetPos = AB_NODE_POS_FARM;
            break;
        case AB_NODE_LUMBER_MILL:
            targetPos = AB_NODE_POS_LUMBER_MILL;
            break;
        case AB_NODE_GOLD_MINE:
            targetPos = AB_NODE_POS_GOLD_MINE;
            break;
        default:
            return false;
    }
    
    return MoveTo(bot->GetMapId(), targetPos.GetPositionX(), targetPos.GetPositionY(), targetPos.GetPositionZ());
}

// Objective Focus System Implementation

// Local helper to check flag carrier status (same logic as in TargetValue.cpp)
static bool IsFlagCarrierBG(Unit* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;
    
    Player* player = unit->ToPlayer();
    
    // WSG flag carrier auras
    if (player->HasAura(23333) || player->HasAura(23335))
        return true;
    
    // EotS flag carrier aura
    if (player->HasAura(34976))
        return true;
    
    return false;
}

bool BGTactics::ShouldEngageInCombat(Unit* target)
{
    if (!target) return false;
    
    // 1. ALWAYS fight if we are defending a node we own/contest
    if (IsDefendingObjective())
        return true;
        
    // 2. NEVER fight if we are carrying a flag (objective is to run)
    if (HasCriticalObjective())
        return false;
        
    // 3. FIGHT if we are capturing a node and enemy tries to stop us
    // (If we are near a node we want, and enemy is there, we must kill them to cap)
    if (IsNearObjective(30.0f))
        return true;
        
    // 4. Default: Fight if attacked or target is close
    if (bot->GetVictim() == target || target->GetVictim() == bot)
        return true;

    // 5. Existing logic overrides:
    if (!target->IsPlayer()) return true; // PvE always ok
    
    Battleground* bg = bot->GetBattleground();
    if (!bg) return true; // Not in BG
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    Player* enemy = target->ToPlayer();
    
    // ALWAYS ENGAGE: Flag carriers
    if (IsFlagCarrierBG(enemy)) return true;
    
    // ALWAYS ENGAGE: Target attacking our flag carrier
    Unit* fcVictim = enemy->GetVictim();
    if (fcVictim && fcVictim->IsPlayer())
    {
        Player* victimPlayer = fcVictim->ToPlayer();
        if (victimPlayer->GetTeamId() == bot->GetTeamId() && IsFlagCarrierBG(victimPlayer))
            return true; 
    }
    
    // IoC: avoid field PvP unless near objectives or vehicles
    if (bgType == BATTLEGROUND_IC)
    {
        bool nearObjective = IsNearObjective(40.0f);
        bool protectingVehicle = ShouldProtectSiegeEngine();
        bool drivingVehicle = IsDrivingSiegeEngine() || IsVehiclePassenger();

        if (target->IsPlayer())
        {
            Unit* victim = target->GetVictim();
            if (victim && victim->IsVehicle() && victim->GetFaction() == bot->GetFaction())
                return true;
        }

        if (HasSeaforiumCharge())
            return nearObjective || protectingVehicle || drivingVehicle;

        if (nearObjective || protectingVehicle || drivingVehicle)
            return true;

        return false;
    }

    // Default yes
    return true; 
}

bool BGTactics::IsNearObjective(float maxDistance)
{
    // SAFETY FIX: BgObjective can be a dangling pointer when BG objects despawn/change ownership
    // Removed unsafe pointer usage - use only position-based logic instead
    Battleground* bg = bot->GetBattleground();
    if (!bg) 
        return false;
    
    Position objectivePos = GetNearestObjectivePosition();
    if (objectivePos.GetPositionX() == 0.0f && objectivePos.GetPositionY() == 0.0f)
        return false; 
    
    return bot->GetDistance(objectivePos) <= maxDistance;
}

bool BGTactics::IsTargetThreateningObjective(Unit* target)
{
    if (!target)
        return false;
    
    Position objectivePos = GetNearestObjectivePosition();
    if (objectivePos.GetPositionX() == 0.0f && objectivePos.GetPositionY() == 0.0f)
        return false;
    
    // Target is near objective (within 40 yards)
    return target->GetDistance(objectivePos) <= 40.0f;
}

bool BGTactics::IsDefendingObjective()
{
    // If our objective is a node we own, we are defending
    // But how do we know if we own it? BgObjective is just a GO.
    // For now, check if we are near an objective and stationary.
    
    if (!IsNearObjective(30.0f))
        return false;
    
    // If we're moving slowly or stationary near objective, we're defending
    return !bot->isMoving() || bot->GetSpeed(MOVE_RUN) < 7.0f;
}

bool BGTactics::IsAttackingObjective()
{
    // Check if bot is moving toward an objective to capture
    // Simplified: if moving and within range of objective
    return bot->isMoving() && IsNearObjective(50.0f);
}

Position BGTactics::GetNearestObjectivePosition()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return Position();
    
    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    float nearestDist = 999999.0f;
    Position nearestPos;
    
    switch (bgType)
    {
        case BATTLEGROUND_WS:
        {
            // WSG: Flag rooms
            Position flags[] = {WS_FLAG_POS_ALLIANCE, WS_FLAG_POS_HORDE};
            for (auto& pos : flags)
            {
                float dist = bot->GetDistance(pos);
                if (dist < nearestDist)
                {
                    nearestDist = dist;
                    nearestPos = pos;
                }
            }
            break;
        }
        case BATTLEGROUND_AB:
        {
            // AB: All 5 bases
            Position bases[] = {
                AB_NODE_POS_STABLES,
                AB_NODE_POS_BLACKSMITH,
                AB_NODE_POS_FARM,
                AB_NODE_POS_LUMBER_MILL,
                AB_NODE_POS_GOLD_MINE
            };
            for (auto& pos : bases)
            {
                float dist = bot->GetDistance(pos);
                if (dist < nearestDist)
                {
                    nearestDist = dist;
                    nearestPos = pos;
                }
            }
            break;
        }
        case BATTLEGROUND_EY:
        {
            // EotS: Mid flag + 4 bases
            Position objectives[] = {
                {2174.782f, 1569.054f, 1160.361f, 0.0f}, // Mid flag
                {2047.19f, 1735.07f, 1187.91f, 0.0f},    // Fel Reaver
                {2047.19f, 1349.19f, 1189.0f, 0.0f},     // Blood Elf
                {2276.8f, 1400.41f, 1196.33f, 0.0f},     // Draenei Ruins
                {2282.102f, 1760.006f, 1189.707f, 0.0f}  // Mage Tower
            };
            for (auto& pos : objectives)
            {
                float dist = bot->GetDistance(pos);
                if (dist < nearestDist)
                {
                    nearestDist = dist;
                    nearestPos = pos;
                }
            }
            break;
        }
        default:
            break;
    }
    
    return nearestPos;
}

bool ArenaTactics::Execute(Event event)
{
    if (!bot->InBattleground())
    {
        bool IsRandomBot = sRandomPlayerbotMgr->IsRandomBot(bot->GetGUID().GetCounter());
        botAI->ChangeStrategy("-arena", BOT_STATE_COMBAT);
        botAI->ChangeStrategy("-arena", BOT_STATE_NON_COMBAT);
        botAI->ResetStrategies(!IsRandomBot);
        return false;
    }

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    if (bg->GetStatus() == STATUS_WAIT_LEAVE)
        return BGStatusAction::LeaveBG(botAI);

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    if (bot->isDead())
        return false;

    // Ruins slime rescue (pre-move): pull bots out of the slime pool quickly
    RescueFromRLSlime(bot, bg);
    // Ruins spawn tunnel rescue: nudge bots out of spawn corridors
    RescueFromRLSpawnTunnel(bot, bg);
    // Blade's Edge start rescue: pull bots off the spawn platform after gates open
    RescueFromBladeEdgeStart(bot, bg);

    // Ring of Valor: avoid clunky movement/attacks while the elevator is rising.
    // Once the platform reaches the top, normal combat logic resumes.
    if (bg->GetBgTypeID() == BATTLEGROUND_RV && bot->GetPositionZ() < 26.5f)
        return false;

    // Ring of Valor elevator cleanup: if we spawned below the platform, move up before acting
    if (bg->GetBgTypeID() == BATTLEGROUND_RV && bot->GetPositionZ() < 25.0f)
    {
        Position platform(764.6f, -283.8f, 28.3f);
        return MoveNear(bg->GetMapId(), platform.GetPositionX(), platform.GetPositionY(), platform.GetPositionZ(), 3.0f,
                        MovementPriority::MOVEMENT_NORMAL);
    }

    if (bot->isMoving())
        return false;

    // startup phase
    if (bg->GetStartDelayTime() > 0)
        return false;

    if (botAI->HasStrategy("collision", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-collision", BOT_STATE_NON_COMBAT);

    if (botAI->HasStrategy("buff", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-buff", BOT_STATE_NON_COMBAT);

    bool isHealerClass = bot->getClass() == CLASS_PRIEST || bot->getClass() == CLASS_DRUID ||
                         bot->getClass() == CLASS_SHAMAN || bot->getClass() == CLASS_PALADIN;
    bool isRangedClass = bot->getClass() == CLASS_MAGE || bot->getClass() == CLASS_WARLOCK ||
                         bot->getClass() == CLASS_HUNTER;

    ArenaOpenerInfo opener = AI_VALUE(ArenaOpenerInfo, "arena opener info");
    uint32 arenaFeatures = opener.featureFlags;

    if (opener.active)
        botAI->ChangeStrategy("arena opener", BOT_STATE_NON_COMBAT);
    else
        botAI->ChangeStrategy("-arena opener", BOT_STATE_NON_COMBAT);

    if (opener.combo != AO_NONE)
        botAI->ChangeStrategy("arena combo", BOT_STATE_NON_COMBAT);
    else
        botAI->ChangeStrategy("-arena combo", BOT_STATE_NON_COMBAT);

    if (bot->IsInCombat())
    {
        // Aggro-aware retreat when mana/heal low and multiple enemies
        if (IsUnderPressure() && bot->getPowerType() == POWER_MANA)
        {
            uint32 maxMana = bot->GetMaxPower(POWER_MANA);
            if (maxMana > 0 && bot->GetPower(POWER_MANA) < maxMana * 0.35f)
            {
                GuidVector enemies = AI_VALUE(GuidVector, "possible targets");
                float avgX = 0.0f, avgY = 0.0f;
                uint8 count = 0;
                for (ObjectGuid guid : enemies)
                {
                    Unit* unit = botAI->GetUnit(guid);
                    if (!unit || !unit->IsPlayer() || !unit->IsAlive())
                        continue;
                    avgX += unit->GetPositionX();
                    avgY += unit->GetPositionY();
                    ++count;
                }
                if (count > 0)
                {
                    avgX /= count;
                    avgY /= count;
                    float dx = bot->GetPositionX() - avgX;
                    float dy = bot->GetPositionY() - avgY;
                    float dist = sqrt(dx * dx + dy * dy);
                    if (dist < 1.0f)
                        dist = 1.0f;
                    float nx = bot->GetPositionX() + (dx / dist) * 6.0f;
                    float ny = bot->GetPositionY() + (dy / dist) * 6.0f;
                    MoveTo(bot->GetMapId(), nx, ny, bot->GetPositionZ(), false, true);
                    return true;
                }
            }
        }

        // Druid off-heal for self sustain (balance/feral in arenas) – travel form is banned in arena on 3.3.5
        if (bot->getClass() == CLASS_DRUID && bot->GetHealthPct() < 55.0f)
        {
            uint32 rejuv = bot->HasSpell(48441) ? 48441 : (bot->HasSpell(26982) ? 26982 : 0);
            if (rejuv && !bot->HasAura(rejuv))
            {
                bot->CastSpell(bot, rejuv, false);
                return true;
            }
        }

        // Quick defensive/kiting cooldowns per class under pressure (throttled)
        static std::unordered_map<ObjectGuid::LowType, time_t> nextDefensive;
        ObjectGuid::LowType key = bot->GetGUID().GetCounter();
        time_t now = time(nullptr);
        bool underPressure = IsUnderPressure();
        if (underPressure && nextDefensive[key] <= now)
        {
            switch (bot->getClass())
            {
                case CLASS_DRUID:
                {
                    // Barkskin
                    if (!bot->HasAura(22812) && bot->HasSpell(22812))
                    {
                        bot->CastSpell(bot, 22812, false);
                        nextDefensive[key] = now + 10;
                        return true;
                    }
                    // Dash (only if in cat form)
                    if (bot->HasAura(768) && bot->HasSpell(1850) && !bot->HasAura(1850))
                    {
                        bot->CastSpell(bot, 1850, false);
                        nextDefensive[key] = now + 10;
                        return true;
                    }
                    break;
                }
                case CLASS_MAGE:
                {
                    // Ice Barrier if available; otherwise Blink away
                    if (bot->HasSpell(43039) && !bot->HasAura(43039))
                    {
                        bot->CastSpell(bot, 43039, false);
                        nextDefensive[key] = now + 8;
                        return true;
                    }
                    if (bot->HasSpell(1953))
                    {
                        float backX = bot->GetPositionX() - cos(bot->GetOrientation()) * 10.0f;
                        float backY = bot->GetPositionY() - sin(bot->GetOrientation()) * 10.0f;
                        bot->CastSpell(bot, 1953, false);
                        MoveTo(bot->GetMapId(), backX, backY, bot->GetPositionZ(), false, true);
                        nextDefensive[key] = now + 8;
                        return true;
                    }
                    break;
                }
                case CLASS_ROGUE:
                {
                    // Sprint already handled by triggers, but backstop it here
                    if (bot->HasSpell(11305) && !bot->HasAura(11305))
                    {
                        bot->CastSpell(bot, 11305, false);
                        nextDefensive[key] = now + 8;
                        return true;
                    }
                    break;
                }
                case CLASS_PRIEST:
                {
                    // Psychic Scream to peel melee
                    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
                    for (ObjectGuid guid : targets)
                    {
                        Unit* unit = botAI->GetUnit(guid);
                        if (!unit || !unit->IsAlive() || !unit->IsPlayer())
                            continue;
                        if (bot->GetDistance(unit) < 8.0f && bot->HasSpell(10890))
                        {
                            bot->CastSpell(unit, 10890, false);
                            nextDefensive[key] = now + 12;
                            return true;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            nextDefensive[key] = now + 5;
        }

        if (isHealerClass)
        {
            Unit* healTarget = AI_VALUE(Unit*, "party member to heal");
            if (healTarget && healTarget->IsAlive())
            {
                bool losBlocked = !bot->IsWithinLOSInMap(healTarget) ||
                                  fabs(bot->GetPositionZ() - healTarget->GetPositionZ()) > 5.0f;
                if (losBlocked || bot->GetDistance(healTarget) > 35.0f)
                {
                    float x, y, z;
                    healTarget->GetPosition(x, y, z);
                    return MoveNear(healTarget->GetMapId(), x, y, z, 6.0f, MovementPriority::MOVEMENT_NORMAL);
                }
            }
        }

        if ((isHealerClass || isRangedClass) && IsUnderPressure())
        {
            if (moveToPillar(bg, true, arenaFeatures))
                return true;
        }
        // Extra LOS when low vs caster focus: if target is caster and HP low, pillar even if not flagged underPressure
        if (isRangedClass || isHealerClass)
        {
            Unit* curTarget = bot->GetVictim();
            if (curTarget && curTarget->IsPlayer() && bot->GetHealthPct() < 60.0f &&
                (curTarget->getPowerType() == POWER_MANA || curTarget->getClass() == CLASS_WARLOCK ||
                 curTarget->getClass() == CLASS_MAGE || curTarget->getClass() == CLASS_PRIEST ||
                 curTarget->getClass() == CLASS_SHAMAN || curTarget->getClass() == CLASS_DRUID))
            {
                if (moveToPillar(bg, true, arenaFeatures))
                    return true;
            }
        }

        // Soft peel: non-healer assists if healer is under heavy focus.
        if (!isHealerClass)
        {
            Unit* healUnit = AI_VALUE(Unit*, "party member to heal");
            Player* healTarget = healUnit && healUnit->GetTypeId() == TYPEID_PLAYER ? healUnit->ToPlayer() : nullptr;
            if (healTarget && healTarget->IsAlive() && botAI->IsHeal(healTarget))
            {
                Position healPos(healTarget->GetPositionX(), healTarget->GetPositionY(), healTarget->GetPositionZ());
                uint8 enemiesOnHealer = CountEnemiesNearPosition(healPos, 18.0f);
                float healerHp = healTarget->GetHealthPct();
                if (enemiesOnHealer >= 2 || healerHp < 45.0f)
                {
                    // Check if another ally already peeling in this arena
                    Battleground* bg = bot->GetBattleground();
                    uint32 instanceId = bg ? bg->GetInstanceID() : 0;
                    auto it = sPeelAssignments.find(instanceId);
                    if (it != sPeelAssignments.end() && it->second.first && it->second.first != bot->GetGUID() &&
                        it->second.second + 5 > time(nullptr))
                    {
                        // Someone else is peeling, continue DPS
                    }
                    else
                    {
                        // assign self as peeler
                        sPeelAssignments[instanceId] = std::make_pair(bot->GetGUID(), time(nullptr));
                    }

                    // find closest enemy to healer to peel
                    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
                    Unit* peelTarget = nullptr;
                    float bestDist = 9999.0f;
                    for (ObjectGuid guid : targets)
                    {
                        Unit* unit = botAI->GetUnit(guid);
                        if (!unit || !unit->IsAlive() || !unit->IsPlayer())
                            continue;
                        float dist = unit->GetDistance(healPos.GetPositionX(), healPos.GetPositionY(),
                                                       healPos.GetPositionZ());
                        if (dist < 25.0f && dist < bestDist)
                        {
                            peelTarget = unit;
                            bestDist = dist;
                        }
                    }

                    if (peelTarget)
                    {
                        if (!bot->IsWithinMeleeRange(peelTarget))
                            return MoveNear(peelTarget, 4.5f, MovementPriority::MOVEMENT_NORMAL);
                    }
                }
            }
        }

        // In combat: if stuck in Ruins slime, climb out
        RescueFromRLSlime(bot, bg);
        // In combat: if still on Blade's Edge start platform, push to mid
        RescueFromBladeEdgeStart(bot, bg);

        // Shaman in combat: drop Ghost Wolf when in casting range so they resume offensive/healing
        if (bot->getClass() == CLASS_SHAMAN && bot->IsInCombat() && bot->HasAura(2645))
        {
            Unit* curVictim = bot->GetVictim();
            if (curVictim)
            {
                float d = bot->GetDistance(curVictim);
                if (d < 30.0f && bot->IsWithinLOSInMap(curVictim))
                    bot->RemoveAurasDueToSpell(2645);
            }
        }

        // Combat idle breaker for arenas: if we haven't moved in a while, force a push toward center/pillar/target.
        if (bg->isArena())
        {
            static std::unordered_map<ObjectGuid::LowType, std::pair<Position, time_t>> sCombatIdle;
            ObjectGuid::LowType keyIdle = bot->GetGUID().GetCounter();
            time_t nowIdle = time(nullptr);
            Position cur(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            auto itIdle = sCombatIdle.find(keyIdle);
            if (itIdle == sCombatIdle.end())
            {
                sCombatIdle[keyIdle] = {cur, nowIdle};
            }
            else
            {
                float moved = itIdle->second.first.GetExactDist2d(cur.GetPositionX(), cur.GetPositionY());
                if (moved < 0.8f && (nowIdle - itIdle->second.second) > 10)
                {
                    Unit* tgt = bot->GetVictim();
                    if (!tgt || !tgt->IsAlive())
                        tgt = FindNearestArenaEnemy(bot, bg);

                    // Directly push to enemy first; if no enemy, fall back to pillar/center
                    if (tgt && tgt->IsAlive())
                    {
                        MoveNear(tgt, 6.0f, MovementPriority::MOVEMENT_NORMAL);
                    }
                    else if (moveToPillar(bg, true, arenaFeatures) || moveToCenter(bg))
                    {
                        // moved
                    }
                    sCombatIdle[keyIdle] = {cur, nowIdle};
                    return true;
                }
                else
                {
                    sCombatIdle[keyIdle] = {cur, nowIdle};
                }
            }
        }
    }

    time_t nowGlobal = time(nullptr);
    Unit* target = bot->GetVictim();
    if (target)
    {
        // If target is far, push closer to avoid idle ranged tunneling
        float distToTarget = bot->GetDistance(target);
        if (distToTarget > 25.0f)
        {
            MoveNear(target, 6.0f, MovementPriority::MOVEMENT_NORMAL);
        }

        // If target becomes immune or hard-CC'd, retarget
        if (IsImmuneOrInvulnerable(target) || IsHardCC(target))
        {
            sLastDefensive[target->GetGUID().GetCounter()] = nowGlobal;
            Unit* swap = AI_VALUE(Unit*, "enemy healer target");
            if (!swap || swap == target)
                swap = AI_VALUE(Unit*, "enemy player target");
            if (swap && swap != target)
            {
                bot->SetTarget(swap->GetGUID());
                target = swap;
                ManagePetDiscipline(bot, target);
            }
        }

        bool losBlocked = !bot->IsWithinLOSInMap(target) || fabs(bot->GetPositionZ() - target->GetPositionZ()) > 5.0f;

        if (losBlocked)
        {
            // Hard chase toward target to break pillar/wall stalls (e.g., Ruins tomb)
            if (bot->GetDistance(target) < 40.0f)
                return MoveNear(target, 5.0f, MovementPriority::MOVEMENT_NORMAL);

            PathGenerator path(bot);
            path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);

            if (path.GetPathType() != PATHFIND_NOPATH)
            {
                // If you are casting a spell and lost your target due to LoS, interrupt the cast and move
                if (bot->IsNonMeleeSpellCast(false, true, true, false, true))
                    bot->InterruptNonMeleeSpells(true);

                float x, y, z;
                target->GetPosition(x, y, z);
                botAI->TellMasterNoFacing("Repositioning to exit the LoS target!");
                return MoveTo(target->GetMapId(), x + frand(-1, +1), y + frand(-1, +1), z, false, true);
            }
        }
    }

    if (bot->IsInCombat() && isRangedClass && !IsUnderPressure() &&
        !bot->IsNonMeleeSpellCast(false, true, true, false, true))
    {
        Position selfPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        uint8 nearby = CountEnemiesNearPosition(selfPos, 25.0f);
        if (nearby > 0)
        {
            // Small anti-clump: if an ally is within 4 yards, shift slightly
            GuidVector allies = AI_VALUE(GuidVector, "nearest friendly players");
            for (ObjectGuid guid : allies)
            {
                Unit* ally = botAI->GetUnit(guid);
                if (!ally || ally == bot || !ally->IsAlive() || !ally->IsPlayer())
                    continue;
                if (bot->GetDistance(ally) < 4.0f)
                {
                    float angle = bot->GetAngle(ally) + 0.8f;
                    float nx = bot->GetPositionX() + cos(angle) * 4.0f;
                    float ny = bot->GetPositionY() + sin(angle) * 4.0f;
                    MoveTo(bot->GetMapId(), nx, ny, bot->GetPositionZ(), false, true);
                    break;
                }
            }

            if (moveToPillar(bg, false, arenaFeatures))
                return true;
        }
    }

    // LOS micro for ranged under focus: dip to pillar briefly if multiple enemies nearby.
    if (bot->IsInCombat() && isRangedClass)
    {
        Position selfPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        uint8 nearEnemies = CountEnemiesNearPosition(selfPos, 14.0f);
        static std::unordered_map<ObjectGuid::LowType, time_t> nextLosMove;
        time_t now = time(nullptr);
        ObjectGuid::LowType key = bot->GetGUID().GetCounter();
        if (nearEnemies >= 2 && nextLosMove[key] <= now)
        {
            if (moveToPillar(bg, true, arenaFeatures))
            {
                nextLosMove[key] = now + 6;  // throttle
                return true;
            }
        }
        else if (nextLosMove.find(key) == nextLosMove.end())
        {
            nextLosMove[key] = 0;
        }
    }

    if (!bot->IsInCombat())
    {
        // Actively hunt remaining enemies instead of idling/resetting
        Unit* hunt = AI_VALUE(Unit*, "enemy player target");
        if ((!hunt || !hunt->IsAlive()) && bg->isArena())
            hunt = FindNearestArenaEnemy(bot, bg);

        if (hunt && hunt->IsAlive())
        {
            // Avoid tunneling an invulnerable target (bubble, ice block, dispersion)
            if (hunt->HasAura(642) || hunt->HasAura(45438) || hunt->HasAura(47585))
            {
                Unit* alt = AI_VALUE(Unit*, "enemy healer target");
                if (alt && alt->IsAlive())
                    hunt = alt;
            }
            else if (IsHardCC(hunt))
            {
                Unit* alt = AI_VALUE(Unit*, "enemy healer target");
                if (alt && alt->IsAlive())
                    hunt = alt;
            }

            float dist = bot->GetDistance(hunt);
            bool losBlocked = !bot->IsWithinLOSInMap(hunt) || fabs(bot->GetPositionZ() - hunt->GetPositionZ()) > 5.0f;

            // If we are swimming in RL slime or distant, push to land/target
            if ((bg->GetBgTypeID() == BATTLEGROUND_RL && bot->IsInWater()) || dist > 10.0f || losBlocked)
            {
                return MoveNear(hunt, 10.0f, MovementPriority::MOVEMENT_NORMAL);
            }

            // Ensure pet discipline aligns with new target
            ManagePetDiscipline(bot, hunt);
        }

        // Arena: if no clean shot or out of LOS, proactively close distance to nearest enemy instead of waiting
        if (bg->isArena())
        {
            Unit* nearest = FindNearestArenaEnemy(bot, bg);
            if (nearest && nearest->IsAlive())
            {
                float dist = bot->GetDistance(nearest);
                bool losBlocked = !bot->IsWithinLOSInMap(nearest) ||
                                  fabs(bot->GetPositionZ() - nearest->GetPositionZ()) > 5.0f;
                if (losBlocked || dist > 20.0f)
                {
                    return MoveNear(nearest, 10.0f, MovementPriority::MOVEMENT_NORMAL);
                }
            }
        }

        // Arena idle-bailout: if stationary too long, push toward center to break stalemates (e.g., Ruins)
        if (bg->isArena())
        {
            static std::unordered_map<ObjectGuid::LowType, std::pair<Position, time_t>> sIdleCheck;
            ObjectGuid::LowType key = bot->GetGUID().GetCounter();
            time_t nowIdle = time(nullptr);
            Position curPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            auto it = sIdleCheck.find(key);
            if (it == sIdleCheck.end())
            {
                sIdleCheck[key] = {curPos, nowIdle};
            }
            else
            {
                float d = it->second.first.GetExactDist2d(curPos.GetPositionX(), curPos.GetPositionY());
                if (d < 0.8f && (nowIdle - it->second.second) > 8)
                {
                    // Blade's Edge special rescue: if stuck on start platforms, move toward mid walkway
                    if (bg->GetBgTypeID() == BATTLEGROUND_BE)
                    {
                        Position beMid(6239.89f, 261.11f, 0.89f);
                        if (bot->GetDistance(beMid) > 8.0f)
                        {
                            MoveNear(bg->GetMapId(), beMid.GetPositionX(), beMid.GetPositionY(),
                                     beMid.GetPositionZ(), 4.0f, MovementPriority::MOVEMENT_NORMAL);
                            sIdleCheck[key] = {curPos, nowIdle};
                            return true;
                        }
                    }

                    Unit* tgt = FindNearestArenaEnemy(bot, bg);
                    if (tgt && tgt->IsAlive())
                    {
                        MoveNear(tgt, 8.0f, MovementPriority::MOVEMENT_NORMAL);
                        sIdleCheck[key] = {curPos, nowIdle};
                        return true;
                    }
                    if (moveToCenter(bg))
                    {
                        sIdleCheck[key] = {curPos, nowIdle};
                        return true;
                    }
                }
                else
                {
                    sIdleCheck[key] = {curPos, nowIdle};
                }
            }
        }

        // If in Ruins slime/water without a clear target, force a climb-out toward mid
        if (bg->GetBgTypeID() == BATTLEGROUND_RL && bot->IsInWater())
        {
            Position safe(1266.8f, 1663.5f, 34.0f);
            return MoveNear(bg->GetMapId(), safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ(), 4.0f,
                            MovementPriority::MOVEMENT_NORMAL);
        }

        if (moveToReset(bg, arenaFeatures))
            return true;

        if (isHealerClass)
        {
            if (moveToPillar(bg, true, arenaFeatures))
                return true;
        }
        else if (isRangedClass)
        {
            Position selfPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            if (CountEnemiesNearPosition(selfPos, 45.0f) > 0)
            {
                if (moveToPillar(bg, false, arenaFeatures))
                    return true;
            }
        }

        return moveToCenter(bg);
    }

    return true;
}

bool ArenaTactics::moveToCenter(Battleground* bg)
{
    // Sanity check
    if (!bg)
    {
        return true;
    }
    uint32 Preference = 6;

    switch (bot->getClass())
    {
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_DRUID:
            Preference = 3;
            break;
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_ROGUE:
        case CLASS_DEATH_KNIGHT:
            Preference = 6;
            break;
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            Preference = 9;
            break;
    }

    switch (bg->GetBgTypeID())
    {
        case BATTLEGROUND_BE:
            if (bg->GetTeamStartPosition(bot->GetBgTeamId())->GetPositionY() < 240)
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 6226.65f + frand(-1, +1), 264.36f + frand(-1, +1), 1.31f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 6239.89f + frand(-1, +1), 261.11f + frand(-1, +1), 0.89f, false, true);
                else
                    MoveTo(bg->GetMapId(), 6235.60f + frand(-1, +1), 258.27f + frand(-1, +1), 0.89f, false, true);
            }
            else
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 6265.72f + frand(-1, +1), 271.92f + frand(-1, +1), 3.65f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 6239.89f + frand(-1, +1), 261.11f + frand(-1, +1), 0.89f, false, true);
                else
                    MoveTo(bg->GetMapId(), 6250.50f + frand(-1, +1), 266.66f + frand(-1, +1), 2.63f, false, true);
            }
            break;
        case BATTLEGROUND_RL:
            if (bg->GetTeamStartPosition(bot->GetBgTeamId())->GetPositionY() < 1600)
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 1262.14f + frand(-1, +1), 1657.63f + frand(-1, +1), 33.76f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 1266.85f + frand(-1, +1), 1663.52f + frand(-1, +1), 34.04f, false, true);
                else
                    MoveTo(bg->GetMapId(), 1274.07f + frand(-1, +1), 1656.36f + frand(-1, +1), 34.58f, false, true);
            }
            else
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 1261.93f + frand(-1, +1), 1669.27f + frand(-1, +1), 34.25f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 1266.85f + frand(-1, +1), 1663.52f + frand(-1, +1), 34.04f, false, true);
                else
                    MoveTo(bg->GetMapId(), 1266.37f + frand(-1, +1), 1672.40f + frand(-1, +1), 34.21f, false, true);
            }
            break;
        case BATTLEGROUND_NA:
            if (bg->GetTeamStartPosition(bot->GetBgTeamId())->GetPositionY() < 2870)
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 4068.85f + frand(-1, +1), 2911.98f + frand(-1, +1), 12.99f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 4056.99f + frand(-1, +1), 2919.75f + frand(-1, +1), 13.51f, false, true);
                else
                    MoveTo(bg->GetMapId(), 4056.27f + frand(-1, +1), 2905.33f + frand(-1, +1), 12.90f, false, true);
            }
            else
            {
                if (Preference == 3)
                    MoveTo(bg->GetMapId(), 4043.66f + frand(-1, +1), 2927.93f + frand(-1, +1), 13.17f, false, true);
                else if (Preference == 6)
                    MoveTo(bg->GetMapId(), 4056.99f + frand(-1, +1), 2919.75f + frand(-1, +1), 13.51f, false, true);
                else
                    MoveTo(bg->GetMapId(), 4054.80f + frand(-1, +1), 2934.28f + frand(-1, +1), 13.72f, false, true);
            }
            break;
        case BATTLEGROUND_DS:
            if (!MoveTo(bg->GetMapId(), 1291.58f + frand(-5, +5), 790.87f + frand(-5, +5), 7.8f, false, true))
            {
                // they like to hang around at the tip of the pipes doing nothing, so we just teleport them down
                if (bot->GetDistance(1333.07f, 817.18f, 13.35f) < 4)
                {
                    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                    bot->TeleportTo(bg->GetMapId(), 1330.96f, 816.75f, 3.2f, bot->GetOrientation());
                }
                if (bot->GetDistance(1250.13f, 764.79f, 13.34f) < 4)
                {
                    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                    bot->TeleportTo(bg->GetMapId(), 1252.19f, 765.41f, 3.2f, bot->GetOrientation());
                }
            }
            break;
        case BATTLEGROUND_RV:
            MoveTo(bg->GetMapId(), 764.65f + frand(-2, +2), -283.85f + frand(-2, +2), 28.28f, false, true);
            break;
        default:
            break;
    }

    return true;
}

bool ArenaTactics::moveToPillar(Battleground* bg, bool preferSafety, uint32 featureFlags)
{
    if (!bg)
        return false;

    std::vector<Position> positions = GetArenaPillarPositions(bg->GetBgTypeID());
    if (positions.empty())
        return false;

    Position best = SelectBestArenaPosition(bg->GetBgTypeID(), positions, preferSafety, featureFlags);
    float dist = bot->GetDistance(best.GetPositionX(), best.GetPositionY(), best.GetPositionZ());
    if (dist <= 6.0f)
        return true;

    return MoveNear(bg->GetMapId(), best.GetPositionX(), best.GetPositionY(), best.GetPositionZ(), 5.0f,
                    MovementPriority::MOVEMENT_NORMAL);
}

bool ArenaTactics::moveToReset(Battleground* bg, uint32 featureFlags)
{
    if (!bg)
        return false;

    float manaPct = GetManaPct();
    bool needsReset = bot->GetHealthPct() < 70.0f;
    if (bot->getPowerType() == POWER_MANA && manaPct < 50.0f)
        needsReset = true;

    if (!needsReset)
        return false;

    Position selfPos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (CountEnemiesNearPosition(selfPos, 45.0f) > 0)
        return false;

    std::vector<Position> positions = GetArenaResetPositions(bg->GetBgTypeID());
    if (positions.empty())
        positions = GetArenaPillarPositions(bg->GetBgTypeID());

    if (positions.empty())
        return false;

    Position best = SelectBestArenaPosition(bg->GetBgTypeID(), positions, true, featureFlags);
    float dist = bot->GetDistance(best.GetPositionX(), best.GetPositionY(), best.GetPositionZ());
    if (dist <= 6.0f)
        return true;

    return MoveNear(bg->GetMapId(), best.GetPositionX(), best.GetPositionY(), best.GetPositionZ(), 5.0f,
                    MovementPriority::MOVEMENT_NORMAL);
}

std::vector<Position> ArenaTactics::GetArenaPillarPositions(BattlegroundTypeId bgType) const
{
    switch (bgType)
    {
        case BATTLEGROUND_NA:
            return {Position(4036.0f, 2898.0f, 13.0f), Position(4036.0f, 2940.0f, 13.0f),
                    Position(4078.0f, 2898.0f, 13.0f), Position(4078.0f, 2940.0f, 13.0f)};
        case BATTLEGROUND_RL:
            return {Position(1266.8f, 1663.5f, 34.0f), Position(1252.5f, 1652.5f, 34.0f),
                    Position(1281.5f, 1674.0f, 34.0f)};
        case BATTLEGROUND_BE:
            return {Position(6244.0f, 277.0f, 1.5f), Position(6232.0f, 247.0f, 1.5f),
                    Position(6224.0f, 261.0f, 1.5f), Position(6252.0f, 261.0f, 1.5f)};
        case BATTLEGROUND_DS:
            return {Position(1252.2f, 765.4f, 3.2f), Position(1330.9f, 816.8f, 3.2f)};
        case BATTLEGROUND_RV:
            return {Position(748.0f, -287.0f, 28.3f), Position(781.0f, -286.0f, 28.3f),
                    Position(764.0f, -266.0f, 28.3f), Position(764.0f, -301.0f, 28.3f)};
        default:
            break;
    }

    return {};
}

std::vector<Position> ArenaTactics::GetArenaResetPositions(BattlegroundTypeId bgType) const
{
    switch (bgType)
    {
        case BATTLEGROUND_DS:
            return {Position(1252.2f, 765.4f, 3.2f), Position(1330.9f, 816.8f, 3.2f)};
        case BATTLEGROUND_RL:
            return {Position(1266.8f, 1663.5f, 34.0f), Position(1254.0f, 1650.0f, 34.0f),
                    Position(1279.0f, 1676.0f, 34.0f)};
        case BATTLEGROUND_BE:
            return {Position(6238.0f, 261.0f, 1.5f), Position(6248.0f, 273.0f, 1.5f)};
        case BATTLEGROUND_NA:
            return {Position(4056.5f, 2919.0f, 13.0f)};
        case BATTLEGROUND_RV:
            return {Position(764.6f, -283.8f, 28.3f)};
        default:
            break;
    }

    return {};
}

Position ArenaTactics::SelectBestArenaPosition(BattlegroundTypeId bgType, std::vector<Position> const& positions,
                                               bool preferSafety, uint32 featureFlags) const
{
    Position best;
    float bestScore = -100000.0f;

    for (Position const& pos : positions)
    {
        float dist = bot->GetDistance(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        uint8 enemyCount = CountEnemiesNearPosition(pos, 25.0f);
        uint8 allyCount = CountAlliesNearPosition(pos, 25.0f);

        float score = 0.0f;
        if (preferSafety)
            score = (allyCount * 2.0f) - (enemyCount * 3.0f);
        else
            score = (allyCount * 1.0f) - (enemyCount * 2.0f);

        score -= dist * 0.02f;

        if (featureFlags & AMF_SHADOW_SIGHT)
            score += allyCount * 0.5f;

        if (featureFlags & AMF_WATER_FLUSH)
            score += (allyCount - enemyCount) * 0.3f;

        if (featureFlags & AMF_PILLAR_ROTATION)
            score += (allyCount * 0.2f);

        if (featureFlags & AMF_FLAME_WALL && enemyCount == 0)
            score += 2.0f;

        if (featureFlags & AMF_WATERFALL)
            score += allyCount * 0.25f - enemyCount * 0.25f;

        float mapBonus = 0.0f;
        if (bgType == BATTLEGROUND_NA)
            mapBonus += preferSafety ? 0.3f : 0.1f;
        else if (bgType == BATTLEGROUND_RL)
            mapBonus += pos.GetPositionX() < 1266.0f ? 0.5f : 0.2f;
        else if (bgType == BATTLEGROUND_DS)
            mapBonus += 0.25f;

        if (!preferSafety && featureFlags & AMF_PILLAR_ROTATION)
            mapBonus += 0.15f;

        score += mapBonus;

        if (score > bestScore)
        {
            bestScore = score;
            best = pos;
        }
    }

    return best;
}

uint8 ArenaTactics::CountEnemiesNearPosition(Position const& pos, float radius) const
{
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    uint8 count = 0;

    for (ObjectGuid guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer() || !unit->IsAlive())
            continue;

        if (unit->GetDistance(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()) <= radius)
            ++count;
    }

    return count;
}

uint8 ArenaTactics::CountAlliesNearPosition(Position const& pos, float radius) const
{
    GuidVector allies = AI_VALUE(GuidVector, "nearest friendly players");
    uint8 count = 0;

    for (ObjectGuid guid : allies)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer() || !unit->IsAlive() || unit == bot)
            continue;

        if (unit->GetDistance(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()) <= radius)
            ++count;
    }

    return count;
}

bool ArenaTactics::IsUnderPressure() const
{
    uint8 attackerCount = AI_VALUE(uint8, "attacker count");
    if (attackerCount >= 2)
        return true;

    if (bot->GetHealthPct() < 45.0f && attackerCount > 0)
        return true;

    return false;
}

float ArenaTactics::GetManaPct() const
{
    if (bot->getPowerType() != POWER_MANA)
        return 100.0f;

    uint32 maxMana = bot->GetMaxPower(POWER_MANA);
    if (maxMana == 0)
        return 100.0f;

    return 100.0f * static_cast<float>(bot->GetPower(POWER_MANA)) / static_cast<float>(maxMana);
}

// ==========================================
// ARENA INTELLIGENCE METHODS
// ==========================================

// Get Arena Focus Target - Coordinate all DPS on enemy healer
Unit* BGTactics::GetArenaFocusTarget()
{
    if (!bot->InArena())
        return nullptr;

    Battleground* bg = bot->GetBattleground();
    uint32 instanceId = bg ? bg->GetInstanceID() : 0;
    time_t now = time(nullptr);
    ObjectGuid burstGuid = GetBurstTarget(instanceId, now);
    if (burstGuid != ObjectGuid::Empty)
    {
        Unit* burstTarget = botAI->GetUnit(burstGuid);
        if (burstTarget && burstTarget->IsAlive())
            return burstTarget;
    }

    // Priority 1: Enemy healer (alive)
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    Unit* enemyHealer = nullptr;
    Unit* lowHPTarget = nullptr;
    Unit* bestDps = nullptr;
    float lowestHP = 100.0f;
    float lowestDpsHP = 100.0f;

    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer() || !unit->IsAlive())
            continue;

        Player* enemy = unit->ToPlayer();
        if (IsImmuneOrInvulnerable(enemy) || WasRecentlyHardCC(enemy->GetGUID(), now))
            continue;
        if (enemy->HasAura(642) || enemy->HasAura(45438) || enemy->HasAura(47585)) // bubble, ice block, dispersion
            continue;
        if (WasRecentlyDefensive(enemy->GetGUID(), now))
            continue;
        if (IsHardCC(enemy))
        {
            sLastHardCC[enemy->GetGUID().GetCounter()] = now;
            if (botAI->IsHeal(enemy))
                StartBurstWindow(instanceId, enemy->GetGUID(), now, 5.0f);
            continue;
        }
        if (enemy->HasAura(48707) || enemy->HasAura(19263))
            sLastDefensive[enemy->GetGUID().GetCounter()] = now;
        bool softCc = enemy->HasAuraType(SPELL_AURA_MOD_SILENCE) || enemy->HasAura(18469) || enemy->HasAura(24858);
        if (softCc)
        {
            sLastSoftCC[enemy->GetGUID().GetCounter()] = now;
            continue;
        }
        if (WasRecentlySoftCC(enemy->GetGUID(), now))
            continue;

        // Check if healer
        if (botAI->IsHeal(enemy))
        {
            if (!enemyHealer || enemy->GetHealthPct() < enemyHealer->GetHealthPct())
                enemyHealer = enemy;
        }

        // Track lowest HP target as fallback
        float hp = enemy->GetHealthPct();
        if (hp < lowestHP)
        {
            lowestHP = hp;
            lowHPTarget = enemy;
        }

        // Track best DPS target (non-healer)
        if (!botAI->IsHeal(enemy) && hp < lowestDpsHP)
        {
            lowestDpsHP = hp;
            bestDps = enemy;
        }
    }

    if (enemyHealer && enemyHealer->GetHealthPct() < 40.0f)
    {
        StartBurstWindow(instanceId, enemyHealer->GetGUID(), now, 5.0f);
        return enemyHealer;
    }

    // If healer exists but is hard-CC'd, focus DPS instead to capitalize
    if (enemyHealer && !IsHardCC(enemyHealer))
        return enemyHealer;

    // Prefer a DPS target if healer is locked in hard CC; fallback to lowest HP overall
    if (bestDps)
        return bestDps;

    return lowHPTarget;
}

// Check if should focus healer in arena
bool BGTactics::ShouldFocusHealer()
{
    if (!bot->InArena())
        return false;

    Unit* focusTarget = GetArenaFocusTarget();
    if (!focusTarget || !focusTarget->IsPlayer())
        return false;

    // Focus healer if one exists
    return botAI->IsHeal(focusTarget->ToPlayer());
}

// Check if bot is under heavy pressure (taking damage from multiple enemies)
bool BGTactics::IsUnderHeavyPressure()
{
    if (!bot->InArena() && !bot->InBattleground())
        return false;

    // Check attacker count
    uint8 attackerCount = AI_VALUE(uint8, "attacker count");
    
    // Under heavy pressure if:
    // 1. Multiple attackers (2+)
    // 2. Low HP (<40%)
    // 3. Combination of attackers + low HP
    
    if (attackerCount >= 2)
        return true;

    if (bot->GetHealthPct() < 40.0f && attackerCount > 0)
        return true;

    return false;
}

// Check if should use defensive cooldown
bool BGTactics::ShouldUseDefensiveCooldown()
{
    if (!bot->InArena() && !bot->InBattleground())
        return false;

    // Use defensive if under heavy pressure
    if (IsUnderHeavyPressure())
        return true;

    // Use defensive if HP dropping rapidly (lost >30% HP recently)
    if (bot->GetHealthPct() < 50.0f && bot->IsInCombat())
    {
        uint8 attackerCount = AI_VALUE(uint8, "attacker count");
        if (attackerCount >= 2)
            return true;
    }

    // Emergency defensive at very low HP
    if (bot->GetHealthPct() < 25.0f && bot->IsInCombat())
        return true;

    return false;
}

// Check if it's a good burst window (enemy vulnerable)
bool BGTactics::IsBurstWindow()
{
    if (!bot->InArena())
        return false;

    Unit* focusTarget = GetArenaFocusTarget();
    if (!focusTarget || !focusTarget->IsPlayer())
        return false;

    float targetHP = focusTarget->GetHealthPct();
    
    // Burst window: Target HP < 60% AND (healer CC'd OR target CC'd OR very low HP)
    if (targetHP < 60.0f && (IsEnemyHealerCCd() || CountEnemyHealers() == 0))
        return true;
    
    if (targetHP < 40.0f)
        return true;

    // Target is CC'd
    if (focusTarget->HasUnitState(UNIT_STATE_STUNNED) || focusTarget->HasUnitState(UNIT_STATE_CONTROLLED))
        return true;

    return false;
}

// Check if should use burst cooldown
bool BGTactics::ShouldUseBurstCooldown()
{
    if (!bot->InArena())
        return false;

    if (IsBurstWindow())
        return true;

    // Use burst if we outnumber enemies and target is low
    Unit* focusTarget = GetArenaFocusTarget();
    if (focusTarget)
    {
        // never burst into major immunities
        if (focusTarget->HasAura(642) || focusTarget->HasAura(45438) || focusTarget->HasAura(47585) ||
            focusTarget->HasAura(19263))
            return false;

        if (focusTarget->GetHealthPct() < 50.0f)
        {
        GuidVector targets = AI_VALUE(GuidVector, "possible targets");
        uint8 aliveEnemies = 0;
        for (auto guid : targets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsPlayer() && unit->IsAlive())
                aliveEnemies++;
        }

        Group* group = bot->GetGroup();
        if (group)
        {
            uint8 aliveAllies = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsAlive())
                    aliveAllies++;
            }
            if (aliveAllies > aliveEnemies)
                return true;
        }
        }
    }
    return false;
}

// Count enemy healers in arena
uint8 BGTactics::CountEnemyHealers()
{
    if (!bot->InArena())
        return 0;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    uint8 healerCount = 0;
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsPlayer() && unit->IsAlive() && botAI->IsHeal(unit->ToPlayer()))
            healerCount++;
    }
    return healerCount;
}

// Check if enemy healer is CC'd
bool BGTactics::IsEnemyHealerCCd()
{
    if (!bot->InArena())
        return false;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer() || !unit->IsAlive())
            continue;

        Player* enemy = unit->ToPlayer();
        if (botAI->IsHeal(enemy))
        {
            if (enemy->HasUnitState(UNIT_STATE_STUNNED) ||
                enemy->HasUnitState(UNIT_STATE_CONTROLLED) ||
                enemy->HasUnitState(UNIT_STATE_CONFUSED) ||
                enemy->HasAuraType(SPELL_AURA_MOD_FEAR))
                return true;
        }
    }
    return false;
}

// ==========================================
// TEAM COORDINATION METHODS
// ==========================================

bool BGTactics::ShouldProtectHealer()
{
    if (!bot->InBattleground() && !bot->InArena())
        return false;
    if (botAI->IsHeal(bot))
        return false;
    return IsAllyHealerThreatened();
}

Unit* BGTactics::GetAllyHealer()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && botAI->IsHeal(member))
            return member;
    }
    return nullptr;
}

bool BGTactics::IsAllyHealerThreatened()
{
    Unit* healer = GetAllyHealer();
    if (!healer)
        return false;
    GuidVector attackers = AI_VALUE(GuidVector, "attackers");
    for (auto guid : attackers)
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (attacker && (attacker->GetVictim() == healer || attacker->GetDistance(healer) < 10.0f))
            return true;
    }
    return false;
}

bool BGTactics::ShouldEscortFlagCarrier()
{
    if (!bot->InBattleground())
        return false;
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive())
        {
            if (member->HasAura(23333) || member->HasAura(23335) || member->HasAura(34976))
            {
                if (bot->GetDistance(member) < 40.0f)
                    return true;
            }
        }
    }
    return false;
}

// ==========================================
// TACTICAL VISION & RISK ASSESSMENT  
// ==========================================

// Count enemies near a position (tactical vision - range + height only, no LoS)
uint8 BGTactics::CountEnemiesNearPosition(Position pos, float radius)
{
    if (!bot->InBattleground())
        return 0;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    uint8 count = 0;

    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer() || !unit->IsAlive())
            continue;

        float dist = unit->GetDistance(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        
        // Range check
        if (dist > radius)
            continue;

        // Height check (reasonable Z-axis limit)
        float zDiff = fabs(bot->GetPositionZ() - unit->GetPositionZ());
        if (zDiff > 15.0f)
            continue;

        count++;
    }

    return count;
}

// Count allies near a position (range + height, no LoS)
uint8 BGTactics::CountAlliesNearPosition(Position pos, float radius)
{
    if (!bot->InBattleground())
        return 0;

    Group* group = bot->GetGroup();
    if (!group)
        return 1; // Just the bot

    uint8 count = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive())
            continue;

        float dist = member->GetDistance(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        
        // Range check
        if (dist > radius)
            continue;

        // Height check
        float zDiff = fabs(bot->GetPositionZ() - member->GetPositionZ());
        if (zDiff > 15.0f)
            continue;

        count++;
    }

    return count;
}

// Check if it's safe to attack an objective (not outnumbered)
bool BGTactics::IsSafeToAttackObjective(Position objPos)
{
    if (!bot->InBattleground())
        return true;

    uint8 enemies = CountEnemiesNearPosition(objPos, 40.0f);
    uint8 allies = CountAlliesNearPosition(objPos, 40.0f);

    // CRITICAL SITUATION OVERRIDE: Attack anyway if urgent!
    // Examples: Enemy FC about to cap, heavily losing, last seconds
    if (IsCriticalSituation())
        return true;  // JUST GO FOR IT!

    // Safe if: 
    // 1. No enemies
    // 2. Equal or more allies than enemies
    // 3. Only 1 enemy and we have at least 1 ally nearby
    
    if (enemies == 0)
        return true;

    if (allies >= enemies)
        return true;

    // Risky: outnumbered scenario (e.g., 1v3)
    // Only attack if the disadvantage is small (1v2 okay, 1v3+ not okay)
    if (enemies - allies >= 2)
        return false;

    return true;
}

// Get the safest objective to attack (best ally:enemy ratio)
Position BGTactics::GetSafestObjective()
{
    if (!bot->InBattleground())
        return bot->GetPosition();

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return bot->GetPosition();

    // AB: Full node tactical analysis
    if (bg->GetBgTypeID() == BATTLEGROUND_AB)
    {
        Position bestObj = bot->GetPosition();
        float bestRatio = -999.0f;

        Position nodes[5] = {
            {1166.0f, 1200.0f, -56.0f, 0.0f},  // Stables
            {1063.0f, 1313.0f, -56.0f, 0.0f},  // Blacksmith
            {990.0f, 1008.0f, -42.0f, 0.0f},   // Gold Mine
            {817.0f, 843.0f, 11.0f, 0.0f},     // Lumber Mill
            {729.0f, 1167.0f, -16.0f, 0.0f}    // Farm
        };

        for (int i = 0; i < 5; i++)
        {
            uint8 enemies = CountEnemiesNearPosition(nodes[i], 40.0f);
            uint8 allies = CountAlliesNearPosition(nodes[i], 40.0f);

            float ratio = allies - enemies;
            if (enemies == 0)
                ratio = 10.0f;

            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                bestObj = nodes[i];
            }
        }

        return bestObj;
    }

    // OTHER BGs: Use nearest objective (already has good logic)
    // WSG, EotS, AV, IoC, SotA all use GetNearestObjectivePosition
    // which is implemented elsewhere and works universally
    return GetNearestObjectivePosition();
}


// Check if should regroup before attacking (outnumbered)
bool BGTactics::ShouldRegroupBeforeAttack()
{
    if (!bot->InBattleground())
        return false;

    Position nearestObj = GetNearestObjectivePosition();
    
    // Check if we're outnumbered at nearest objective
    uint8 enemies = CountEnemiesNearPosition(nearestObj, 40.0f);
    uint8 allies = CountAlliesNearPosition(nearestObj, 40.0f);

    // Regroup if outnumbered by 2+ enemies
    if (enemies >= allies + 2)
        return true;

    return false;
}

// ==========================================
// URGENCY OVERRIDES (Critical Situations)
// ==========================================

// Check if situation demands aggressive play despite risk
bool BGTactics::IsCriticalSituation()
{
    if (!bot->InBattleground())
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // CRITICAL 1: Enemy FC about to score (WSG, EotS)
    if (IsEnemyFCNearCap())
        return true;  // MUST ATTACK even if 1v5!

    // CRITICAL 2: Heavily losing (need aggressive plays)
    if (IsLosingBadly(bg))
        return true;

    // CRITICAL 3: Allied FC in danger (must help!)
    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsAlive())
            {
                // Allied FC exists and is low HP
                if ((member->HasAura(23333) || member->HasAura(23335) || member->HasAura(34976)) &&
                    member->GetHealthPct() < 50.0f)
                    return true;  // MUST HELP FC!
            }
        }
    }

    return false;
}

// Check if enemy FC is near their cap point
bool BGTactics::IsEnemyFCNearCap()
{
    if (!bot->InBattleground())
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // Only for flag BGs
    if (bg->GetBgTypeID() != BATTLEGROUND_WS && bg->GetBgTypeID() != BATTLEGROUND_EY)
        return false;

    // Find enemy FC
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsPlayer())
            continue;

        Player* enemy = unit->ToPlayer();

        // Check if enemy has flag
        if (enemy->HasAura(23333) || enemy->HasAura(23335) || enemy->HasAura(34976))
        {
            float x = enemy->GetPositionX();
            float y = enemy->GetPositionY();

            if (bg->GetBgTypeID() == BATTLEGROUND_WS)
            {
                // Horde FC near Alliance base (about to cap)
                if (enemy->GetTeamId() == TEAM_HORDE)
                {
                    if (x > 1500 && x < 1580 && y > 1440 && y < 1520)
                        return true;  // EMERGENCY!
                }
                // Alliance FC near Horde base
                else
                {
                    if (x > 880 && x < 960 && y > 1390 && y < 1480)
                        return true;  // EMERGENCY!
                }
            }
            else if (bg->GetBgTypeID() == BATTLEGROUND_EY)
            {
                // Near flag cap point in middle
                if (x > 2000 && x < 2100 && y > 1320 && y < 1420)
                    return true;  // EMERGENCY!
            }
        }
    }

    return false;
}

// ==========================================
// IOC VEHICLE & COORDINATION METHODS
// ==========================================

// Check if bot is driving a siege engine
bool BGTactics::IsDrivingSiegeEngine()
{
    if (!bot->GetVehicle())
        return false;
    
    Unit* vehicle = bot->GetVehicle()->GetBase();
    if (!vehicle)
        return false;
    
    uint32 entry = vehicle->GetEntry();
    return (entry == NPC_SIEGE_ENGINE_A || entry == NPC_SIEGE_ENGINE_H);
}

// Check if bot is a passenger in any vehicle
bool BGTactics::IsVehiclePassenger()
{
    // Simple check: in vehicle but not the first person to enter (driver)
    // For IoC purposes, we just need to know if bot is passenger
    if (!bot->GetVehicle())
        return false;
    
    // If in vehicle, assume passenger for now (specific seat check causes issues)
    // Driver actions vs passenger actions will be handled in action logic
    return true;
}

// Get enemy gate position for IoC
Position BGTactics::GetEnemyGatePosition()
{
    // Alliance Gate: ~341, -872, 47 (Horde attacks)
    // Horde Gate: ~1270, -765, 48 (Alliance attacks)
    if (bot->GetTeamId() == TEAM_ALLIANCE)
        return Position(1270.0f, -765.0f, 48.0f, 0.0f);  // Horde gate
    else
        return Position(341.0f, -872.0f, 47.0f, 0.0f);   // Alliance gate
}

// Find nearest siege vehicle to enter
Unit* BGTactics::FindNearestSiegeVehicle(float radius)
{
    if (!bot->InBattleground())
        return nullptr;
    
    // Use existing bot AI to get all units
    GuidVector targets = AI_VALUE(GuidVector, "all targets");
    Unit* nearestVehicle = nullptr;
    float nearestDist = radius;
    
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsVehicle())
            continue;
        
        // Check siege engine entries
        uint32 entry = unit->GetEntry();
        if (entry != NPC_SIEGE_ENGINE_A && entry != NPC_SIEGE_ENGINE_H)
            continue;
        
        // Check faction
        if (unit->GetFaction() != bot->GetFaction())
            continue;
        
        // Check distance
        float dist = bot->GetDistance(unit);
        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearestVehicle = unit;
        }
    }
    
    return nearestVehicle;
}

// Check if should wait for allies before siege assault
bool BGTactics::ShouldWaitForSiegeGroup()
{
    if (!IsDrivingSiegeEngine())
        return false;
    
    // Count nearby allies for escort
    uint8 nearbyAllies = CountAlliesNearPosition(bot->GetPosition(), 30.0f);
    
    // Wait if less than 3 allies nearby
    return nearbyAllies < 3;
}

// Check if should protect allied siege engine
bool BGTactics::ShouldProtectSiegeEngine()
{
    Unit* siegeEngine = FindAlliedSiegeEngine(40.0f);
    if (!siegeEngine)
        return false;
    
    // Check if siege has attackers
    uint8 enemies = CountEnemiesNearPosition(siegeEngine->GetPosition(), 20.0f);
    return enemies > 0;
}

// Find allied siege engine to protect
Unit* BGTactics::FindAlliedSiegeEngine(float radius)
{
    if (!bot->InBattleground())
        return nullptr;
    
    // Use existing bot AI
    GuidVector targets = AI_VALUE(GuidVector, "all targets");
    
    for (auto guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsVehicle())
            continue;
        
        // Check allied IoC vehicles
        uint32 entry = unit->GetEntry();
        bool isVehicle = (entry == NPC_SIEGE_ENGINE_A || entry == NPC_SIEGE_ENGINE_H ||
                          entry == NPC_GLAIVE_THROWER_A || entry == NPC_GLAIVE_THROWER_H ||
                          entry == NPC_DEMOLISHER);
        if (!isVehicle)
            continue;
        
        // Check faction
        if (unit->GetFaction() != bot->GetFaction())
            continue;
        
        // Check distance
        if (bot->GetDistance(unit) <= radius)
            return unit;
    }
    
    return nullptr;
}

bool BGTactics::HasSeaforiumCharge()
{
    const uint32 SEAFORIUM_BOMBS_ITEM = 46847;
    const uint32 HUGE_SEAFORIUM_BOMBS_ITEM = 47030;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && (item->GetEntry() == SEAFORIUM_BOMBS_ITEM || item->GetEntry() == HUGE_SEAFORIUM_BOMBS_ITEM))
            return true;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            Item* item = bag->GetItemByPos(slot);
            if (item && (item->GetEntry() == SEAFORIUM_BOMBS_ITEM || item->GetEntry() == HUGE_SEAFORIUM_BOMBS_ITEM))
                return true;
        }
    }

    return false;
}

// Check if team controls Workshop
bool BGTactics::TeamControlsWorkshop()
{
    if (!bot->InBattleground())
        return false;
    
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_IC)
        return false;
    
    BattlegroundIC* isleOfConquestBG = dynamic_cast<BattlegroundIC*>(bg);
    if (!isleOfConquestBG)
        return false;

    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_WORKSHOP);
    if (bot->GetTeamId() == TEAM_ALLIANCE)
        return nodePoint.nodeState == NODE_STATE_CONTROLLED_A;

    return nodePoint.nodeState == NODE_STATE_CONTROLLED_H;
}

// Check if team controls Hangar
bool BGTactics::TeamControlsHangar()
{
    if (!bot->InBattleground())
        return false;
    
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_IC)
        return false;
    
    BattlegroundIC* isleOfConquestBG = dynamic_cast<BattlegroundIC*>(bg);
    if (!isleOfConquestBG)
        return false;

    ICNodePoint const& nodePoint = isleOfConquestBG->GetICNodePoint(NODE_TYPE_HANGAR);
    if (bot->GetTeamId() == TEAM_ALLIANCE)
        return nodePoint.nodeState == NODE_STATE_CONTROLLED_A;

    return nodePoint.nodeState == NODE_STATE_CONTROLLED_H;
}

// =============================================
// REACTIVE DEFENSE SYSTEM IMPLEMENTATION
// =============================================

// Global node state storage
// Global node state storage
std::unordered_map<uint32, std::unordered_map<uint32, NodeStateInfo>> bgNodeStates;
std::recursive_mutex bgNodeStatesMutex;

// Update node states and detect changes - called periodically
void BGTactics::UpdateNodeStates(Battleground* bg)
{
    // Safety checks
    if (!bg || !bot)
        return;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);
    
    // Only process node-based battlegrounds
    if (bgType != BATTLEGROUND_AB && bgType != BATTLEGROUND_EY && bgType != BATTLEGROUND_IC)
        return;

    uint32 bgInstanceId = bg->GetInstanceID();
    TeamId botTeam = bot->GetTeamId();

    // LOCK MUTEX
    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);

    // Throttling Logic (Thread-Safe)
    static std::unordered_map<uint32, time_t> lastUpdateTime;
    time_t now = time(nullptr);
    
    if (lastUpdateTime.find(bgInstanceId) != lastUpdateTime.end() && 
        (now - lastUpdateTime[bgInstanceId]) < 2)
    {
        return; // Throttled
    }
    lastUpdateTime[bgInstanceId] = now;

    // Arathi Basin node state tracking
    if (bgType == BATTLEGROUND_AB)
    {
        BattlegroundAB* abBg = dynamic_cast<BattlegroundAB*>(bg);
        if (!abBg)
            return;

        // Track all 5 AB nodes
        for (uint32 nodeId = 0; nodeId < 5; ++nodeId)
        {
            NodeStateInfo& nodeInfo = bgNodeStates[bgInstanceId][nodeId];
            nodeInfo.nodeId = nodeId;
            nodeInfo.position = GetNodePosition(nodeId, bgType);
            nodeInfo.previousState = nodeInfo.currentState;

            // Get node state from BG
            CaptureABPointInfo const& captureInfo = abBg->GetCapturePointInfo(nodeId);
            uint8 nodeState = captureInfo._state;
            TeamId nodeOwner = captureInfo._ownerTeamId;
            
            // Decode node state
            NodeOwnerState newState = NODE_STATE_NEUTRAL;
            
            // Check current state using BG_AB_NODE_STATE enums
            if (nodeState == BG_AB_NODE_STATE_NEUTRAL)
            {
                newState = NODE_STATE_NEUTRAL;
            }
            else if (nodeState == BG_AB_NODE_STATE_ALLY_OCCUPIED && nodeOwner == botTeam)
            {
                newState = NODE_STATE_ALLY_CONTROLLED;
            }
            else if (nodeState == BG_AB_NODE_STATE_HORDE_OCCUPIED && nodeOwner == botTeam)
            {
                newState = NODE_STATE_ALLY_CONTROLLED;
            }
            else if ((nodeState == BG_AB_NODE_STATE_ALLY_OCCUPIED || nodeState == BG_AB_NODE_STATE_HORDE_OCCUPIED) && nodeOwner != botTeam)
            {
                newState = NODE_STATE_ENEMY_CONTROLLED;
            }
            else if (nodeState == BG_AB_NODE_STATE_ALLY_CONTESTED && botTeam == TEAM_ALLIANCE)
            {
                // Alliance is contesting - we're capturing enemy node
                newState = NODE_STATE_ENEMY_CONTESTED;
            }
            else if (nodeState == BG_AB_NODE_STATE_HORDE_CONTESTED && botTeam == TEAM_HORDE)
            {
                // Horde is contesting - we're capturing enemy node
                newState = NODE_STATE_ENEMY_CONTESTED;
            }
            else if (nodeState == BG_AB_NODE_STATE_ALLY_CONTESTED && botTeam == TEAM_HORDE)
            {
                // Alliance is contesting our node - defend!
                newState = NODE_STATE_ALLY_CONTESTED;
            }
            else if (nodeState == BG_AB_NODE_STATE_HORDE_CONTESTED && botTeam == TEAM_ALLIANCE)
            {
                // Horde is contesting our node - defend!
                newState = NODE_STATE_ALLY_CONTESTED;
            }

            // Detect state change
            if (newState != nodeInfo.currentState)
            {
                nodeInfo.currentState = newState;
                nodeInfo.stateChangeTime = time(nullptr);

                // Trigger event handlers
                if (nodeInfo.previousState == NODE_STATE_ALLY_CONTROLLED &&
                    newState == NODE_STATE_ALLY_CONTESTED)
                {
                    OnNodeContested(nodeId, nodeInfo.position);
                }
                else if (nodeInfo.previousState == NODE_STATE_ALLY_CONTROLLED &&
                         newState == NODE_STATE_ENEMY_CONTROLLED)
                {
                    OnNodeLost(nodeId, nodeInfo.position);
                }
                else if (newState == NODE_STATE_ALLY_CONTROLLED &&
                         nodeInfo.previousState == NODE_STATE_ALLY_CONTESTED)
                {
                    OnNodeRecaptured(nodeId);
                }
            }

            // Update defensive priority
            if (newState == NODE_STATE_ALLY_CONTESTED)
            {
                nodeInfo.needsDefense = true;
                float timeRemaining = GetCaptureTimeRemaining(nodeId);
                nodeInfo.defensivePriority = GetDefensiveRecapturePriority(nodeId, nodeInfo.position);
            }
            else
            {
                nodeInfo.needsDefense = false;
                nodeInfo.defensivePriority = 0.0f;
            }
        }
    }
    // Eye of the Storm tracking (4 bases)
    else if (bgType == BATTLEGROUND_EY)
    {
        BattlegroundEY* eyBg = dynamic_cast<BattlegroundEY*>(bg);
        if (!eyBg)
            return;

        TeamId enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;

        for (uint32 nodeId = 0; nodeId < EY_POINTS_MAX; ++nodeId)
        {
            NodeStateInfo& nodeInfo = bgNodeStates[bgInstanceId][nodeId];
            nodeInfo.nodeId = nodeId;
            nodeInfo.position = GetNodePosition(nodeId, bgType);
            nodeInfo.previousState = nodeInfo.currentState;

            CaptureEYPointInfo const& captureInfo = eyBg->GetCapturePointInfo(nodeId);
            TeamId nodeOwner = captureInfo._ownerTeamId;
            int8 allyCount = captureInfo._playersCount[botTeam];
            int8 enemyCount = captureInfo._playersCount[enemyTeam];

            NodeOwnerState newState = NODE_STATE_NEUTRAL;

            if (nodeOwner == botTeam)
            {
                newState = (enemyCount > 0) ? NODE_STATE_ALLY_CONTESTED : NODE_STATE_ALLY_CONTROLLED;
            }
            else if (nodeOwner == enemyTeam)
            {
                newState = (allyCount > 0) ? NODE_STATE_ENEMY_CONTESTED : NODE_STATE_ENEMY_CONTROLLED;
            }
            else
            {
                if (allyCount > 0 && enemyCount == 0)
                    newState = NODE_STATE_ENEMY_CONTESTED;
                else if (allyCount > 0 && enemyCount > 0)
                    newState = (allyCount >= enemyCount) ? NODE_STATE_ENEMY_CONTESTED : NODE_STATE_ALLY_CONTESTED;
                else
                    newState = NODE_STATE_NEUTRAL;
            }

            if (newState != nodeInfo.currentState)
            {
                nodeInfo.currentState = newState;
                nodeInfo.stateChangeTime = time(nullptr);

                if (nodeInfo.previousState == NODE_STATE_ALLY_CONTROLLED &&
                    newState == NODE_STATE_ALLY_CONTESTED)
                {
                    OnNodeContested(nodeId, nodeInfo.position);
                }
                else if (nodeInfo.previousState == NODE_STATE_ALLY_CONTROLLED &&
                         newState == NODE_STATE_ENEMY_CONTROLLED)
                {
                    OnNodeLost(nodeId, nodeInfo.position);
                }
                else if (newState == NODE_STATE_ALLY_CONTROLLED &&
                         nodeInfo.previousState == NODE_STATE_ALLY_CONTESTED)
                {
                    OnNodeRecaptured(nodeId);
                }
            }

            if (newState == NODE_STATE_ALLY_CONTESTED)
            {
                nodeInfo.needsDefense = true;
                nodeInfo.defensivePriority = GetDefensiveRecapturePriority(nodeId, nodeInfo.position);
            }
            else
            {
                nodeInfo.needsDefense = false;
                nodeInfo.defensivePriority = 0.0f;
            }
        }
    }
}

// Handler: Our node is being captured
// Handler: Our node is being captured
void BGTactics::OnNodeContested(uint32 nodeId, Position pos)
{
    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    std::string nodeName = GetNodeName(nodeId, bgType);
    
    LOG_DEBUG("playerbots", "BGTactics: {} detected {} is being captured! Triggering defensive response.",
              bot->GetName(), nodeName);

    // Mark this node for defensive priority
    bgNodeStates[bg->GetInstanceID()][nodeId].needsDefense = true;
}

// Handler: We lost a node
// Handler: We lost a node
void BGTactics::OnNodeLost(uint32 nodeId, Position pos)
{
    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    std::string nodeName = GetNodeName(nodeId, bgType);
    
    LOG_DEBUG("playerbots", "BGTactics: {} - ALERT! {} has been lost to enemy!",
              bot->GetName(), nodeName);
}

// Handler: We recaptured a node
// Handler: We recaptured a node
void BGTactics::OnNodeRecaptured(uint32 nodeId)
{
    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return;

    bgNodeStates[bg->GetInstanceID()][nodeId].needsDefense = false;
    
    LOG_DEBUG("playerbots", "BGTactics: {} - Successfully defended/recaptured node {}!",
              bot->GetName(), nodeId);
}

// Check if this node needs defensive recapture
bool BGTactics::IsDefensiveRecaptureTarget(uint32 nodeId)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);
    auto& nodes = bgNodeStates[bg->GetInstanceID()];
    if (nodes.find(nodeId) == nodes.end())
        return false;

    return nodes[nodeId].needsDefense;
}

// Calculate defensive recapture priority
float BGTactics::GetDefensiveRecapturePriority(uint32 nodeId, Position nodePos)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 0.0f;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    // Base strategic value
    float baseValue = GetNodeStrategicValue(nodeId, bgType);
    
    // Time urgency multiplier (more urgent as timer runs out)
    float timeRemaining = GetCaptureTimeRemaining(nodeId);
    float urgencyMultiplier = GetDefensivePriorityMultiplier(timeRemaining);
    
    // Proximity bonus (closer bots get higher priority)
    float distance = bot->GetDistance(nodePos.GetPositionX(), nodePos.GetPositionY(), nodePos.GetPositionZ());
    float proximityBonus = 1.0f;
    if (distance < 50.0f)
        proximityBonus = 2.0f;  // Very close - high priority
    else if (distance < 100.0f)
        proximityBonus = 1.5f;  // Medium distance
    else if (distance > DEFENSIVE_RESPONSE_RADIUS)
        proximityBonus = 0.1f;  // Too far - low priority

    // Calculate final priority
    float priority = baseValue * DEFENSIVE_PRIORITY_MULTIPLIER_BASE * urgencyMultiplier * proximityBonus;

    // Log decision
    LOG_DEBUG("playerbots", "BGTactics: {} defense priority for node {} = {:.2f} (base={:.1f}, urgency={:.2f}, proximity={:.2f})",
              bot->GetName(), nodeId, priority, baseValue, urgencyMultiplier, proximityBonus);

    return priority;
}

// Should this bot respond to defense?
bool BGTactics::ShouldRespondToDefense(uint32 nodeId, Position nodePos)
{
    // Don't respond if carrying flag
    if (HasCriticalObjective())
        return false;

    // Check distance
    float distance = bot->GetDistance(nodePos.GetPositionX(), nodePos.GetPositionY(), nodePos.GetPositionZ());
    if (distance > DEFENSIVE_RESPONSE_RADIUS)
        return false;

    // Check if enough allies already responding
    uint8 alliesAtNode = GetAlliesAtNode(nodeId, nodePos);
    uint8 enemiesAtNode = GetEnemiesAtNode(nodeId, nodePos);
    
    // Need at least enough to match enemies + 1
    uint32 neededDefenders = std::min(static_cast<uint32>(enemiesAtNode + 1), MAX_DEFENDERS_RESPONSE);
    
    if (alliesAtNode >= neededDefenders)
    {
        LOG_DEBUG("playerbots", "BGTactics: {} - enough defenders ({}) at node {}, not responding",
                  bot->GetName(), alliesAtNode, nodeId);
        return false;
    }

    return true;
}

// Find closest contested node that needs defense
uint32 BGTactics::GetClosestContestedNode()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return UINT32_MAX;

    std::lock_guard<std::recursive_mutex> lock(bgNodeStatesMutex);
    auto& nodes = bgNodeStates[bg->GetInstanceID()];
    
    float closestDist = 99999.0f;
    uint32 closestNode = UINT32_MAX;

    for (auto& pair : nodes)
    {
        if (!pair.second.needsDefense)
            continue;

        float dist = bot->GetDistance(pair.second.position.GetPositionX(), 
                                       pair.second.position.GetPositionY(), 
                                       pair.second.position.GetPositionZ());
        if (dist < closestDist)
        {
            closestDist = dist;
            closestNode = pair.first;
        }
    }

    return closestNode;
}

// Get capture time remaining (simplified - actual needs BG API)
float BGTactics::GetCaptureTimeRemaining(uint32 nodeId)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return AB_CAPTURE_TIME;

    auto& nodes = bgNodeStates[bg->GetInstanceID()];
    if (nodes.find(nodeId) == nodes.end())
        return AB_CAPTURE_TIME;

    time_t elapsed = time(nullptr) - nodes[nodeId].stateChangeTime;
    float remaining = AB_CAPTURE_TIME - static_cast<float>(elapsed);
    return std::max(0.0f, remaining);
}

// Get base strategic value for a node
float BGTactics::GetNodeStrategicValue(uint32 nodeId, BattlegroundTypeId bgType)
{
    if (bgType == BATTLEGROUND_AB)
    {
        // AB strategic values (Blacksmith most important)
        switch (nodeId)
        {
            case AB_NODE_BLACKSMITH:  return 1.5f;  // Central, highest value
            case AB_NODE_LUMBER_MILL: return 1.2f;  // Good position
            case AB_NODE_STABLES:     return 1.0f;  // Alliance side
            case AB_NODE_FARM:        return 1.0f;  // Horde side
            case AB_NODE_GOLD_MINE:   return 0.8f;  // Far corner
            default: return 1.0f;
        }
    }
    else if (bgType == BATTLEGROUND_EY)
    {
        // EotS values - prioritize Fel Reaver/Blood Elf towers (guide emphasis)
        switch (nodeId)
        {
            case POINT_FEL_REAVER:
            case POINT_BLOOD_ELF:
                return 1.3f;
            case POINT_DRAENEI_RUINS:
            case POINT_MAGE_TOWER:
                return 1.1f;
            default: return 1.0f;
        }
    }
    
    return 1.0f;
}

// Calculate urgency multiplier based on time remaining
float BGTactics::GetDefensivePriorityMultiplier(float timeRemaining)
{
    // Scale from 1.0 (just started) to 2.0 (about to lose)
    // Formula: 1.0 + (1 - timeRemaining/60) → ranges from 1.0 to 2.0
    float urgency = 1.0f + (1.0f - (timeRemaining / AB_CAPTURE_TIME));
    return std::min(2.0f, std::max(1.0f, urgency));
}

// Check if bot has critical objective (flag carrier, etc.)
bool BGTactics::HasCriticalObjective()
{
    // Check if carrying BG flags
    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || 
        bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return true;
    }
    
    return false;
}

// Get node position for a given BG type
Position BGTactics::GetNodePosition(uint32 nodeId, BattlegroundTypeId bgType)
{
    if (bgType == BATTLEGROUND_AB)
    {
        switch (nodeId)
        {
            case AB_NODE_STABLES:     return AB_NODE_POS_STABLES;
            case AB_NODE_BLACKSMITH:  return AB_NODE_POS_BLACKSMITH;
            case AB_NODE_FARM:        return AB_NODE_POS_FARM;
            case AB_NODE_LUMBER_MILL: return AB_NODE_POS_LUMBER_MILL;
            case AB_NODE_GOLD_MINE:   return AB_NODE_POS_GOLD_MINE;
            default: return Position();
        }
    }
    else if (bgType == BATTLEGROUND_EY)
    {
        // EotS base positions (approximate)
        switch (nodeId)
        {
            case 0: return Position(2048.8f, 1393.65f, 1194.05f);  // Mage Tower
            case 1: return Position(2286.56f, 1402.36f, 1197.11f); // Draenei Ruins
            case 2: return Position(2048.35f, 1749.68f, 1190.03f); // Blood Elf Tower
            case 3: return Position(2284.48f, 1731.13f, 1189.99f); // Fel Reaver Ruins
            default: return Position();
        }
    }
    
    return Position();
}

// Get readable node name
std::string BGTactics::GetNodeName(uint32 nodeId, BattlegroundTypeId bgType)
{
    if (bgType == BATTLEGROUND_AB)
    {
        switch (nodeId)
        {
            case AB_NODE_STABLES:     return "Stables";
            case AB_NODE_BLACKSMITH:  return "Blacksmith";
            case AB_NODE_FARM:        return "Farm";
            case AB_NODE_LUMBER_MILL: return "Lumber Mill";
            case AB_NODE_GOLD_MINE:   return "Gold Mine";
            default: return "Unknown";
        }
    }
    else if (bgType == BATTLEGROUND_EY)
    {
        switch (nodeId)
        {
            case 0: return "Mage Tower";
            case 1: return "Draenei Ruins";
            case 2: return "Blood Elf Tower";
            case 3: return "Fel Reaver Ruins";
            default: return "Unknown";
        }
    }
    
    return "Unknown";
}

// Count enemies at a node position
// Count enemies at a node position
uint8 BGTactics::GetEnemiesAtNode(uint32 nodeId, Position nodePos)
{
    return CountEnemiesNearPosition(nodePos, 40.0f);
}

// Count allies at a node position
uint8 BGTactics::GetAlliesAtNode(uint32 nodeId, Position nodePos)
{
    return CountAlliesNearPosition(nodePos, 40.0f);
}
