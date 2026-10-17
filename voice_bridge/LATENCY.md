# 有界响应等待与连接复用

2026-10-17 已部署后端0.3.3、固件0.14.0及公网隧道新路由，合成语音经原Bot自动回复与首屏下载通过。云端 routine 未更新，转写预送保持关闭；实体松手到上屏和新旧性能对照尚未验证。详见[部署记录](../docs/implementation/voice-latency-next.md)。

设备可以请求 `GET /v1/recordings/{request_id}/response`，沿用设备身份和令牌校验。缺省立即返回；单个 `X-Voice-After` 可为 `stored`、`transcribing`、`ready`、`claimed`。状态相同则最多等 1 秒，变化后立即返回。非法值或重复头返回 400。每设备最多 1 个等待、全局最多 4 个；配额满立即返回当前状态，不排队。

未回复时返回原状态 JSON。已回复时返回原 VPG1 第 0 页，并加 `X-Voice-Request-ID` 和 `X-Voice-Read: 0|1`；页面、状态和 read 从同一锁保护的快照读取。获取页面不标记已读，仍需原 `/read` 确认。旧状态与分页端点保持可用。

只有明确发送单个 `Connection: keep-alive`、通过本次认证、完整接收请求且得到成功响应的连接才可复用。全局最多 8 个，每设备最多 2 个；总处理线程仍为 16。空闲 3 秒、持久寿命 30 秒或第 64 次请求后关闭；错误关闭，后续请求重新校验认证及撤销状态。配额满时本次成功请求照常响应，但关闭连接。停止服务会唤醒等待者返回 503，并关闭空闲连接和未完成的 TLS 连接。

# 通知转写实验

`--grok-trigger-transcript` 默认关闭，且必须同时提供原有 `--grok-trigger`。默认通知仍只有 `event`、`version`、`request_id`、`mailbox_id`。

启用后，仅把已落库且完整的最多 800 Unicode 字符、3200 UTF-8 字节转写加入通知：`transcript`、`transcript_version: 1`、`transcript_sha256`。摘要按未经再次规范化的原始转写 UTF-8 计算，与转写同一事务持久化；转写不可被不同文本覆盖。长文不截断，自动保留原 ID 路径。旧记录迁移会计算相同摘要。重试仍发送相同内容，不发送 claim_id。

claim 和 read 回执也带相同版本及摘要；旧 claim 的重试回执会补齐这两个字段。预读不授予执行权限：原 Bot 必须成功 claim 后才能执行，以 claim 的关联回执正文为准。字段缺失、版本未知或摘要不符时丢弃预读，继续权威正文路径。`grok-routine.txt` 与 `grok-routine-update.txt` 已更新为此契约；更新云端 routine 属于后续部署工作。

验证只使用合成输入、临时 SQLite 和 loopback HTTPS。没有对生产时延作比较。
