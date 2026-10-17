# 圆屏独立配网验证记录

2026-10-17，固件 `0.8.0-wifi-setup`。
方案与分工见 [设计记录](wifi-setup-design.md)，使用方式见 [跨网使用](../remote-access.md)。

## 已完成的自动验证

| 检查 | 结果 |
| --- | --- |
| 生产网络控制与驱动 | `check_wifi_setup.py` 通过。模拟 IDF 事件覆盖暂停确认、25秒准备期限、扫描代次、取消与保存边界、旧 IP、STOP/fence、事件溢出、恢复和 USB 互斥 |
| 配置与远程回归 | `check_remote_core.py` 三组通过。包含旧配置迁移、单 blob 长度边界、提交失败与回读/回滚、未知版本拒读、NVS 初始化失败不擦除，以及原 WSS/时钟/HTTPS 行为 |
| 语音回归 | `check_voice_core.py` 三组通过。暂停须等工作结束，待处理 Close/Discard 不可被越过，Pending 后不新开轮询；录音、PCM、journal 不因打开设置而清除 |
| 真实 LVGL 配网界面 | `check_wifi_ui.py` 通过。真实指针输入 ASCII 32–126、7/8/63/64/65 密码边界、隐藏密码、长按删除、过期点击、异步刷新、取消/重试/退出及10轮操作 |
| 语音页与设置页共存 | 上述配网 harness 编译真实 `voice_ui.cpp`，覆盖 UploadFailed 页面上的 Wi-Fi 入口、无触摸透传、退出保留原页。两套 UI 的主机 LVGL 当前占用48,104 B，不代表 ESP32 总堆峰值 |
| 原语音界面 | `check_voice_ui.py` 通过，覆盖按住/松开/滑出/快速点击、收件箱、分页、已读与旧点击 |
| 界面可见性 | 已查看网络列表、大小写/符号键盘、32字节 HEX 名称和忙提示截图。按钮至少44×44，矩形四角均在360圆屏内；修复分隔符缺字 |
| 独立只读审查 | 修复非零 operation 初始 `Failed/VoiceBusy` 被误画成空列表，新增契约测试，复核关闭；未发现其他确定 blocker |
| 固件构建 | ESP-IDF 5.5.3 成功。app 1,784,608 B，2 MiB 分区余312,544 B（约15%）。修复 GCC14 对隐式聚合赋值的编译失败 |

app SHA256：`8d308288d9b3b074af5017b1aeff43fe70c820693bf7f2661d524b3834d4fefb`。

## 实体验证

仅烧录 app `0x10000`，写入1,784,608 B并通过Hash校验，未擦写NVS或分区表。
烧录前已确认旧版 `0.7.0-remote` 为 WSS 在线、语音 Idle，
保留旧应用与权限0600的私有 NVS 备份；备份包含凭据，不进入仓库。

烧录后USB实读 `firmware=0.8.0-wifi-setup`、`transport=wss`、`link=wifi`、
`clock_ready=true`、空transport_error，原SSID和远程域名保留。
原MCP发送 `animation.trigger/notify`，真实设备 `simulated=false` 回执 `executed`；
这是协议回执，尚未目视动作。语音Idle、unread=true，设置Closed。
启动后两次采样内部空闲堆75,207–108,651 B，最低63,347 B，最大连续块31,744 B；
未覆盖真实扫描或录音峰值。用户随后回复“测试可用”，确认圆屏配网可用；
未逐项说明错误密码恢复、重启保存或热点等场景，不将这些扩展项视为已验收。

## 验证边界

- 主机测试替换了射频、DHCP、NVS、触摸设备和网络响应，不能证明真实错误密码、
  掉电写入、慢 DHCP、弱网或连续扫描的设备内存稳定性。
- 圆屏配网已获用户可用确认；错误密码恢复、重启保存和异常恢复仍待逐项验收。
- 支持2.4GHz开放／个人密码网络与ASCII密码；不提供企业认证、隐藏SSID手工输入、网页认证、多网络自动漫游。SSID包含内嵌NUL不支持。
- 常用中文由LVGL字库显示，未覆盖字符使用HEX原始字节；实际连接保留原SSID字节。
- 设置期间暂停语音，失败恢复最多等15秒；准备阶段25秒超时，不会为设置跨线程关闭正在用的网络连接。Saving为短暂不可取消提交点。
- Mac须继续在线并运行网关、语音、relay、隧道。手机热点跨网、真实Grok完整语音链路仍另行验收。

## 复跑

```sh
python3 firmware/grokbot/tools/check_wifi_setup.py
python3 firmware/grokbot/tools/check_remote_core.py
python3 firmware/grokbot/tools/check_voice_core.py --idf-path "$IDF_PATH"
python3 firmware/grokbot/tools/check_wifi_ui.py --work-dir /tmp/my-bot-wifi-ui
python3 firmware/grokbot/tools/check_voice_ui.py --work-dir /tmp/my-bot-voice-ui
uv run tools/configure_ring.py
```

实体操作：点顶部 Wi-Fi → 选择网络 → 先输一次错误密码并检查原网络恢复 → 重试正确密码 →
连接成功后点完成；重新打开设置或重启确认保存。配网过程中不在聊天或日志中发送密码。
