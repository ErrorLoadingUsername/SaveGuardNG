# SaveGuard NG

Safe saving and smart autosaves for Skyrim SE / AE - an SKSE plugin.

**Nexus Mods:** https://www.nexusmods.com/skyrimspecialedition/mods/199246

Papyrus mods send their mod events through SKSE objects (`ModEvent.Create` ... `Send`). While such an object is still open, saving can crash the game. SaveGuard NG watches the SKSE object storage in the background, tells you when saving is risky, holds quicksave back until it is safe, and can make its own safe autosaves.

## Features

- **Save crash guard** - right before every save (any kind, also by other mods), objects of finished or broken scripts are handed to SKSE for clean-up, so a script that ended a moment ago can no longer crash the save. When this saves a save that would have crashed, a notification says so and the menu counts it.
- **Live save state** - safe / running / danger; short-lived objects only count after one second.
- **Notifications** when the state changes, optionally when the pause menu opens.
- **Quicksave guard** - F5 is held back while it is not safe (and while sleeping/waiting) and done as soon as it is safe.
- **Smart autosaves** - after loading screens, sleep/wait, level up, completed journal quests, or every X minutes of play; only when safe and not in combat, optionally not during scenes, sneaking, riding or jail.
- **No saving while sleeping or waiting** - protects against plugins that must not be saved in that window.
- **Rotating slots** `Autosave11`, `Autosave12` ... with the game's naming scheme (shown as AUTO in the load menu).
- **Deutsch / English**, log with the open objects and the scripts that created them.

Settings: `Documents\My Games\Skyrim Special Edition\SKSE\SaveGuardNG.ini` (created on first start, English comments).

## How it works

- The SKSE object storage is found at runtime through SKSE's own RTTI (`SKSEPersistentObjectStorage` type descriptor -> complete object locator -> vtable -> the single instance in `.data`). The layout is checked before anything is read; if it does not match, the plugin stays inactive. No hard-coded SKSE addresses.
- Quicksave guard: hook on `QuickSaveLoadHandler::ProcessButton` (vtable slot 5).
- Sleep/wait window: `TESSleepStart/Stop` and `TESWaitStart/Stop` events.
- Last moment before saving: on SKSE's `kSaveGame` message (saving thread, right before the save is written) objects of finished or stale Papyrus stacks get an owner id that is not in the VM's stack table, so SKSE frees them without the stack lookup that crashes (`skse64+5D51F`). Objects of running scripts are left alone.

## Requirements

- SKSE64 matching the game version
- Address Library for SKSE Plugins
- SKSE Menu Framework (optional, in-game menu)

Skyrim SE 1.5.97, AE 1.6.x, 1.7.104 (SKSE 2.3.x, Address Library for 1.7) and Skyrim VR - tested on 1.7.104, VR and 1.6.1170. Do not install together with SaveGuard (`SaveGuard.dll`) - use one of the two.

## Building

Requirements: Visual Studio 2022 (C++ workload), CMake 3.21+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

1. Put `SKSEMenuFramework.h` into `include/` (see [include/README.md](include/README.md)).
2. Open the folder in Visual Studio 2022, select the `release` preset and build (`Ctrl+Shift+B`) - or from a developer prompt:
   ```
   cmake --preset release
   cmake --build --preset release
   ```
3. The plugin is written to `build/release/SaveGuardNG.dll`.

Dependencies come from vcpkg: [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) (multi-target SE/AE), spdlog, fmt.

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) by CharmedBaryon and contributors, based on CommonLibSSE by Ryan McKenzie and powerof3
- SKSE team
- meh321 for Address Library for SKSE Plugins
- SkyrimThiago for SKSE Menu Framework
- spdlog (Gabi Melman) and fmt (Victor Zverovich)

## License

[GPL-3.0-or-later](LICENSE) - built with CommonLibSSE NG (alandtse), which is GPL-3.0-or-later.
