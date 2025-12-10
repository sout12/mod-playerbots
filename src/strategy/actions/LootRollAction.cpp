/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LootRollAction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "AiFactory.h"
#include "AiObjectContext.h"
#include "Event.h"
#include "Group.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SkillDiscovery.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"

// Loot roll logic is DB/core-first: DB/core fields drive decisions; heuristics only fill gaps.
// Policy is controlled via AiPlayerbot.Roll.* config flags instead of hard-coded class/weapon/armor tables.

// Forward declarations used by helpers defined later in this file
static bool IsPrimaryForSpec(Player* bot, ItemTemplate const* proto);
static bool HasAnyStat(ItemTemplate const* proto,
                       std::initializer_list<ItemModType> mods);

// Forward declaration for the common "item usage" key builder
static std::string BuildItemUsageParam(uint32 itemId, int32 randomProperty);

// Generic helper: iterate over all bot members in the same group.
template <typename Func>
static bool ForEachBotGroupMember(Player* self, Func&& func)
{
    if (!self)
    {
        return false;
    }

    Group* group = self->GetGroup();
    if (!group)
    {
        return false;
    }

    for (GroupReference* it = group->GetFirstMember(); it; it = it->next())
    {
        Player* member = it->GetSource();
        if (!member || member == self || !member->IsInWorld())
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI) // ignore real players
            continue;

        if (func(member, memberAI))
        {
            return true;
        }
    }

    return false;
}

// Collects class/archetype info used by loot rules
struct SpecTraits
{
    uint8 cls = 0;
    std::string spec;
    bool isCaster = false;   // caster-stat profile
    bool isHealer = false;
    bool isTank = false;
    bool isPhysical = false; // physical-stat profile
    bool isDKTank = false;
    bool isWarProt = false;
    bool isEnhSham = false;
    bool isFeralTk = false;
    bool isFeralDps = false;
    bool isHunter = false;
    bool isRogue = false;
    bool isWarrior = false;
    bool isRetPal = false;
    bool isProtPal = false;
};

static SpecTraits GetSpecTraits(Player* bot)
{
    SpecTraits t;
    if (!bot)
        return t;
    t.cls = bot->getClass();
    t.spec = AiFactory::GetPlayerSpecName(bot);

    auto specIs = [&](char const* s) { return t.spec == s; };

    // "Pure caster" classes
    const bool pureCasterClass = (t.cls == CLASS_MAGE || t.cls == CLASS_WARLOCK || t.cls == CLASS_PRIEST);

    // Paladin
    const bool holyPal = (t.cls == CLASS_PALADIN && specIs("holy"));
    const bool protPal = (t.cls == CLASS_PALADIN && (specIs("prot") || specIs("protection")));
    t.isProtPal = protPal;
    t.isRetPal = (t.cls == CLASS_PALADIN && !holyPal && !protPal);
    // DK
    const bool dk = (t.cls == CLASS_DEATH_KNIGHT);
    const bool dkBlood = dk && specIs("blood");
    const bool dkFrost = dk && specIs("frost");
    const bool dkUH = dk && (specIs("unholy") || specIs("uh"));
    t.isDKTank = (dkBlood || dkFrost) && !dkUH;  // tanks “blood/frost”
    // Warrior
    t.isWarrior = (t.cls == CLASS_WARRIOR);
    t.isWarProt = t.isWarrior && (specIs("prot") || specIs("protection"));
    // Hunter & Rogue
    t.isHunter = (t.cls == CLASS_HUNTER);
    t.isRogue = (t.cls == CLASS_ROGUE);
    // Shaman
    const bool eleSham = (t.cls == CLASS_SHAMAN && specIs("elemental"));
    const bool restoSh = (t.cls == CLASS_SHAMAN && (specIs("resto") || specIs("restoration")));
    t.isEnhSham = (t.cls == CLASS_SHAMAN && (specIs("enhance") || specIs("enhancement")));
    // Druid
    const bool balance = (t.cls == CLASS_DRUID && specIs("balance"));
    const bool restoDr = (t.cls == CLASS_DRUID && (specIs("resto") || specIs("restoration")));
    t.isFeralTk = (t.cls == CLASS_DRUID && (specIs("feraltank") || specIs("bear")));
    t.isFeralDps = (t.cls == CLASS_DRUID && (specIs("feraldps") || specIs("cat") || specIs("kitty")));

    // Roles
    t.isHealer = holyPal || restoSh || restoDr || (t.cls == CLASS_PRIEST && !specIs("shadow"));
    t.isTank = protPal || t.isWarProt || t.isFeralTk || t.isDKTank;
    t.isCaster = pureCasterClass || holyPal || eleSham || balance || restoDr || restoSh ||
                 (t.cls == CLASS_PRIEST && specIs("shadow"));  // Shadow = caster DPS
    t.isPhysical = !t.isCaster;
    return t;
}

// Tank avoidance stats helper
static bool HasAnyTankAvoidance(ItemTemplate const* proto)
{
    // Tank avoidance stats (defense, dodge, parry, block rating)
    return HasAnyStat(proto, {
        ITEM_MOD_DEFENSE_SKILL_RATING,
        ITEM_MOD_DODGE_RATING,
        ITEM_MOD_PARRY_RATING,
        ITEM_MOD_BLOCK_RATING
    });
}

// Return true if the invType is a "body armor" slot (not jewelry/cape/weapon/shield/relic/holdable)
static bool IsBodyArmorInvType(uint8 invType)
{
    switch (invType)
    {
        case INVTYPE_HEAD:
        case INVTYPE_SHOULDERS:
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
        case INVTYPE_WAIST:
        case INVTYPE_LEGS:
        case INVTYPE_FEET:
        case INVTYPE_WRISTS:
        case INVTYPE_HANDS:
            return true;
        default:
            return false;
    }
}

// True if the invType is a jewelry/cloak slot (rings, trinkets, neck, cloak)
static bool IsJewelryOrCloak(ItemTemplate const* proto)
{
    if (!proto)
    {
        return false;
    }

    switch (proto->InventoryType)
    {
        case INVTYPE_TRINKET:
        case INVTYPE_FINGER:
        case INVTYPE_NECK:
        case INVTYPE_CLOAK:
            return true;
        default:
            return false;
    }
}

// Preferred armor subclass (ITEM_SUBCLASS_ARMOR_*) for the bot (WotLK rules)
static uint8 PreferredArmorSubclassFor(Player* bot)
{
    if (!bot)
        return ITEM_SUBCLASS_ARMOR_CLOTH;

    uint8 cls = bot->getClass();
    uint32 lvl = bot->GetLevel();

    // Pure cloth classes
    if (cls == CLASS_MAGE || cls == CLASS_PRIEST || cls == CLASS_WARLOCK)
        return ITEM_SUBCLASS_ARMOR_CLOTH;

    // Leather forever
    if (cls == CLASS_DRUID || cls == CLASS_ROGUE)
        return ITEM_SUBCLASS_ARMOR_LEATHER;

    // Hunter / Shaman: <40 leather, >=40 mail
    if (cls == CLASS_HUNTER || cls == CLASS_SHAMAN)
        return (lvl >= 40u) ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;

    // Warrior / Paladin: <40 mail, >=40 plate
    if (cls == CLASS_WARRIOR || cls == CLASS_PALADIN)
        return (lvl >= 40u) ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;

    // Death Knight: plate from the start
    if (cls == CLASS_DEATH_KNIGHT)
        return ITEM_SUBCLASS_ARMOR_PLATE;

    return ITEM_SUBCLASS_ARMOR_CLOTH;
}

// True if the item is a body armor piece of a strictly lower tier than preferred (cloth<leather<mail<plate)
static bool IsLowerTierArmorForBot(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;
    if (proto->Class != ITEM_CLASS_ARMOR)
        return false;
    if (!IsBodyArmorInvType(proto->InventoryType))
        return false; // ignore jewelry/capes/etc.
    // Shields / relics / holdables are not considered here
    if (proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD || proto->InventoryType == INVTYPE_RELIC || proto->InventoryType == INVTYPE_HOLDABLE)
        return false;

    uint8 preferred = PreferredArmorSubclassFor(bot);
    // ITEM_SUBCLASS_ARMOR_* are ordered Cloth(1) < Leather(2) < Mail(3) < Plate(4) on 3.3.5
    return proto->SubClass < preferred;
}

// True if the bot lacks the armor proficiency for this body piece (cloth is always allowed).
static bool IsArmorProficiencyMissingForBody(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;

    if (proto->Class != ITEM_CLASS_ARMOR)
        return false;

    if (!IsBodyArmorInvType(proto->InventoryType))
        return false;

    InventoryResult const result = bot->CanUseItem(proto);
    // Core uses EQUIP_ERR_NO_REQUIRED_PROFICIENCY when the armor proficiency is missing
    return result == EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
}

static bool GroupHasPrimaryArmorUserLikelyToNeed(Player* self, ItemTemplate const* proto, int32 randomProperty)
{
    if (!self || !proto)
        return false;

    if (proto->Class != ITEM_CLASS_ARMOR || !IsBodyArmorInvType(proto->InventoryType))
        return false;

    std::string const param = BuildItemUsageParam(proto->ItemId, randomProperty);

    return ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* memberAI) -> bool
        {
            // Do not treat it as "primary" for bots for which this is also cross-armor
            if (IsLowerTierArmorForBot(member, proto))
                return false;

            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            if (!ctx)
                return false;

            ItemUsage otherUsage = ctx->GetValue<ItemUsage>("item usage", param)->Get();
            if (otherUsage == ITEM_USAGE_EQUIP || otherUsage == ITEM_USAGE_REPLACE)
            {
                LOG_DEBUG("playerbots",
                          "[LootRollDBG] cross-armor: primary armor user {} likely to need item={} \"{}\"",
                          member->GetName(), proto->ItemId, proto->Name1);
                return true;
            }

            return false;
        });
}

// True if some bot has this armor as an upgrade and an empty/very bad item in that slot.
static bool GroupHasDesperateUpgradeUser(Player* self, ItemTemplate const* proto, int32 randomProperty)
{
    if (!self || !proto)
    {
        return false;
    }

    // Only makes sense for real body armor pieces (no jewelry/capes/weapons).
    if (proto->Class != ITEM_CLASS_ARMOR || !IsBodyArmorInvType(proto->InventoryType))
    {
        return false;
    }

    std::string const param = BuildItemUsageParam(proto->ItemId, randomProperty);

    return ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* memberAI) -> bool
        {
            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            if (!ctx)
                return false;

            ItemUsage usage = ctx->GetValue<ItemUsage>("item usage", param)->Get();
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                return false; // not a true upgrade for this bot

            ItemTemplate const* bestProto = nullptr;

            // Look at the best currently equipped armor of the same InventoryType
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* oldItem = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!oldItem)
                    continue;

                ItemTemplate const* oldProto = oldItem->GetTemplate();
                if (!oldProto)
                    continue;

                if (oldProto->Class != ITEM_CLASS_ARMOR)
                    continue;

                if (oldProto->InventoryType != proto->InventoryType)
                    continue;

                if (!bestProto || oldProto->ItemLevel > bestProto->ItemLevel)
                    bestProto = oldProto;
            }

            bool hasVeryBadItem = false;

            if (!bestProto)
            {
                // Empty slot -> extremely undergeared for this piece
                hasVeryBadItem = true;
            }
            else if (bestProto->Quality <= ITEM_QUALITY_NORMAL)
            {
                // Poor (grey) or Common (white) gear -> considered "bad"
                hasVeryBadItem = true;
            }

            if (hasVeryBadItem)
            {
                LOG_DEBUG("playerbots",
                          "[LootRollDBG] cross-armor: desperate upgrade candidate {} blocks cross-armor NEED on item={} \"{}\"",
                          member->GetName(), proto->ItemId, proto->Name1);
                return true;
            }

            return false;
        });
}

// True if some bot has this jewelry/cloak as an upgrade and an empty/very bad item in the corresponding slots.
static bool GroupHasDesperateJewelryUpgradeUser(Player* self, ItemTemplate const* proto, int32 randomProperty)
{
    if (!self || !proto)
    {
        return false;
    }

    uint8 jewelrySlots[2];
    uint8 slotsCount = 0;

    switch (proto->InventoryType)
    {
        case INVTYPE_NECK:
            jewelrySlots[0] = EQUIPMENT_SLOT_NECK;
            slotsCount = 1;
            break;
        case INVTYPE_FINGER:
            jewelrySlots[0] = EQUIPMENT_SLOT_FINGER1;
            jewelrySlots[1] = EQUIPMENT_SLOT_FINGER2;
            slotsCount = 2;
            break;
        case INVTYPE_TRINKET:
            jewelrySlots[0] = EQUIPMENT_SLOT_TRINKET1;
            jewelrySlots[1] = EQUIPMENT_SLOT_TRINKET2;
            slotsCount = 2;
            break;
        case INVTYPE_CLOAK:
            jewelrySlots[0] = EQUIPMENT_SLOT_BACK;
            slotsCount = 1;
            break;
        default:
            // Not jewelry/cloak -> nothing to do here
            return false;
    }

    std::string const param = BuildItemUsageParam(proto->ItemId, randomProperty);

    return ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* memberAI) -> bool
        {
            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            if (!ctx)
                return false;

            ItemUsage usage = ctx->GetValue<ItemUsage>("item usage", param)->Get();
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                return false; // not a true upgrade for this bot

            ItemTemplate const* bestProto = nullptr;

            // Look at the best currently equipped jewelry in the corresponding slots
            for (uint8 i = 0; i < slotsCount; ++i)
            {
                uint8 const slot = jewelrySlots[i];
                Item* oldItem = member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!oldItem)
                    continue;

                ItemTemplate const* oldProto = oldItem->GetTemplate();
                if (!oldProto)
                    continue;

                if (!bestProto || oldProto->ItemLevel > bestProto->ItemLevel)
                    bestProto = oldProto;
            }

            bool hasVeryBadItem = false;

            if (!bestProto)
            {
                // Empty slot -> extremely undergeared for this jewelry/cloak
                hasVeryBadItem = true;
            }
            else if (bestProto->Quality <= ITEM_QUALITY_NORMAL)
            {
                // Poor (grey) or Common (white) jewelry -> considered "bad"
                hasVeryBadItem = true;
            }

            if (hasVeryBadItem)
            {
                LOG_DEBUG("playerbots",
                          "[LootRollDBG] jewelry-fallback: desperate upgrade candidate {} for item={} \"{}\"",
                          member->GetName(), proto->ItemId, proto->Name1);
                return true;
            }

            return false;
        });
}

// True if some bot has this item as a primary-spec upgrade.
static bool GroupHasPrimarySpecUpgradeCandidate(Player* self, ItemTemplate const* proto, int32 randomProperty)
{
    if (!self || !proto)
        return false;

    std::string const param = BuildItemUsageParam(proto->ItemId, randomProperty);

    bool found = ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* memberAI) -> bool
        {
            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            if (!ctx)
                return false;

            ItemUsage otherUsage = ctx->GetValue<ItemUsage>("item usage", param)->Get();
            if (otherUsage != ITEM_USAGE_EQUIP && otherUsage != ITEM_USAGE_REPLACE)
                return false;

            if (!IsPrimaryForSpec(member, proto))
                return false;

            LOG_DEBUG("playerbots",
                      "[LootRollDBG] group primary spec upgrade: {} is primary candidate for item={} \"{}\" (usage={})",
                      member->GetName(), proto->ItemId, proto->Name1, static_cast<int32>(otherUsage));
            return true;
        });

    if (!found)
    {
        LOG_DEBUG("playerbots",
                  "[LootRollDBG] group primary spec upgrade: no primary candidate for item={} \"{}\"",
                  proto->ItemId, proto->Name1);
    }

    return found;
}

// Small aggregate of commonly used stat flags for loot rules.
struct ItemStatProfile
{
    bool hasINT = false;
    bool hasSPI = false;
    bool hasMP5 = false;
    bool hasSP = false;
    bool hasSTR = false;
    bool hasAGI = false;
    bool hasSTA = false;
    bool hasAP = false;
    bool hasARP = false;
    bool hasEXP = false;
    bool hasHIT = false;
    bool hasHASTE = false;
    bool hasCRIT = false;
    bool hasDef = false;
    bool hasAvoid = false;
    bool hasBlockValue = false;
};

// Build stat flags once for a given item and reuse them in higher-level helpers.
static ItemStatProfile BuildItemStatProfile(ItemTemplate const* proto)
{
    ItemStatProfile s;
    if (!proto)
        return s;

    s.hasINT = HasAnyStat(proto, {ITEM_MOD_INTELLECT});
    s.hasSPI = HasAnyStat(proto, {ITEM_MOD_SPIRIT});
    s.hasMP5 = HasAnyStat(proto, {ITEM_MOD_MANA_REGENERATION});
    s.hasSP = HasAnyStat(proto, {ITEM_MOD_SPELL_POWER});
    s.hasSTR = HasAnyStat(proto, {ITEM_MOD_STRENGTH});
    s.hasAGI = HasAnyStat(proto, {ITEM_MOD_AGILITY});
    s.hasSTA = HasAnyStat(proto, {ITEM_MOD_STAMINA});
    s.hasAP = HasAnyStat(proto, {ITEM_MOD_ATTACK_POWER, ITEM_MOD_RANGED_ATTACK_POWER});
    s.hasARP = HasAnyStat(proto, {ITEM_MOD_ARMOR_PENETRATION_RATING});
    s.hasEXP = HasAnyStat(proto, {ITEM_MOD_EXPERTISE_RATING});
    s.hasHIT = HasAnyStat(proto, {ITEM_MOD_HIT_RATING});
    s.hasHASTE = HasAnyStat(proto, {ITEM_MOD_HASTE_RATING});
    s.hasCRIT = HasAnyStat(proto, {ITEM_MOD_CRIT_RATING});
    s.hasDef = HasAnyStat(proto, {ITEM_MOD_DEFENSE_SKILL_RATING});
    s.hasAvoid = HasAnyStat(proto, {ITEM_MOD_DODGE_RATING, ITEM_MOD_PARRY_RATING, ITEM_MOD_BLOCK_RATING});
    s.hasBlockValue = HasAnyStat(proto, {ITEM_MOD_BLOCK_VALUE});

    return s;
}

// True if it is still reasonable for this spec to NEED the item as off-spec when no primary-spec upgrade exists.
static bool IsFallbackNeedReasonableForSpec(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;

    SpecTraits const traits = GetSpecTraits(bot);

    bool const isJewelry = IsJewelryOrCloak(proto);

    bool const isBodyArmor = proto->Class == ITEM_CLASS_ARMOR && IsBodyArmorInvType(proto->InventoryType);

    ItemStatProfile const stats = BuildItemStatProfile(proto);

    // Spirit-only regen jewelry (no INT/SP/offensive stats).
    bool const pureSpiritJewelry = isJewelry && stats.hasSPI && !stats.hasSP && !stats.hasINT &&
                                   !stats.hasSTR && !stats.hasAGI && !stats.hasAP && !stats.hasARP && !stats.hasEXP &&
                                   !stats.hasHIT && !stats.hasHASTE && !stats.hasCRIT;

    bool const looksCaster = stats.hasSP || stats.hasSPI || stats.hasMP5 ||
                             (stats.hasINT && !stats.hasSTR && !stats.hasAGI && !stats.hasAP);
    bool const looksPhysical = stats.hasSTR || stats.hasAGI || stats.hasAP || stats.hasARP || stats.hasEXP;

    // Physical specs: never fallback-NEED pure caster body armor or SP weapons/shields.
    // Also, if the bot cannot even wear the armor (no proficiency skill), NEED is never reasonable.
    if (IsArmorProficiencyMissingForBody(bot, proto))
    {
        return false;
    }

    if (traits.isPhysical)
    {
        // Do not fallback-NEED caster body armor (cloth/leather/mail/plate with SP/INT caster profile)
        if (isBodyArmor && looksCaster)
        {
            return false;
        }

        // Do not fallback-NEED caster jewelry/cloaks (pure SP/INT/SPI/MP5 profiles)
        if (isJewelry && looksCaster)
        {
            return false;
        }

        // Do not fallback-NEED spell power weapons or shields
        if ((proto->Class == ITEM_CLASS_WEAPON ||
             (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)) &&
            stats.hasSP)
        {
            return false;
        }
    }

    // DPS casters: do not fallback-NEED pure Spirit regen jewelry; leave that for healers.
    if (pureSpiritJewelry && traits.isCaster && !traits.isHealer)
    {
        return false;
    }

    // Caster/healer specs: never fallback-NEED body armor without caster stats or pure melee jewelry.
    if (traits.isCaster || traits.isHealer)
    {
        // Body armor without caster-friendly stats (INT/SP/SPI/MP5) is not a reasonable
        // fallback NEED for healers/casters. This covers cases where the template is neutral
        // and the real stats come from a very physical random suffix.
        if (isBodyArmor && !looksCaster)
            return false;

        // Pure melee jewelry (STR/AGI/AP/ARP/EXP only) is also not reasonable as fallback NEED.
        if (isJewelry && looksPhysical && !stats.hasSP && !stats.hasINT)
            return false;
    }

    // Default: allow fallback NEED for this spec/item combination.
    return true;
}

// Lowercase helper for item names (ASCII/locale-agnostic enough for pattern matching).
static std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Detect classic lockboxes a rogue can pick (with an English name fallback).
static bool IsLockbox(ItemTemplate const* proto)
{
    if (!proto)
    {
        return false;
    }
    // Primary, data-driven detection
    if (proto->LockID)
    {
        // Most lockboxes are misc/junk and openable in WotLK
        if (proto->Class == ITEM_CLASS_MISC)
            return true;
    }
    // English-only fallback on name (align with TokenSlotFromName behavior)
    std::string n = ToLowerAscii(proto->Name1);
    return n.find("lockbox") != std::string::npos;
}

// Local helper: not a class member
static bool HasAnyStat(ItemTemplate const* proto,
                       std::initializer_list<ItemModType> mods)
{
    if (!proto)
    {
        return false;
    }

    for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        if (!proto->ItemStat[i].ItemStatValue)
            continue;

        ItemModType const t = ItemModType(proto->ItemStat[i].ItemStatType);
        for (ItemModType const m : mods)
        {
            if (t == m)
            {
                return true;
            }
        }
    }
    return false;
}

// Special stat patterns (currently INT+AP for Ret / Hunter / Enh).
static bool GroupHasPreferredIntApUser(Player* self)
{
    if (!self)
    {
        return false;
    }

    bool found = ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* /*memberAI*/) -> bool
        {
            SpecTraits t = GetSpecTraits(member);
            bool const isProtPal = t.isProtPal || (t.cls == CLASS_PALADIN && t.isTank);  // fallback
            if (t.isRetPal || isProtPal || t.cls == CLASS_HUNTER || t.isEnhSham)
            {
                LOG_DEBUG("playerbots",
                          "[LootPaternDBG] INT+AP group check: priority looter present -> {} (spec='{}')",
                          member->GetName(), t.spec);
                return true;
            }

            return false;
        });

    if (!found)
    {
        LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP group check: no loot-priority bot present");
    }

    return found;
}

// Helper: Is there a priority "likely" to NEED (upgrade/equipable)?
static bool GroupHasPreferredIntApUserLikelyToNeed(Player* self, ItemTemplate const* proto)
{
    if (!self || !proto)
    {
        return false;
    }

    std::string const param = BuildItemUsageParam(proto->ItemId, 0);

    bool found = ForEachBotGroupMember(self,
        [&](Player* member, PlayerbotAI* memberAI) -> bool
        {
            SpecTraits t = GetSpecTraits(member);
            bool const isProtPal = t.isProtPal || (t.cls == CLASS_PALADIN && t.isTank);
            bool const isPreferred = t.isRetPal || isProtPal || t.cls == CLASS_HUNTER || t.isEnhSham;
            if (!isPreferred)
            {
                return false;
            }

            // Estimate if the priority bot will NEED (plausible upgrade).
            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            if (!ctx)
                return false;

            ItemUsage usage = ctx->GetValue<ItemUsage>("item usage", param)->Get();

            LOG_DEBUG("playerbots",
                      "[LootPaternDBG] INT+AP likely-to-need: {} (spec='{}') usage={} (EQUIP=1 REPLACE=2) itemId={}",
                      member->GetName(), t.spec, static_cast<int>(usage), proto->ItemId);

            if (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP)
            {
                LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP likely-to-need: {} -> TRUE", member->GetName());
                return true;  // a priority bot will probably NEED
            }

            return false;
        });

    if (!found)
    {
        LOG_DEBUG("playerbots",
                  "[LootPaternDBG] INT+AP likely-to-need: no loot-priority bot in a position to NEED");
    }

    return found;
}

//Registry "stat patterns" (currently: INT+AP only)
struct StatPattern
{
    const char* name;
    bool (*matches)(ItemTemplate const* proto);
    bool (*decide)(Player* bot, ItemTemplate const* proto, const SpecTraits& traits, bool& outPrimary);
};

// Pattern INT + AP
static bool Match_IntAndAp(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    ItemStatProfile const stats = BuildItemStatProfile(proto);

    // Treat as INT+AP hybrid only if it is not clearly a caster/healer item (SP/SPI/MP5).
    const bool looksCaster = stats.hasSP || stats.hasSPI || stats.hasMP5;
    const bool match = stats.hasINT && stats.hasAP && !looksCaster;

    LOG_DEBUG("playerbots",
              "[LootPaternDBG] INT+AP match? {} -> item={} \"{}\" (INT={} AP={} casterStats={})",
              match ? "YES" : "NO", proto->ItemId, proto->Name1,
              stats.hasINT ? 1 : 0, stats.hasAP ? 1 : 0, looksCaster ? 1 : 0);

    return match;
}

// Decision: Ret/Hunter/Enh -> primary; non-caster physical -> not primary; casters -> primary only if no priority in group/raid
static bool Decide_IntAndAp(Player* bot, ItemTemplate const* proto, const SpecTraits& traits, bool& outPrimary)
{
    LOG_DEBUG("playerbots", "[LootPaternDBG] patterns: evaluation bot={} item={} \"{}\"", bot->GetName(), proto->ItemId,
              proto->Name1);
    const bool isProtPal = traits.isProtPal || (traits.cls == CLASS_PALADIN && traits.isTank);  // fallback
    if (traits.isRetPal || isProtPal || traits.cls == CLASS_HUNTER || traits.isEnhSham)
    {
        outPrimary = true;
        LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP decide: {} (spec='{}') prioritaire -> primary=1",
                  bot->GetName(), traits.spec);
        return true;
    }
    if (!traits.isCaster)
    {
        outPrimary = false;
        LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP decide: {} (spec='{}') physique non-caster -> primary=0",
                  bot->GetName(), traits.spec);
        return true;
    }
    // Casters: primary if no priority present
    if (!GroupHasPreferredIntApUser(bot))
    {
        outPrimary = true;
        LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP decide: {} (spec='{}') caster, aucun prioritaire -> primary=1",
                  bot->GetName(), traits.spec);
        return true;
    }
    // or if there are no "likely NEED" priorities
    outPrimary = !GroupHasPreferredIntApUserLikelyToNeed(bot, proto);
    LOG_DEBUG("playerbots", "[LootPaternDBG] INT+AP decide: {} (spec='{}') caster, prioritaires présents -> primary={}",
              bot->GetName(), traits.spec, (int)outPrimary);
    return true;
}

// List of active patterns (only INT+AP for now)
static const std::array<StatPattern, 1> kStatPatterns = {{
    {"INT+AP", &Match_IntAndAp, &Decide_IntAndAp},
}};

static bool ApplyStatPatternsForPrimary(Player* bot, ItemTemplate const* proto, const SpecTraits& traits,
                                        bool& outPrimary)
{
    for (auto const& p : kStatPatterns)
    {
        if (p.matches(proto))
        {
            if (p.decide(bot, proto, traits, outPrimary))
            {
                LOG_DEBUG("playerbots", "[LootPaternDBG] pattern={} primary={} bot={} item={} \"{}\"", p.name,
                          (int)outPrimary, bot->GetName(), proto->ItemId, proto->Name1);
                return true;
            }
        }
    }
    LOG_DEBUG("playerbots", "[LootPaternDBG] patterns: no applicable pattern bot={} item={} \"{}\"", bot->GetName(),
              proto->ItemId, proto->Name1);
    return false;
}

// Encode "random enchant" parameter for CalculateRollVote / ItemUsage.
// >0 => randomPropertyId, <0 => randomSuffixId, 0 => none
static inline int32 EncodeRandomEnchantParam(uint32 randomPropertyId, uint32 randomSuffix)
{
    if (randomPropertyId)
    {
        return static_cast<int32>(randomPropertyId);
    }
    if (randomSuffix)
    {
        return -static_cast<int32>(randomSuffix);
    }

    return 0;
}

static std::string BuildItemUsageParam(uint32 itemId, int32 randomProperty)
{
    if (randomProperty != 0)
    {
        return std::to_string(itemId) + "," + std::to_string(randomProperty);
    }

    return std::to_string(itemId);
}

// Profession helpers: true if the item is a recipe/pattern/book (ITEM_CLASS_RECIPE).
static inline bool IsRecipeItem(ItemTemplate const* proto) { return proto && proto->Class == ITEM_CLASS_RECIPE; }

// Detect the spell taught by a recipe and whether the bot already knows it; otherwise fall back to skill checks.
static bool BotAlreadyKnowsRecipeSpell(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;

    // Many recipes have a single spell that "teaches" another spell (learned spell in EffectTriggerSpell).
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 teach = proto->Spells[i].SpellId;
        if (!teach)
            continue;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(teach);
        if (!si)
            continue;
        for (int eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
        {
            if (si->Effects[eff].Effect == SPELL_EFFECT_LEARN_SPELL)
            {
                uint32 learned = si->Effects[eff].TriggerSpell;
                if (learned && bot->HasSpell(learned))
                    return true;  // already knows the taught spell
            }
        }
    }
    return false;
}

// Mounts / pets / vanity items: treat them as cosmetic collectibles for roll purposes.
static bool IsCosmeticCollectible(ItemTemplate const* proto)
{
    if (!proto)
    {
        return false;
    }

    if (proto->Class != ITEM_CLASS_MISC)
    {
        return false;
    }

// Mounts and companion pets are Misc; prefer core enums, otherwise fall back to known 3.3.5 subclasses.
#if defined(ITEM_SUBCLASS_MISC_MOUNT) || defined(ITEM_SUBCLASS_MISC_PET)
    if (
#  if defined(ITEM_SUBCLASS_MISC_MOUNT)
        proto->SubClass == ITEM_SUBCLASS_MISC_MOUNT
#  endif
#  if defined(ITEM_SUBCLASS_MISC_MOUNT) && defined(ITEM_SUBCLASS_MISC_PET)
        ||
#  endif
#  if defined(ITEM_SUBCLASS_MISC_PET)
        proto->SubClass == ITEM_SUBCLASS_MISC_PET
#  endif
    )
    {
        return true;
    }
#else
    if (proto->SubClass == 2 || proto->SubClass == 5)
    {
        return true;
    }
#endif

    return false;
}

// Returns true if the bot already owns or knows the collectible (mount/pet) in a meaningful way.
static bool BotAlreadyHasCollectible(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
    {
        return false;
    }

    // First, check if the item teaches a spell the bot already knows (typical for mounts/pets).
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 const spellId = proto->Spells[i].SpellId;
        if (!spellId)
        {
            continue;
        }

        if (bot->HasSpell(spellId))
        {
            return true;
        }
    }

    // Fallback: if the bot already has at least one copy of the item (bags or bank),
    // consider the collectible as "already owned" and do not roll NEED again.
    if (bot->GetItemCount(proto->ItemId, true) > 0)
    {
        return true;
    }

    return false;
}

// Special-case: Book of Glyph Mastery (can own several; do not downgrade NEED on duplicates).
static bool IsGlyphMasteryBook(ItemTemplate const* proto)
{
    if (!proto)
    {
        return false;
    }

    // 1) Must be a recipe book.
    if (proto->Class != ITEM_CLASS_RECIPE || proto->SubClass != ITEM_SUBCLASS_BOOK)
    {
        return false;
    }

    // 2) Primary signal: the on-use spell of the book on DBs
    // (Spell 64323: "Book of Glyph Mastery").
    constexpr uint32 SPELL_BOOK_OF_GLYPH_MASTERY = 64323;
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId == SPELL_BOOK_OF_GLYPH_MASTERY)
            return true;
    }

    // 3) Fallback: Inscription recipe book whose name hints glyph mastery (covers DB forks/locales).
    if (proto->RequiredSkill == SKILL_INSCRIPTION)
    {
        std::string n = ToLowerAscii(proto->Name1);
        if (n.find("glyph mastery") != std::string::npos || n.find("book of glyph mastery") != std::string::npos)
            return true;
    }

    return false;
}

// Pretty helper for RollVote name in logs
static inline char const* VoteTxt(RollVote v)
{
    switch (v)
    {
        case NEED:
            return "NEED";
        case GREED:
            return "GREED";
        case PASS:
            return "PASS";
        case DISENCHANT:
            return "DISENCHANT";
        default:
            return "UNKNOWN";
    }
}

// Centralised debug dump for recipe decisions
static void DebugRecipeRoll(Player* bot, ItemTemplate const* proto, ItemUsage usage, bool recipeChecked,
                            bool recipeUseful, bool recipeKnown, uint32 reqSkill, uint32 reqRank, uint32 botRank,
                            RollVote before, RollVote after)
{
    LOG_DEBUG("playerbots",
              "[LootPaternDBG] {} JC:{} item:{} \"{}\" class={} sub={} bond={} usage={} "
              "recipeChecked={} useful={} known={} reqSkill={} reqRank={} botRank={} vote:{} -> {} dupCount={}",
              bot->GetName(), bot->GetSkillValue(SKILL_JEWELCRAFTING), proto->ItemId, proto->Name1, proto->Class,
              proto->SubClass, proto->Bonding, (int)usage, recipeChecked, recipeUseful, recipeKnown, reqSkill, reqRank,
              botRank, VoteTxt(before), VoteTxt(after), bot->GetItemCount(proto->ItemId, true));
}

// Map a RECIPE subclass to the SkillLine when RequiredSkill is missing (fallback for DBs).
static uint32 GuessRecipeSkill(ItemTemplate const* proto)
{
    if (!proto)
        return 0;
    // If the core DB is sane, this is set and we can just return it in the caller.
    // Fallback heuristic on SubClass (books used by professions)
    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_BOOK:  // e.g. Book of Glyph Mastery
            // If the name hints glyphs, assume Inscription
            {
                std::string n = ToLowerAscii(proto->Name1);
                if (n.find("glyph") != std::string::npos)
                    return SKILL_INSCRIPTION;
            }
            break;
        case ITEM_SUBCLASS_LEATHERWORKING_PATTERN:
            return SKILL_LEATHERWORKING;
        case ITEM_SUBCLASS_TAILORING_PATTERN:
            return SKILL_TAILORING;
        case ITEM_SUBCLASS_ENGINEERING_SCHEMATIC:
            return SKILL_ENGINEERING;
        case ITEM_SUBCLASS_BLACKSMITHING:
            return SKILL_BLACKSMITHING;
        case ITEM_SUBCLASS_COOKING_RECIPE:
            return SKILL_COOKING;
        case ITEM_SUBCLASS_ALCHEMY_RECIPE:
            return SKILL_ALCHEMY;
        case ITEM_SUBCLASS_FIRST_AID_MANUAL:
            return SKILL_FIRST_AID;
        case ITEM_SUBCLASS_ENCHANTING_FORMULA:
            return SKILL_ENCHANTING;
        case ITEM_SUBCLASS_FISHING_MANUAL:
            return SKILL_FISHING;
        case ITEM_SUBCLASS_JEWELCRAFTING_RECIPE:
            return SKILL_JEWELCRAFTING;
        default:
            break;
    }
    return 0;
}

// True if this recipe/pattern/book is useful for one of the bot's professions and not already known.
static bool IsProfessionRecipeUsefulForBot(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !IsRecipeItem(proto))
    {
        return false;
    }

    // Primary path: DB usually sets RequiredSkill/RequiredSkillRank on recipe items.
    uint32 reqSkill = proto->RequiredSkill;
    uint32 reqRank = proto->RequiredSkillRank;

    if (!reqSkill)
    {
        reqSkill = GuessRecipeSkill(proto);
    }

    if (!reqSkill)
    {
        return false;  // unknown profession, be conservative
    }

    // Bot must have the profession (or secondary skill like Cooking/First Aid)
    if (!bot->HasSkill(reqSkill))
    {
        return false;
    }

    // Required rank check (can be disabled by config) — flatten nested if
    if (!sPlayerbotAIConfig->recipesIgnoreSkillRank && reqRank && bot->GetSkillValue(reqSkill) < reqRank)
    {
        return false;
    }

    // Avoid NEED if the taught spell is already known
    if (BotAlreadyKnowsRecipeSpell(bot, proto))
    {
        return false;
    }

    return true;
}

// Shared helper: check DB AllowableClass bitmask for a given class.
static bool IsClassAllowedByItemTemplate(uint8 cls, ItemTemplate const* proto)
{
    if (!proto)
    {
        // Non-item or invalid template: do not block by class here
        return true;
    }

    int32 const allowable = proto->AllowableClass;
    // item_template.AllowableClass is a bitmask of classes that can use the item.
    // -1 (and often 0 on custom DBs) means "no class restriction".
    if (allowable <= 0)
    {
        return true;
    }

    if (!cls)
    {
        return false;
    }

    uint32 const classMask = static_cast<uint32>(allowable);
    uint32 const thisClassBit = 1u << (cls - 1u);
    return (classMask & thisClassBit) != 0;
}

// Weapon/shield/relic eligibility per class, based on DB AllowableClass.
// Returns false when the item is a WEAPON / SHIELD / RELIC the class should NOT use.
static bool IsWeaponOrShieldOrRelicAllowedForClass(SpecTraits const& traits, ItemTemplate const* proto)
{
    if (!proto)
    {
        return true; // non-weapon items handled elsewhere
    }

    // Only filter weapons, shields and relics here. Other items are handled by spec/stat logic.
    bool const isShield = (proto->Class == ITEM_CLASS_ARMOR &&
                           proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD) ||
                          proto->InventoryType == INVTYPE_SHIELD;
    bool const isRelic = (proto->InventoryType == INVTYPE_RELIC);
    bool const isWeapon = (proto->Class == ITEM_CLASS_WEAPON);

    if (!isShield && !isRelic && !isWeapon)
    {
        return true;
    }
    return IsClassAllowedByItemTemplate(traits.cls, proto);
}

static bool IsPrimaryForSpec(Player* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;

    bool const isJewelry = IsJewelryOrCloak(proto);

    // Fishing poles: never treat them as primary weapons for any spec.
    if (proto->Class == ITEM_CLASS_WEAPON &&
        proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
    {
        return false;
    }

    const SpecTraits traits = GetSpecTraits(bot);

    // Never treat armor that the bot cannot wear (missing proficiency) as main-spec.
    if (IsArmorProficiencyMissingForBody(bot, proto))
    {
        return false;
    }

    // Tank fast-path for jewelry/cloaks
    // Typical case: Str+Dodge+Stam collar without Defense
    if (isJewelry && traits.isTank)
    {
        if (HasAnyTankAvoidance(proto))
        {
           // Jewelry/cloak with avoidance => "primary" for tanks
            return true;
        }
    }

    // HARD GUARD: never consider lower-tier armor as "primary" for the spec (body armor only)
    if (!isJewelry && proto->Class == ITEM_CLASS_ARMOR && IsBodyArmorInvType(proto->InventoryType))
    {
        if (IsLowerTierArmorForBot(bot, proto))
        {
            return false; // forces NEED->GREED earlier when SmartNeedBySpec is enabled
        }
    }

    // Hard filter first: do not NEED weapons/shields/relics the class shouldn't use.
    // If this returns false, the caller will downgrade to GREED (off-spec/unsupported).
    if (!IsWeaponOrShieldOrRelicAllowedForClass(traits, proto))
    {
        return false;
    }

    // Flags class/spec
    const bool isCasterSpec = traits.isCaster;
    const bool isTankLikeSpec = traits.isTank;
    const bool isPhysicalSpec = traits.isPhysical;

    ItemStatProfile const stats = BuildItemStatProfile(proto);

    // Quick profiles
    const bool looksCaster = stats.hasSP || stats.hasSPI || stats.hasMP5 ||
                             (stats.hasINT && !stats.hasSTR && !stats.hasAGI && !stats.hasAP);
    const bool looksPhysical = stats.hasSTR || stats.hasAGI || stats.hasAP || stats.hasARP || stats.hasEXP;
    const bool hasDpsRatings = stats.hasHIT || stats.hasHASTE || stats.hasCRIT;  // Common to all DPS (physical & casters)
    const bool pureSpiritJewelry = isJewelry && stats.hasSPI && !stats.hasSP && !stats.hasINT &&
                                   !stats.hasSTR && !stats.hasAGI && !stats.hasAP && !stats.hasARP && !stats.hasEXP &&
                                   !stats.hasHIT && !stats.hasHASTE && !stats.hasCRIT; // Spirit-only regen jewelry (no INT/SP/offensive stats): treat as healer-oriented.
    // Tank-only profile: Defense / Avoidance (dodge/parry/block rating) / Block value
    // Do NOT tag all shields as "tank": there are caster shields (INT/SP/MP5)
    const bool looksTank = stats.hasDef || stats.hasAvoid || stats.hasBlockValue;

    // Do not let patterns override jewelry/cloak logic; these are handled separately below.
    bool primaryByPattern = false;
    if (!isJewelry && ApplyStatPatternsForPrimary(bot, proto, traits, primaryByPattern))
    {
        return primaryByPattern;
    }

    // Non-tanks (DPS, casters/heals) never NEED purely tank items
    if (!isTankLikeSpec && looksTank)
    {
        return false;
    }

    // Generic rules by role/family
    if (isPhysicalSpec)
    {
        // (1) All physicals/tanks: never Spell Power/Spirit/MP5 (even if plate/mail)
        if (looksCaster)
        {
            return false;
        }
        // (2) Weapon/shield with Spell Power: always off-spec for DK/War/Rogue/Hunter/Ret/Enh/Feral/Prot
        if ((proto->Class == ITEM_CLASS_WEAPON ||
             (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)) &&
            stats.hasSP)
        {
            return false;
        }
        // (3) Jewelry/cloaks with caster stats (SP/SPI/MP5/pure INT) -> off-spec
        if (isJewelry && looksCaster)
        {
            return false;
        }
    }
    else  // Caster/Healer
    {
        // Caster/healer specs: body armor without caster-friendly stats (INT/SP/SPI/MP5)
        // is never considered primary. This prevents healers/casters from NEEDing on
        // purely physical or neutral armor (e.g. AGI/AP-only shoulders) even if it is
        // a big upgrade over grey items.
        if (proto->Class == ITEM_CLASS_ARMOR &&
            IsBodyArmorInvType(proto->InventoryType) &&
            !looksCaster)
        {
            return false;
        }

        // Pure Spirit regen jewelry (SPI only, no INT/SP/throughput stats) is healer mainspec only.
        // DPS casters should treat it as off-spec so they do not NEED it over proper DPS jewelry.
        if (pureSpiritJewelry && !traits.isHealer)
        {
            return false;
        }

        // (1) Casters/healers should not NEED pure melee items (STR/AP/ARP/EXP) without INT/SP
        if (looksPhysical && !stats.hasSP && !stats.hasINT)
        {
            return false;
        }

        // (2) Melee jewelry (AP/ARP/EXP/STR/AGI) without INT/SP -> off-spec
        if (isJewelry && looksPhysical && !stats.hasSP && !stats.hasINT)
        {
            return false;
        }

        // (3) Healers: treat pure "DPS caster Hit" pieces as off-spec.
        // Profile: SP/INT + Hit and *no* regen stats (no SPI, no MP5).
        //
        // This only marks the item as *not primary* for healers.
        // The actual priority is decided later by SmartNeedBySpec + GroupHasPrimarySpecUpgradeCandidate:
        //  - if a DPS caster bot has this item as a mainspec upgrade, healers are downgraded to GREED;
        //  - if nobody in the group has it as mainspec upgrade, healers are allowed to keep NEED.
        if (traits.isHealer && stats.hasHIT && !stats.hasMP5 && !stats.hasSPI)
        {
            return false;
        }

        // Paladin Holy (plate INT+SP/MP5), Shaman Elemental/Restoration (mail INT+SP/MP5),
        // Druid Balance/Restoration (leather/cloth caster) -> OK
    }

    // Extra weapon sanity for Hunters/Ferals (avoid wrong stat-sticks):
    // - Hunters: for melee weapons, require AGI (prevent Haste/AP-only daggers without AGI).
    // - Feral (tank/DPS): for melee weapons, require AGI or STR.
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        const bool meleeWeapon =
            proto->InventoryType == INVTYPE_WEAPON || proto->InventoryType == INVTYPE_WEAPONMAINHAND ||
            proto->InventoryType == INVTYPE_WEAPONOFFHAND || proto->InventoryType == INVTYPE_2HWEAPON;

        if (meleeWeapon && traits.isHunter && !stats.hasAGI)
        {
            return false;
        }

        if (meleeWeapon && (traits.isFeralTk || traits.isFeralDps) && !stats.hasAGI && !stats.hasSTR)
        {
            return false;
        }

        // Enhancement shamans prefer slow weapons for Windfury; avoid very fast melee weapons as main-spec.
        if (meleeWeapon && traits.isEnhSham)
        {
            // Delay is in milliseconds; 2000 ms = 2.0s. Anything faster than this is treated as off-spec.
            if (proto->Delay > 0 && proto->Delay < 2000)
            {
                return false;
            }
        }
    }

    // Class/spec specific adjustments (readable)
    // DK Unholy (DPS): allows STR/HIT/HASTE/CRIT/ARP; rejects all caster items
    if (traits.cls == CLASS_DEATH_KNIGHT && (traits.spec == "unholy" || traits.spec == "uh") && looksCaster)
    {
        return false;
    }

    // DK Blood/Frost tanks: DEF/AVOID/STA/STR are useful; reject caster items
    if (traits.isDKTank && looksCaster)
    {
        return false;
    }
    // Pure caster DPS rings/trinkets already filtered above.

    // Hunter (BM/MM/SV): agi/hit/haste/AP/crit/arp → OK; avoid STR-only or caster items
    if (traits.isHunter)
    {
        if (looksCaster)
        {
            return false;
        }
        // Avoid rings with "pure STR" without AGI/AP/DPS ratings
        if (isJewelry && stats.hasSTR && !stats.hasAGI && !stats.hasAP && !hasDpsRatings)
        {
            return false;
        }
    }

    // Rogue (all specs): same strict physical filter (no caster items)
    if (traits.isRogue && looksCaster)
    {
        return false;
    }

    // Rogue: do not treat INT leather body armor as primary (off-spec leveling pieces only).
    if (traits.isRogue && proto->Class == ITEM_CLASS_ARMOR && IsBodyArmorInvType(proto->InventoryType) &&
        proto->SubClass == ITEM_SUBCLASS_ARMOR_LEATHER && stats.hasINT)
    {
        return false;
    }

    // Warrior Arms/Fury : no caster items
    if (traits.isWarrior && !traits.isWarProt && looksCaster)
    {
        return false;
    }

    // Warrior Protection: DEF/AVOID/STA/STR are useful; no caster items
    if (traits.isWarProt && looksCaster)
    {
        return false;
    }

    // Shaman Enhancement: no Spell Power weapons/shields, no pure INT/SP items
    if (traits.isEnhSham)
    {
        if (looksCaster)
        {
            return false;
        }
        if ((proto->Class == ITEM_CLASS_WEAPON ||
             (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)) &&
            stats.hasSP)
        {
            return false;
        }
    }

    // Druid Feral (tank/DPS): AGI/STA/AVOID/ARP/EXP → OK; no caster items
    if ((traits.isFeralTk || traits.isFeralDps) && looksCaster)
    {
        return false;
    }

    // Paladin Retribution: physical DPS (no caster items; forbid SP weapons/shields; enforce 2H only)
    if (traits.isRetPal)
    {
        if (looksCaster)
        {
            return false;
        }

        // No Spell Power weapons or shields for Ret
        if ((proto->Class == ITEM_CLASS_WEAPON ||
             (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)) &&
            stats.hasSP)
        {
            return false;
        }
        // Enforce 2H only (no 1H/off-hand/shields/holdables)
        switch (proto->InventoryType)
        {
            case INVTYPE_WEAPON:          // generic 1H
            case INVTYPE_WEAPONMAINHAND:  // explicit main-hand 1H
            case INVTYPE_WEAPONOFFHAND:   // off-hand weapon
            case INVTYPE_SHIELD:          // shields
            case INVTYPE_HOLDABLE:        // tomes/orbs
                return false;             // never NEED for Ret
            default:
                break;  // INVTYPE_2HWEAPON is allowed; others handled elsewhere
        }
    }

    // Global VETO: a "physical" spec never considers a caster profile as primary
    if (sPlayerbotAIConfig->smartNeedBySpec && traits.isPhysical && looksCaster)
    {
        return false;
    }

    // Let the cross-armor rules (CrossArmorExtraMargin) decide for major off-armor upgrades.
    return true;
}

// Local mini-helper: maps an InventoryType (INVTYPE_*) to an EquipmentSlot (EQUIPMENT_SLOT_*)
// Only covers the slots relevant for T7-T10 tokens (head/shoulders/chest/hands/legs).
static uint8 EquipmentSlotByInvTypeSafe(uint8 invType)
{
    switch (invType)
    {
        case INVTYPE_HEAD:
            return EQUIPMENT_SLOT_HEAD;
        case INVTYPE_SHOULDERS:
            return EQUIPMENT_SLOT_SHOULDERS;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            return EQUIPMENT_SLOT_CHEST;
        case INVTYPE_HANDS:
            return EQUIPMENT_SLOT_HANDS;
        case INVTYPE_LEGS:
            return EQUIPMENT_SLOT_LEGS;
        default:
            return EQUIPMENT_SLOT_END;  // unknown/not applicable
    }
}

// All equippable items -> corresponding slots
static void GetEquipSlotsForInvType(uint8 invType, std::vector<uint8>& out)
{
    out.clear();
    switch (invType)
    {
        case INVTYPE_HEAD:
            out = {EQUIPMENT_SLOT_HEAD};
            break;
        case INVTYPE_NECK:
            out = {EQUIPMENT_SLOT_NECK};
            break;
        case INVTYPE_SHOULDERS:
            out = {EQUIPMENT_SLOT_SHOULDERS};
            break;
        case INVTYPE_BODY: /* shirt, ignore */
            break;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            out = {EQUIPMENT_SLOT_CHEST};
            break;
        case INVTYPE_WAIST:
            out = {EQUIPMENT_SLOT_WAIST};
            break;
        case INVTYPE_LEGS:
            out = {EQUIPMENT_SLOT_LEGS};
            break;
        case INVTYPE_FEET:
            out = {EQUIPMENT_SLOT_FEET};
            break;
        case INVTYPE_WRISTS:
            out = {EQUIPMENT_SLOT_WRISTS};
            break;
        case INVTYPE_HANDS:
            out = {EQUIPMENT_SLOT_HANDS};
            break;
        case INVTYPE_FINGER:
            out = {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2};
            break;
        case INVTYPE_TRINKET:
            out = {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2};
            break;
        case INVTYPE_CLOAK:
            out = {EQUIPMENT_SLOT_BACK};
            break;
        case INVTYPE_WEAPON:
            out = {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
            break;
        case INVTYPE_2HWEAPON:
            out = {EQUIPMENT_SLOT_MAINHAND};
            break;
        case INVTYPE_SHIELD:
            out = {EQUIPMENT_SLOT_OFFHAND};
            break;
        case INVTYPE_WEAPONMAINHAND:
            out = {EQUIPMENT_SLOT_MAINHAND};
            break;
        case INVTYPE_WEAPONOFFHAND:
            out = {EQUIPMENT_SLOT_OFFHAND};
            break;
        case INVTYPE_HOLDABLE:
            out = {EQUIPMENT_SLOT_OFFHAND};
            break;  // tome/orb
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
            out = {EQUIPMENT_SLOT_RANGED};
            break;
        case INVTYPE_RELIC:
            out = {EQUIPMENT_SLOT_RANGED};
            break;  // totem/idol/sigil/libram
        case INVTYPE_TABARD:
        case INVTYPE_BAG:
        case INVTYPE_AMMO:
        case INVTYPE_QUIVER:
        default:
            break;  // not relevant for gear
    }
}

// Internal prototypes
static bool CanBotUseToken(ItemTemplate const* proto, Player* bot);
static bool RollUniqueCheck(ItemTemplate const* proto, Player* bot);

// WotLK Heuristic: We can only DE [UNCOMMON..EPIC] quality ARMOR/WEAPON
static inline bool IsLikelyDisenchantable(ItemTemplate const* proto)
{
    if (!proto)
    {
        return false;
    }


    // Prefer the core-provided disenchant mapping when available
    if (proto->DisenchantID > 0)
    {
        return true;
    }

    // Respect items explicitly flagged as non-disenchantable by the core
    if (proto->DisenchantID < 0)
    {
        return false;
    }

    // Fallback heuristic for custom or missing data
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
    {
        return false;
    }
    return proto->Quality >= ITEM_QUALITY_UNCOMMON && proto->Quality <= ITEM_QUALITY_EPIC;
}

// Internal helpers
// Deduces the target slot from the token's name.
// Returns an expected InventoryType (HEAD/SHOULDERS/CHEST/HANDS/LEGS) or -1 if unknown.
static int8 TokenSlotFromName(ItemTemplate const* proto)
{
    if (!proto)
        return -1;
    std::string n = std::string(proto->Name1);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n.find("helm") != std::string::npos || n.find("head") != std::string::npos)
    {
        return INVTYPE_HEAD;
    }
    if (n.find("shoulder") != std::string::npos || n.find("mantle") != std::string::npos ||
        n.find("spauld") != std::string::npos)
    {
        return INVTYPE_SHOULDERS;
    }
    if (n.find("chest") != std::string::npos || n.find("tunic") != std::string::npos ||
        n.find("robe") != std::string::npos || n.find("breastplate") != std::string::npos ||
        n.find("chestguard") != std::string::npos)
    {
        return INVTYPE_CHEST;
    }
    if (n.find("glove") != std::string::npos || n.find("handguard") != std::string::npos ||
        n.find("gauntlet") != std::string::npos)
    {
        return INVTYPE_HANDS;
    }
    if (n.find("leg") != std::string::npos || n.find("pant") != std::string::npos ||
        n.find("trouser") != std::string::npos)
    {
        return INVTYPE_LEGS;
    }
    return -1;
}

// Upgrade heuristic for a token: if the slot is known,
// consider it a "likely upgrade" if ilvl(token) >= ilvl(best equipped item in that slot) + margin.
static bool IsTokenLikelyUpgrade(ItemTemplate const* token, uint8 invTypeSlot, Player* bot)
{
    if (!token || !bot)
        return false;
    uint8 eq = EquipmentSlotByInvTypeSafe(invTypeSlot);
    if (eq >= EQUIPMENT_SLOT_END)
    {
        return true;  // unknown slot -> do not block Need
    }
    Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eq);
    if (!oldItem)
        return true;  // empty slot -> guaranteed upgrade
    ItemTemplate const* oldProto = oldItem->GetTemplate();
    if (!oldProto)
        return true;
    float margin = sPlayerbotAIConfig->tokenILevelMargin;  // configurable
    return (float)token->ItemLevel >= (float)oldProto->ItemLevel + margin;
}

static bool TryTokenRollVote(ItemTemplate const* proto, Player* bot, RollVote& outVote)
{
    if (!proto || !bot)
    {
        return false;
    }

    if (proto->Class != ITEM_CLASS_MISC || proto->SubClass != ITEM_SUBCLASS_JUNK ||
        proto->Quality != ITEM_QUALITY_EPIC)
    {
        return false;
    }

    if (CanBotUseToken(proto, bot))
    {
        int8 const tokenSlot = TokenSlotFromName(proto);
        if (tokenSlot >= 0)
        {
            outVote = IsTokenLikelyUpgrade(proto, static_cast<uint8>(tokenSlot), bot) ? NEED : GREED;
        }
        else
        {
            outVote = GREED;  // Unknown slot (e.g. T10 sanctification tokens)
        }
    }
    else
    {
        outVote = GREED;  // Not eligible, so Greed
    }

    return true;
}

static RollVote ApplyDisenchantPreference(RollVote currentVote, ItemTemplate const* proto, ItemUsage usage,
                                          Group* group, Player* bot, char const* logTag)
{
    std::string const tag = logTag ? logTag : "[LootRollDBG]";

    bool const isDeCandidate = IsLikelyDisenchantable(proto);
    bool const hasEnchantSkill = bot && bot->HasSkill(SKILL_ENCHANTING);
    int32 const lootMethod = group ? static_cast<int32>(group->GetLootMethod()) : -1;

    if (currentVote != NEED && sPlayerbotAIConfig->useDEButton && group &&
        (group->GetLootMethod() == NEED_BEFORE_GREED || group->GetLootMethod() == GROUP_LOOT) &&
        hasEnchantSkill && isDeCandidate && usage == ITEM_USAGE_DISENCHANT)
    {
        LOG_DEBUG("playerbots",
                  "{} DE switch: {} -> DISENCHANT (lootMethod={}, enchSkill={}, deOK=1, usage=DISENCHANT)",
                  tag, VoteTxt(currentVote), lootMethod, hasEnchantSkill ? 1 : 0);
        return DISENCHANT;
    }

    LOG_DEBUG("playerbots", "{} no DE: vote={} lootMethod={} enchSkill={} deOK={} usage={}", tag, VoteTxt(currentVote),
              lootMethod, hasEnchantSkill ? 1 : 0, isDeCandidate ? 1 : 0, static_cast<uint32>(usage));

    return currentVote;
}

static RollVote FinalizeRollVote(RollVote vote, ItemTemplate const* proto, ItemUsage usage, Group* group, Player* bot,
                                 char const* logTag)
{
    std::string const tag = logTag ? logTag : "[LootRollDBG]";

    vote = ApplyDisenchantPreference(vote, proto, usage, group, bot, tag.c_str());

    if (sPlayerbotAIConfig->lootRollLevel == 0)
    {
        LOG_DEBUG("playerbots", "{} LootRollLevel=0 forcing PASS (was {})", tag, VoteTxt(vote));
        return PASS;
    }

    if (sPlayerbotAIConfig->lootRollLevel == 1)
    {
        if (vote == NEED)
        {
            vote = RollUniqueCheck(proto, bot) ? PASS : GREED;
        }
        else if (vote == GREED)
        {
            vote = PASS;
        }

        LOG_DEBUG("playerbots", "{} LootRollLevel=1 adjusted vote={}", tag, VoteTxt(vote));
    }

    return vote;
}

bool LootRollAction::Execute(Event event)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::vector<Roll*> const& rolls = group->GetRolls();
    for (Roll* const roll : rolls)
    {
        if (!roll)
            continue;

        // Avoid server crash, key may not exit for the bot on login
        auto it = roll->playerVote.find(bot->GetGUID());
        if (it != roll->playerVote.end() && it->second != NOT_EMITED_YET)
        {
            continue;
        }

        ObjectGuid guid = roll->itemGUID;
        uint32 itemId = roll->itemid;
        int32 randomProperty = EncodeRandomEnchantParam(roll->itemRandomPropId, roll->itemRandomSuffix);

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        LOG_DEBUG("playerbots",
                  "[LootRollDBG] start bot={} item={} \"{}\" class={} q={} lootMethod={} enchSkill={} rp={}",
                  bot->GetName(), itemId, proto->Name1, proto->Class, proto->Quality, (int)group->GetLootMethod(),
                  bot->HasSkill(SKILL_ENCHANTING), randomProperty);

        RollVote vote = PASS;

        std::string const itemUsageParam = BuildItemUsageParam(itemId, randomProperty);
        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);

        LOG_DEBUG("playerbots", "[LootRollDBG] usage={} (EQUIP=1 REPLACE=2 BAD_EQUIP=8 DISENCHANT=13)", (int)usage);
        if (!TryTokenRollVote(proto, bot, vote))
        {
            // Lets CalculateRollVote decide (includes SmartNeedBySpec, BoE/BoU, unique, cross-armor)
            vote = CalculateRollVote(proto, randomProperty);
            LOG_DEBUG("playerbots", "[LootRollDBG] after CalculateRollVote: vote={}", VoteTxt(vote));
        }

        vote = FinalizeRollVote(vote, proto, usage, group, bot, "[LootRollDBG]");
        // Announce + send the roll vote (if ML/FFA => PASS)
        RollVote sent = vote;
        if (group->GetLootMethod() == MASTER_LOOT || group->GetLootMethod() == FREE_FOR_ALL)
            sent = PASS;

        LOG_DEBUG("playerbots", "[LootPaternDBG] send vote={} (lootMethod={} Lvl={}) -> guid={} itemId={}",
                  VoteTxt(sent), (int)group->GetLootMethod(), sPlayerbotAIConfig->lootRollLevel, guid.ToString(),
                  itemId);

        group->CountRollVote(bot->GetGUID(), guid, sent);
        // One item at a time
        return true;
    }

    return false;
}

RollVote LootRollAction::CalculateRollVote(ItemTemplate const* proto, int32 randomProperty)
{
    // Player mimic: upgrade => NEED; useful => GREED; otherwise => PASS
    std::string const itemUsageParam = BuildItemUsageParam(proto->ItemId, randomProperty);
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);

    RollVote vote = PASS;

    bool const isCollectibleCosmetic = IsCosmeticCollectible(proto);
    bool const alreadyHasCollectible = isCollectibleCosmetic && BotAlreadyHasCollectible(bot, proto);

    if (isCollectibleCosmetic)
    {
        // Simple rule for mounts/pets:
        // NEED if the bot does not have it yet, GREED if already known/owned.
        vote = alreadyHasCollectible ? GREED : NEED;
    }

    bool recipeChecked = false;
    bool recipeNeed = false;
    bool recipeUseful = false;
    bool recipeKnown = false;
    uint32 reqSkillDbg = 0, reqRankDbg = 0, botRankDbg = 0;

    // Professions: NEED on useful recipes/patterns/books when enabled.
    if (sPlayerbotAIConfig->needOnProfessionRecipes && IsRecipeItem(proto))
    {
        recipeChecked = true;
        // Collect debug data (what the helper va décider)
        reqSkillDbg = proto->RequiredSkill ? proto->RequiredSkill : GuessRecipeSkill(proto);
        reqRankDbg = proto->RequiredSkillRank;
        botRankDbg = reqSkillDbg ? bot->GetSkillValue(reqSkillDbg) : 0;
        recipeKnown = BotAlreadyKnowsRecipeSpell(bot, proto);

        recipeUseful = IsProfessionRecipeUsefulForBot(bot, proto);
        if (recipeUseful)
        {
            vote = NEED;
            recipeNeed = true;
        }
        else
        {
            vote = GREED;  // recipe not for the bot -> GREED
        }
    }

    // Do not overwrite the choice if already decided by recipe or cosmetic logic.
    if (!recipeChecked && !isCollectibleCosmetic)
    {
        switch (usage)
        {
            case ITEM_USAGE_EQUIP:
            case ITEM_USAGE_REPLACE:
            {
                vote = NEED;
                // SmartNeedBySpec: downgrade to GREED only if a mainspec upgrade exists or this spec is too far off.
                if (sPlayerbotAIConfig->smartNeedBySpec && !IsPrimaryForSpec(bot, proto))
                {
                    if (GroupHasPrimarySpecUpgradeCandidate(bot, proto, randomProperty))
                    {
                        vote = GREED;
                    }
                    else if (!IsFallbackNeedReasonableForSpec(bot, proto))
                    {
                        // No mainspec candidate, but the item is too far off for this spec -> GREED.
                        vote = GREED;
                    }
                    else
                    {
                        // Off-spec fallback: allow NEED only if there is no "desperate" jewelry user
                        // with an empty/very bad item in the same slot(s).
                        bool const isJewelry = IsJewelryOrCloak(proto);

                        if (isJewelry && GroupHasDesperateJewelryUpgradeUser(bot, proto, randomProperty))
                        {
                            LOG_DEBUG("playerbots",
                                      "[LootRollDBG] jewelry-fallback: downgrade NEED to GREED, desperate upgrade user present for item={} \"{}\"",
                                      proto->ItemId, proto->Name1);
                            vote = GREED;
                        }
                        else
                        {
                            LOG_DEBUG("playerbots",
                                      "[LootRollDBG] secondary-fallback: no primary spec upgrade in group, {} may NEED item={} \"{}\"",
                                      bot->GetName(), proto->ItemId, proto->Name1);
                        }
                    }
                }
                break;
            }
            case ITEM_USAGE_BAD_EQUIP:
            case ITEM_USAGE_GUILD_TASK:
            case ITEM_USAGE_SKILL:
            case ITEM_USAGE_USE:
            case ITEM_USAGE_DISENCHANT:
            case ITEM_USAGE_AH:
            case ITEM_USAGE_VENDOR:
            case ITEM_USAGE_KEEP:
            case ITEM_USAGE_AMMO:
                vote = GREED;
                break;
            default:
                vote = PASS;
                break;
        }
    }

    // Policy: turn GREED into PASS on off-armor (lower tier) if configured.
    if (vote == GREED && proto->Class == ITEM_CLASS_ARMOR && sPlayerbotAIConfig->crossArmorGreedIsPass)
    {
        if (IsLowerTierArmorForBot(bot, proto))
            vote = PASS;
    }

    // Lockboxes: if the item is a lockbox and the bot is a Rogue with Lockpicking, prefer NEED (ignored by BoE/BoU).
    const SpecTraits traits = GetSpecTraits(bot);
    const bool isLockbox = IsLockbox(proto);
    if (isLockbox && traits.isRogue && bot->HasSkill(SKILL_LOCKPICKING))
        vote = NEED;

    // Generic BoP rule: if the item is BoP, equippable, matches the spec
    // AND at least one relevant slot is empty -> allow NEED
    constexpr uint32 BIND_WHEN_PICKED_UP = 1;
    if (vote != NEED && proto->Bonding == BIND_WHEN_PICKED_UP)
    {
        std::vector<uint8> slots;
        GetEquipSlotsForInvType(proto->InventoryType, slots);
        if (!slots.empty())
        {
            const bool specOk = !sPlayerbotAIConfig->smartNeedBySpec || IsPrimaryForSpec(bot, proto);
            if (specOk)
            {
                for (uint8 s : slots)
                {
                    if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
                    {
                        vote = NEED;  // fills an empty slot -> NEED
                        break;
                    }
                }
            }
        }
    }

    // BoE/BoU rule: by default, avoid NEED on Bind-on-Equip / Bind-on-Use (raid etiquette)
    // BoE/BoU etiquette: avoid NEED on BoE/BoU, except useful profession recipes.
    constexpr uint32 BIND_WHEN_EQUIPPED = 2;  // BoE
    constexpr uint32 BIND_WHEN_USE = 3;       // BoU

    if (vote == NEED && !recipeNeed && !isLockbox && !isCollectibleCosmetic &&
        proto->Bonding == BIND_WHEN_EQUIPPED &&
        !sPlayerbotAIConfig->allowBoENeedIfUpgrade)
    {
        vote = GREED;
    }
    if (vote == NEED && !recipeNeed && !isLockbox && !isCollectibleCosmetic &&
        proto->Bonding == BIND_WHEN_USE &&
        !sPlayerbotAIConfig->allowBoUNeedIfUpgrade)
    {
        vote = GREED;
    }

    // Non-unique soft rule: NEED -> GREED on duplicates, except Book of Glyph Mastery.
    if (vote == NEED)
    {
        if (!IsGlyphMasteryBook(proto))
        {
            // includeBank=true to catch banked duplicates as well.
            if (bot->GetItemCount(proto->ItemId, true) > 0)
                vote = GREED;
        }
    }

    // Unique-equip: never NEED a duplicate (already equipped/owned)
    if (vote == NEED && RollUniqueCheck(proto, bot))
    {
        vote = PASS;
    }

    // Cross-armor: allow NEED on BAD_EQUIP only if no primary armor user needs it and it is a massive upgrade.
    if (vote == GREED && usage == ITEM_USAGE_BAD_EQUIP && proto->Class == ITEM_CLASS_ARMOR &&
        IsLowerTierArmorForBot(bot, proto))
    {
        if (!GroupHasPrimaryArmorUserLikelyToNeed(bot, proto, randomProperty))
        {
            // Extra guard: if another bot is clearly undergeared in this slot (empty or grey/white item)
            // and sees this piece as an upgrade, do not allow cross-armor NEED here.
            if (GroupHasDesperateUpgradeUser(bot, proto, randomProperty))
            {
                LOG_DEBUG("playerbots",
                          "[LootRollDBG] cross-armor: keeping GREED, desperate upgrade user present for item={} \"{}\"",
                          proto->ItemId, proto->Name1);
            }
            else
            {
                // Reuse the same sanity as the generic fallback:
                // even in cross-armor mode, do not allow NEED on completely off-spec items
                // (e.g. rogues on cloth SP/INT/SPI, casters on pure STR/AP plate, etc.).
                if (!IsFallbackNeedReasonableForSpec(bot, proto))
                {
                    LOG_DEBUG("playerbots",
                              "[LootRollDBG] cross-armor: {} too far off-spec for item={} \"{}\", keeping GREED",
                              bot->GetName(), proto->ItemId, proto->Name1);
                }
                else
                {
                    StatsWeightCalculator calc(bot);
                    float newScore = calc.CalculateItem(proto->ItemId);
                    float bestOld = 0.0f;

                    // Find the best currently equipped item of the same InventoryType
                    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    {
                        Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                        if (!oldItem)
                            continue;

                        ItemTemplate const* oldProto = oldItem->GetTemplate();
                        if (!oldProto)
                            continue;
                        if (oldProto->Class != ITEM_CLASS_ARMOR)
                            continue;
                        if (oldProto->InventoryType != proto->InventoryType)
                            continue;

                        float oldScore = calc.CalculateItem(
                            oldProto->ItemId, oldItem->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
                        if (oldScore > bestOld)
                            bestOld = oldScore;
                    }

                    if (bestOld > 0.0f && newScore >= bestOld * sPlayerbotAIConfig->crossArmorExtraMargin)
                    {
                        vote = NEED;
                    }
                }
            }
        }
        else
        {
            LOG_DEBUG("playerbots",
                      "[LootRollDBG] cross-armor: keeping GREED, primary armor user present for item={} \"{}\"",
                      proto->ItemId, proto->Name1);
        }
    }

    // Cross-armor hard override: if configured, never let bots NEED or GREED on lower-tier body armor.
    if (sPlayerbotAIConfig->crossArmorGreedIsPass &&
        proto->Class == ITEM_CLASS_ARMOR &&
        IsLowerTierArmorForBot(bot, proto) &&
        vote != PASS)
    {
        LOG_DEBUG("playerbots",
                  "[LootRollDBG] cross-armor: CrossArmorGreedIsPass forcing PASS (was {}), item={} \"{}\"",
                  VoteTxt(vote), proto->ItemId, proto->Name1);
        vote = PASS;
    }

    // Final decision (with allow/deny from loot strategy).
    RollVote finalVote = StoreLootAction::IsLootAllowed(proto->ItemId, GET_PLAYERBOT_AI(bot)) ? vote : PASS;

    // DEBUG: dump for recipes
    if (IsRecipeItem(proto))
    {
        DebugRecipeRoll(bot, proto, usage, recipeChecked, recipeUseful, recipeKnown, reqSkillDbg, reqRankDbg,
                        botRankDbg,
                        /*before*/ (recipeNeed ? NEED : PASS),
                        /*after*/ finalVote);
    }

    return finalVote;
}

bool MasterLootRollAction::isUseful() { return !botAI->HasActivePlayerMaster(); }

bool MasterLootRollAction::Execute(Event event)
{
    Player* bot = QueryItemUsageAction::botAI->GetBot();

    WorldPacket p(event.getPacket());  // WorldPacket packet for CMSG_LOOT_ROLL, (8+4+1)
    ObjectGuid creatureGuid;
    uint32 mapId;
    uint32 itemSlot;
    uint32 itemId;
    uint32 randomSuffix;
    uint32 randomPropertyId;
    uint32 count;
    uint32 timeout;

    p.rpos(0);              // reset packet pointer
    p >> creatureGuid;      // creature guid what we're looting
    p >> mapId;             // 3.3.3 mapid
    p >> itemSlot;          // the itemEntryId for the item that shall be rolled for
    p >> itemId;            // the itemEntryId for the item that shall be rolled for
    p >> randomSuffix;      // randomSuffix
    p >> randomPropertyId;  // item random property ID
    p >> count;             // items in stack
    p >> timeout;           // the countdown time to choose "need" or "greed"

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    LOG_DEBUG("playerbots",
              "[LootEnchantDBG][ML] start bot={} item={} \"{}\" class={} q={} lootMethod={} enchSkill={} rp={}",
              bot->GetName(), itemId, proto->Name1, proto->Class, proto->Quality, (int)group->GetLootMethod(),
              bot->HasSkill(SKILL_ENCHANTING), randomPropertyId);

    // Compute random property and usage, same pattern as LootRollAction::Execute
    int32 randomProperty = EncodeRandomEnchantParam(randomPropertyId, randomSuffix);

    std::string const itemUsageParam = BuildItemUsageParam(itemId, randomProperty);
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);

    // 1) Token heuristic: ONLY NEED if the target slot is a likely upgrade
    RollVote vote = PASS;

    if (!TryTokenRollVote(proto, bot, vote))
    {
        vote = CalculateRollVote(proto, randomProperty);
    }

    vote = FinalizeRollVote(vote, proto, usage, group, bot, "[LootEnchantDBG][ML]");

    RollVote sent = vote;
    if (group->GetLootMethod() == MASTER_LOOT || group->GetLootMethod() == FREE_FOR_ALL)
        sent = PASS;

    LOG_DEBUG("playerbots", "[LootEnchantDBG][ML] vote={} -> sent={} lootMethod={} enchSkill={} deOK={}", VoteTxt(vote),
              VoteTxt(sent), (int)group->GetLootMethod(), bot->HasSkill(SKILL_ENCHANTING),
              IsLikelyDisenchantable(proto));

    group->CountRollVote(bot->GetGUID(), creatureGuid, sent);

    return true;
}

static bool CanBotUseToken(ItemTemplate const* proto, Player* bot)
{
    if (!proto || !bot)
    {
        return false;
    }

    return IsClassAllowedByItemTemplate(bot->getClass(), proto);
}

static bool RollUniqueCheck(ItemTemplate const* proto, Player* bot)
{
    // Count the total number of the item (equipped + in bags)
    uint32 totalItemCount = bot->GetItemCount(proto->ItemId, true);

    // Count the number of the item in bags only
    uint32 bagItemCount = bot->GetItemCount(proto->ItemId, false);

    // Determine if the unique item is already equipped
    bool isEquipped = (totalItemCount > bagItemCount);
    if (isEquipped && proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
    {
        return true;  // Unique Item is already equipped
    }
    else if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) && (bagItemCount > 1))
    {
        return true;  // Unique item already in bag, don't roll for it
    }
    return false;  // Item is not equipped or in bags, roll for it
}

bool RollAction::Execute(Event event)
{
    std::string link = event.getParam();

    if (link.empty())
    {
        bot->DoRandomRoll(0, 100);
        return false;
    }
    ItemIds itemIds = chat->parseItems(link);
    if (itemIds.empty())
        return false;
    uint32 itemId = *itemIds.begin();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
    {
        return false;
    }
    std::string itemUsageParam;
    itemUsageParam = std::to_string(itemId);

    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);
    switch (proto->Class)
    {
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
            {
                bot->DoRandomRoll(0, 100);
            }
    }
    return true;
}