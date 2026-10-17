# 动画融合独立只读审查（0.12.0 最终 integration 补审）

当前最终生产改动与 integration 中，未发现有充分证据的正确性阻断。已补审 0.12.0-voice-character 的实际 flush、静态刷新和 Wi-Fi 恢复测试；剩余验证边界见末尾。

## 范围与方法

同模型独立会话，未参与实现。对照 /tmp/my-bot-fusion-baseline 的 main/voice_ui/voice_motion 基线与候选裁决，阅读 presenter、voice_character、voice_motion、voice_ui、main 改动、CMake、voice/wifi smoke 新整合；按 rigor 范围内代码检查和 no-comments 独立评论规约执行。补读 CharacterRenderer、LVGL 删除/事件/隐藏实现、voice actor 输入验证，只作调用链证据，不将前轮未提交修改混入本轮审查。

做过一次针对 my-bot 动画/流程/HoldVisual 的 Nowledge memory_search；历史仅作约束提示，结论依据当前源码。仅写本报告。无产品改动、串口、硬件、服务、外部消息或构建运行。

## 已核对的路径

- CharacterPresenter.attach 注册 canvas DELETE 回调，回调清 canvas 和 framebuffer；析构/重新 attach 移除自身 user_data 的回调。LVGL 当前 remove_event_cb_with_user_data 支持 callback=nullptr 匹配任意同 user_data；不是猜测兼容性。
- 删除 canvas 后 update 立即 Covered、draw_home 不访问对象；重建 attach 清 renderer/skin/motion 并先隐藏。生产主 timer 即使保留亦不访问悬空 canvas。
- 生产统一 frame 调用 voice_ui_update 后立即观察同一返回值并绘制，没有中间 timer/yield。root 初始化隐藏；语音 UI 不再有独立 timer。
- Covered 分支仅进入时隐藏 canvas/reset；遮挡时不调用 render。恢复 Voice 或 Home 的第一次 present 在实际 render 后揭开 canvas，避免显示旧帧。Wi-Fi 自己先关闭 overlay 至下一 frame 之间可短暂只见背景，这是设计明确接受的范围。
- Voice 静态阶段只因 phase/hold/scene/skin 的实际变化或动态最后一帧 render。错误和 Cancelled 不继承 morph/rings/blink；Renderer 清上一个实际写范围，reset 首帧全清覆盖皮肤和 scene 大小切换。
- phase/hold 驱动进入时间，generation 没进入动画时间模型。覆盖中仍观察边沿，恢复不把旧 Replied 当新回复再计时。Recording 使用 voice_motion 唯一 envelope，meter 和 pose 同帧共享 level。
- HoldVisual 只覆盖 actor 仍 Idle 的本地准备/取消窗口；真实 Recording/Ready 等来自 actor phase。按住控件对象保持，释放、滑出、DELETE 的 Cancel 路径未被改写。
- UI stale 按钮继续检查 drawn generation/phase/action，actor 再校验 published generation。AutoOpen 在观察 Waiting 等→Replied 的同次 update 内入队，没有等待动画；Wi-Fi 覆盖期间消费阶段且不补 Open，Idle unread 不抢占。
- main 恢复 Home 的 IMU previous baseline/jerk 已重置，隐藏时间未累加为物理 dt。
- CMake 与 voice/wifi smoke 消费方均迁移到实际 CharacterPresenter；无旧 voice timer 兼容路径。

## 可视和验证边界

独立只读查看主协调者生成的 recording.png、reply-first.png、failure.png；录音中文、角色、meter、时间、提示和按钮未见明显重叠；回复标题/page/button 分区清楚。其余皮肤、圆形 mask、真实字形页面、首帧截图和实机效果未由本审阅者验证。

没有独立实跑测试。稳定截图的 screenshot 仍额外推进 200ms；最终新 fusion_checks 已另用实际 flush callback 检查首帧，详见补审。

## 注释审查

新 presenter/voice_character/voice_motion 无旁白注释。voice_ui.h 两条保留：LVGL task/display lock 的调用前提及同帧提交契约，均属公共 API 契约例外。voice_ui.cpp `} // namespace` 属不必要旧旁白，可删除；非正确性阻断，无 MUST KILL 结构目标。main 本轮只改调用/恢复 baseline，未新增旁白；不清理前轮 main、bot_character 或既有 smoke 注释。未发现新增 suppressions。实际删除 0、恢复 0、重审 0（只读职责）。

## 本次已读文件 SHA-256

- `main/character_presenter.h` `243149ad074cb0881261006a5e45746f10b01fb86100460cd44c72ec07a56531`
- `main/character_presenter.cpp` `34f5a66bcc1b0cbb914b15f2078669e6932eccb97e36da10c36dd27606774da8`
- `main/voice_character.h` `389965ec65f2943d216fe90d08dda9936ae7fa1ead05874d4acdf190aa2e346c`
- `main/voice_character.cpp` `570a00bd2d46e102bd17e256f81d634d03b1e76f435be489decaeea8a58dc114`
- `main/voice_motion.h` `2c256924556436690fa6b1a2f52800058870b0e29eea44e6084d16ed2d450338`
- `main/voice_motion.cpp` `f870fe15f297c3e001e3420e6b29c352fbead308c68b1688e07774ce22431cf3`
- `main/voice_ui.h` `020a34b55cd7c5c39a251ad5daaa0c3d5308e981aa63d293012a5c84be6fab71`
- `main/voice_ui.cpp` `83e0446bc8102088a09f801b753ad75e7164c407ddc44ee0e345a444900da66b`
- `main/main.cpp` `748d7f543d0d9b98141c93f42b524d35651367415f583ce35e4fdcfabf60e9c3`
- `main/CMakeLists.txt` `7d8b01528f2017bf0b0c80c25c3c3870dc4cc5ba422be2f00acdd69271bf24be`
- `tools/voice_ui_smoke/character_fixture.h` `8e5115f42aebe04e02508351a85b349c436edc2dcfcec0170da120e286d2b282`
- `tools/voice_ui_smoke/character_sources.cmake` `38f485ad60e473b99ae5f5a9b38c6edc601295e6f88f256ed88314fdadf69d75`
- `tools/voice_ui_smoke/CMakeLists.txt` `5b2b259b0f513933005ee5f371858d58b325c02c1f691b24beff0d561e7a53db`
- `tools/voice_ui_smoke/smoke.cpp` `5ffe9861c8e483315cee87ae38e9a1b02820dce773706fab2e9ff045772cf9b6`
- `tools/wifi_ui_smoke/CMakeLists.txt` `2f7baffaa90097feeb664d9759805c2213f306b4d2aee1e343be62e5f06cfecd`
- `tools/wifi_ui_smoke/smoke.cpp` `5e4bd621756917a653c6d70c19ac8c016cc92d2dd703fa7e8eca2de4d164bcb4`

## 最终 integration 补审

已独立只读核对最终 voice_ui_smoke/smoke.cpp、character_fixture.h、Wi-Fi smoke 新退出测试和 /tmp/my-bot-{voice,wifi}-fusion.log；两日志已有 PASS。日志来自主协调者执行，不计为本审阅者独立实跑。

- voice smoke 改为实际 RGB565 partial display buffer；flush 按 active buffer stride 转换，并在 flush_is_last 才递增 completed_frames/执行 probe。因此静态零刷新断言观测真实 LVGL 提交，不是把“像素没变”当作“未刷新”。
- 主协调者测试发现 root bg_opa 每帧写入引起不必要 invalidation，最终生产代码已改仅 changed 时设置。phase/initialized/generation 的变更均会进入 changed；ViewingReply 进出、首次创建都覆盖，不因限流保留错误透明度。
- Home→Preparing 探针在每个实际完成 flush 上观察 root 进入显示时旧大角色内部 (100,180) 已为背景，三皮肤各执行一次。该验证不调用 screenshot 的额外 200ms；但只检查一个代表点，不是全帧图像与独立参考逐像素比对，也未变化 timer 到期偏移。
- 三皮肤各检查 Ready/UploadFailed/Failed/NeedsReview/ReplyFailed/Unavailable 稳定后的 framebuffer 不变且 completed_frames 零增量。该断言能抓到这次实际 bg_opa 回归。
- voice Covered 测试同时切换 Recording 与 Waiting，证明 framebuffer 隐藏期间不写，恢复后实际重新绘制；Reading 期间保持 canvas hidden 和 buffer 不变，回 Preparing/Home 恢复可见。
- Wi-Fi smoke 使用真实 wifi_setup_ui 的菜单返回和 operation Closed 两条路径；隐藏期间由 Claimed→Replied，恢复后 buffer 更新到新角色、文案为收到回复，voice_actions 不增加。没有用布尔 stub 冒充这两条实际关闭链路。
- 432 个三皮肤×六阶段×24帧文件属于可视采样产物，不能宣称为 432 条独立参考像素断言。以实际渲染正文 fixture 生成的新 reply-first.png 已只读查看，标题、正文与按钮无明显碰撞。
- 长预览后用实际 pointer tap 唤出首页控件，再按住验证 begin 增加，然后 DELETE root 恰一次 Cancel；避免原测试对已隐藏 talk 假按压后误报取消。

本轮没有确认的生产 bug。此前指出的 canvas 删除/重建 timer 覆盖缺口已补：已只读核对 smoke 尾部在 lv_obj_clean 后执行 tick + lv_refr_now，断言 mode 为 Covered 且 framebuffer 不变；重新 create canvas/UI 后再执行 tick + lv_refr_now，断言 Home 且 canvas 可见。最新 /tmp/my-bot-voice-fusion.log 两项 PASS 均通过；执行者仍为主协调者，本审阅者只读核对。该项已关闭。其余没有独立实跑的实机/性能/全清对照仍由主协调者证据负责。

新增 integration 无注释旁白或规则抑制；实际注释删除仍 0，不改变前述保留判断。

### 最终变更文件 SHA-256

- `main/voice_ui.cpp` `81bfbecd54d4ba207b3a27b629aa1675e54e5b65e1f80bfcc7858ee66c80e000`
- `main/main.cpp` `748d7f543d0d9b98141c93f42b524d35651367415f583ce35e4fdcfabf60e9c3`
- `tools/voice_ui_smoke/smoke.cpp` `2e447567616195c843dc4c6b5d10ec329c316b6d7f72e523f76aaeff76e912c2`
- `tools/voice_ui_smoke/character_fixture.h` `8e5115f42aebe04e02508351a85b349c436edc2dcfcec0170da120e286d2b282`
- `tools/wifi_ui_smoke/smoke.cpp` `18d98aa3c22cfe643a40bd139ef0e157c1c13be3808979101084b67da8adc868`

删除/重建覆盖补齐后的 `tools/voice_ui_smoke/smoke.cpp` SHA-256：`2e447567616195c843dc4c6b5d10ec329c316b6d7f72e523f76aaeff76e912c2`。
