/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "GuildAcceptAction.h"

#include "Event.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "GuildPackets.h"
#include "PlayerbotSecurity.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

bool GuildAcceptAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    p.rpos(0);
    Player* inviter = nullptr;
    std::string Invitedname;
    p >> Invitedname;

    if (normalizePlayerName(Invitedname))
        inviter = ObjectAccessor::FindPlayerByName(Invitedname.c_str());

    if (!inviter)
        return false;

    bool accept = true;
    uint32 guildId = inviter->GetGuildId();
    if (!guildId)
    {
        botAI->TellError("You are not in a guild!");
        accept = false;
    }
    else if (bot->GetGuildId())
    {
        botAI->TellError("Sorry, I am in a guild already");
        accept = false;
    }
    else if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, false, inviter, true))
    {
        botAI->TellError("Sorry, I don't want to join your guild :(");
        accept = false;
    }
    else if (sRandomPlayerbotMgr->IsRandomBot(bot))
    {
        // Random bots should only join bot-owned guilds, not player-owned guilds
        // Check if the guild master is a real player
        if (Guild* guild = sGuildMgr->GetGuildById(guildId))
        {
            ObjectGuid guildMasterGuid = guild->GetLeaderGUID();
            Player* guildMaster = ObjectAccessor::FindPlayer(guildMasterGuid);
            
            // If guild master is online, check if they're a bot
            // If offline, check the character database
            bool isPlayerOwnedGuild = false;
            
            if (guildMaster)
            {
                // Guild master is online - check if they're a real player
                isPlayerOwnedGuild = !sRandomPlayerbotMgr->IsRandomBot(guildMaster);
            }
            else
            {
                // Guild master is offline - check if GUID is in random bot list
                isPlayerOwnedGuild = !sRandomPlayerbotMgr->IsRandomBot(guildMasterGuid.GetCounter());
            }
            
            if (isPlayerOwnedGuild)
            {
                LOG_DEBUG("playerbots", "Random bot {} declining invite to player-owned guild {} (GM: {})",
                         bot->GetName(), guild->GetName(), guildMasterGuid.ToString());
                botAI->TellError("Sorry, I only join bot guilds!");
                accept = false;
            }
        }
    }

    if (accept)
    {
        WorldPackets::Guild::AcceptGuildInvite data = WorldPacket(CMSG_GUILD_ACCEPT);
        bot->GetSession()->HandleGuildAcceptOpcode(data);
    }
    else
    {
        WorldPackets::Guild::GuildDeclineInvitation data = WorldPacket(CMSG_GUILD_DECLINE);
        bot->GetSession()->HandleGuildDeclineOpcode(data);
    }

    return true;
}
