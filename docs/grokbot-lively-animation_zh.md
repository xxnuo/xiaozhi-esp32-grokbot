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
| `research/skywt/grok-bot-avatar-animation-analysis.html` | https://skywt.cn/blog/grok-bot-avatar-animation-analysis | 用户补充的球面眼睛、动画轨道与过渡分析 |

本地上游 checkout 为 `9d917f8dcbe48396c74145341a55c8f7b8492081`。网页引擎与提取数据是社区分析/重实现资料，不是已证实的官方完整源码。

源 atlas 关键发现：`bored=sleeping`、`blocked=confused`、`orbit=waiting`、`progress=loading`、`dragging=surprised`、`showcase=proud` 字节级相同；`reading/working/coding/running/computing/controlling` 六者相同；`bouncing/dictating/sending` 为 1%-3% 像素的细线 morph，不适合角色表情。

## 根因

1. 说话态 `streaming` 完全遮蔽 LLM 情绪，`tts stop` 后又被 `neutral` 清除，情绪 clip 几乎不可见。
2. STT 到 TTS 之间设备仍在 listening，没有思考态。
3. 闲时无阶段推进；每分钟时钟 `SetStatus("HH:MM")` 会误触发状态刷新。

## 方案

- **素材**：atlas 28 -> 39 clip（5,281,647 B），新增 `humming suspicious finished celebrate working turntable logo spawning writing receiving powering-down`。
- **导演** `main/display/grokbot_director.{h,cc}`：纯 C++、零 ESP 依赖，注入单调时钟和种子；输出 `{clip, mode, max_frames, seq, from_frame_zero}`；三种播放模式 `kLoopPingPong / kOnceForward / kOncePingPong`；闲时等循环边界、思考链往返回到共享第 0 帧后切换；按实际 atlas 过滤 clip，旧 28-clip 资产自动降级。
- **闲时阶段**：0-20s 余韵（6-11s 一个动作）-> 20-120s 探索与彩蛋（10-18s，`turntable/celebrate/logo` 独立长冷却）-> 120-240s 打盹（8-15s）-> 240-300s `drowsy` 循环 -> 300s+ `sleeping`。这些是对话结束后的闲时年龄；开机预置 20s，故从探索起算，100s 起打盹、220s 起等边界进入 `drowsy`、280s 起等边界进入 `sleeping`。最近两个动作不重复。
- **过渡**：开机 `logo[2..56]` 往返；睡眠唤醒 `waking[2..72]`（跨 Connecting->Listening->Streaming 及 user 回显保留，播完再按最新状态继续）；思考链 `thinking -> working -> searching/writing/working`，前两段各约 2.03s，以 `0..30..0` 往返在共享第 0 帧接合；说话中情绪 8s 并在边界柔切回 `streaming`；对话结束 `finished[2..29]`（0.93s）；失败 `confused -> failed`；省电 `drowsy` 往返接 `sleeping`（30fps 播完再降 5fps）；OTA `receiving`；Wi-Fi 扫描 `radar`。
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

- [x] 主机测试 `uv run python -m unittest discover -s scripts/tests`：171 项，1 跳过（缺 cJSON 开发头）。含导演测试与帧游标测试（`-UNDEBUG`，副作用不在断言内）。
- [x] 固件：`espressif/idf:release-v6.1` 构建 `m5stack-stopwatch-grokbot` 成功，app `0x2ddcc0`，分区剩余 27%；assets 6,860,726 B < 8 MiB。
- [x] 真机闲时（2026-11-17，M5Stack StopWatch，SKU `m5stack-stopwatch-grokbot`，烧录 app + assets，保留 NVS）：加载 39 个 clip；闲时 29.55-29.71fps，解码约 15-18ms/帧，TE 59-60Hz 无超时；内部 SRAM 最低 33,468 B，PSRAM 余约 4.0 MB；330s 内无崩溃。实测时间线：logo 开机 -> idle(10.8s) -> suspicious/curious/humming/shy 小动作间隔约 12-18s，含随机停顿 -> 120s 起 drowsy/humming/shy -> 232s drowsy 循环 -> 288s sleeping。旧固件基线为 28 clip、开机 21.35fps。
- [x] 真机对话（临时 debug 固件定时调用 `WakeWordInvoke("hi esp")`，服务端回问候；debug 代码未提交，验证后已刷回干净固件）：空闲唤醒 `loading -> listening -> streaming -> happy` 情绪随说话出现，回显不再闪 `thinking`；服务端结束会话后 `finished -> idle` 并恢复小动作；睡眠 412s 后唤醒完整播 `waking`（约 2.4s）再接 `excited`。各状态帧率：idle 29.57、listening 27.19（最低 25.73，音频并发）、happy 28.47、waking 27.41、finished 27.62fps；无崩溃，日志 `.tmp/hw/autowake*.log`。
- [ ] 真机思考链：需真人说话产生 STT，主机测试与预览已覆盖；人耳评估说话音频连续性也待真人确认。

提交：`ae616fb`（atlas）、`15cbf0c`（导演与显示层）。

## 主机预览与像素调参（2026-11-17）

`uv run --with numpy --with pillow python scripts/grokbot_preview.py all` 生成 `.tmp/preview/{boot_idle,conversation,wake_from_sleep}.mp4`、`_timeline.txt`、`_contact.png` 和 `_stats.json`。也可传单个场景及 `--seed N`、`--speed N`；默认开机闲时视频 8 倍速，其余原速。预览复用导演与帧游标，解码真实 atlas，不替代真机帧率和音频并发验证。

本轮对 360×360 灰度帧计算亮度均值、与 `idle[2]` 的 MAE、相邻帧差（均为 0–255 灰度单位）：

- 基础闲时保留完整 `idle[2..179]` 呼吸往返，两端都是同脸接缝，相隔约 5.9s。至少播放两趟完整呼吸后，每个接缝有 1/3 概率插入随机 0.4–2.8s 静止段（`idle[2..14]` 循环，实际呈现约 0.43–2.83s），然后恢复呼吸。两趟呼吸约 11.8s，因此呼吸段占基础闲时 >80%；不再整段闲时使用静止脸。小动作到期优先在接缝切入，呼吸段最多晚约 5.9s，静止段最多晚约 0.4s。
- `spawning[40..60]` 亮度约 1.98，`powering-down[12..]` 约 1.47 继续趋暗，而 `sleeping[2]` 为 66.99。帧数上限无法跳过开头，故开机换无黑屏的 `logo`、省电换 `drowsy` 往返，低电量情绪也只用 `drowsy`，避免熄灭后突然恢复亮脸。
- `waking[67..72]` 与 idle 的 MAE 约 0.008，故播到 72；`finished[29..53]` 完全静止，`logo[57..119]` 的相邻帧差已小于 0.05（60 起归零），分别限 30、57 帧。探索期和打盹期的 `shy` 均限 90 帧。
- 思考片段的第 0 帧完全相同；加入 `from_frame_zero`，通过 `kOnceDone` 在往返结束后推进，避免图标中段突然切成明亮人脸。
- 真机自动唤醒日志 `.tmp/hw/autowake.log` 的 431–438s 显示，`>> hi esp` 回显先触发思考、服务端问候随即切入 speaking，原本 2.37s 的 `waking` 约 0.3s 就被打断。现将 waking 置于 listening/streaming 和 user 消息之前，播完才按最新状态接 speaking 情绪或思考链；失败、OTA 等强业务状态仍可抢占。进入 listening 或开始 waking 后 1.5s 内的 user 消息仅按唤醒词回显处理，不启动思考动画，不改变识别与服务端消息。

种子 42 的新时间表：开机闲时等待依次为 14.27 / 12.27 / 17.73 / 17.73 / 19.40s，后续打盹期常见 11.83s，间有 17.73s；呼吸接缝保留部分 5.9s 倍数，随机静止段打散其余等待。开机 221.57s 进入 `drowsy`，280.10s 进入 `sleeping`。对话场景仍在 5.50 / 7.53 / 9.57s 进入 `thinking` / `working` / `searching`，24.00–24.93s 播 `finished`，随后余韵首次等待 11.83s（包含等待呼吸接缝），36.77s 开始 `humming`。省电场景 3.50–6.13s 播 `drowsy`、6.13s 接 `sleeping`；10.00–12.37s 完整唤醒，10.30s 回显、10.35s streaming、10.40s happy 情绪均不截断；21.00–23.37s 再次完整唤醒，21.30s 回显被忽略，22.90s 的真实 user 消息在 23.37s 开始思考链。开机/省电路径不含长黑屏素材。

预览 `_stats.json` 同时统计相对 `idle[2]` 的非静止帧与相邻帧运动占比，阈值均为 MAE >0.01/255，仅统计 `idle` 基础画面。种子 42 三场景的相邻运动帧分别为 **86.15% / 88.70% / 60.61%**；相对静脸的非静止帧为 86.50% / 89.06% / 57.14%。睡醒场景现用于回显/问候/真实用户消息回归，只有 35 帧基础 idle，不用于评估长期呼吸占比。20 个种子的开机预览，相邻运动帧占 **84.70%–88.70%**；完整小动作间的冷却最小 / P25 / 中位数 / P75 / P95 / 最大为 **11.833 / 11.833 / 13.467 / 17.733 / 19.400 / 20.600s**（排除开机初段和进入长困倦的截断段），统计保存在 `.tmp/preview/idle_distribution.json`。这些是实际播放等待；配置冷却仍是余韵 6–11s、探索 10–18s、打盹 8–15s，额外等待来自上述接缝。


## 第二轮：锯齿、流畅度、切换闪屏（2026-11-17）

| 问题 | 根因 | 修复 | 证据 |
| --- | --- | --- | --- |
| 边缘锯齿 | 上游 1x 截图后 400->360 非整数缩放并二次 4-bit 量化；圆环 96 边形；显示端 `i*17` 线性写入 AMOLED，50% 覆盖边缘只亮约 22%，过渡被吃掉 | `scripts/bake_grokbot_atlas.mjs` 从矢量引擎 1440px 超采样 + 4x4 面积平均重烘 360 atlas（补丁需恰好匹配一次，384 边形）；灰阶 LUT 改 gamma 2.2 编码，淡化仍在覆盖率域 | 39 clip/帧数/start_frame 不变，4.849 MiB；接缝 MAE 保持；细线平移总亮度恒定测试；`.tmp/aa/*_compare.png` |
| 不流畅 | decode 15-18ms 与 present 7-15ms 串行，超出 33ms 偶尔错过 TE，帧间隔 33/50/67ms 抖动 | 专用呈现任务，解码下一帧与呈现当前帧流水化，首个 DMA strip 对齐固定 TE 相位，XOR 与展开融合 | 真机稳态 P95 33.61-33.65ms（原 36.4-37.6ms），over40ms=0，decode 18.2->14.2ms |
| 切换闪屏 | clip 硬切；切换需从关键帧连续解码 3 帧，每次切换掉 1-2 帧（67ms） | 切换 6 帧（200ms）4-bit 交叉淡化；39 个 clip 起始帧缓存在 PSRAM（2.53 MB，余约 1.35 MB）；失败候选帧不污染淡化源 | 真机连续 12 次切换 over40ms=0，最大帧间隔 33.69ms |

日志：`.tmp/hw/aa1.log`（修前）、`.tmp/hw/smooth-final*.log`、`.tmp/hw/smooth-cache-swap.log`。缓存分配失败会回退重建，届时切换不保证零丢帧；Wi-Fi 重连期间仍可能出现长帧。

## 第三轮：TE 相位与触摸、IMU（2026-11-17）

- CO5300 使用垂直 TE，扫描线从 466 调整到 128。360 行写入约 15.7–17.6ms，面板扫描同一区域约 14.9ms，因此原先从空白期开始写仍可能被扫描追上；新相位让扫描先行。状态栏独立刷新也对齐 TE。DMA 完成回调按条带首尾时间计算 `tear_risk`，这是时序估计，不是光学检测。
- 审查修复了 5fps 省电动画与状态刷新争抢同一个 TE semaphore 的问题：低帧率长等待使用锁外延时，最终 TE 等待统一在持有 LVGL 锁的 flush 回调中，锁内只等一个扫描周期。主机模拟和源码复查通过，睡眠并发状态刷新仍需真机复测。
- `.tmp/tear/metrics.json` 的 15 个采样窗口：旧相位风险计数 2233，新相位最终记录为 0，P95 最大 33.676ms，最长间隔 33.918ms，超过 40ms 为 0。后续输入版启动记录仍出现过 1 次风险计数，不能宣称所有情况下无撕裂。
- 板级增加 CST820B 与 BMI270 可选输入，20Hz 低优先级轮询。短按对应 happy，抚摸对应 loving，摇晃对应 confused，持续倾斜对应 curious；仅空闲时接收，刷新闲时计时并退出视觉睡眠，交互表情保持约 2.5s。共用 1.8s 冷却，主任务最多积压一条回调，超过 400ms 丢弃。
- 点击阈值 50–600ms、路程小于 30px；抚摸 100–4000ms、路程至少 65px；移动累计达到 6px 后更新参考点，避免丢失慢速抚摸。摇晃需 350ms 内两次至少 1400mg 的加速度变化；倾斜需超过基准 45° 并稳定 600ms，接受后重设基准。
- `.tmp/hw/inputs-boot.log` 已确认真机 BMI270 初始化成功、CST820B 在 `0x15` 响应，稳态 P95 约 33.7ms。传感器初始化不代表手势验收；实际触摸、摇晃阈值和光学撕裂仍需实物动作/观察。

## 第四轮：实时几何表情

- `GrokBotFace` 直接插值九种球面眼形与身体姿态，眼形 280ms、身体 350ms，切换或打断从当前输出几何继续。普通脸间停用交叉淡化。闲时轮换 Idle_B/B/D/B/A，加入呼吸、巡视和受眼型限制的随机眨眼。
- `thinking/searching/radar/waiting/loading/uploading/alerting/working/logo/spawning/writing/receiving/powering-down` 保留 atlas；普通脸进入这些图标前先用 350ms 回到共同姿态，再从 frame0 播放。独立解压验证 13 个图标 frame0 逐像素完全相同；程序回位与真实首帧的 MAE 为 0.048/255，358 个像素不同，最大差 2 个 4-bit 灰阶。其他复杂片段仍使用 6 帧淡化，不应称为所有路径完全几何化。
- 起始帧缓存缩为 18 个、1,166,400 B，首版真机 PSRAM 余约 2.70 MB。BMI270 首个有效三轴样本为 `(-821,276,461)mg`；这只确认真实数据读取，不替代手势校准。
- 首版真机暴露性能不足：程序脸约 52–59ms/帧，只有约 15fps；第二版 21–23ms、25–27fps。最终改为直接描边、活动边扫描、非零绕数填充和身体内部整段填充。中间形变自交产生的白孔已修复，891 组形变/旋转/离屏与独立参考比较最多差一个 4-bit 灰阶，ASan/UBSan 通过。
- 最终普通闲时/害羞真机约 29.3–29.7fps，几何平均 13–14ms，P95 帧间隔不超过 33.741ms，栈最低余 4200B。20s 稳态样本仍有 4 个约 67ms 长帧，不能宣称绝对无掉帧；启动/网络重试也有更长帧。日志 `.tmp/hw/face-edges-boot.log`，前两版对照为 `face-first-boot.log` / `face-fast-boot.log`。
- 静态 JS 对照覆盖 neutral/happy/calm/surprised 的正面、90°、180°及共享首帧；MAE 0.039–0.108/255。共享首帧与现有 atlas 的 MAE 为 0.053/255；结果 `.tmp/face-reference/metrics.json`。
- 预览复用真实 C++ 渲染器，生成 `.tmp/preview-final/{boot_idle,conversation,wake_from_sleep,interactions}.mp4`、时间线和接触表。交互场景明确标注模拟输入；基础 idle 相邻运动帧占约 97.09%，不同于旧版 atlas 呼吸约 86%。预览不代表硬件帧率。
- 进一步将无淡化几何帧的 packed/RGB565 输出融合，并使用固定行缓冲后批量复制，避免整帧再次展开和快照复制。提交成功前保留上一份已显示快照；失败不会污染下一次淡化源。动画优先级仅真实 idle 使用 5，其他应用状态使用 1，让 AFE/Opus 优先运行。融合输出逐字节对照、失败快照保护、96 组离屏/转向检查均通过。
- 主机最终 192 项测试通过、1 项因缺 cJSON 开发头跳过；8 项几何与 6 项预览测试在其中。缺少本地 research 的干净 checkout 会额外跳过数据提取一致性检查。ESP-IDF 6.1 构建剩余约 25% app 分区。固件仅修改该 GrokBot 板型的源文件选择，不影响普通 StopWatch 或无显示/OLED/LVGL 板型的编译路径。
- 临时诊断固件实测四种软件交互均接受，省电中 60 次状态刷新全部完成，约 4.94fps、TE 超时 0；自动唤醒实际收到服务端问候并进入听音/说话。诊断补丁用正反向校验恢复，不保留在生产源码。日志 `.tmp/hw/row-diag.log`，这些模拟事件不代表实体手势校准。
- 最终采用 idle 30fps、真实 listening/speaking 20fps、power-save sleeping 5fps。30fps 语音并发候选仍有编码队列压力，20fps 对照 `.tmp/hw/voice20-diag.log` 的约 30s 听音未出现编码队列丢帧，实测 19.7–19.8fps、P95 50.55–50.63ms，栈最低余 2868B；60 次状态刷新和四种模拟交互再次通过。首末条带耗时约 22ms，风险估计仍有 5–7 次/5s，不能承诺光学无撕裂或音频长期零丢帧。
- 20fps 时按帧计数的 atlas/导演段时长为 30fps 的 1.5 倍，waking 约从 2.37s 增至 3.55s；眼形/姿态 280/350ms 插值不变。这是保留音频预算的明确取舍。最终版本不包含自动唤醒或模拟输入代码；实体点击/抚摸/摇晃阈值与人耳音频连续性仍待人工实物验收。
- 干净最终构建 app `0x2f17a0` 已烧录，保留 NVS 和 assets；`.tmp/hw/release-clean-boot.log` 再次确认传感器正常、闲时约 29.3–29.5fps、P95 约 33.7ms，无诊断自动触发。研究资料仍位于本地 `research/`；实现代码和记录纳入本次提交。
- 后续无重启串口采集 `.tmp/hw/post-release-audit.log`：设备运行约 371s 时收到真实 `confused` 交互，从 sleeping 切到 suspicious，约 2.5s 后回 idle；用户随后明确确认“我刚刚摇晃了”，完成一次实物摇晃与日志对应验证。同段还记录多次 loving 和一次 happy，分别对应触摸抚摸/点击检测链路，但用户未确认这些手势，暂不作为触摸阈值验收。该段未出现崩溃或 TE 超时，不能据此推断光学无撕裂。

## 后续想法（未实施）

- 根据麦克风音量/VAD 让 listening 眼神跟随声音强弱。
- 按本地时间调整闲时节奏（深夜更早犯困，早晨 `waking` 问候）。
