# Module: Pet Loot

<p align="center">
  <img src="icon.png" alt="Pet Loot" width="246" height="246">
</p>

This is a module for [AzerothCore](https://www.azerothcore.org).

It allows a configured vanity pet to act as an immersive auto-loot assistant for the player. Any pet whose entry is
listed in PetLoot.PetIds can loot (default list includes Bananas, Azure Whelpling, Golden Pig, Perky Pug, Mini Tyrael,
Phoenix Hatchling, Pandaren Monk and Spectral Tiger Cub).

## Features
- **Visual Movement**: The pet physically runs to the location of the killed creature before looting.
- **Looting Animation**: Upon reaching the corpse, the pet plays a "Loot" animation (emote) for immersion.
- **Auto-Loot**: Items and gold are automatically handled using AzerothCore native loot systems, including inventory validation, quest requirements, unique item checks, group loot rules, and gold sharing.
- **Distance Check**: The pet only loots if the player is within a configurable distance from the corpse (default: 50 yards).
- **Stability**: Uses `Player::StoreLootItem` to ensure compatibility with unique items, bag space, and quest requirements.
- **Group Loot Support**: Fully respects AzerothCore group loot rules (Free For All, Group Loot,
  Need Before Greed, Master Loot, Round-Robin). The pet automatically triggers the appropriate
  loot system exactly once, shares gold correctly between nearby group members, and leaves active
  loot rolls and master-loot distributions fully managed by the core. An optional FFA-only mode
  (`PetLoot.RequireFreeForAll`) can restrict looting to Free For All groups; solo kills are always
  allowed.
- **Safe Group Roll Handling**: Prevents duplicate Need/Greed or Master Loot rolls when players manually open the corpse after the pet has already initialized the loot.
- **Configurable**: Enable/disable the module, change allowed pet IDs, and adjust the looting radius via config.

## Requirements
- AzerothCore (latest version)
- A player with one of the configured vanity pets summoned (see PetLoot.PetIds).

## Installation
1. Place the `mod-pet-loot` directory into your `modules/` folder.
2. Recompile your AzerothCore project.
3. Apply the SQL file provided in the `sql/` directory (if not done automatically).
4. Copy `conf/mod_pet_loot.conf.dist` to `conf/mod_pet_loot.conf`.
5. Enjoy!

## Configuration
- `PetLoot.Enable`: Enable/Disable the module.
- `PetLoot.PetIds`: Comma-separated list of creature entries allowed to loot
  (default: 23234,7547,25146,37865,29089,26119,36911,36511 - Bananas, Azure Whelpling, Golden Pig,
  Perky Pug, Mini Tyrael, Phoenix Hatchling, Pandaren Monk, Spectral Tiger Cub).
- `PetLoot.Radius`: Maximum distance for looting (default: 50.0 yards).
- `PetLoot.CacheTTL`: Seconds to reuse a group's previously found pet owner before a fresh scan
  (default: 30.0 seconds; 0 disables caching). Every cached entry is fully revalidated on each kill.
- `PetLoot.DebugLog`: Enable verbose per-kill/group-scan diagnostics in the worldserver log (default: 0 - disabled).
- `PetLoot.AllowInDungeons`: Allow pet looting in 5-player dungeon instances (default: 1 - enabled).
- `PetLoot.AllowInRaids`: Allow pet looting in raid instances (default: 1 - enabled).
- `PetLoot.AllowInBattlegrounds`: Allow pet looting in battlegrounds (default: 0 - disabled).
- `PetLoot.AllowInArenas`: Allow pet looting in arenas (default: 0 - disabled).
- `PetLoot.SpeedMultiplier`: Movement-speed coefficient for the pet's trip to the corpse
  (default: 1.0; clamped to 0.1-10.0). 1 = normal, 2 = double, 3 = triple. Normally only the
  outbound trip is affected; follow-home uses the player's speed, and the fixed 1s loot delay
  is unchanged.
- `PetLoot.RequireFreeForAll`: When enabled, the pet loots only if the group's loot method is
  Free For All; under Group Loot, Need Before Greed, Master Loot or Round-Robin it does nothing
  (default: 0 - disabled, loots under all methods). Solo kills are always allowed (no group is
  treated as implicit Free For All).

## License
Released under the [GNU AGPL v3 license](LICENSE).
