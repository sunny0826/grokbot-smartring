# 异步已读独立设计评判

结论：**选 A 为完整基础方案，有条件通过；吸收 B 的 16 条容量上限和页面来源 scope 绑定。使用有界 header，不使用 query，不引入数据库 UUID 迁移。** 两份方案均不能原样视为已验收实现；以下调整是实施、验收的必要条件。

本评判由当前继承配置自身完成，没有再分工，也不构成跨模型评审。完整阅读 A、B，并只读核对当前源码；HEAD 为 `0b608293ce121be869110a5aa0f571bc65c4ddc7`，以当前未提交工作区为设计对象。未运行产品、测试或设备操作。唯一写入为本报告。

## 选择依据与取舍

| 事项 | 裁决与依据 |
|---|---|
| actor + mailbox worker | 采用 A §Chosen structure：actor 独占前台状态和 journal 写入；一个 worker 只执行 Read、Idle Inbox。`voice.cpp:207–215` 同步 Read，`:342–352` 同步 Inbox，二者都必须移走。只先显示 Idle 不满足下一次录音可立即响应。 |
| 工作交接 | 采用 A 的不可变 work item + 一项可靠 completion；actor 在处理输入后安排下一项。比 B“最新工作快照 + worker 自主调度”更容易明确哪一代结果可应用，避免重复权威状态。worker 等结果空间时不得持有 gate/快照锁；HTTP 已销毁后即可释放网络 lane，不要把等 actor 消费结果算作在用网络。 |
| 持久提交 | 采用 A §Durable transition：一个版本化 blob 原子表达 active pending 与 read receipts；提交后才关页，不再调用旧 `close()` 的第二次清 journal。`voice.cpp:58–77,89–105` 证明当前 pending/关闭路径必须一并迁移。 |
| 容量 | 吸收 B:21、33 的 16 条上限。没有证据要求 32；16 是本轮资源预算选择，不是计算出的业务最低值。至少两条才能承载多个未确认已读，实际允许多少离线已读没有流量依据。第 17 条须明确失败并保留原页，不静默驱逐。 |
| 排除协议 | 采用 A:106–112 的 `X-Voice-Read-Exclude`，缩到 16 项。B:45–51 的 query 需改变精确路由、扩 `path[128]`/`url[256]`，旧服务还会 404；header 保持 URL 和响应结构，旧服务自然忽略，改动更窄。 |
| scope | 调整两方案的默认严格 endpoint 绑定，采用下节明确的 enrollment 假设；不把“切路由必须新增 UUID”作为本轮阻断。 |
| 验证 | 采用 A:143 的真实 LVGL → actor 链路，并吸收 B:85 的真实入口 barrier 验证。B:90 将旧 UI smoke 与 actor 测试分开报告是诚实边界，但两组独立测试仍不能替代一次真正贯通。 |

拒绝：只在原 worker 延后 Read；为本任务搬迁 Upload/Status/Page 到通用 transport actor；32 条默认扩容；query 协议；持久化重复的 hidden/sent 标志；把未升级后端的降级行为说成多消息功能已完成。

## 实施前必须明确的调整

### 1. 本轮身份以 enrollment 为约束，不声称识别任意数据库

推荐 journal 的 scope 为版本化、长度明确编码的 `SHA-256(device ID, enrollment token, 保存的 LAN CA)`，不含 SSID、密码、LAN 地址、remote_host 或当前 route。使用保存的 LAN CA，不随 remote 模式改成系统 CA。这保留 A:85 已允许的较小方案，并吸收 B:65 的要求：页面/请求发现时捕获 scope；点击时不能用新配置给旧页面重新贴身份。

理由：当前 `GadgetWifiCred` 明确持有 enrollment token 与 LAN CA（`gadget_wifi.h:12–19`）；remote 仅切换目标（`gadget_endpoint.h:43–46`、`gadget.cpp:411–415`）。项目既有操作说明明确切换保留设备身份、LAN CA 和 token，并通过同一 Mac 服务提供信箱（`docs/remote-access.md:35–42,61–68,71–83`）。因此本轮可以延续“同一 enrollment 的 LAN/公网配置指向同一权威信箱”的部署约束，避免在普通换路由时主动让已读消息复活。

**这是一项部署假设，不是协议证明。** 后端 `config.py:61–65` 只校验设备/token，未证明数据库身份。复用相同 token/CA 指向另一份库、重建/回滚库、任意修改公网目标，都超出该保证：可能误过滤碰巧相同 ID，或向另一库重放 Read。运营上应给不同信箱换 enrollment；不能宣传 LAN/public 任意地址同库。若产品要求自动识别任意重配置/数据库切换，才需要稳定且经过验证的 mailbox identity；此项留到独立变更。

token/CA 变化会隔离旧回执；保持它们、只切换受信任的同库路由可继续同步。CA 轮换即使实际同库也可能隔离旧条目，这是剩余限制。旧 scope 仍占 16 条总容量，不自动删除。factory reset 目前只擦 grok namespace（`gadget_config.cpp:150–157`），不能假定 voice journal 已被清空。旧 pending 没有 scope，首次迁移只能沿用当前 enrollment 恢复假设，必须在报告中承认此边界。

同时保留独立 runtime config epoch：scope 不变也不等于网络配置没变。gate 内取得网络 reservation 后，捕获/核对一次 credentials+scope+epoch；整个请求使用这份一致快照。不能先检查 scope，再让 `VoiceHttp` 无约束重读配置（当前读取发生于 `voice_http.cpp:29–34`）。

### 2. header 与 16 条容量要落实到最坏长度

16 个 32-hex ID 的逗号列表最多 **527 字节**，含 NUL 的值缓冲为 528；header 本身约 551 字节。再叠加最大 256 字符 token、127 字符主机及默认请求头，现有 1024 TX 不能据此保证足够（`voice_http.cpp:35–58`、`gadget_wifi.h:15–18`）。建议只给 Inbox 配 2048 TX，并检查 header 设置结果及实际发送；不全局扩大页面、上传路径或 URL 缓冲。

服务端只在认证成功的 GET `/v1/inbox` 消费该 header；无 header 保持原行为；拒绝重复 header、畸形值和超过 16 项，统一小写、规定重复 ID 的去重语义。SQL 仍限定 device/status/read，并用绑定参数排除 ID（`server.py:99–151`、`store.py:325–329`）。排除不写 read 状态；Read 保留现有归属校验与幂等性（`store.py:339–347`）。无 schema 迁移。

16 × (32-byte scope + 16-byte ID) = 768 字节，active 项与编码头仍可保持约 1KB。NVS 总分区仅 24KiB（`partitions.csv:2`），净 payload 小不等于真实更新一定成功：需计入 CA、其他 namespace、blob 元数据和写入/回收余量。不能以这个算式代替满配置 NVS 更新与断电验证。

### 3. 明确事件失效与 lane 完成时序

Read completion 只能删除它捕获的 `(scope,id)`，删除持久化失败则继续保留；不因前台 generation 改变就丢成功，也不能改变新录音的 request ID、active pending、页面或 phase。读回执删除也应推进 journal revision。Inbox 必须同时满足当前 Idle、scope/config epoch 正确、与发起时兼容的前台世代及 journal revision，并再检查当前本地 receipt 集合；否则丢弃、重排 poll。必须覆盖“旧 A 响应在 A receipt 已删除后才消费”，不能只判断此刻队列里有没有 A。

gate 改成 foreground/mailbox 两个独立槽和一个 settings owner。当前 `voice_settings.h:20–25,40–47` 的单槽/无参数 finish 不可继续用于两个 worker。检查与 reserve 在同一短锁内，finish 精确释放所属槽；设置 Pending 后不得开始新网络请求；两个槽均释放且输入/hold 安全才可 Paused。普通输入不受 mailbox 槽限制（公开入口见 `voice.cpp:377–383,414–432`）。

A:91 的“每个 polling interval 至少一次 Inbox”需改成**调度公平性**：一个串行 worker 遇到慢 Read，不可能保证每 5 秒发 Inbox。以“每次完成/超时后让到期 Inbox 获得机会，失败回执轮转且有退避”为可实施承诺。远端 15000ms 是请求配置下限，不能写成完整 HTTP 的严格最长时间（`voice_http.cpp:50`）。录音不等 HTTP 才是本轮硬要求；下一消息发现允许受这一个后台请求影响。

录音期间优先 capture，后台确认可暂留可靠 completion，延后非必要 journal 删除提交，避免 flash 写入影响音频。不能通过全局 HTTP mutex 解决资源压力；需核对并发 TLS 内存，必要时推迟新后台请求。

### 4. 存储错误 UI 是两方案都漏写的具体改动

两方案都要求本地提交失败留在阅读页且提示。现有 `voice_ui.cpp:274–307` 在 ViewingReply 隐藏 hint，正常图片分支不展示 `drawn.error`；仅设置 error 字段并不会让用户看到失败。须增加阅读页内可见且不遮住按钮的本地保存/容量错误，保留图片、末页已读按钮及重试路径。`voice_ui_update():341–344` 已检测 error 改变，可利用现有刷新机制。

删除 MarkingRead 的枚举、动画和 switch 引用，保留 LoadingPage；不能把所有 loading 分支一起删（`voice_ui.cpp:263–267`）。网络失败只留在后台，不触发 ReplyFailed 或新同步文案。新 blob 损坏不能作为“没找到”退回旧 pending；提交结果不确定要单独处理。A 的 readback 可区分运行时读到的新旧值，但未核验具体 NVS 持久化契约前，不可把“读回相同”直接宣传为断电安全证明。

## 可实施边界

- `voice_journal.{h,cpp}`：版本化编码、16 项回执、旧 pending 迁移、明确保存结果；actor 为唯一写者。所有 send/close/read-completion 更新都保留其余 receipts，不只替换 MarkRead 一处。
- `voice_mailbox.{h,cpp}`：一个后台任务，有限 work/completion，独立 HTTP 生命周期；不写 UI/NVS，不借用 actor 可变字符串。
- `voice.cpp`：末页本地提交→清页面→Idle；消费精确匹配的结果，安排公平重试与 Inbox。保持单一前台状态，抽走原 journal/Idle HTTP 后控制在 500 行偏好内。
- `voice_settings.h`、transport/HTTP：两个 lane；同次凭据快照；仅 Inbox 的 exclude header 和容量扩展。Upload/Status/Page 保留现有控制流程。
- backend Handler/Store：解析有界 header + 参数化排除；不改 DB schema。UI/character/logic 仅迁移 MarkingRead 和本地保存失败显示。

## 验收必选项与部署关口

1. **真实 actor + 因果 barrier**：分别卡住 Read、Idle Inbox；真实末页 MarkRead 后已到 Idle，HTTP 仍未放行时 BOOT 和 hold 都能进入 Recording。同一检查在基线应暴露阻塞。不用 reducer 模型或短 sleep 代替。
2. **真实 LVGL 入口**：发 PRESSED/CLICKED 到末页按钮，经生产 `voice_action` 和 actor 回到首页；断言无 MarkingRead、无同步提示，LoadingPage/头像未读入口仍正常。旧 smoke 的 `voice_action` 是桩（`tools/voice_ui_smoke/smoke.cpp:33`），不能据其通过宣称异步完成。验证本地写失败/满队列时错误确实可见、原页仍可重试。
3. **持久边界**：enqueue 前/后、回首页前、服务端成功响应丢失、删除失败/完成后分别重启；测试 legacy 双键、损坏 blob、容量 16/17、重复点击。已本地确认的 A 不恢复为 active；保留的回执可幂等重试。
4. **交错与设置**：Read(A) 返回时已录/上传 B，B 不受影响；过期 Inbox(A) 在 enqueue、确认删除、切配置后消费都不能复活；两个 lane 各自 finish、Pending/cancel/重新申请不误放行；排队输入/hold 不被后台轮询吞掉。
5. **真实 HTTPS Handler**：A/B/C，Read(A) 持续失败而 Inbox 可用时，exclude A 可发现 B；“稍后”不标 B；重复/畸形/超限 header、设备隔离、无 header 旧客户端都验证。新固件对忽略 header 的旧服务本地仍过滤 A，允许 B 等待 A 同步，不能显示错误页或把 A 重新亮为未读。
6. **部署与实机**：正式功能验收先升级后端，再升级固件，验证 LAN 与实际公网代理都透传 header、连到预期同一库；不是仅看版本号。跑既有 core/UI/Wi-Fi/remote/character/backend 回归及固件构建，核对文件长度；实机量 click→Idle、本地 NVS 延迟、HTTP 卡住时录音启动、并发内存/录音连续性和断电恢复。本评判未执行这些步骤。

历史核对：本轮已完成一次定向 Nowledge 搜索 `my-bot 已读 异步 信箱`，命中 `13a6a239-263c-487a-8a57-a83f40fd45a1` 的显式已读/持久信箱记录；只用于确认没有历史强制同步 UI 要求，不作为本设计已经成功的证据。

回执：独立评判已完成；推荐可实施，但必须落实上述具体调整并通过真实入口与持久/并发验证后才能报告功能完成。报告写入结束。
