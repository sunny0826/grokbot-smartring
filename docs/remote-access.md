# 圆屏跨网连接家中的 Mac

固件 `0.9.0-phone-wifi` 支持用手机网页或圆屏搜索并连接其他 Wi-Fi／手机热点，
再经互联网连接家中的 Mac。
Mac 仍负责网关、语音转写和信箱，需要联网、保持唤醒并运行服务。
Grok 沿用原连接器；语音到达后仍需说「处理圆屏语音」。

当前已验证实体圆屏经公网域名连接和接收指令、发现未读回复；
手机热点下的触摸录音与真实 Grok 回复仍待实测。
[完整验证记录](implementation/remote-access-verification.md)。
圆屏配网的自动验证和实物验收边界见[配网验证记录](implementation/wifi-setup-verification.md)。
新增手机网页流程见[手机配网验证记录](implementation/phone-wifi-verification.md)，手机实物验收仍待完成。

## 带出门前

已经配置远程模式时，推荐用手机输入：

1. 轻点圆屏唤出控件，再点顶部 **Wi-Fi 图标 → 手机配网**。
2. 手机扫描第一张二维码，加入 `MyBot-XXXX` 临时热点。若提示无法上网，选择继续连接。
3. 点圆屏下一步，扫描网页二维码；也可在浏览器输入 `http://192.168.4.1`。
4. 在网页选网络、输入密码并连接，以圆屏结果为准。

临时热点最多开放 5 分钟，成功后约 30 秒关闭，也可在圆屏退出。
连接或重新搜索时热点可能短暂断开，重新加入同一个 MyBot 热点即可；网页不会自动重复提交密码。
若目标就是这台手机自己的热点，请使用另一台手机操作网页，或选 **圆屏输入**。

圆屏输入仍可直接选网络、用键盘输入密码并连接，支持大小写、数字和符号。
成功取得 IP 后才保存网络；失败或取消会尝试恢复旧网络。

换网络不需要接电脑。支持 2.4 GHz 开放网络和个人密码网络，密码键盘支持 ASCII；
不支持企业认证、隐藏网络手工输入和网页认证。较少见的中文／其他字符可能以 HEX
字节显示，点击网络详情可辨认完整名称。只保存一个网络，不会自动在家中与热点间漫游。
配网期间语音会暂停，退出后继续；正在录音时请先结束录音。

首次设置远程域名，才需要通过 USB 接到电脑，在项目根目录执行：

```sh
uv run tools/configure_ring.py --remote-host YOUR_EXISTING_TUNNEL_HOSTNAME
```

把 `YOUR_EXISTING_TUNNEL_HOSTNAME` 换成已有 Cloudflare 隧道域名，不带 `https://`。
配置会保留原设备身份和 LAN 证书。随后在圆屏选择 Wi-Fi。

也可保留原来的 USB 配网方式（在本机提示输入，不把密码放进参数）：

```sh
uv run tools/configure_ring.py --network
```

查看状态：

```sh
uv run tools/configure_ring.py
```

`transport: wss`、`link: wifi`、`clock_ready: true` 且 `transport_error` 为空，
表示远程控制连接已建立。刚重启可能短暂显示 `wifi-down`，稍后再查。
随后在 Grok 要求圆屏做一个动作；按住屏幕说话，松开后让 Grok 处理语音。
配置完成后，日常通信通过 Wi-Fi；USB 不承载控制指令。

## 回到局域网模式

```sh
uv run tools/configure_ring.py --lan --network
```

重新输入家中 Wi-Fi。此操作清空远程域名并使用之前保存的 LAN 网关及私有 CA，
不删除设备 token。若 Wi-Fi 没变化，可省略 `--network`。
当前只保存一个 Wi-Fi，不会自动在热点和家庭网络之间切换。

## Mac 部署

原网关保持运行：MCP loopback 8766、设备 loopback 8764、LAN TLS 8767。
先完成[语音服务安装](boot-voice.md)，再分别运行两个常驻进程：

```sh
uv run --project voice_bridge --extra transcription voice-bridge --host YOUR_PRIVATE_LAN_IP
uv run --project voice_bridge voice-device-relay
```

它们需要在不同终端运行。已有进程运行时不要重复启动。
relay 只监听 `127.0.0.1:8770`，每条认证 WSS 会话连接原网关的 loopback 8764。
它不创建第二份语音数据库或转写任务。

在现有本地管理 Cloudflare 隧道中添加精确路径：

```sh
uv run tools/configure_tunnel.py \
  --hostname YOUR_EXISTING_TUNNEL_HOSTNAME \
  --voice-origin https://YOUR_PRIVATE_LAN_IP:8768
```

先验证，确认参数后加 `--apply` 保存。工具会备份原配置；重复执行不产生重复路由。
随后重启原 cloudflared 进程，使配置生效。

| 公网路径 | Mac 源站 |
| --- | --- |
| `/v1/control` | 独立 relay `http://127.0.0.1:8770` |
| `/v1/inbox`、合法 `/v1/recordings/...` | 原语音 HTTPS 8768，验证私有 CA |
| 原 MCP、OAuth 等路径 | 原 `http://127.0.0.1:8766` |

不需要新域名、路由器端口映射或第二个 Grok 连接器。语音和控制使用既有设备身份。
Cloudflare 对设备路径须允许正常认证请求及 WebSocket，不应弹浏览器交互挑战。
这些路径不得缓存；服务端发送 `Cache-Control: no-store`。
本轮没有安装开机自启；Mac 重启后需恢复原网关、语音、relay 和隧道进程。

## 固件与排障

首次升级需同时烧录新分区表与应用，使用项目的常规 `idf.py flash`；
不能仅将新应用写入旧 1500 KiB 分区。新应用区为 2 MiB，NVS 地址及大小保持原值。
不要执行 `erase-flash`。升级前应保留私有 Flash 备份，备份包含凭据，不能上传仓库。

- `time_unavailable`：没有有效时钟，正在等待互联网 SNTP 校时。公网和 LAN TLS
  都检查证书日期；无法校时的冷启动不会跳过检查。
- `wss_connect_failed`／`tls_connect_failed`：检查域名、隧道、relay 及网络是否可达。
- 控制正常但语音不通：确认语音服务在运行、源站 IP 与证书匹配、语音路径已生效。
- Mac 地址变化：需要同步 LAN 地址／证书和隧道语音源站；远程域名不替代这些源站设置。

录音在 Mac 确认前只保存在圆屏内存，断电会丢失。远程模式没有改变这一边界，
也不包含自动唤醒 Grok、语音播报或多网络自动漫游。
