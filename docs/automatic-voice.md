# 让原 Grok Bot 自动回复圆屏

配置后：按住说话 → 松手上传 → Mac 转写 → webhook 唤醒原 Bot → 原 MCP 领取、回复 → 圆屏查看。
不需要每条语音再发送「处理圆屏语音」，也不需要换模型、添加第二个 MCP 或重烧固件。

本地实现已提供；必须完成下列 Bot 配置和真实验证，才能认为你的设备已启用。

## 1. 给原 Bot 创建一次例行任务

在项目根目录打印设置消息：

```sh
uv run --project voice_bridge voice-bridge --print-grok-routine
```

将输出发给已连接 grok-gadgets 的原 Bot。它创建「圆屏语音自动回复」，
仅由 webhook 触发，不定时检查空信箱。完整指令在
[grok-routine.txt](../voice_bridge/src/voice_bridge/grok-routine.txt)。

打开 Bot 名称 → 详情 → 例行任务 → 圆屏语音自动回复。
确认任务属于原 Bot、已启用、触发方式为 webhook。
Webhook 区域的 `POST to`（中文「POST 到」）是 URL，`key`（中文「密钥」）是 Bearer 密钥。
若点击任务只能停留在列表，请让原 Bot 提供「直接定位这个任务 webhook-url 的应用内链接」，
再点它返回的 `Webhook URL` 按钮。Grok Bot 0.71.2 的旧版列表入口有此差异；
本次已通过该直接入口打开详情，不需要修改任务或凭据。
不要把 key 贴到聊天、仓库或命令参数中。

这是 [Grok Bot routine 接口](https://cursor.com/help/grok-bot/routines)，
返回 200 表示启动。它与另一套 Grok automation 的 HMAC 签名／202 接口不同。

## 2. 私密保存入口

```sh
uv run --project voice_bridge voice-bridge \
  --configure-grok-trigger ~/.config/grok-gadgets/voice-trigger/trigger.json
```

按提示分别粘贴 URL 和 key；输入不回显。文件权限为 0600，目录为 0700。
此命令只保存配置，不发送请求。复制 `key` 字段本身，不包含 `Bearer ` 前缀。
配置格式为 `{"url":"https://…/…","key":"…"}`，URL 不允许凭据、查询参数或重定向。

## 3. 重启现有语音服务并启用

停止当前 voice-bridge 进程后，用原来的 host、证书、data 等参数启动，并加：

```sh
uv run --project voice_bridge --extra transcription voice-bridge \
  --host YOUR_PRIVATE_LAN_IP \
  --grok-trigger ~/.config/grok-gadgets/voice-trigger/trigger.json
```

保留原网关和隧道。启动第二个语音服务会被存储锁或端口拒绝。
Mac 必须保持唤醒；此改动未安装开机自启。

默认只自动通知启用之后转写好的录音，以及此前已入通知队列但尚未领取的录音。
旧的 ready 录音需要显式加 `--activate-existing` 才纳入，已 claimed/replied 永远不会重新执行。
不带 `--grok-trigger` 启动即关闭自动通知，原手动 MCP 流程仍可用。

## 4. 验证一次

1. 在圆屏录一句「这是自动回复测试，请回我收到」。
2. 不在 Grok 聊天中发送处理指令。
3. 确认原 Bot 自动运行；通过 MCP 看到同一个 request_id 从 ready 到 claimed、replied。
4. 在圆屏打开回复并确认文字。仅网页请求返回 200 不能证明这一步。

Webhook 只传请求 ID、信箱 ID、事件名与版本。录音和转写继续通过现有信箱传递。
新运行生成新 claim_id，只有原子领取成功才处理；重复通知可能启动多个运行，
但不能让它们接管同一条已领取语音。

## 故障恢复

`gadgets_get_state` 的 `activation` 返回 enabled、error、全局 not_before 时间和各待通知阶段的数量。
设备录音状态接口在 ready 时也返回 activation，但旧固件仍显示「等待 Grok 领取」。

- 200 后或发送结果不确定时，先等至少 120 秒让 Bot 领取；仍 ready 才退避重发。
- 429、5xx 使用全局退避，遵循最多一小时的 Retry-After，重启后仍生效。
  所有请求合计最多每 15 秒发送一次。
- 每条录音最多自动尝试 6 次，耗尽后仍保留 ready，可手动领取。
- 非 200 且非 429/5xx（含 401/403、重定向、错误接口的 202）持久暂停发送器。
  修好 URL/key 或任务状态后，重启时加 `--retry-activation` 恢复 blocked 通知。
- Bot 已领取但未回复时，不自动解锁重做。先检查原运行记录，避免重复执行外部动作。
- 官方已记录 routine 内部限流可能使运行失败；HTTP 200 后的领取观察负责区分这一情况。

本地进程崩溃后，保留发送前写下的领取宽限期，再核对 ready 状态恢复。
不会把发送成功写成回复完成。停止发送器不会撤回已经启动的 Bot 运行。

隔离验证命令：

```sh
uv run --project voice_bridge --extra transcription pytest -q voice_bridge/tests
```

测试覆盖真实本地 HTTPS 上传、触发接收和原网关 MCP 领取／回复／页面下载，
其中触发接收方是测试服务；不能据此宣称云端 Grok 已配置或实体屏幕已显示。

## 已有例行任务的延迟优化

2026-10-17 的本地指令模板已改为正常路径直接 `voice.claim`，
优先 `voice.reply` 后再补充聊天说明，保留领取和外部动作进度规则。
**仅修改本地模板不会更新已经创建的云端任务。**
本设备现有任务已通过原 Bot 语音入口应用
[更新指令](../voice_bridge/src/voice_bridge/grok-routine-update.txt)，Bot 确认保存；
后续五次真实请求的网关记录均为 `voice.claim → voice.reply`，无前置列表/状态查询。
详见[实测与边界](implementation/overall-throughput.md)。
不要重建任务或更换现有 webhook URL/key。
