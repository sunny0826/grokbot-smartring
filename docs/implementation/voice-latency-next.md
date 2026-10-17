# 下一轮语音提速实施记录

状态：用户追加授权“部署并测试”；后端0.3.3及固件0.14.0已部署，真实公网自动回复测试通过；实体录音与上屏验证待用户操作。用户于2026-10-17在分析方案后明确“开始优化”。基线759f03c；保留此前未提交分析文件。不自动commit/push。

## 执行单元与ownership

- 协调者：主工作区firmware/grokbot/**、docs/**，以下timing worker范围除外；HTTP复用、后台等待首屏、编译与最终集成验收。
- backend worker：独立worktree `/tmp/my-bot-latency-work/backend`，分支`codex/voice-latency-backend`；唯一写入范围`voice_bridge/**`。实现有界服务连接、等待首屏、通知正文试验开关及其隔离测试。不得修改主工作区、部署或真实发消息。
- timing worker：独立worktree `/tmp/my-bot-latency-work/timing`，分支`codex/voice-latency-timing`；独占`main/voice_timing.*`、`main/voice_ui.cpp`、`main/voice.h`（仅增加snapshot request_id）、`components/esp_lvgl_adapter/src/display/bridge/v9/lvgl_bridge_v9.c`、`tests/voice_timing*`、`tools/check_voice_timing.py`（均相对firmware/grokbot）。其他CMake接线由协调者完成。实现无正文计时、显示成功边界及其测试，不部署。
- 独立设计／代码审查：只读冻结的协议／最终diff；回执交协调者。新任务新会话，继承宿主模型，非跨模型。
- 边界例外：无。后端停止写入并交付后由协调者回灌、测试和必要修正；交接前不得修改后端文件。
- transport worker：独立worktree `/tmp/my-bot-latency-work/transport`；独占 `firmware/grokbot/main/voice_http.*`、`voice_transport.*`、`voice_page_transport.cpp`、新增`voice_response_transport.cpp`、`tests/voice_http*`、`tools/check_voice_http.py`。主会话不写这些文件，负责mailbox调用和CMake接线。

## 最小协议（独立复核后冻结）

`GET /v1/recordings/{request_id}/response`，沿用身份认证和设备归属检查。请求头`X-Voice-After`为stored/transcribing/ready/claimed之一；状态相同且未终结时最多等1秒，状态变化立即响应。缺省则不等待；非法／重复字段返回400。

- 未回复：200 application/json，现有status结构。
- 已回复：200 application/octet-stream，原VPG1首屏，`X-Voice-Request-ID`为本次ID，`X-Voice-Read`为0/1。VPG1内含page_count与CRC，不新增第二套页面格式。不自动已读。
- 等待配额耗尽：立即返回当前JSON状态，不排队占满线程。
- 授权、错误、页面完整性规则沿用旧端点；旧服务404/405/501时固件退回status＋page路径。

连接仅对明确keep-alive且已认证的完整成功请求复用；持久连接最多8、每设备最多2，等待最多4；限制空闲及累计寿命，错误关闭。不得仅删除Connection:close。等待不持Store锁，服务关闭须唤醒等待并关闭空闲连接。

复用键除VoiceScope外须包含实际endpoint(host/port/TLS模式)、配置epoch和网络连接变化；当前VoiceScope刻意不含endpoint，不能直接把它当连接池键。凭据与端点变化后关闭旧session，不跨任务共享handle。页面与read状态须在同一Store快照中取出。

独立审查已完成：Condition在同一锁下检查／等待且提交后通知，避免丢唤醒；关闭返回503且断开连接。server_close必须唤醒并关闭持久socket。每请求重验认证，清理前次响应头及排除列表。固件再次检查结果ID/scope/generation/epoch；显示计时必须覆盖淡入后完整页面与最后成功DMA，失败flush_ready不得算完成。

固件复用连接按网络任务独占、凭据scope分隔；错误／网络变化丢弃连接，提交不自动重试。后台等待结果携带request_id、scope、generation、config_epoch，过期结果释放页面且不改变前台。读确认仍走原持久journal。

通知带正文开关默认关闭；仅发送已落库的完整短转写、版本和SHA256，不截断；超限保留ID路径。claim仍返回权威内容并是执行前提。纯问答免claim、分段回复、换模型不在本轮。

## 验证计划

先跑现有基线检查，再以真实loopback HTTPS测试连接复用、状态变化唤醒、超时、归属／认证、容量、丢ACK和shutdown；固件主机actor与真实LVGL检查响应过期、返回主页、设置、已读、页面展示完成。最后完整IDF编译。

真实松手→显示最后DMA完成须绑定页面代次且区分显示失败；旧日志没有该指标，不以stored→replied冒充端到端。若做性能对照，同输入每侧至少5次交错，说明范围、正确性、错误计数。无实机验证不得报告实体提速。

## 当前进展

- 已恢复前次方案并重新检索跨会话记录；核对当前源码版本。
- Lody完整目录已核对：6机器（4在线、2离线）、32配置、6角色；本轮沿用当前会话模型，不保存全局配置。
- 已集成后端、传输和计时三个独立worktree；主会话完成actor/CMake及既有测试适配。生产服务、固件尚未变更。

## 实现结果

设备通过后台mailbox任务等待状态变化；同次响应携带已校验首屏，直接展示，仍由用户确认已读。相同状态后有250ms的下一轮间隔，替代前台固定2秒轮询。关闭、设置、录音及迟到结果由请求ID、scope、generation、配置epoch和同一snapshot锁保护；结果检查与状态发布不可被新控制输入插入。

只为后台response保留一个TLS连接，其他上传、Inbox、Read、翻页仍使用短连接，控制内部堆占用。明确发送Connection: keep-alive；配置、网络断开、开始录音及设置暂停会使缓存失效。2.5秒未使用或连接创建满28秒时重建，为服务端30秒硬限留余量。仅已复用的只读GET遇到网络中断可重连一次；上传、鉴权和格式错误不重试。终态释放TLS，旧服务404/405/501改走原status＋page路径。

触摸松手在hold_end记录；本地journal保存失败后的重试保留首次松手时间。BOOT是按下沿触发发送，不能冒充松手，记录为未知。计时保留8个请求的无正文记录：上传开始、open完成、上传确认、回复可用、首屏CRC通过、首次完整显示。最后一项要求完整页面、淡入结束、无遮挡、当前页面代次以及最后成功DMA通知；时间是LVGL任务观察到DMA完成的时刻，不是光学屏幕扫描结束。

通知预送转写已实现，默认OFF；保留claim、权威正文及摘要核对。云端routine尚未更新，预送没有已验证收益。

## 验证证据

- 后端全量：133 passed，95.70秒；临时SQLite及loopback HTTPS，未访问生产。
- 实际actor、journal、mailbox＋真实LVGL：22个隔离进程通过；追加BOOT/存储重试计时2个进程通过，共24场景。线程均正常回收。
- 计时核心＋真实LVGL：8场景通过，覆盖遮挡、旧代次、淡入和首/中/末条失败；主机flush完成仅为受控边界，不是实体DMA证据。
- 实际HTTP客户端源码＋SDK mock：ASan/UBSan通过，覆盖连接复用、失效、旧服务回退、响应归属、完整度、CRC和一次重连。
- 原remote_core、voice_core回归通过。ESP-IDF 5.5.3完整构建通过，生成grokbot.bin，app分区剩余约11%。
- 独立审查先发现并修复：关闭/设置与响应竞争、BOOT假松手、缺keep-alive请求头、服务端寿命截断等待；最终复核又发现超长JSON误走重连，已改为先拒绝尺寸再判网络中断。

[后端审查](voice-latency-evidence/backend-review.md)、[显示修复前审查](voice-latency-evidence/display-review.md)、[传输修复前审查](voice-latency-evidence/transport-review.md)记录证据；[最终修复复核](voice-latency-evidence/final-review.md)确认未发现残留缺陷；回执中的文档待更新备注已在本记录补齐。各测试结果位于同目录。源码与测试命令均可在本仓库复跑。

## 交付边界

未提交、推送、部署、烧录或更新云端routine，未发送新的真实Bot请求。本轮没有松手到实体上屏的AB数据，不能给出已省秒数或稳定10秒承诺。主要云端调度/模型间隙仍取决于原Bot；转写预送须在部署后单独对照。持续response与已读确认共享单个网络worker，先前已在途的短请求仍须完成或超时；后台等待不会阻塞界面，但不保证零排队。现有底层DMA通知丢失后的无限等待未在本轮改变。

后续部署后按分析方案对相同录音逐侧至少5次交错；读新voice_timing日志核对完整首屏、失败/重试和内存余量。没有真实测量时只验功能及可观测性，不把理论减少请求当成测得的端到端提升。

复跑入口：`voice_bridge/.venv/bin/python -m pytest -q voice_bridge/tests`；`python3 firmware/grokbot/tools/check_voice_actor.py`；`python3 firmware/grokbot/tools/check_voice_timing.py`；`python3 firmware/grokbot/tools/check_voice_http.py --idf-path /Users/guoxudong/esp/esp-idf-v5.5.3`；`python3 firmware/grokbot/tools/check_remote_core.py`；`python3 firmware/grokbot/tools/check_voice_core.py --idf-path /Users/guoxudong/esp/esp-idf-v5.5.3`。载入该ESP-IDF环境后，在firmware/grokbot运行`idf.py build`。

## 2026-10-17 部署验收

用户追加明确授权部署并测试。数据库已在线一致备份并通过integrity_check；读出旧应用区，与上次0.13.0部署哈希一致。服务升级0.3.3；固件0.14.0-voice-latency只写应用区，烧录哈希校验通过，保留NVS身份/网络配置。设备USB回读新版本、Wi-Fi重连、时钟正常。

实际公网测试发现隧道精确白名单缺少`/response`，导致请求落入原MCP服务并返回401。已更新`tools/configure_tunnel.py`，支持原白名单安全迁移，保留MCP/OAuth/控制路由；新增迁移/幂等/未知配置保护测试，6项通过。备份并验证Cloudflare隧道配置后重启既有隧道。修复后局域网与公网分别连续3次真实HTTPS读取，同socket、首屏91408字节、ID/read头及CRC全部通过。

转写预送实验保持关闭，云端routine未修改。已完成设备状态采集及自动语音测试，尚无实体录音到首屏显示的延迟数据。[部署回执](voice-latency-evidence/deployment.json)。前文“未部署”描述的是实现阶段，已由本节更新。

自动全链路测试：合成4.387秒语音经公网上传，原Bot自动领取并回复“测试成功。”，首屏CRC通过，一次通知。从开始上传到Mac取得完整首屏26.970秒；其中存储音频→回复落库25.373秒、通知接受→领取12.069秒、领取→回复接收10.565秒。仅清理本次合成测试记录的未读标志，未更改既有消息。该测试不含实体按住说话/上屏，也不是新旧交错AB；不能据此宣称提速。见[自动回复结果](voice-latency-evidence/synthetic-result.json)及[分段计时](voice-latency-evidence/synthetic-timing.json)。

设备烧录后连续采集240秒、114次状态：版本均正确，未捕获显示错误，内部堆最低31811字节。13次采样出现`wifi-down`，分为两段，错误为`response_timeout`或`wss_connect_failed`，随后均恢复，末次连接正常且错误为空。该link字段反映WSS传输状态，不能据此断言无线接入点断开；目前未定位两次中断的根因，不能称全程连接稳定。未观察到实体录音或voice_timing阶段事件；已提示用户触摸录音，当前仍待操作。[设备观察](voice-latency-evidence/device-observation.json)仅证明这段空闲/后台查询状态，不能替代前台录音与TLS并发验证。
