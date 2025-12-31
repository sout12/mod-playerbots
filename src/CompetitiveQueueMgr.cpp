/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "CompetitiveQueueMgr.h"
#include "DatabaseEnv.h"
#include "PlayerbotAIConfig.h"
#include <ctime>

CompetitiveQueueMgr::CompetitiveQueueMgr() : _lastCacheUpdate(0), _lastArenaCacheUpdate(0)
{
}

bool CompetitiveQueueMgr::ShouldQueueForBG(Player* player, bool isArena)
{
    if (!sPlayerbotAIConfig->competitiveQueueEnabled)
        return true; // If system is disabled, allow all bots to queue

    // Update caches if needed
    time_t now = std::time(nullptr);
    if (now - _lastCacheUpdate > CACHE_DURATION)
    {
        UpdateTopPlayersCache();
    }
    if (now - _lastArenaCacheUpdate > CACHE_DURATION)
    {
        UpdateTopArenaPlayersCache();
    }

    // Check appropriate ranking based on content type
    bool isTopRanked = isArena ? IsTopRankedArenaPlayer(player) : IsTopRankedPlayer(player);
    float queueChance = isTopRanked ? sPlayerbotAIConfig->competitiveQueueTopChance 
                                    : sPlayerbotAIConfig->competitiveQueueNormalChance;

    // Roll random chance
    float roll = frand(0.0f, 1.0f);
    bool canQueue = roll < queueChance;
    
    // Debug logging to track queue decisions
    LOG_DEBUG("playerbots", "CompetitiveQueue: Bot {} ({}) - isTopRanked={}, chance={:.1f}%, roll={:.3f}, canQueue={}",
             player->GetName(), isArena ? "Arena" : "BG", isTopRanked, queueChance * 100.0f, roll, canQueue);
    
    return canQueue;
}

bool CompetitiveQueueMgr::IsTopRankedPlayer(Player* player)
{
    if (!player)
        return false;

    uint32 guidLow = player->GetGUID().GetCounter();
    return _topPlayerGuids.find(guidLow) != _topPlayerGuids.end();
}

bool CompetitiveQueueMgr::IsTopRankedArenaPlayer(Player* player)
{
    if (!player)
        return false;

    uint32 guidLow = player->GetGUID().GetCounter();
    return _topArenaPlayerGuids.find(guidLow) != _topArenaPlayerGuids.end();
}

void CompetitiveQueueMgr::RefreshRankings()
{
    UpdateTopPlayersCache();
    UpdateTopArenaPlayersCache();
}

void CompetitiveQueueMgr::UpdateTopPlayersCache()
{
    _topPlayerGuids.clear();

    // Query top N players by total killing blows from pvpstats_players table
    uint32 topRankLimit = sPlayerbotAIConfig->competitiveQueueTopRank;

    LOG_DEBUG("playerbots", "CompetitiveQueue (BG): Querying top {} players from pvpstats_players table", topRankLimit);

    // Use INNER JOIN with characters table to match mod-pvp-stats-npc approach
    QueryResult result = CharacterDatabase.Query(
        "SELECT c.guid, SUM(ps.score_killing_blows) AS totalKills "
        "FROM pvpstats_players ps "
        "INNER JOIN characters c ON ps.character_guid = c.guid "
        "GROUP BY ps.character_guid "
        "HAVING totalKills > 0 "
        "ORDER BY totalKills DESC "
        "LIMIT {}",
        topRankLimit
    );

    // If pvpstats_players has no data (table empty or no matches played yet),
    // fall back to lifetime honor kills from characters table
    if (!result)
    {
        LOG_INFO("playerbots", "CompetitiveQueue (BG): pvpstats_players returned 0 results, falling back to lifetime honorable kills from characters table");
        
        result = CharacterDatabase.Query(
            "SELECT guid, totalHonorableKills "
            "FROM characters "
            "WHERE totalHonorableKills > 0 "
            "ORDER BY totalHonorableKills DESC "
            "LIMIT {}",
            topRankLimit
        );
        
        if (result)
        {
            LOG_INFO("playerbots", "CompetitiveQueue (BG): Fallback query found {} players with honorable kills", result->GetRowCount());
        }
    }
    else
    {
        LOG_INFO("playerbots", "CompetitiveQueue (BG): pvpstats_players query found {} players", result->GetRowCount());
    }

    if (!result)
    {
        LOG_WARN("playerbots", "CompetitiveQueue (BG): No PvP data found in either pvpstats_players or characters table - no top-ranked BG players");
        _lastCacheUpdate = std::time(nullptr);
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].Get<uint32>();
        uint32 score = fields[1].Get<uint32>();
        _topPlayerGuids.insert(guid);
        count++;
        
        // Log first few entries for debugging
        if (count <= 5)
        {
            LOG_DEBUG("playerbots", "CompetitiveQueue (BG): Top player #{} - GUID: {}, Score: {}", count, guid, score);
        }
    } while (result->NextRow());

    _lastCacheUpdate = std::time(nullptr);
    LOG_INFO("playerbots", "CompetitiveQueue (BG): Loaded {} top-ranked BG players", _topPlayerGuids.size());
}

void CompetitiveQueueMgr::UpdateTopArenaPlayersCache()
{
    _topArenaPlayerGuids.clear();

    // Query top N players by highest arena team rating
    uint32 topRankLimit = sPlayerbotAIConfig->competitiveQueueTopRank;

    QueryResult result = CharacterDatabase.Query(
        "SELECT atm.guid, MAX(at.rating) AS maxRating "
        "FROM arena_team_member atm "
        "INNER JOIN arena_team at ON atm.arenaTeamId = at.arenaTeamId "
        "WHERE at.seasonGames >= 10 "
        "GROUP BY atm.guid "
        "HAVING maxRating > 0 "
        "ORDER BY maxRating DESC "
        "LIMIT {}",
        topRankLimit
    );

    // Fallback: use personal rating if no qualified teams
    if (!result)
    {
        LOG_INFO("playerbots", "CompetitiveQueue (Arena): No qualified teams, trying personal rating");
        
        result = CharacterDatabase.Query(
            "SELECT atm.guid, MAX(atm.personalRating) AS maxPR "
            "FROM arena_team_member atm "
            "WHERE atm.seasonGames >= 10 "
            "GROUP BY atm.guid "
            "HAVING maxPR > 0 "
            "ORDER BY maxPR DESC "
            "LIMIT {}",
            topRankLimit
        );
    }

    if (!result)
    {
        LOG_INFO("playerbots", "CompetitiveQueue (Arena): No arena data found");
        _lastArenaCacheUpdate = std::time(nullptr);
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].Get<uint32>();
        _topArenaPlayerGuids.insert(guid);
    } while (result->NextRow());

    _lastArenaCacheUpdate = std::time(nullptr);
    LOG_INFO("playerbots", "CompetitiveQueue (Arena): Loaded {} top-ranked arena players", _topArenaPlayerGuids.size());
}

