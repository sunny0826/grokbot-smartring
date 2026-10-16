# Grok Bot 接入指南（grok-gadgets 协议 0.1.0）

> 2026-10-16 — SmartRing-Plus（ESP32-S3）通过 grok-gadgets-gateway 接入 Grok Bot 的
> 完整部署与运维说明。全链路已验证：Grok Bot 远程命令 executed、物理事件实时到达网关。

文中的 `gadget.example.com`、`TUNNEL_ID`、`DEVICE_ID`、`GATEWAY_LAN_IP`
和 `/dev/cu.usbmodemXXXX` 均为示例占位符，使用前替换为本机配置。
密码、令牌、证书和运行日志仅保存在本机，不提交到仓库。

## 架构

```
Grok Bot (xAI app)
  │  HTTPS + OAuth
  ▼
gadget.example.com          ← Cloudflare 命名隧道（固定域名）
  │  tunnel id TUNNEL_ID
  ▼
grok-gadgets-gateway serve         ← 本地 MCP 服务 127.0.0.1:8766，OAuth 保护
  │  设备监听 127.0.0.1:8764
  ▼
usb-bridge                         ← 串口 ↔ TCP 桥（同步请求-响应转发）
  │  /dev/cu.usbmodemXXXX
  ▼
ESP32-S3 固件（firmware/grokbot）  ← gadget 协议 0.1.0 客户端
```

## 组件与位置

| 组件 | 位置 | 说明 |
|---|---|---|
| 固件 | `firmware/grokbot/main/gadget.{h,cpp}` | 协议客户端：hello/state/event/ack，cJSON + USB-Serial/JTAG |
| 网关 | `.venv-grok/`（grok-gadgets-gateway 0.1.0a14） | 第三方项目 github.com/adidshaft/grok-gadgets-gateway |
| 凭据 | `~/.config/grok-gadgets/` | `smartring.token`（设备令牌）、`mcp-token`（本地 bearer）、`credentials.json`、`oauth/` |
| 隧道配置 | `~/.cloudflared/` | `cert.pem`、`config.yml`、`<tunnel-id>.json` |
| 请求日志 | `~/grok-gadgets-requests.jsonl` | 所有工具调用审计（remote 字段区分本地/远程） |

## 启动顺序（重启电脑后按此拉起）

```bash
cd ~/codes/my-bot

# 1. 网关（注意 NO_PROXY 必须，否则本机代理会导致依赖崩溃）
NO_PROXY="localhost,127.0.0.1,::1" nohup .venv-grok/bin/grok-gadgets-gateway serve \
  --simulator \
  --credentials ~/.config/grok-gadgets/credentials.json \
  --device-port 8764 \
  --allowed-host gadget.example.com \
  --oauth \
  --request-log ~/grok-gadgets-requests.jsonl \
  > /tmp/ggg-serve.log 2>&1 &

# 2. 命名隧道（读 ~/.cloudflared/config.yml）
nohup cloudflared tunnel run grok-gadgets > /tmp/ggg-tunnel.log 2>&1 &

# 3. USB 桥接（先 ls /dev/cu.usbmodem* 确认节点名——设备复位后 macOS 会重新编号）
GROK_GADGETS_DEVICE_TOKEN=$(cat ~/.config/grok-gadgets/smartring.token) \
  PYTHONUNBUFFERED=1 nohup .venv-grok/bin/grok-gadgets-gateway usb-bridge \
  /dev/cu.usbmodemXXXX --port 8764 > /tmp/ggg-bridge.log 2>&1 &
```

验证：`curl -s -o /dev/null -w "%{http_code}" https://gadget.example.com/mcp` 应返回 401（OAuth 保护正常）。

## 固件构建与烧录

```bash
export PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin:$PATH"
. ~/esp/esp-idf-v5.5.3/export.sh
cd firmware/grokbot
idf.py build
pkill -f usb-bridge                       # 烧录前释放串口
idf.py -p /dev/cu.usbmodemXXXX flash      # 烧完重启桥接
```

改了 `sdkconfig.defaults` 后必须 `rm -f sdkconfig sdkconfig.old && idf.py set-target esp32s3` 再生。

## 设备能力

- 命令：`character.set{grokbot|pika|rowlet}`、`backlight.set{0-100}`、
  `animation.trigger{thinking|happy|notify|orbit|sleepy|dizzy}`、`poke{}`
- 事件：`poke_event`（remote 字段区分物理/远程戳）、`shake{jerk}`、`switched{character}`
- 状态：character / anim / tilt / backlight / battery / uptime_s（每帧随响应上报）

## Grok Bot 侧配置

- 连接器 URL：`https://gadget.example.com/mcp`
- OAuth 授权：Grok Bot 给出 8 位授权码后，在网关侧批准：
  `NO_PROXY="localhost,127.0.0.1,::1" .venv-grok/bin/grok-gadgets-gateway oauth approve <CODE>`
- 授权态查看：`... oauth list`（refresh token 有效期约 30 天，滚动续期）

## 常用运维命令

```bash
# 急停远程访问（断隧道侧，本地不受影响）
.venv-grok/bin/grok-gadgets-gateway remote off    # 恢复：remote on

# 本地全链路演练（不经 Grok Bot，直接打设备）
.venv-grok/bin/grok-gadgets-gateway rehearse --device DEVICE_ID \
  --command animation.trigger --args '{"name":"happy"}'

# 设备/授权/用量
.venv-grok/bin/grok-gadgets-gateway devices
.venv-grok/bin/grok-gadgets-gateway oauth list
.venv-grok/bin/grok-gadgets-gateway usage

# 远程调用审计
tail -5 ~/grok-gadgets-requests.jsonl
```

## 踩坑记录（不要再踩）

1. **本机代理**：所有网关 CLI 和本地 HTTP 脚本必须前缀 `NO_PROXY="localhost,127.0.0.1,::1"`。
   只写 `localhost,127.0.0.1` 不含 `[::1]` 会让其依赖 httpx2 直接崩溃。
2. **串口节点会变**：设备每次复位/重插，macOS 可能重新编号。
   桥接起不来先 `ls /dev/cu.usbmodem*`。
3. **serve 重启后桥接不一定重连**：重启 serve 后检查 `lsof -nP -iTCP:8764` 有无 ESTABLISHED，
   没有就重启桥接。杀桥接要杀 python 子进程，包装进程杀了会留孤儿占串口。
4. **打开串口会复位芯片**（USB_UART_CHIP_RESET）：烧录/桥接启动时设备重启是正常现象。
5. **快速隧道会过期**：`trycloudflare.com` 快速隧道随时被回收（"Tunnel not found"），
   这就是切命名隧道的原因。若隧道断，看 `/tmp/ggg-tunnel.log`。
6. **协议实现要点**（固件侧已处理）：
   - `cJSON_PrintPreallocated` 返回 bool 不是长度
   - hello 的 `state` 必须放 device 对象内（schema additionalProperties:false）
   - command_id 是 "gw-"+32 hex 共 35 字符，缓冲至少 68 字节
   - 会话严格同步：发一帧读一帧，不得并发
   - `sdkconfig.defaults` 里 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 必须保留，
     否则日志污染协议帧（调试需日志时改 sdkconfig 临时打开，调完还原）

## 遗留项

- **未注册开机自启**：serve/隧道/桥接都是手动进程，重启电脑后按上文顺序拉起。
  如需自启：网关自带 `service` 子命令（launchd），cloudflared 有 `cloudflared service install`。
- **Wi-Fi 链路已验证，独立供电测试待完成**：通过 `wifi-provision` 写入配置，
  使用 LAN TLS 连接 `serve --lan-listen`。2026-10-16 修正 SSID 拼写后，设备获取 IP，
  并建立到 `GATEWAY_LAN_IP:8767` 的 TLS 连接。legacy / modern MCP 的
  `animation.trigger` 均返回 `executed`，设备状态为 `link: wifi`。
  本次仍由 USB 供电；尚未验证改接充电器后的运行，也未目视确认动画效果。
- `firmware/smartring-gadget/` 是 PlatformIO 最小验证草图（中间产物，留作参考）。

## Wi-Fi 诊断补充（2026-10-16）

- USB `status` 回复包含 `link`（usb / wifi-down / wifi）和已配置的 `ssid`，
  不返回密码、设备令牌或证书。`wifi_ssid` 命令接受 `ssid`，只更新网络名称并重启，
  保留其余配网字段；仅在已有完整配网配置时允许使用。
- 配网结构原来占用 `console_handle` 的 3,600 字节栈帧，导致 4 KB
  `gadget_console` 在处理请求时溢出。将临时凭据移到 PSRAM 后，栈帧降为 48 字节。
  处理完请求即释放临时缓冲。
- Wi-Fi 断开日志包含原因码。每次启动后的首次连接失败会扫描一次，仅记录 AP
  数量、目标匹配数量及目标的信道/信号/认证方式，不打印其他网络名称或凭据。
  本次 `WIFI_REASON_NO_AP_FOUND`（201）由目标 SSID 拼写错误引起，修正后即连接成功。
- 本机存在 SOCKS 代理环境变量时，`NO_PROXY` 仍可能触发 httpx2 的
  `socksio` 缺失错误，CLI 会将它概括成“网关未响应”。本地验证命令可临时清除代理：

```bash
env -u ALL_PROXY -u all_proxy -u HTTP_PROXY -u http_proxy \
  -u HTTPS_PROXY -u https_proxy \
  .venv-grok/bin/grok-gadgets-gateway rehearse \
  --device DEVICE_ID \
  --command animation.trigger --args '{"name":"happy"}'
```
