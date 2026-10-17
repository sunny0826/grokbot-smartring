# 本轮整体提速独立范围审查

结论：未发现阻塞交付的本轮生产路径缺陷。发现的一项 benchmark-only P3 统计边界问题已由主会话修复并由本审查回读确认，当前无未修复正确性发现。独立同模型只读审查，不是跨模型审查。未改产品、未连接串口、未重启设备；唯一产物为此报告。

## 范围与依据

审查 main/bot_render.h/.cpp、bot_character.h/.cpp、main.cpp 本轮相关代码，新增 bot_motion.h/.cpp；adapter v9 refresh_guard.h 与 lvgl_bridge_v9.c 的相关生命周期和 GPIO TE flush；两处 benchmark CMake 开关；tools/render_smoke/{device_benchmark.*,legacy_frame.h,render_cases.h,pixels.cpp,ui.cpp,CMakeLists.txt}、check_render.py、measure_display.py、tests/bot_motion_test.cpp。

关联读取了 LVGL 本地 lv_display.c、lv_refr.c、lv_canvas.c 与 adapter display_manager.c。已读独立 judge 方案，以及 rigor/code-review、no-comments/comment-reviewer、benchmark-checklist；进行了定向 Nowledge 检索，仅将历史作为约束。没有扩展复审之前 0.10.0/0.3.1 全部未提交变更。

## 发现

[P3，已修复] 超过 255 ms 的完成间隔被错误表示为分位数上界。

位置：firmware/grokbot/tools/render_smoke/device_benchmark.cpp:96–97、29–32、40。

interval_ms[min(255, interval)] 把 256 ms 及更长间隔归入桶 255；percentile 返回 255 并以 p50_ms_upper / p95_ms_upper 输出。因此若分位数落入末桶，读者会得到一个低于真实分位数的所谓上界。建议末桶输出明确 overflow/null，或同时输出饱和标志，使它表示“≥255 ms”。本次日志 p95 为 82–115 ms，未落末桶，因此不影响本次结论。主会话已将255桶和不足2帧输出改为JSON null；本审查回读 report() 的 quantile 处理确认修复，没有其他已证明的本轮缺陷。

## 已核对的正确性边界

- bot_render.cpp 所有直接 framebuffer 写入位于 clear/clear_rect/blend_px/poly/ellipse/capsule；每条路径均有 collector。poly 在实际子行 span 收集，ellipse/capsule 收集实际裁剪循环框；空交集被 PixelRect::empty 排除。arc 经 ellipse 写入，没有发现旁路。
- CharacterRenderer::render（bot_character.cpp:188–199）参数按值传入。清旧框时关闭 collector，绘制时只让局部 Fb 指向栈上 current，结束后仅保存值，不会把悬空指针留进调用者。返回旧新并集，首次和 reset 清整个 framebuffer。
- main 用 canvas 实际坐标转换 dirty，并在页面遮挡期间 reset；角色切换也 reset。原 fullClear registry 和 main 的手动 bbox 清屏状态已删除。当前 canvas 固定320×320，未引入尺寸变化问题。
- refresh_guard 的两个 callback 按 guard user_data 注册。LVGL lv_display_remove_event_cb_with_user_data（lv_display.c:899）逆序删除全部匹配 callback，故 detach 同时移除 START/READY；impl calloc 初始化，display_manager.c:709–718 先销毁 bridge 后删 display，未见新 use-after-free。
- REFR_START 清 failed；首/中条带失败后 failed 保留，末条带不得走成功 FPS 入口。te_frame_in_progress 仍按实际 last 更新，未因中间失败误开启新帧。
- LVGL lv_refr.c 在 REFR_READY 前清空 invalidation list，故 guard 在 READY invalidate system layer 会留给下一轮，且覆盖全显示和叠层。同步 GPIO TE 路径在每 strip 返回前等待 DMA notification；失败整帧不计成功 FPS。
- bot_motion.cpp 保留原加速度/阻尼/边界反弹/挤压关系，正常 elapsed 在≤25ms 子步中完整积分，仅 >250ms stall 限制追赶。main 视觉时钟不再50ms截断。centered 分支保持指数归中和速度清零；未见物理状态新增不合法路径。

## 验证证据与实际限制

读过 pixels.cpp 的逐像素 full framebuffer 和模拟 panel 双 oracle，含皮肤/状态切换、alpha、贴边、空角色、reset、外侧guard；读过 ui.cpp 的真实 LVGL 透明叠层、平移、隐藏恢复和首/中/末 strip 故障恢复。主会话提供的执行结果为2620帧像素比较、180帧真实LVGL比较、ASAN/UBSAN及motion测试通过。本审查未重跑，也不把它们写成独立二次执行。最后回读确认 check_render.py 已纳入 motion 的 ASAN/UBSAN 编译执行；主会话进一步报告该整体验证通过和最终 IDF benchmark=OFF 构建成功。

UI 故障测试使用真实 refresh_guard 与 LVGL，但 flush callback 为测试实现，不是 ESP GPIO TE bridge 真机故障注入；实际 adapter 分支通过源码审查。未验证丢失 DMA 完成通知的恢复：portMAX_DELAY 行为仍在，不能宣称覆盖该故障。

## 性能结论

读取 /tmp/my-bot-display-benchmark.log 的完整十段结构化数据，每侧五段，AB/BA 配对交错、每段1秒热身+15秒同一墙钟轨迹。原清屏基线对固定 Idle 轨迹由 legacy oracle 检查；两侧实际完成工作均在同步末 DMA 通知后记时，没有提前结束计数。

使用 (frames-1)*1e6/elapsed_us，因为 elapsed_us 是 first_done 至 last_done：

| 指标 | legacy-full | tracked-partial |
| --- | --- | --- |
| 完成帧率中位数 | 11.812 fps | 17.656 fps |
| 五段范围 | 11.733–12.170 fps | 17.305–17.796 fps |
| 每帧完成字节中位数 | 204800 B | 89967.6 B |
| refresh_us/frames 中位数 | 53.247 ms | 29.300 ms |

本场景变快，完成吞吐中位数提高49.48%；十段 failed_frames=0、te_timeouts=0、interrupted=false。较小传输范围与整刷新阶段缩短共同支持收益来自显示重绘路径；数值没有超过显示链路能力。

refresh_us 是首 strip 计时起点至末 strip 完成确认，包含TE等待和条带间LVGL处理，不能叫纯DMA耗时；它也不包含首条带计时前的全部准备。render_us/renders 的样本皮肤权重随帧率变化，不能单独将平均值降低归因为相同帧 CPU 加速。

该 benchmark 二进制在新 physics 提取前构建，且 render_case 覆盖实际 pose，不证明新的物理时钟收益。结论只覆盖固定 three-skins-idle-v1 轨迹的旧/新重绘路径，不能外推全部姿态、录音/TLS并发、触摸体验或承诺30fps。最新 bot_motion.cpp 和 main.cpp 的时钟/状态使用均已纳入本次静态审查。主会话已完成生产 OFF 构建，部署与真实手感验收仍属主会话后续工作；当前实机收益数据不能替代最新 physics 构建的实机体验证据。

## 注释处置

本审查实际删除0行、修改0行；建议删除1行：bot_motion.cpp:7 的“Bound catch-up…”是对随后的 clamp/子步逻辑的旁白。若要让250ms/25ms意图更直观，可命名 max_catch_up 与 max_step 常量；这属于可选结构说明，不是功能缺陷或阻塞项。

保留例外：refresh_guard.h:16 说明 LVGL 外部刷新事件与 invalidation 清空的时序约束，证据为本地 lv_refr.c:427–439；bot_character.h 的 raw preview/live UI 说明属于调用契约；measure_display.py 文档字符串被 argparse 的 description=__doc__ 消费，属于命令行帮助内容。旧 main/render 中未在本轮改动的新旧混合历史注释未扩展清理；未发现需要本轮 MUST KILL 的 workaround 或规则抑制。
