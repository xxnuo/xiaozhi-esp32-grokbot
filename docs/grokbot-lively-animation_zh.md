# GrokBot 灵动化：目标、计划与记录

适用板型：`m5stack/stopwatch-grokbot`。运行行为说明见 `main/boards/m5stack/stopwatch-grokbot/README.md`，本文记录目标、决策和进度。

## 目标

实际使用时表情动画太少、闲时只有单一 `idle` 循环。目标是让机器人在闲时和对话各阶段都有可信的“生命感”，且不改动 core application、不改板级引脚、不超出 8 MiB assets 分区。

## 研究资料（本地 `research/`，已通过 `.git/info/exclude` 排除，不入库）

| 资料 | 来源 | 用途 |
| --- | --- | --- |
| `research/grokbot.yin.wiki/` | https://grokbot.yin.wiki/index.html 完整镜像（含 `js/vendor/grok-player.js`、`grok-recipes.json`、`shape-data.json`） | 原版程序化引擎思路：idle clip 池随机、bridge 过渡、眨眼、弹簧 |
| `research/grokbot.yin.wiki.crawl.py` | 自写 ES module 递归抓取脚本 | 重新镜像：先 `wget -m -p -k -E -np` 再 `uv run python` 该脚本 |
| `research/Stopwatch-GrokBot/` | https://github.com/Ocean-lhy/Stopwatch-GrokBot | 完整 53 状态 400x400 `main/grokbot.atlas`，子集脚本的输入 |

源 atlas 关键发现：`bored=sleeping`、`blocked=confused`、`orbit=waiting`、`progress=loading`、`dragging=surprised`、`showcase=proud` 字节级相同；`reading/working/coding/running/computing/controlling` 六者相同；`bouncing/dictating/sending` 为 1%-3% 像素的细线 morph，不适合角色表情。

## 根因

1. 说话态 `streaming` 完全遮蔽 LLM 情绪，`tts stop` 后又被 `neutral` 清除，情绪 clip 几乎不可见。
2. STT 到 TTS 之间设备仍在 listening，没有思考态。
3. 闲时无阶段推进；每分钟时钟 `SetStatus("HH:MM")` 会误触发状态刷新。

## 方案

- **素材**：atlas 28 -> 39 clip（5,281,647 B），新增 `humming suspicious finished celebrate working turntable logo spawning writing receiving powering-down`。
- **导演** `main/display/grokbot_director.{h,cc}`：纯 C++、零 ESP 依赖，注入单调时钟和种子；输出 `{clip, mode, max_frames, seq, from_frame_zero}`；三种播放模式 `kLoopPingPong / kOnceForward / kOncePingPong`；闲时等循环边界、思考链往返回到共享第 0 帧后切换；按实际 atlas 过滤 clip，旧 28-clip 资产自动降级。
- **闲时阶段**：0-20s 余韵（6-11s 一个动作）-> 20-120s 探索与彩蛋（10-18s，`turntable/celebrate/logo` 独立长冷却）-> 120-240s 打盹（8-15s）-> 240-300s `drowsy` 循环 -> 300s+ `sleeping`。这些是对话结束后的闲时年龄；开机预置 20s，故从探索起算，100s 起打盹、220s 起等边界进入 `drowsy`、280s 起等边界进入 `sleeping`。最近两个动作不重复。
- **过渡**：开机 `logo[2..56]` 往返；睡眠唤醒 `waking[2..72]`（跨 Connecting->Listening 保留）；思考链 `thinking -> working -> searching/writing/working`，前两段各约 2.03s，以 `0..30..0` 往返在共享第 0 帧接合；说话中情绪 8s 并在边界柔切回 `streaming`；对话结束 `finished[2..29]`（0.93s）；失败 `confused -> failed`；省电 `drowsy` 往返接 `sleeping`（30fps 播完再降 5fps）；OTA `receiving`；Wi-Fi 扫描 `radar`。
- **情绪多变体**：每种 LLM 情绪对应加权变体池，避开上一变体，2s 内同组合并；系统图标保持确定映射。
- **显示层** `grokbot_display.{h,cc}`：portMUX mailbox 取代零散 atomic 指针；帧游标正/倒放 XOR、关键帧重建、失败重试判定抽成 `grokbot_player.h` 中可主机测试的纯函数。

## 协作流程记录

| 阶段 | 执行者 | 结果 |
| --- | --- | --- |
| 计划 | Gemini 4 Carbon（high）+ GPT-6 Astra（high）并行 | 两份计划合并为统一规格 |
| 编码 | GPT-6.1 Sol（显示层/导演/测试）+ Gemini 4 Argon（atlas/子集脚本/README） | 首版完成并构建通过 |
| 审阅 | Carbon + Astra 交叉审阅 | 发现资产解绑后读旧指针（P0）、单帧 delta 重复 XOR、`sleeping` 情绪卡死、waking 被 Connecting 截断、通知死帧、Auto 模式漏播 finished、`receiving` 不可达等 |
| 修复 | Sol（显示层）+ Argon（导演/测试/README） | 审阅清单全部修复 |

顺带修复的原有问题：`MapEmotion` 返回调用方临时字符串指针、通知过期清零竞态、解码/呈现失败仍推进游标、缺 clip 每帧重选、`force_refresh` 不触发重新 init、XOR 尾字节。

## 验证状态

- [x] 主机测试 `uv run python -m unittest discover -s scripts/tests`：169 项，1 跳过（缺 cJSON 开发头）。含导演测试与帧游标测试（`-UNDEBUG`，副作用不在断言内）。
- [x] 固件：`espressif/idf:release-v6.1` 构建 `m5stack-stopwatch-grokbot` 成功，app `0x2dd600`，分区剩余 27%；assets 6,860,726 B < 8 MiB。
- [ ] 真机：30fps 帧率与解码耗时、音频并发连续性、PSRAM 余量、TE 同步、10 分钟以上闲时全阶段观感、唤醒/思考/说话/结束过渡、旧 28-clip 资产降级。

提交：`ae616fb`（atlas）、`15cbf0c`（导演与显示层）。

## 主机预览与像素调参（2026-11-17）

`uv run --with numpy --with pillow python scripts/grokbot_preview.py all` 生成 `.tmp/preview/{boot_idle,conversation,wake_from_sleep}.mp4`、`_timeline.txt`、`_contact.png` 和 `_stats.json`。也可传单个场景及 `--seed N`、`--speed N`；默认开机闲时视频 8 倍速，其余原速。预览复用导演与帧游标，解码真实 atlas，不替代真机帧率和音频并发验证。

本轮对 360×360 灰度帧计算亮度均值、与 `idle[2]` 的 MAE、相邻帧差（均为 0–255 灰度单位）：

- 基础闲时保留完整 `idle[2..179]` 呼吸往返，两端都是同脸接缝，相隔约 5.9s。至少播放两趟完整呼吸后，每个接缝有 1/3 概率插入随机 0.4–2.8s 静止段（`idle[2..14]` 循环，实际呈现约 0.43–2.83s），然后恢复呼吸。两趟呼吸约 11.8s，因此呼吸段占基础闲时 >80%；不再整段闲时使用静止脸。小动作到期优先在接缝切入，呼吸段最多晚约 5.9s，静止段最多晚约 0.4s。
- `spawning[40..60]` 亮度约 1.98，`powering-down[12..]` 约 1.47 继续趋暗，而 `sleeping[2]` 为 66.99。帧数上限无法跳过开头，故开机换无黑屏的 `logo`、省电换 `drowsy` 往返，低电量情绪也只用 `drowsy`，避免熄灭后突然恢复亮脸。
- `waking[67..72]` 与 idle 的 MAE 约 0.008，故播到 72；`finished[29..53]` 完全静止，`logo[57..119]` 的相邻帧差已小于 0.05（60 起归零），分别限 30、57 帧。探索期和打盹期的 `shy` 均限 90 帧。
- 思考片段的第 0 帧完全相同；加入 `from_frame_zero`，通过 `kOnceDone` 在往返结束后推进，避免图标中段突然切成明亮人脸。

种子 42 的新时间表：开机闲时等待依次为 14.27 / 12.27 / 17.73 / 17.73 / 19.40s，后续打盹期常见 11.83s，间有 17.73s；呼吸接缝保留部分 5.9s 倍数，随机静止段打散其余等待。开机 221.57s 进入 `drowsy`，280.10s 进入 `sleeping`。对话场景仍在 5.50 / 7.53 / 9.57s 进入 `thinking` / `working` / `searching`，24.00–24.93s 播 `finished`，随后余韵首次等待 11.83s（包含等待呼吸接缝），36.77s 开始 `humming`。省电场景 3.50–6.13s 播 `drowsy`、6.13s 接 `sleeping`；10.00–12.37s 完整唤醒，开机/省电路径不含长黑屏素材。

预览 `_stats.json` 同时统计相对 `idle[2]` 的非静止帧与相邻帧运动占比，阈值均为 MAE >0.01/255，仅统计 `idle` 基础画面。种子 42 三场景的相邻运动帧分别为 **86.15% / 88.70% / 87.00%**；相对静脸的非静止帧为 86.50% / 89.06% / 87.13%。20 个种子的开机预览，相邻运动帧占 **84.70%–88.70%**；完整小动作间的冷却最小 / P25 / 中位数 / P75 / P95 / 最大为 **11.833 / 11.833 / 13.467 / 17.733 / 19.400 / 20.600s**（排除开机初段和进入长困倦的截断段），统计保存在 `.tmp/preview/idle_distribution.json`。这些是实际播放等待；配置冷却仍是余韵 6–11s、探索 10–18s、打盹 8–15s，额外等待来自上述接缝。


## 后续想法（未实施）

- 根据麦克风音量/VAD 让 listening 眼神跟随声音强弱。
- 按本地时间调整闲时节奏（深夜更早犯困，早晨 `waking` 问候）。
- 触摸/按键时触发 `surprised` 或 `shy` 的即时反应。
- 网页引擎的眨眼与弹簧若需移植，需要额外的程序化叠加层，代价是破坏 4-bit XOR 管线，当前不做。
