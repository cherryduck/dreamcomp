# Ports registry

One section per port: paths, commands, keys, status. Skills read this instead of guessing.

## Soulcalibur (USA) — `soulcalibur-recomp`

| | |
|---|---|
| Local repo | `ports/soulcalibur-recomp` (branch `main`), public at https://github.com/danielgomesvieira2000/soulcalibur-recomp (2026-10-08) |
| Disc | T1401N V1.000 (1999-07-30); boot `1ST_READ.BIN` 3,679,460 bytes, SHA-1 `967ca1fe2e8c7df57a5de1e83d939bed483cc6c9` |
| SDK | Shinobi 1.43, Ninja (Apr 1999), Kamui 1.06, sd 1.00.18 (strings in the boot file) |
| Build | `python tools/dc.py build ports/soulcalibur-recomp` → `build/game/soulcalibur-recomp.exe` |
| Run | `python tools/dc.py run ports/soulcalibur-recomp --window`, or double-click the exe |
| Settings | `%APPDATA%\dreamcomp\soulcalibur\settings.ini`, VMU `vmu_a1.bin` beside it |
| Standard script | `--press start@600,start@900,a@1200,a@1260` → title, Arcade, Kilik, first fight (~frame 1700+) |
| Port flags | `--widescreen` (anamorphic, Flycast value 0.75 at `0x8C266C28`) |
| Status | boots, intro, menus, arcade fights across stages, saves; 2026-10-06 accuracy pass (HUD, shadows, fog, texture invalidation, tile clip, mipmaps) and performance pass; scenarios: `python tools/scenario.py boot|menus|fight|fight-long|attract` |

## Jet Grind Radio (USA) — `jet-grind-radio-recomp`

| | |
|---|---|
| Local repo | `ports/jet-grind-radio-recomp` (branch `main`; no GitHub remote yet) |
| Disc | MK-51058 V1.005 (2000-10-02); boot `1ST_READ.BIN` 3,075,648 bytes, SHA-1 `dc0bda1431d97223885a22c68f7ddb3524757895` |
| SDK | Shinobi 1.68, Ninja (Feb 2000), Kamui 1.11; CRI ADX/SJ middleware (streamed music) |
| Build | `python tools/dc.py build ports/jet-grind-radio-recomp` → `build/game/jet-grind-radio-recomp.exe` |
| Settings | `%APPDATA%\dreamcomp\jetgrindradio\settings.ini` |
| Standard script | `--press start@600,start@900` → title flyby (~frame 2100), "PRESS START BUTTON" |
| Not translated | `2_DP.BIN` (Dream Passport 2 browser for the online features) |
| Status | 2026-10-06: boots, `scenario.py play --port jetgrindradio` reaches Gum's tutorial and skates the first street (0 untranslated / 0 unmapped); widescreen to 21:9 with the HUD corrected (polygon rule + one override); idle skip; translated = interpreted at attract frame 10040. Not yet: compared with Flycast, listened to by Daniel, played past the tutorial. Plan: the port's docs/PLAN.md |

## Ecco the Dolphin: Defender of the Future (Europe) — `ecco-the-dolphin-defender-of-the-future-recomp`

| | |
|---|---|
| Local repo | `ports/ecco-the-dolphin-defender-of-the-future-recomp` (branch `main`), public at https://github.com/cherryduck/ecco-the-dolphin-defender-of-the-future-recomp (2026-10-15); needs dreamcomp fork `cherryduck/dreamcomp` branch `linux-ecco` |
| Disc | MK-5103350 V1.003 (2000-05-09); boot `0.BIN` 2,969,164 bytes, SHA-1 `e6b209962f6d3d9565a4a6f8d430f1e921d264c0` |
| SDK | CodeWarrior; Shinobi 1.43, Kamui2 16.3.2.4 (no Ninja); AUDIO64 sound driver; CRI ADX 5.76 |
| Build | Linux: `python tools/dc.py build ports/ecco-the-dolphin-defender-of-the-future-recomp` (70 s fresh with 32 shards) |
| Settings | `~/.config/dreamcomp/ecco/settings.ini`, VMU `vmu_a1.bin` beside it |
| Scenarios | `python tools/scenario.py boot|menus|play|attract --port ecco` |
| Port flags | none yet; Flycast widescreen entry exists (`0x8C275418`, `0x8C040E68`, `0x8C040D1C`) |
| Status | 2026-10-15: boots, menus, new game, prologue film, first controllable scene; 0 untranslated over 20000 frames; translated = interpreted (TA) at frame 13100. Not yet: listened to, played past the first scene, widescreen |
