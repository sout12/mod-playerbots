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

    // Get the appropriate player list
    uint32 topPlayerCount = isArena ? _topArenaPlayerGuids.size() : _topPlayerGuids.size();
    
    // Check appropriate ranking based on content type
    bool isTopRanked = isArena ? IsTopRankedArenaPlayer(player) : IsTopRankedPlayer(player);
    
    // Graduated fallback system: increase queue chance when there are few top-ranked players
    // This ensures queues stay active even with low competitive participation
    float queueChance;
    
    if (topPlayerCount == 0)
    {
        // No top players at all - allow all bots to queue
        LOG_DEBUG("playerbots", "CompetitiveQueue ({}): No ranking data, allowing bot {} to queue",
                  isArena ? "Arena" : "BG", player->GetName());
        return true;
    }
    else if (isTopRanked)
    {
        // Top-ranked players always get the configured high chance
        queueChance = sPlayerbotAIConfig->competitiveQueueTopChance;
    }
    else
    {
        // Graduated fallback for non-top players based on how many top players exist
        // Fewer top players = higher queue chance to keep queues active
        if (topPlayerCount <= 10)
        {
            // Very few top players (≤10): 80% chance
            queueChance = 0.80f;
            LOG_DEBUG("playerbots", "CompetitiveQueue ({}): Only {} top players, using HIGH fallback chance",
                     isArena ? "Arena" : "BG", topPlayerCount);
        }
        else if (topPlayerCount <= 50)
        {
            // Few top players (11-50): 50% chance
            queueChance = 0.50f;
            LOG_DEBUG("playerbots", "CompetitiveQueue ({}): {} top players, using MEDIUM fallback chance",
                     isArena ? "Arena" : "BG", topPlayerCount);
        }
        else if (topPlayerCount <= 100)
        {
            // Some top players (51-100): 30% chance
            queueChance = 0.30f;
        }
        else
        {
            // Many top players (101+): use configured normal chance (15% default)
            queueChance = sPlayerbotAIConfig->competitiveQueueNormalChance;
        }
    }

    // Roll random chance
    float roll = frand(0.0f, 1.0f);
    bool canQueue = roll < queueChance;
    
    // Debug logging to track queue decisions
    LOG_DEBUG("playerbots", "CompetitiveQueue: Bot {} ({}) - topPlayers={}, isTopRanked={}, chance={:.1f}%, roll={:.3f}, canQueue={}",
             player->GetName(), isArena ? "Arena" : "BG", topPlayerCount, isTopRanked, queueChance * 100.0f, roll, canQueue);
    
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

bool CompetitiveQueueMgr::IsMidRankedArenaPlayer(Player* player)
{
    if (!player)
        return false;

    uint32 guidLow = player->GetGUID().GetCounter();
    return _midArenaPlayerGuids.find(guidLow) != _midArenaPlayerGuids.end();
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
    uint32 topRankLimit = sPlayerbotAIConfig->competitiveQueueTopRankBG;

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
    _midArenaPlayerGuids.clear();

    // Query top N + M players by highest arena team rating
    uint32 topRankLimit = sPlayerbotAIConfig->competitiveQueueTopRankArena;
    uint32 totalLimit = topRankLimit * 5; // e.g. 200 top, 800 mid = 1000 total

    QueryResult result = CharacterDatabase.Query(
        "SELECT atm.guid, MAX(at.rating) AS maxRating "
        "FROM arena_team_member atm "
        "INNER JOIN arena_team at ON atm.arenaTeamId = at.arenaTeamId "
        "WHERE at.seasonGames >= 10 "
        "GROUP BY atm.guid "
        "HAVING maxRating > 0 "
        "ORDER BY maxRating DESC "
        "LIMIT {}",
        totalLimit
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
            totalLimit
        );
    }

    if (!result)
    {
        LOG_INFO("playerbots", "CompetitiveQueue (Arena): No arena data found");
        _lastArenaCacheUpdate = std::time(nullptr);
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].Get<uint32>();
        
        if (count < topRankLimit)
            _topArenaPlayerGuids.insert(guid);
        else
            _midArenaPlayerGuids.insert(guid);
            
        count++;
    } while (result->NextRow());

    _lastArenaCacheUpdate = std::time(nullptr);
    LOG_INFO("playerbots", "CompetitiveQueue (Arena): Loaded {} top and {} mid arena players", 
             _topArenaPlayerGuids.size(), _midArenaPlayerGuids.size());
}
