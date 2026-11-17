# M5Stack StopWatch GrokBot

Xiaozhi board variant for M5Stack StopWatch / C152. It keeps the existing
StopWatch hardware setup and replaces the default emoji UI with a GrokBot
animation rendered through the normal LVGL display pipeline.

## Runtime design

- 360x360, 4-bit grayscale: idle targets 30 FPS, voice sessions 20 FPS, and
  power-save sleep 5 FPS. Ordinary sphere faces are rendered
  from eye geometry and motion tracks; complex icon animations use the atlas.
- The 39 GrokBot clips used by Xiaozhi are packaged (~4.85 MiB). Byte-identical
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
- Eye shapes interpolate over 280ms and body poses over 350ms. Interrupted
  transitions resume from the current geometry. Idle uses breathing, gaze,
  blinks and the Idle_B/Idle_D/Idle_A motion tracks.
- Face-to-icon transitions with a known shared starting pose first return to
  that pose geometrically. Other atlas transitions retain a six-frame blend.
- CO5300 vertical TE at scanline 128 aligns strip transfers. Status refreshes
  share the same serialized TE wait; low-rate sleep pacing stays outside the
  LVGL lock. The diagnostic `tear_risk` estimates scan timing and is not an
  optical measurement.
- Voice-session pacing leaves time for AFE and Opus; animation runs below them
  in priority outside idle. Atlas frame-count-based segments take 1.5 times
  their 30 FPS duration during voice sessions, including the waking sequence;
  geometric eye/body transitions keep their 280/350ms durations.

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
| drowsy, low_battery | `drowsy` |
| warning | `alerting` |

Additional clips and mappings in the 39-clip subset:
- `logo`: non-black boot reveal (frames 2–56, once ping-pong). `spawning` and
  `powering-down` remain packaged; automatic boot/sleep transitions avoid their dark holds.
- `humming`, `suspicious`, `celebrate`, `turntable`, `logo`: idle fidgets, easter eggs, and emotion variants.
- `working`, `writing`: multi-stage thinking chain (`thinking` -> `working` -> `searching`/`writing`/`working`).
- `finished`: one-shot completion transition when returning to idle after speaking (including auto-mode `streaming` -> `listening` -> `idle`) or after >=1.5s of listening.
- `receiving`: OTA upgrade / download progress (`OTA_UPGRADE` / `UPGRADING` status, plus `cloud_download` and `download` emotions; falls back to `uploading` on older 28-clip assets).
- `radar`: Wi-Fi scanning notification (`SCANNING_WIFI`) and radar status.

`GrokBotDirector` orchestrates transitions and idle phases on cycle boundaries:
- **Idle phases**: 0–20s post-chat afterglow (`humming`/`curious`/`happy`/`playful`),
  20–120s exploration & easter eggs (`turntable`/`celebrate`/`logo`), 120–240s getting
  sleepy (`drowsy`/`humming`/`shy`), 240–300s `drowsy` loop (entered on cycle boundary without interrupting active fidgets), and >=300s `sleeping` loop.
  Fidget cooldowns after completion are 6–11s / 10–18s / 8–15s respectively.
  Base idle keeps the complete breathing ping-pong (`idle[2..179]`); each end is a
  switch seam (~5.9s apart). After at least two breathing passes, each seam has a
  1-in-3 chance of inserting a random 0.4–2.8s pause (`idle[2..14]`); pauses then
  resume breathing. Thus full breathing occupies >80% of base time. Due fidgets
  take priority at seams: at most ~5.9s late while breathing, ~0.4s during a pause.
  Boot deliberately starts with 20s of idle age:
  exploration starts immediately, getting sleepy at 100s, `drowsy` at >=220s and
  `sleeping` at >=280s (each waits for the current animation boundary).
  Minute clock status updates (`HH:MM`) do not reset the idle timer.
- **Transitions**: Boot starts with `logo`; power-save plays a `drowsy` round trip
  before `sleeping`, with the whole transition at 30 FPS before the 5 FPS sleep loop.
  Waking plays `waking[2..72]`, ending near the idle face. It completes across
  connecting/listening/streaming changes and user transcripts, then follows the latest
  status (including a queued speaking emotion or thought sequence); failure and strong
  business states such as OTA can interrupt it. User messages during the first 1.5s
  after entering listening (or starting waking) are treated as wake-word echoes for
  animation and do not start thinking.
  Recognition and server messages are unaffected. User speech during listening
  starts `thinking` -> `working` -> `searching`/`writing`/`working`: the first two
  round trips last ~2.03s each (frames 0–30–0), and every thought clip returns to
  its shared frame 0 before the next starts; non-neutral emotions during speaking override
  `streaming` for 8s and return on a cycle boundary (up to 2s grace, with `idle` variants falling back to `streaming`); session end plays `finished[2..29]` (~0.93s) before returning to idle.

Button 1 keeps the normal wake/chat behavior and Button 2 keeps the normal
volume behavior. No GrokBot showcase controls are assigned to them.

The optional CST820B touchscreen and BMI270 accelerometer are polled at 20 Hz.
While idle, a tap produces a happy reaction, a stroke an affectionate reaction,
a shake a confused reaction, and a sustained tilt a curious reaction. These
interactions restart the idle timer and wake a visually sleeping face. They do
not start a voice session or interrupt listening, speaking, notifications, or
power-save mode. Reactions share a 1.8-second cooldown; stale queued input is
dropped. Missing sensors do not prevent startup.

## Build

```sh
. "$HOME/esp/esp-idf/export.ps1"
python scripts/build.py m5stack/stopwatch-grokbot --name m5stack-stopwatch-grokbot
```

The checked-in 360x360 atlas can be baked directly from the GrokBot vector engine
with 4x supersampling (`1440x1440` -> anti-aliased `360x360`):

```sh
(cd .tmp && pnpm add puppeteer-core)
node scripts/bake_grokbot_atlas.mjs \
  --grokbot-root research/Stopwatch-GrokBot \
  --frames-dir .tmp/grokbot-frames-1440 \
  --output main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas \
  --size 360 --supersample 4 --max-size 0x700000
```

Or re-encoded from an existing baked directory / full StopWatch-GrokBot atlas:

```sh
uv run --with numpy --with pillow python scripts/build_grokbot_subset.py \
  .tmp/grokbot-frames-1440 \
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
