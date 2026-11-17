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
- **导演** `main/display/grokbot_director.{h,cc}`：纯 C++、零 ESP 依赖，注入单调时钟和种子；输出 `{clip, mode, max_frames, seq}`；三种播放模式 `kLoopPingPong / kOnceForward / kOncePingPong`；只在循环边界切换，避免跳帧；按实际 atlas 过滤 clip，旧 28-clip 资产自动降级。
- **闲时阶段**：0-20s 余韵（6-11s 一个动作）-> 20-120s 探索与彩蛋（10-18s，`turntable/celebrate/logo` 独立长冷却）-> 120-240s 打盹 -> 240-300s `drowsy` 循环 -> 300s+ `sleeping`。最近两个动作不重复。
- **过渡**：开机 `spawning`；睡眠唤醒 `waking`（跨 Connecting->Listening 保留）；思考链 `thinking -> working -> searching/writing/working`；说话中情绪 8s 并在边界柔切回 `streaming`；对话结束 `finished`；失败 `confused -> failed`；省电 `powering-down`（30fps 播完再降 5fps）；OTA `receiving`；Wi-Fi 扫描 `radar`。
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

- [x] 主机测试 `uv run python -m unittest discover -s scripts/tests`：168 通过，1 跳过（缺 cJSON 开发头）。含导演测试与帧游标测试（`-UNDEBUG`，副作用不在断言内）。
- [x] 固件：`espressif/idf:release-v6.1` 构建 `m5stack-stopwatch-grokbot` 成功，app `0x2dd3c0`，分区剩余 27%；assets 约 6.86 MB < 8 MiB。
- [ ] 真机：30fps 帧率与解码耗时、音频并发连续性、PSRAM 余量、TE 同步、10 分钟以上闲时全阶段观感、唤醒/思考/说话/结束过渡、旧 28-clip 资产降级。

提交：`ae616fb`（atlas）、`15cbf0c`（导演与显示层）。

## 后续想法（未实施）

- 根据麦克风音量/VAD 让 listening 眼神跟随声音强弱。
- 按本地时间调整闲时节奏（深夜更早犯困，早晨 `waking` 问候）。
- 触摸/按键时触发 `surprised` 或 `shy` 的即时反应。
- 网页引擎的眨眼与弹簧若需移植，需要额外的程序化叠加层，代价是破坏 4-bit XOR 管线，当前不做。
