/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_PVPGUILDNAMES_H
#define _PLAYERBOT_PVPGUILDNAMES_H

#include <string>
#include <vector>

class PvPGuildNames
{
public:
    // Get a random PvP guild name (normal or elite)
    static std::string GetRandomPvPGuildName(bool isElite = false);
    
private:
    static std::vector<std::string> const normalGuildNames;
    static std::vector<std::string> const eliteGuildNames;
};

#endif
