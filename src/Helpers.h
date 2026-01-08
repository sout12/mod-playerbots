/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_HELPERS_H
#define _PLAYERBOT_HELPERS_H

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <locale>
#include <map>
#include <sstream>
#include <vector>

#include "Common.h"

inline void split(std::vector<std::string>& dest, std::string const str, char const* delim)
{
    char* pTempStr = strdup(str.c_str());
    char* pWord = strtok(pTempStr, delim);

    while (pWord != nullptr)
    {
        dest.push_back(pWord);
        pWord = strtok(nullptr, delim);
    }

    free(pTempStr);
}

inline std::vector<std::string>& split(std::string const s, char delim, std::vector<std::string>& elems)
{
    std::stringstream ss(s);
    std::string item;

    while (getline(ss, item, delim))
    {
        elems.push_back(item);
    }

    return elems;
}

inline std::vector<std::string> split(std::string const s, char delim)
{
    std::vector<std::string> elems;
    return split(s, delim, elems);
}

// Forward declarations
class Player;
class Unit;

// Helper functions for bot operations
void SafeTeleport(Player* player, uint32 mapId, float x, float y, float z, float orientation, std::string const& reason = "");
bool IsValidUnit(Unit* unit);
bool IsValidPlayer(Player* player);

#endif
