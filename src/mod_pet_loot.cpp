/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Creature.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cctype>
#include <limits>
#include <sstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Gate verbose per-kill/group-scan diagnostics behind PetLoot.DebugLog.
#define PETLOOT_LOG(...)                                                 \
    do {                                                                 \
        if (PetLootConfig::instance()->DebugLog)                         \
            LOG_INFO("module.pet_loot", __VA_ARGS__);                    \
    } while (false)

// Configuration class to store module settings
class PetLootConfig
{
public:
    static PetLootConfig* instance()
    {
        static PetLootConfig instance;
        return &instance;
    }

    bool Enabled;
    std::set<uint32> PetIds;
    float Radius;
    float CacheTTL{30.0f};
    bool DebugLog{false};

    // Prevents the pet from starting group loot rolls multiple times
    // for the same creature corpse.
    std::set<ObjectGuid> GroupLootStarted;

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("PetLoot.Enable", true);
        DebugLog = sConfigMgr->GetOption<bool>("PetLoot.DebugLog", false);
        Radius = sConfigMgr->GetOption<float>("PetLoot.Radius", 50.0f);
        CacheTTL = sConfigMgr->GetOption<float>("PetLoot.CacheTTL", 30.0f);
        if (CacheTTL < 0.0f)
            CacheTTL = 0.0f;

        std::string petIdsCfg = sConfigMgr->GetOption<std::string>("PetLoot.PetIds",
            "23234,7547,25146,37865,29089,26119,36911,36511");
        PetIds.clear();
        std::istringstream petIdsStream(petIdsCfg);
        std::string token;
        while (std::getline(petIdsStream, token, ','))
        {
            size_t begin = 0;
            size_t end = token.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(token[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])))
                --end;

            std::string id = token.substr(begin, end - begin);
            if (id.empty())
                continue;

            bool digitsOnly = true;
            for (char c : id)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                {
                    digitsOnly = false;
                    break;
                }
            }

            if (!digitsOnly)
                continue;

            try
            {
                unsigned long value = std::stoul(id);
                if (value <= std::numeric_limits<uint32>::max())
                    PetIds.insert(static_cast<uint32>(value));
            }
            catch (...)
            {
                // Ignore malformed or out-of-range entries.
            }
        }

        if (PetIds.empty())
        {
            PETLOOT_LOG("PetLoot.PetIds parsed to empty; using default pet list");
            PetIds.insert({ 23234, 7547, 25146, 37865, 29089, 26119, 36911, 36511 });
        }

        std::ostringstream idsStream;
        bool firstId = true;
        for (uint32 petId : PetIds)
        {
            if (!firstId)
                idsStream << ',';
            idsStream << petId;
            firstId = false;
        }
        std::string idsText = idsStream.str();
        PETLOOT_LOG("Load: enabled={} radius={} cacheTTL={} debugLog={} petIds=[{}]",
                    Enabled, Radius, CacheTTL, DebugLog, idsText);
    }
};

static bool IsLootPet(uint32 entry)
{
    return PetLootConfig::instance()->PetIds.count(entry) != 0;
}

// Last valid pet owner per group. Every cached entry is fully revalidated before reuse.
struct CachedLooter
{
    ObjectGuid playerGuid;
    ObjectGuid critterGuid;
    uint32 cachedAtMs{0};
};
static std::unordered_map<ObjectGuid /* group GUID */, CachedLooter> groupLooterCache;
static constexpr uint32 CACHE_TTL_DISABLED = 0;

// Resolve the player whose vanity pet should be moved to the corpse. The kill hook can
// fire for a playerbot with no vanity pet, so fall back to the first eligible group member.
static Player* FindPetLooter(Player* killer, Creature* victim)
{
    if (!killer || !victim)
        return nullptr;

    auto hasPetNear = [victim](Player* player, Creature** outCritter) -> bool
    {
        if (!player || !player->IsAlive() || !player->GetSession() || player->GetSession()->IsHeadless())
            return false;

        ObjectGuid critterGuid = player->GetCritterGUID();
        if (critterGuid.IsEmpty())
            return false;

        Creature* critter = ObjectAccessor::GetCreature(*player, critterGuid);
        if (!critter || !critter->IsAlive() || !IsLootPet(critter->GetEntry()))
            return false;

        if (!player->IsInMap(victim) || !player->InSamePhase(victim) ||
            !critter->IsInMap(victim) || !critter->InSamePhase(victim))
        {
            PETLOOT_LOG("Candidate {} skipped: different map or phase", player->GetName());
            return false;
        }

        if (player->GetDistance(victim) > PetLootConfig::instance()->Radius)
            return false;

        if (outCritter)
            *outCritter = critter;

        return true;
    };

    if (killer->GetSession() && killer->GetSession()->IsHeadless())
        PETLOOT_LOG("Killer {} skipped: headless (bot) session", killer->GetName());

    if (hasPetNear(killer, nullptr))
        return killer;

    Group* group = killer->GetGroup();
    if (!group)
    {
        PETLOOT_LOG("No pet owner found: killer {} is not in a group", killer->GetName());
        return nullptr;
    }

    const float cacheTTL = PetLootConfig::instance()->CacheTTL;
    const bool useCache = cacheTTL > CACHE_TTL_DISABLED;
    const double ttlMsD = double(cacheTTL) * 1000.0;
    const uint32 ttlMs = ttlMsD >= double(std::numeric_limits<uint32>::max())
                             ? std::numeric_limits<uint32>::max()
                             : uint32(ttlMsD);

    if (useCache)
    {
        // Lazily prune expired entries; the map is small (one entry per group).
        for (auto itr = groupLooterCache.begin(); itr != groupLooterCache.end(); )
        {
            if (GetMSTimeDiffToNow(itr->second.cachedAtMs) > ttlMs)
            {
                PETLOOT_LOG("Cache entry expired for group {}", itr->first.ToString());
                itr = groupLooterCache.erase(itr);
            }
            else
                ++itr;
        }

        auto cached = groupLooterCache.find(group->GetGUID());
        if (cached != groupLooterCache.end())
        {
            const CachedLooter& entry = cached->second;
            Player* player = ObjectAccessor::FindPlayer(entry.playerGuid);

            bool valid = player != nullptr;
            if (valid)
            {
                valid = player->GetGroup() == group && player->IsAlive() &&
                        player->GetSession() != nullptr && !player->GetSession()->IsHeadless() &&
                        player->GetCritterGUID() == entry.critterGuid;
            }

            Creature* critter = nullptr;
            if (valid)
            {
                critter = ObjectAccessor::GetCreature(*player, entry.critterGuid);
                valid = critter != nullptr && critter->IsAlive() && IsLootPet(critter->GetEntry());
            }

            if (valid && (!player->IsInMap(victim) || !player->InSamePhase(victim) ||
                          !critter->IsInMap(victim) || !critter->InSamePhase(victim)))
                valid = false;

            if (valid && player->GetDistance(victim) > PetLootConfig::instance()->Radius)
                valid = false;

            if (valid && GetMSTimeDiffToNow(entry.cachedAtMs) > ttlMs)
            {
                PETLOOT_LOG("Cache entry expired during validation; rescanning");
                valid = false;
            }

            if (valid)
            {
                PETLOOT_LOG("Cache hit: reused pet looter {} critter entry {} dist {}",
                            player->GetName(), critter->GetEntry(), player->GetDistance(victim));
                return player;
            }

            PETLOOT_LOG("Cache miss: cached pet owner for killer {} no longer valid; rescanning",
                        killer->GetName());
            groupLooterCache.erase(cached);
        }
    }

    PETLOOT_LOG("Scanning group of killer {} for pet owner", killer->GetName());

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member)
            continue;

        if (member->GetSession() && member->GetSession()->IsHeadless())
        {
            PETLOOT_LOG("Group member {} skipped: headless (bot) session", member->GetName());
            continue;
        }

        Creature* critter = nullptr;
        if (hasPetNear(member, &critter))
        {
            PETLOOT_LOG("Selected pet looter {} critter entry {} dist {}",
                        member->GetName(), critter->GetEntry(), member->GetDistance(victim));
            if (useCache)
            {
                groupLooterCache[group->GetGUID()] =
                    CachedLooter{ member->GetGUID(), critter->GetGUID(), getMSTime() };
                PETLOOT_LOG("Cached pet looter {} for group of killer {}",
                            member->GetName(), killer->GetName());
            }
            return member;
        }
    }

    if (useCache)
        groupLooterCache.erase(group->GetGUID());

    PETLOOT_LOG("No pet owner found in killer {} group", killer->GetName());
    return nullptr;
}

// Event to handle the actual looting after the pet reaches the corpse
class PetLootEvent : public BasicEvent
{
public:
    PetLootEvent(ObjectGuid playerGuid, ObjectGuid victimGuid, ObjectGuid petGuid)
        : _playerGuid(playerGuid), _victimGuid(victimGuid), _petGuid(petGuid) { }

    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        Player* player = ObjectAccessor::FindPlayer(_playerGuid);
        if (!player)
            return true;

        Creature* victim = ObjectAccessor::GetCreature(*player, _victimGuid);
        Creature* pet = ObjectAccessor::GetCreature(*player, _petGuid);

        if (!victim || !pet)
            return true;

        if (pet->GetMap() != player->GetMap())
            return true;

        pet->HandleEmoteCommand(EMOTE_ONESHOT_LOOT);

        // Fill loot if empty
        if (victim->loot.items.empty() && victim->loot.gold == 0 && victim->loot.quest_items.empty())
        {
            if (auto cTemplate = victim->GetCreatureTemplate())
            {
                // Some creatures have no loot table.
                // Avoid calling FillLoot with lootid 0 to prevent console errors.
                if (cTemplate->lootid)
                {
                    bool personal = (player->GetGroup() == nullptr);
                    victim->loot.FillLoot(cTemplate->lootid, LootTemplates_Creature, player, personal);
                }
            }
        }

        // Mark corpse loot as already initialized.
        // This must be done even if FillLoot was not called,
        // otherwise manual looting will start group rolls again.
        victim->loot.loot_type = LOOT_CORPSE;

        Group* group = player->GetGroup();
        LootMethod lootMethod = group ? group->GetLootMethod() : FREE_FOR_ALL;

        // Trigger group loot system once.
        if (group && lootMethod != FREE_FOR_ALL)
        {
            auto& startedLoots = PetLootConfig::instance()->GroupLootStarted;

            if (!startedLoots.count(victim->GetGUID()))
            {
                switch (lootMethod)
                {
                    case GROUP_LOOT:
                        group->GroupLoot(&victim->loot, victim);
                        break;

                    case NEED_BEFORE_GREED:
                        group->NeedBeforeGreed(&victim->loot, victim);
                        break;

                    case MASTER_LOOT:
                        group->MasterLoot(&victim->loot, victim);
                        break;

                    default:
                        break;
                }

                startedLoots.insert(victim->GetGUID());
            }
        }

        // Handle Gold
        if (uint32 gold = victim->loot.gold)
        {
            victim->loot.NotifyMoneyRemoved();

            if (group)
            {
                std::vector<Player*> playersNear;

                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member)
                        continue;

                    if (player->IsAtLootRewardDistance(member))
                        playersNear.push_back(member);
                }

                if (!playersNear.empty())
                {
                    uint32 goldPerPlayer = uint32(gold / playersNear.size());

                    for (Player* member : playersNear)
                    {
                        member->ModifyMoney(goldPerPlayer);
                        member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);

                        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                        data << uint32(goldPerPlayer);
                        data << uint8(playersNear.size() > 1 ? 0 : 1);
                        member->SendDirectMessage(&data);
                    }
                }
            }
            else
            {
                player->ModifyMoney(gold);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);

                // Send standard loot notification
                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(gold);
                data << uint8(1); // "You loot..."
                player->GetSession()->SendPacket(&data);
            }

            victim->loot.gold = 0;
        }

        // Handle Items
        uint32 items_count = uint32(victim->loot.items.size());
        uint32 max_slot = victim->loot.GetMaxSlotInLootFor(player);

        for (uint32 i = 0; i < max_slot; ++i)
        {
            // Quest item slots
            if (i >= items_count)
            {
                InventoryResult msg;
                player->StoreLootItem(uint8(i), &victim->loot, msg);
                continue;
            }

            LootItem& lootItem = victim->loot.items[i];

            if (lootItem.is_looted)
                continue;

            // Items under roll / master loot must stay handled by the group system.
            if (lootItem.is_blocked)
                continue;

            // Skip items allocated to other players (round-robin / master loot).
            const AllowedLooterSet& allowed = lootItem.GetAllowedLooters();
            if (!allowed.empty() && !allowed.count(player->GetGUID()))
                continue;

            InventoryResult msg;
            player->StoreLootItem(uint8(i), &victim->loot, msg);
        }

        // Cleanup corpse visual and decay if empty
        if (victim->loot.isLooted())
        {
            PetLootConfig::instance()->GroupLootStarted.erase(victim->GetGUID());

            victim->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
            victim->AllLootRemovedFromCorpse();
        }

        // Pet returns to player
        pet->GetMotionMaster()->MoveFollow(player, 1.0f, 0.0f);

        return true;
    }

private:
    ObjectGuid _playerGuid;
    ObjectGuid _victimGuid;
    ObjectGuid _petGuid;
};

class PetLootWorldScript : public WorldScript
{
public:
    PetLootWorldScript() : WorldScript("PetLootWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        PetLootConfig::instance()->Load();
        groupLooterCache.clear();
    }
};

class PetLootPlayerScript : public PlayerScript
{
public:
    PetLootPlayerScript() : PlayerScript("PetLootPlayerScript") { }

    void OnPlayerCreatureKill(Player* killer, Creature* victim) override
    {
        PETLOOT_LOG("OnPlayerCreatureKill: hook fired (killer={})", killer != nullptr);
        ProcessPetLoot(killer, victim);
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* victim) override
    {
        PETLOOT_LOG("OnPlayerCreatureKilledByPet: hook fired (owner={})", petOwner != nullptr);
        ProcessPetLoot(petOwner, victim);
    }

private:
    void ProcessPetLoot(Player* player, Creature* victim)
    {
        PETLOOT_LOG("ProcessPetLoot: entry");

        if (!PetLootConfig::instance()->Enabled)
        {
            PETLOOT_LOG("ProcessPetLoot: disabled");
            return;
        }

        if (!player || !victim)
        {
            PETLOOT_LOG("ProcessPetLoot: null player={} victim={}", player != nullptr, victim != nullptr);
            return;
        }

        // The killer may be a playerbot with no vanity pet; resolve the human pet owner first.
        Player* looter = FindPetLooter(player, victim);
        if (!looter)
        {
            PETLOOT_LOG("ProcessPetLoot: no eligible pet owner (killer {})", player->GetName());
            return;
        }

        // Check if the player has one of the configured vanity pets summoned
        ObjectGuid critterGuid = looter->GetCritterGUID();
        if (critterGuid.IsEmpty())
        {
            PETLOOT_LOG("ProcessPetLoot: critter-guid-empty owner={}", looter->GetName());
            return;
        }

        Creature* critter = ObjectAccessor::GetCreature(*looter, critterGuid);
        if (!critter)
        {
            PETLOOT_LOG("ProcessPetLoot: critter not found owner={}", looter->GetName());
            return;
        }

        PETLOOT_LOG("ProcessPetLoot: critter found=true entry={} configured-pets={}",
                    critter->GetEntry(), PetLootConfig::instance()->PetIds.size());
        if (!IsLootPet(critter->GetEntry()))
            return;

        // Distance check between player and victim
        float dist = looter->GetDistance(victim);
        PETLOOT_LOG("ProcessPetLoot: dist={} radius={}", dist, PetLootConfig::instance()->Radius);
        if (dist > PetLootConfig::instance()->Radius)
            return;

        PETLOOT_LOG("ProcessPetLoot: PASS moving pet owner={}", looter->GetName());

        // Visual: Pet moves to the corpse
        critter->GetMotionMaster()->MovePoint(1, victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ());

        // Schedule the looting event
        uint32 delay = 1000;
        looter->m_Events.AddEvent(new PetLootEvent(looter->GetGUID(), victim->GetGUID(), critter->GetGUID()), looter->m_Events.CalculateTime(delay));
    }
};

void Addmod_pet_lootScripts()
{
    new PetLootWorldScript();
    new PetLootPlayerScript();
}
