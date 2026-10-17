# 远程接入验证记录

2026-10-17，固件 `0.7.0-remote`、voice mailbox `0.2.0`，原 gateway `0.1.0a14`。
方案见 [设计记录](remote-access-design.md)。

## 已完成

| 层次 | 结果与证据 |
| --- | --- |
| 后端 | `voice_bridge/.venv/bin/pytest -q voice_bridge/tests`：51 passed，33.08s。包含真实隔离 gateway 的 MCP command→poll→ack、突断 unconfirmed、不重播、凭据撤销、限额与进程退出 |
| 固件主机检查 | `check_remote_core.py` 三组 ASan/UBSan 通过；原 `check_voice_core.py` 两组通过。覆盖 WS 分片/控制帧/短写/释放、升级状态、NVS兼容、时间门槛与 HTTP配置 |
| 隧道工具 | 5 项 unittest 通过；部署前 `cloudflared tunnel ingress validate` 通过；备份后加入两条精确路径，原 MCP/OAuth 路由保持 |
| 构建 | ESP-IDF 5.5.3 成功；app 1,556,464 B；2 MiB app 余 540,688 B（26%） |
| Flash | 实查芯片 16 MB，备份原 Flash；烧录 bootloader、app、新分区表且写入校验通过；NVS `0x9000/0x6000`、phy `0xf000/0x1000` 未被擦写 |
| LAN 回归 | 升级后缺省仍为 LAN；SNTP 校时成功，实体圆屏通过原 TLS 上线，原设备身份和网络保留 |
| 公网设备认证 | WSS Upgrade 无凭据 401/no-store；语音无凭据 401/no-store。有效设备凭据 GET inbox 200、no-store、Cloudflare DYNAMIC |
| 原 MCP/OAuth | 原公网 MCP 无凭据仍401；OAuth discovery 200；原 loopback MCP 客户端初始化及工具调用成功 |
| 实体公网控制 | USB状态 transport=wss、clock_ready=true、link=wifi、无错误；真实设备 `simulated=false`，原 MCP 动画测试指令收到 executed 回执 |
| 公网语音服务 | 测试 WAV 经公网 HTTPS上传→真实本地Whisper→ready→原MCP claim/reply→replied；公网下载91408 B、VPG1中文页成功 |
| 圆屏 HTTPS | 实体圆屏在 WSS 在线期间经公网 inbox 发现测试回复，USB状态 unread=true；本次未目视打开页面 |
| 重连 | 重启独立 relay 后圆屏自动恢复 WSS，后续原 MCP 测试指令再次 executed；未重启网关或 Wi-Fi AP |
| 内存观察 | 实体 WSS 与周期 HTTPS inbox 并行时，所采样空闲内部 heap 约95–137KB、最低86,687 B、最大连续块31,744 B；不是完整录音/页面下载峰值或长期泄漏证明 |

最终构建 app SHA256：
`50efb00de50ed36a199e5b9f92b486c1f4bc80cc9c581327396c98cd1d6fccf9`。

独立只读审查发现并修正：

1. IDF WS 升级写失败会打印完整认证头：首次 connect 前永久禁用 `transport_ws`
   这一日志标签，保留应用层脱敏错误；主机回归断言调用顺序。
2. IDF 对重定向返回正状态码，旧判定误认为连接成功：现在同时要求返回0及HTTP101；
   301/302/303/307/308和返回0但状态200均拒绝并释放两层 transport。

初期从旧 map 对象名推断公共 CA bundle 已链接不成立；实际二进制核验旧版未包含，
新版 bundle 增加68,987 B，因此实查容量及备份后扩大应用分区。

公网探测使用直连 HTTPS；继承本机代理环境的 urllib 请求收到代理链路403，
不能将其误判为隧道路由失败。直连客户端及实体固件均已取得上述成功结果。
测试回复明确标记为链路测试，不是实际 Grok 回答，也没有领取用户任务。

## 尚未证明的边界

- 实体圆屏目前仍连家中 Wi-Fi，只是控制和语音路径已改为公网域名。
  手机热点／外部 Wi-Fi 的真正跨网验收待用户切网后执行。
- `0.6.0-touch-voice` 的触摸录音→真实 Grok→屏幕回复已获用户验收；
  不能据此将新远程模式的实体录音、打开页面、目视显示和真实 Grok 标为已通过。
- 错误 CA／主机名／过期证书的实体握手负例、完整录音与页面传输期间的峰值 heap／
  栈水位、长时间断网与弱网恢复未全面执行。当前公共 TLS 成功、配置与 mock
  负例不替代这些实测。
- 没有有效时间且 SNTP 全部不可达的冷启动会拒绝公网和 LAN TLS；SNTP 无认证。
- Mac 仍须运行原网关、语音服务、relay、cloudflared；没有新增开机自启。
- 仍需手动在原 Grok Bot 触发处理；未增加自动唤醒、多 Wi-Fi 漫游或云端转写。

## 可复跑检查

```sh
voice_bridge/.venv/bin/pytest -q voice_bridge/tests
python3 firmware/grokbot/tools/check_voice_core.py --idf-path "$IDF_PATH"
python3 firmware/grokbot/tools/check_remote_core.py
uv run --with PyYAML python -m unittest discover -s tools/tests -q
uv run tools/configure_ring.py
git diff --check
```

热点验收：固件0.8.0起可点圆屏Wi-Fi图标输入2.4GHz热点凭据，
也可本机运行 `configure_ring.py --network`；
确认WSS在线后让Grok执行动作；按住说话→发送→Grok处理→打开回复→标记已读。
Mac留在家庭网络。此流程通过后再追加实体验收结果。
