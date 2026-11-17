# M5Stack StopWatch GrokBot

Xiaozhi board variant for M5Stack StopWatch / C152. It keeps the existing
StopWatch hardware setup and replaces the default emoji UI with a GrokBot
animation rendered through the normal LVGL display pipeline.

## Runtime design

- 360x360, 4-bit grayscale, 30 FPS GrokBot clips.
- Only the 39 GrokBot clips used by Xiaozhi are packaged (~5.04 MiB). Byte-identical
  showcase duplicates (`bored`, `blocked`, `orbit`, `progress`, `dragging`,
  `showcase`, `reading`, `coding`, `running`, `computing`, `controlling`) and thin-line
  morphs (`bouncing`, `dictating`, `sending`) from the 53-state showcase atlas are excluded.
- The atlas is stored in the existing 8 MiB `assets` partition, so the standard
  dual-OTA partition table remains unchanged.
- RGB565 double buffers, packed frames and delta frames are allocated in PSRAM.
- CO5300 QSPI runs at 80 MHz. LVGL continues to flush through its internal
  64-line DMA buffer rather than reading a full PSRAM frame directly.
- Animation decoding runs in a separate low-priority task on core 0. Audio and
  the application event loop are not blocked by frame decoding.

## State mapping & director behavior

Built-in Xiaozhi emoji names select from weighted multi-variant GrokBot pools
(avoiding back-to-back repeats and coalescing duplicate emotions within 2s):

| Xiaozhi emotion | GrokBot variants |
| --- | --- |
| neutral | clears emotion override (`idle`) |
| relaxed | `humming`, `idle` |
| happy | `happy`, `excited`, `celebrate`, `humming` |
| loving | `happy`, `shy`, `humming` |
| delicious | `happy`, `playful`, `excited` |
| laughing | `laughing`, `excited`, `celebrate` |
| excited | `excited`, `celebrate`, `playful` |
| confident, cool | `proud`, `turntable`, `celebrate` |
| funny, silly | `playful`, `laughing`, `turntable` |
| winking, kissy | `playful`, `shy`, `happy` |
| embarrassed | `shy`, `confused` |
| curious | `curious`, `suspicious` |
| thinking | `curious`, `suspicious`, `thinking` |
| confused | `confused`, `suspicious` |
| suspicious | `suspicious`, `curious` |
| shocked, surprised | `surprised`, `scared`, `waking` |
| scared | `scared`, `surprised` |
| angry | `angry`, `suspicious` |
| crying, sad | `sad`, `shy` |
| sleepy | `sleeping`, `drowsy` |
| drowsy, low_battery | `drowsy`, `powering-down` |
| warning | `alerting` |

Additional clips and mappings in the 39-clip subset:
- `spawning`, `powering-down`: boot reveal and power-save sleep transition.
- `humming`, `suspicious`, `celebrate`, `turntable`, `logo`: idle fidgets, easter eggs, and emotion variants.
- `working`, `writing`: multi-stage thinking chain (`thinking` -> `working` -> `searching`/`writing`/`working`).
- `finished`: one-shot completion transition when returning to idle after speaking (including auto-mode `streaming` -> `listening` -> `idle`) or after >=1.5s of listening.
- `receiving`: OTA upgrade / download progress (`OTA_UPGRADE` / `UPGRADING` status, plus `cloud_download` and `download` emotions; falls back to `uploading` on older 28-clip assets).
- `radar`: Wi-Fi scanning notification (`SCANNING_WIFI`) and radar status.

`GrokBotDirector` orchestrates transitions and idle phases on cycle boundaries:
- **Idle phases**: 0–20s post-chat afterglow (`humming`/`curious`/`happy`/`playful`),
  20–120s exploration & easter eggs (`turntable`/`celebrate`/`logo`), 120–240s getting
  sleepy (`drowsy`/`humming`/`shy`), 240–300s `drowsy` loop (entered on cycle boundary without interrupting active fidgets), and >=300s `sleeping` loop.
  Minute clock status updates (`HH:MM`) do not reset the idle timer.
- **Transitions**: Boot starts with `spawning`; waking from deep sleep/power-save plays
  `waking`; user speech during listening starts the `thinking` -> `working` ->
  `searching`/`writing`/`working` chain; non-neutral emotions during speaking override
  `streaming` for 8s and return on a cycle boundary (up to 2s grace, with `idle` variants falling back to `streaming`); session end plays `finished` before returning to idle.

Button 1 keeps the normal wake/chat behavior and Button 2 keeps the normal
volume behavior. No GrokBot showcase controls are assigned to them.

## Build

```sh
. "$HOME/esp/esp-idf/export.ps1"
python scripts/build.py m5stack/stopwatch-grokbot --name m5stack-stopwatch-grokbot
```

The checked-in atlas can be regenerated from the full StopWatch-GrokBot atlas:

```sh
python scripts/build_grokbot_subset.py \
  ../Stopwatch-GrokBot/main/grokbot.atlas \
  main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas \
  --size 360 --max-size 0x700000
```

## Assets update note

Normal firmware OTA only updates the application partition and does not update `grokbot.atlas`
in the `assets` partition. Updating the atlas (e.g. upgrading from the 28-clip to the
39-clip atlas) requires either a separate `assets` resource package update (`assets.download_url`)
or flashing the `assets` partition (`0x800000` size) / complete merged image
(`idf.py flash` / `esptool.py write_flash`). `GrokBotDirector` also degrades gracefully
if run against an older 28-clip `assets` partition.
The display pauses animation decoding while Xiaozhi reports that it is loading assets.

## Hardware validation still required

Build success does not validate the panel at 80 MHz or the concurrent
animation/audio workload. Check frame rate, audio continuity, PSRAM headroom,
panel stability, touch/button behavior and long-running playback on hardware.
