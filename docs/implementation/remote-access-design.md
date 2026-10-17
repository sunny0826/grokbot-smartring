# 跨网连接家中 Mac

2026-10-17。基于 `183a400` 加当前已验收的未提交语音实现；初始固件
`0.6.0-touch-voice`、语音信箱 `0.2.0`、原网关 `0.1.0a14`。
用户选择在其他 Wi-Fi／手机热点使用圆屏，继续由家中 Mac 提供后台。
此文件先记录设计，实施结果另见验收记录；不把方案写成已通过测试。

## 使用方式与范围

- 保留现有 Wi-Fi、设备 token、LAN 网关和私有 CA。
- USB 设置一个可选 `remote_host` 后，控制和语音都使用该域名的公网 443。
- 清空 `remote_host` 恢复原 LAN 地址；一次只使用一种模式。
- 切换 Wi-Fi 使用独立 USB 命令，只更新 SSID 和密码，不要求重贴设备 token／CA。
- 原 Grok OAuth 连接、工具、语音信箱和手动触发处理方式保留。
- 本轮不增加自动漫游、多 Wi-Fi 列表、开机自启、云端转写或自动唤醒 Grok。

## 结构与归属

```text
Grok ── 原 HTTPS/OAuth MCP ── Cloudflare ── Mac 127.0.0.1:8766
圆屏 ── WSS /v1/control ──── Cloudflare ── Mac relay 127.0.0.1:8770
                                                 └── 原设备 TCP 127.0.0.1:8764
圆屏 ── HTTPS /v1/inbox、/v1/recordings/... ── Cloudflare ── 原语音 HTTPS 8768
```

复用现有公网 hostname，在原 MCP 路由前插入两条精确路径规则。不修改 OAuth
注册或 DNS。设备控制适配器为独立进程，只拥有 socket 生命周期；一条 WebSocket
对应一条原网关 TCP。网关仍拥有设备会话、命令和回执，语音进程仍独占 SQLite、
Whisper 和软件信箱。适配器不代理 WAV，不保存命令，不替离线设备轮询。

控制 Upgrade 先验证设备身份；首帧是绑定同一身份的 hello，再建立上游 TCP，
避免公网握手消耗网关的 hello 时限。每条 binary message 包含一条 LF 结尾 JSON。
拒绝多行、文本、超长帧和后续 hello；连接数、消息队列、速率及等待时间有界。
关闭或错误必须释放对应上游连接。重连重新 hello，不重播不确定是否执行的命令。

固件复用 IDF 的 `esp_transport_ws` 和 `esp_transport_ssl`，继续向应用提供原有
LF-JSON read/write。WS 的部分写不能像 TCP 一样重发余量；必须关闭该连接。
SSL parent 与 WS wrapper 均有明确释放责任。响应超时后结束旧流，防止迟到响应
被当成下一次请求的结果。控制 socket 重连只重开 socket，不重启已连通 Wi-Fi，
从而不打断独立的录音 HTTPS 上传。

公网 WSS 与 HTTPS 共享模式、域名、公共证书 bundle 和时间就绪条件，始终验证
主机名、链和有效期，不沿用 LAN 跳过主机名的配置。语音路径仍使用相同请求 ID、
持久化与重试规则。所有语音响应加 `Cache-Control: no-store`，部署时核验公网缓存。
Cloudflare 到语音源站也用原私有 CA 验证，不能设置 `noTLSVerify`。

## 方案比较与裁决

两个独立候选和一个独立评判均使用宿主默认模型；这不是跨模型实验。候选首轮
未互读，评判在两者完成后读取完整方案，主会话核对了全部原文与关键源码。

共同标准：保留原功能；外部 Wi-Fi 可达；认证/TLS/资源边界；状态与重试正确；
接口与回退维护成本；能用真实网关、公网和实体设备分层证明结果。

选候选 B 的 WSS 薄桥、独立 relay 与显式模式作为基础。结构不同的备选是
HTTPS exchange：Mac 维护会话句柄、序号、上游连接、响应缓存和回收。它能复用
HTTP 客户端，但 poll 响应丢失、重复 open 和并发请求都需新增状态约束；现有协议
本来就是长连接，没有证据要求承担这些额外状态，因此不实施此备选。

吸收候选 A 的两项具体约束：先构建测 Flash 再决定是否改分区；明确低层 WS
输入缓冲可写、部分写失败和 parent 释放。拒绝 A 的自动路由、多网络与整体 NVS
迁移，它们超出本轮目标。分区先测量再决定：初始 app 1,468,944 字节，1500 KiB
分区余 67,056 字节。最初从 map 对象名推断公共 bundle 已链接，此推断经二进制
核验被否定；新增 bundle 实占 68,987 字节，最终 app 1,556,464 字节已超过旧分区。
实查芯片为 16 MB，并备份原 Flash 后，将 factory 应用分区扩大至 2 MiB；NVS、phy
地址及大小保持原值。新分区余 540,688 字节。

## 时间边界

现有固件未启用证书日期校验。本轮启用全局日期检查，Wi-Fi 获得地址后 SNTP 校时，
控制与语音在时间就绪后才建立 TLS。失败时保持 Wi-Fi，明确报告时间不可用并重试。
不把构建时间或旧 NVS 时间冒充当前时间，也不关闭公网有效期检查。

因此，无有效时钟且所有校时源都不可达的冷启动，LAN 也暂不可连接；已有有效时钟
时的 LAN 回退保留。本轮目标是有互联网的外部 Wi-Fi，不扩展完全离线时间引导。
SNTP 不是认证时间协议，不能据此宣称已抵御恶意时间源。

## 验证顺序

1. 隔离真实网关的 WS 注册、命令、回执、身份拒绝、撤销、断线及资源限制。
2. 固件端点、帧和超时行为检查；构建大小、并发 TLS 与原语音测试。
3. 离线验证 Cloudflare 精确路由、原 MCP/OAuth 路由保持和私有源站证书。
4. 部署后验证公网认证、真实设备 WSS、控制与语音；同一 LAN 经公网域名只算公网路径测试。
5. 用户把圆屏连到外部 Wi-Fi／手机热点，确认真实 Grok 动作及触摸语音闭环，才标记跨网实体通过。

依据：[Cloudflare 协议](https://developers.cloudflare.com/tunnel/concepts/routing/)、
[WebSocket 支持及断线](https://developers.cloudflare.com/network/websockets/)、
[本地管理的路径路由](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/local-management/configuration-file/)。
