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

#include <set>
#include <vector>

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
    uint32 PetId;
    float Radius;

    // Prevents the pet from starting group loot rolls multiple times
    // for the same creature corpse.
    std::set<ObjectGuid> GroupLootStarted;

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("PetLoot.Enable", true);
        PetId = sConfigMgr->GetOption<uint32>("PetLoot.PetId", 23234); // Default: Bananas
        Radius = sConfigMgr->GetOption<float>("PetLoot.Radius", 50.0f);

        LOG_INFO("module.pet_loot", "Load: enabled={} petId={} radius={}",
                 Enabled, PetLootConfig::instance()->PetId, PetLootConfig::instance()->Radius);
    }
};

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
        if (!critter || !critter->IsAlive() || critter->GetEntry() != PetLootConfig::instance()->PetId)
            return false;

        if (!player->IsInMap(victim) || !player->InSamePhase(victim) ||
            !critter->IsInMap(victim) || !critter->InSamePhase(victim))
        {
            LOG_INFO("module.pet_loot", "Candidate {} skipped: different map or phase", player->GetName());
            return false;
        }

        if (player->GetDistance(victim) > PetLootConfig::instance()->Radius)
            return false;

        if (outCritter)
            *outCritter = critter;

        return true;
    };

    if (killer->GetSession() && killer->GetSession()->IsHeadless())
        LOG_INFO("module.pet_loot", "Killer {} skipped: headless (bot) session", killer->GetName());

    if (hasPetNear(killer, nullptr))
        return killer;

    LOG_INFO("module.pet_loot", "Scanning group of killer {} for pet owner", killer->GetName());
    Group* group = killer->GetGroup();
    if (!group)
    {
        LOG_INFO("module.pet_loot", "No pet owner found: killer {} is not in a group", killer->GetName());
        return nullptr;
    }

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member)
            continue;

        if (member->GetSession() && member->GetSession()->IsHeadless())
        {
            LOG_INFO("module.pet_loot", "Group member {} skipped: headless (bot) session", member->GetName());
            continue;
        }

        Creature* critter = nullptr;
        if (hasPetNear(member, &critter))
        {
            LOG_INFO("module.pet_loot", "Selected pet looter {} critter entry {} dist {}",
                     member->GetName(), critter->GetEntry(), member->GetDistance(victim));
            return member;
        }
    }

    LOG_INFO("module.pet_loot", "No pet owner found in killer {} group", killer->GetName());
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
    }
};

class PetLootPlayerScript : public PlayerScript
{
public:
    PetLootPlayerScript() : PlayerScript("PetLootPlayerScript") { }

    void OnPlayerCreatureKill(Player* killer, Creature* victim) override
    {
        LOG_INFO("module.pet_loot", "OnPlayerCreatureKill: hook fired (killer={})", killer != nullptr);
        ProcessPetLoot(killer, victim);
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* victim) override
    {
        LOG_INFO("module.pet_loot", "OnPlayerCreatureKilledByPet: hook fired (owner={})", petOwner != nullptr);
        ProcessPetLoot(petOwner, victim);
    }

private:
    void ProcessPetLoot(Player* player, Creature* victim)
    {
        LOG_INFO("module.pet_loot", "ProcessPetLoot: entry");

        if (!PetLootConfig::instance()->Enabled)
        {
            LOG_INFO("module.pet_loot", "ProcessPetLoot: disabled");
            return;
        }

        if (!player || !victim)
        {
            LOG_INFO("module.pet_loot", "ProcessPetLoot: null player={} victim={}",
                     player != nullptr, victim != nullptr);
            return;
        }

        // The killer may be a playerbot with no vanity pet; resolve the human pet owner first.
        Player* looter = FindPetLooter(player, victim);
        if (!looter)
        {
            LOG_INFO("module.pet_loot", "ProcessPetLoot: no eligible pet owner (killer {})", player->GetName());
            return;
        }

        // Check if the player has the required vanity pet (Bananas) summoned
        ObjectGuid critterGuid = looter->GetCritterGUID();
        if (critterGuid.IsEmpty())
        {
            LOG_INFO("module.pet_loot", "ProcessPetLoot: critter-guid-empty owner={}", looter->GetName());
            return;
        }

        Creature* critter = ObjectAccessor::GetCreature(*looter, critterGuid);
        if (!critter)
        {
            LOG_INFO("module.pet_loot", "ProcessPetLoot: critter not found owner={}", looter->GetName());
            return;
        }

        LOG_INFO("module.pet_loot", "ProcessPetLoot: critter found=true entry={} configured={}",
                 critter->GetEntry(), PetLootConfig::instance()->PetId);
        if (critter->GetEntry() != PetLootConfig::instance()->PetId)
            return;

        // Distance check between player and victim
        float dist = looter->GetDistance(victim);
        LOG_INFO("module.pet_loot", "ProcessPetLoot: dist={} radius={}", dist, PetLootConfig::instance()->Radius);
        if (dist > PetLootConfig::instance()->Radius)
            return;

        LOG_INFO("module.pet_loot", "ProcessPetLoot: PASS moving pet owner={}", looter->GetName());

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
