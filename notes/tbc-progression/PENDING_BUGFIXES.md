# Pending Bugfixes

Use this file to track code fixes that have been made, built, installed, or still need to be made live.

## Current Handoff

- Date: 2026-04-25
- Hardware maintenance interrupted the final startup check.
- Build completed successfully with `cmake --build /home/cbur/azerothcore-wotlk/build -j72`.
- Install completed successfully with `cmake --install /home/cbur/azerothcore-wotlk/build`.
- Installed binaries/config dists under `/home/cbur/azeroth-server`.
- `authserver` and `worldserver` were stopped cleanly before install.
- Start commands were interrupted, so service startup was not confirmed.
- After machine maintenance, start and check:
  - `sudo systemctl start authserver`
  - `sudo systemctl start worldserver`
  - `systemctl status authserver --no-pager`
  - `systemctl status worldserver --no-pager`
  - `journalctl -u worldserver -n 100 --no-pager`
  - `journalctl -u authserver -n 50 --no-pager`

## Installed, Not Confirmed Running

### Core and Playerbots Update

- Date: 2026-04-25
- Core branch: `Playerbot`
- Core revision installed: `d115eb658`
- Playerbots branch: `enhanced-strategies`
- Playerbots revision installed: `59e8bc8cd`
- GitHub push: completed to `origin/enhanced-strategies`
- Merge commit: `301addf7b Merge upstream master into enhanced strategies`
- Final fix commit: `59e8bc8cd Fix Magtheridon and Arcatraz target focus`
- Auchenai Crypts resolution: kept upstream official implementation under `src/Ai/Dungeon/AuchenaiCrypts`; removed old duplicate custom implementation under `src/strategy/dungeons/tbc/auchenaicrypts`.
- Build status: successful.
- Install status: successful.
- Live status: pending post-maintenance service start/verification.

### Magtheridon's Lair - Channeler Target Focus

- Date: 2026-04-25
- Area: `modules/mod-playerbots/src/Ai/Raid/Magtheridon`
- Files changed:
  - `Multiplier/RaidMagtheridonMultipliers.cpp`
  - `Multiplier/RaidMagtheridonMultipliers.h`
  - `Strategy/RaidMagtheridonStrategy.cpp`
- Problem: Bots could compete with generic `dps assist` / `tank assist` while Hellfire Channelers were alive, causing add-target confusion.
- Fix: Added `MagtheridonChannelerTargetMultiplier` to suppress generic assist actions while any channeler is alive, leaving the Magtheridon strategy's channeler kill order in control.
- Build status: successful.
- Install status: successful.
- Live status: pending post-maintenance service start/verification.

### Arcatraz - Harbinger Skyriss Clone Targeting

- Date: 2026-04-24
- Area: `modules/mod-playerbots/src/strategy/dungeons/tbc/arcatraz`
- Files changed:
  - `ArcatrazActions.cpp`
  - `ArcatrazTriggers.cpp`
  - `ArcatrazMultipliers.cpp`
- Problem: During the final boss clone phase, bots could stop attacking after focusing a Skyriss illusion.
- Fix: Bots now focus valid attackable illusions, fall back to Skyriss when no clone is attackable, and Skyriss no longer suppresses normal `dps assist` as a Mellichar add.
- Build status: successful.
- Install status: successful.
- Live status: pending post-maintenance service start/verification.

## Went Live

Move entries here after the fixed binary is installed and `worldserver` has been restarted.
