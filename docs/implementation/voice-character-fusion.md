# 语音流程与圆屏角色融合

2026-10-17，固件 `0.12.0-voice-character`。用户要求“将流程动画与圆屏的动画效果融合”。

后续 `0.12.1` 已按用户要求[接入原有 Notify / Thinking / Orbit 动画](state-animations.md)，下方保留 0.12.0 的实现记录；动画预览链接展示最新版本。

[打开动画预览](voice-character-fusion.html)。预览使用实际固件角色绘图、LVGL 字形和控件，注入测试状态与音量；不是设备录像，也不表示实际任务耗时或设备帧率。

## 可见变化

| 任务阶段 | 角色表现 |
| --- | --- |
| 准备录音 | 小幅呼吸，保留角色面部 |
| 正在录音 | 与音量条共用同一音量包络，声音越大，角色轻微伸缩、抬头；静音无假音量变化 |
| 上传 | 向上轻浮与环形装饰 |
| 转写 | 环形装饰与轻微左右注视 |
| 等待接单 | 慢呼吸 |
| 已接单、正在处理 | 较快注视与两侧脉冲，和等待接单有区别 |
| 收到回复 | 500 ms 提示后静止；前台等待仍立即自动打开，不等动画播完 |
| 失败、需要核对、待发送、取消 | 静止，具体情况由文字和按钮说明 |
| 阅读回复 / 配网菜单 | 隐藏角色并停止角色绘制；退出后先画当前状态再显示 |

Grokbot、Pika、Rowlet 共用这些状态。录音按钮、滑出取消、过期点击校验、手动已读及后台不抢占保持原行为。首页恢复时重设 IMU 比较基线，避免将遮挡期间的姿态差误判为摇晃。

## 实现选择

独立候选和评判后选择单个主 canvas，由 `CharacterPresenter` 在同一主帧内更新语音 UI 和角色。语音 UI 的独立 timer 已删除。两套 timer 即使同线程串行，仍可能先画旧角色、再显示新文案；统一入口消除了这个提交顺序问题。

`VoiceCharacterMotion` 只接收实际阶段、按住显示状态与平滑音量。阶段进入时间不使用会因其他数据变化而递增的 generation。继续使用此前的脏区绘制，不增加 framebuffer。页面遮挡时观察阶段变化但不绘制，避免返回时重新庆祝旧回复。

测试发现根容器每帧重复设置同一个背景透明度仍会使 LVGL 刷新；改为展示状态变化时才设置。静态错误因此不仅像素不动，也没有重复显示刷新。

## 已运行验证

- 1,920 帧语音角色全清参考 / 局部绘制逐像素一致，覆盖三皮肤、16阶段、音量变化、静态错误、遮挡时钟与 tick 回绕；ASan/UBSan 通过。
- 原渲染回归 2,620 帧、实际 LVGL 合成 180 帧通过；不将这些数量当作 4,720 个独立用户场景。
- 真实 LVGL 语音界面：首次可见 flush 的旧大角色区域探针清除；三皮肤静态状态连续八帧零刷新；遮挡期间 framebuffer 不写；阅读/首页恢复、按钮按住释放/滑出取消、旧点击、自动打开、删除后 timer 及重建首帧通过。首帧探针不是整帧截图等价证明。
- 真实 Wi-Fi 菜单“返回”和 operation Closed 两条退出路径通过：角色隐藏期间不绘制，恢复当前状态，不补发自动打开；原配网交互回归通过。
- 生成三皮肤 × 六阶段 × 24 帧预览。已目检三皮肤六阶段合成图、录音、中文回复和菜单返回截图；标题、正文、角色及按钮未见重叠。18组动图及静态图片可解码，HTML JavaScript 语法通过。
- ESP-IDF benchmark ON/OFF 均构建成功，最终生产包为 OFF，大小 `0x1c4940`，应用分区剩余 12%；烧录内容哈希校验成功。
- 实机 55 秒观察收到 11 次状态回读，均为 `0.12.0-voice-character`，Wi-Fi 重新连接，未记录到 Draw bitmap failed / TE wait timeout；实体录音到回复的观感尚未验收。
- 同模型独立只读审查完成，未发现生产阻断；审查本身没有独立运行测试。

证据位于 [voice-character-evidence](voice-character-evidence/)：测试日志、审查报告、源文件与固件哈希、设备回读。构建和主机验证不替代实体按键录音到回复的体验验收。

## 复跑

```sh
python3 firmware/grokbot/tools/check_render.py
python3 firmware/grokbot/tools/check_voice_ui.py --work-dir /tmp/my-bot-voice-fusion --page /path/to/reply.bin
python3 firmware/grokbot/tools/check_wifi_ui.py --work-dir /tmp/my-bot-wifi-fusion
# 下列打包器需要 Pillow；上面的 UI 行为检查不需要。
voice_bridge/.venv/bin/python firmware/grokbot/tools/preview_voice_fusion.py /tmp/my-bot-voice-fusion docs/implementation/voice-character-fusion.html
```

VPG1 测试页面由现有 `RenderPages` 本地生成，不向云端发送消息。设备本轮只做烧录、版本及连接状态回读；没有远程伪造实体录音操作，也未重新测量云端回复速度。此前 49% 的 Idle 重绘对照收益不能用于声称这些新阶段提速或保证 30 fps。
