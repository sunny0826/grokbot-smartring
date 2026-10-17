# MCP 语音接入契约

2026-10-17，实施依据。默认模型的两个独立候选和独立评判选择软件信箱方案：
保留现有 OAuth `/mcp`、六个工具及运行中的网关；语音进程作为真实软件设备连接
loopback 8764。SQLite 由该进程独占。公开名称明确为「SmartRing voice mailbox (Mac)」。

## 使用过程

屏幕按住录音、松手上传（滑出取消） → Mac 转写 → 等待 Grok 领取 → 在已有 Bot 说「处理圆屏语音」
→ Bot 领取并返回短回复 → 圆屏查看、翻页、标记已读。自动唤醒尚无已验证接口，
此实现不承诺按键直接启动 Bot 推理，不新增 webhook 或另一模型助手。

## 软件信箱

独立 ID `smartring-voice-mailbox`，独立设备凭据。`state` 只有待处理/未读数量和
最多 5 条请求摘要（request_id、device_id、status、简短 preview），不暴露 claim_id。
真实请求不使用现有 128 条事件缓存保存。

通过 `gadgets_command` 使用以下 capability；按返回 command 的 `reported_state.result`
读取结果，未完成时查询 `gadgets_command_status`，不读取会被别人覆盖的最新 state。

- `voice.claim(request_id, claim_id)`：两 ID 均为 32 位小写 hex，ready → claimed；
  同 request/claim 重试返回相同业务结果，其他 claim 冲突，claim_id 不跨请求复用。
- `voice.read(request_id, offset=0)`：读状态及转写的一页，最多 800 Unicode 码点；
  返回 next_offset（结束时 null）。通常 claim 的第一页已经包含全部转写。
- `voice.reply(request_id, claim_id, text)`：最多 128 Unicode 码点的非空纯文字；
  同内容重试幂等，不同内容冲突。首次提交原子保存文字与所有页面后才报告 replied。

结果始终携带 request_id；claim_id 由调用者保留，回执不回传它，避免网关把 ack.state
同步为最新设备状态时暴露领取令牌。命令结果也可能短暂出现在最新 state 中，因此
调用者必须按 command_id 读取对应回执，不能将最新 state 当成自己的结果。
claim_id 和不可变 reply 是业务幂等键，独立于网关短期 command_id。领取没有自动
到期重领，避免把含外部操作的任务再次执行。明确失败与不确定结果需在 Bot 中核对。
重复 claim 返回 duplicate=true 及当前状态；已 replied 时同时返回原回复，不能再次执行。
默认每 token 每分钟 10 次 command，与物理设备控制共享；正常一轮只需 claim + reply。
128 个非 BMP 字符加固定长度 ID 的完整 poll 帧实测 1772B，小于 2048B。

## 服务与设备协议

沿用设备凭据的 HTTPS 8768；所有接口按 X-Device-ID 限定归属。

- PUT /v1/recordings/{id}：原 WAV 上传，落盘后确认。
- GET /v1/recordings/{id}：`{request_id,status,page_count,read,error?}`，小于 1KiB。
- POST /v1/recordings/{id}/retry：仅本地转写失败可重试。
- GET /v1/inbox：最老未读回复的同形状态；没有时 `{request_id:"",status:"empty",page_count:0,read:false}`。
- GET /v1/recordings/{id}/pages/{index}：不可变二进制页面，0 起始。
- POST /v1/recordings/{id}/read：用户明确标记已读，幂等；下载不等于已读。

任务状态：stored → transcribing → ready → claimed → replied；本地失败为 failed。
旧 needs_configuration 且已有 transcript 迁为 ready；旧 submitted/dispatching/unconfirmed
迁为 needs_review，不自动执行。read 为独立布尔值，只有用户动作更新。当前不声称
保存了物理显示回执；页面下载成功也不升级为「用户已阅读」。

页面为 272×168 原生 RGB565 little-endian，长度 91392B；前置 16B 头：
`<4sHHHHI` = magic `VPG1`、width、height、index、count、payload CRC32。
Mac 用固定 Noto Sans CJK SC 22px 排版，最多 8 页，不静默截断。缺字替换为 □ 并
在回复回执返回 warning。渲染页面在回复事务中持久化，重启不改变分页。

2026-10-17 用户改选触摸入口：手势有独立 ID，松手/取消不受状态代次或命令队列挤满影响；
录音准备、录音、满 30 秒等待松手分别呈现，滑出取消后不恢复该次录音。
设备保留录音与重试逻辑；ready/claimed 显示真实等待状态。回复就绪可查看；
返回不标记已读，主页显示未读入口，启动从服务端信箱恢复。录音期间不被回复抢占。
当前 UI 只内嵌固定文案小字库，内容页面不扩 Flash 分区。页面 actor 缓冲与 LVGL
显示缓冲分属各自线程，切换经锁/版本校验，不能把释放后的指针留给 LVGL。

## 分工与验收

公共契约先固定。后端拥有 Store/转写/HTTP/页面渲染及对应测试；固件拥有 firmware/grokbot
下语音/UI/字体与测试；主 Agent 拥有软件设备适配器、CLI/依赖、端到端验收和文档。
无并行 SQLite 写进程；部署及注册凭据由主 Agent 串行操作，保留旧网关及 OAuth。

先用隔离真实网关和 MCP 客户端验证发现、claim/reply、冲突、重启、最大帧；再验
中文页面、真机状态与显示。真实 Bot 领取、实体录音和物理目视分别记录，不能以
MCP 客户端替代 Bot、以编译替代显示。设计选择不构成 commit/push 或外部聊天授权。
