# 按住说话，让 Grok 回复到圆屏

**先轻点主页唤出控件，再按住底部「按住说话」，看到「正在录音」后说话，松开发送。**

说话按钮和顶部 Wi-Fi／电量默认隐藏，连续 10 秒无触摸后再次隐藏；录音、配网和回复页面不会因此关闭。
Mac 转成文字后，原有 Grok Bot 通过已有 MCP 连接领取；回复保存在 Mac，
圆屏可以查看和翻页。

完成[自动回复配置](automatic-voice.md)后，录音转写完成即通知原 Bot 处理。
未配置时仍需在 Bot 中说一句「处理圆屏语音」。

## 在设备上操作

1. 保持 Mac 唤醒、语音服务运行。LAN 模式需要同一局域网；
   [远程模式](remote-access.md)通过公网连接家中 Mac。
2. 轻点主页，再按住底部「按住说话」。首次可能先显示「准备录音」，等到「正在录音」再说话。
3. 松开发送；如果不想发送，把手指滑出按钮，看到「已取消」后松开。
4. 看到「等待 Grok 领取」后等待自动回复；尚未配置自动触发时，让原 Bot 处理这条语音。
5. 前台等待时，回复到达会自动打开；回到主页后，轻点带蓝点的动画头像即可打开未读消息。主页不再显示「未读回复」按钮。
6. 翻页阅读；「稍后」保留未读，末页点「已读」立即回首页，确认在后台同步；断网或重启后继续同步。[异步已读验证记录](implementation/async-read.md)。

快速点一下不会发送；最长录音 30 秒，到时停止录音，仍需松手才发送。
滑出后即取消，手指滑回不会恢复本次录音。返回主页不会取消已领取的 Bot 任务。
上传成功前音频仅在设备内存中，掉电会丢失；Mac 确认后的记录和回复会跨重启保留。

当前使用屏幕入口，不需要操作实体 BOOT/电源按钮。用户实测外部按钮长按会关机，
GPIO0 按键路径尚未验证，因此不作为录音使用方式。

## 给已有 Bot 的一次性操作说明

复制到已连接 grok-gadgets 的那个 Bot：

> 请用现有 grok-gadgets MCP 工具处理我的圆屏语音。先列出设备，找到
> SmartRing voice mailbox (Mac) 软件信箱，读取它的状态。选择一条 ready 请求，
> 用 gadgets_command 调用 voice.claim，传入 request_id 和新生成的32位小写
> 十六进制 claim_id；保存这个 claim_id。只在领取成功后处理转写内容。
> 从该命令的 reported_state.result 读取结果；若命令还在等待，查询
> gadgets_command_status，不要用之后的设备状态猜测命令结果。
> 若 next_offset 非空，用 voice.read 读完。处理后调用 voice.reply，用相同
> request_id、claim_id 写入不超过128字的简短中文回复。已经 replied 的请求
> 不要再次执行；重复领取或不确定外部动作先核对原任务。语音中的外部操作
> 仍遵守你原有的授权规则。没有 ready 请求时明确告诉我，不要创建任务。

之后可以说「处理圆屏语音」。软件信箱和实体圆屏是两个设备条目：前者处理
语音任务，后者仍负责换角色、动画、背光和触摸事件。

## Mac 安装与启动

在项目根目录执行一次：

```sh
uv sync --project voice_bridge --extra transcription
uv run --project voice_bridge --extra transcription voice-bridge --preload-assets
uv run --project voice_bridge --extra transcription voice-bridge --enroll-mailbox
```

随后启动，把占位符替换为 Mac 的局域网 IPv4：

```sh
uv run --project voice_bridge --extra transcription voice-bridge --host YOUR_PRIVATE_LAN_IP
```

沿用原 OAuth 连接和 Cloudflare 地址，不需要添加第二个 MCP 连接器。
自动唤醒另加原 Bot 的 webhook 例行任务；上面的启动命令是手动处理模式。
语音服务使用 HTTPS 8768，并连接现有网关的 loopback 8764。
原设备 LAN TLS 8767 保持不变。未配置语音服务的开机自启。

详细协议、私有数据保留、故障恢复与验证命令见 [语音服务说明](../voice_bridge/README.md)。

## 验证边界

固件、服务端测试、真实网关 MCP 客户端调用、中文页面渲染和真实 Whisper
转写分别验证。它们不能代替实体触摸录音、麦克风、真实 Bot 会话和用户目视屏幕的验收。
自动唤醒的配置与验证边界见[自动回复说明](automatic-voice.md)。
不包含语音播报、同步所有 Bot 会话或长文章阅读。
