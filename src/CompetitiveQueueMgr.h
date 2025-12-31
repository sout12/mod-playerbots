/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_COMPETITIVEQUEUEMGR_H
#define _PLAYERBOT_COMPETITIVEQUEUEMGR_H

#include "Player.h"
#include <unordered_map>
#include <unordered_set>

class CompetitiveQueueMgr
{
public:
    CompetitiveQueueMgr();
    static CompetitiveQueueMgr* instance()
    {
        static CompetitiveQueueMgr instance;
        return &instance;
    }

    // Check if player should queue based on their competitive ranking
    // For BGs: checks BG killing blows ranking
    // For Arenas: checks arena team rating ranking
    bool ShouldQueueForBG(Player* player, bool isArena = false);
    
    // Check if player is in top-ranked list (for BGs)
    bool IsTopRankedPlayer(Player* player);
    
    // Check if player is in top-ranked arena list
    bool IsTopRankedArenaPlayer(Player* player);

    // Force refresh of rankings cache
    void RefreshRankings();

private:
    void UpdateTopPlayersCache();
    void UpdateTopArenaPlayersCache();

    std::set<uint32> _topPlayerGuids;      // Top BG players by killing blows
    std::set<uint32> _topArenaPlayerGuids; // Top arena players by team rating
    time_t _lastCacheUpdate;
    time_t _lastArenaCacheUpdate;

    static constexpr uint32 CACHE_DURATION = 300; // 5 minutes
};

#define sCompetitiveQueueMgr CompetitiveQueueMgr::instance()

#endif
