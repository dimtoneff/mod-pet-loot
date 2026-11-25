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
- **Group Loot Support**: Fully respects AzerothCore group loot rules (Free For All, Group Loot, Need Before Greed, Master Loot, Round-Robin). The pet automatically triggers the appropriate loot system exactly once, shares gold correctly between nearby group members, and leaves active loot rolls and master-loot distributions fully managed by the core.
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
- `PetLoot.DebugLog`: Enable verbose per-kill/group-scan diagnostics in the worldserver log (default: 0 - disabled).

## License
Released under the [GNU AGPL v3 license](LICENSE).
