# 异步已读独立只读审核

最终结论：**范围内无剩余已证实代码 blocker。初审发现的两项 blocker 已由主线程修复，并通过失败基线和修复回归；本人已复核源码并独立重跑两个修复场景。实机验收缺口仍保留，不能把此结论表述为已部署或硬件验收完成。**没有修改产品、测试或设备，没有 commit/push、生产服务或外网操作。唯一审核产物是本文。身份使用当前继承配置，不称跨模型审核。

## 初审发现与最终复核

1. **持久日志加载失败会终止 FreeRTOS 任务并引发整机异常。** `firmware/grokbot/main/voice.cpp:337` 的 `journal.load` 失败路径发布 Unavailable 后直接 `return`。损坏的新 blob、NVS 读取错误或迁移提交失败即可触发，持久坏 blob 将令每次启动重复失败。真实 IDF v5.5.3 `FreeRTOS-Kernel/portable/xtensa/port.c:330` 初始化返回地址 a0=0；本次 release sdkconfig 没有 TASK_FUNCTION_WRAPPER，直接返回无效地址。启用 wrapper 的配置则在该文件 :135–145 显式 abort。因此不能把当前配置错误说成 wrapper 路径，但两者都不能正常保留错误页面。建议失败后合法删除自身任务或停驻，回归应让 host 也拒绝任务正常返回。
2. **Ready 后更换 enrollment 会让实际上传身份与持久 scope 不一致。** `voice.cpp:124` 捕获旧 request_scope；`voice_settings.h` 允许 Ready 暂停并切配置；恢复只更新 mailbox_scope，不改已有录音来源。`voice.cpp:154–161` 以旧 scope 写 pending，却调用没有 expected scope 的 foreground Upload，让 VoiceHttp 使用新凭据上传。后续已读会存入旧 scope，而 mailbox 仅处理新 scope：新服务的同一消息可再次出现在 Inbox，重启也不能按新 scope 恢复 pending。所有前台 Upload/Status/Retry/Page 应与 request_scope 使用一致凭据快照校验；普通 route 切换同 scope 应仍能继续。主线程已接受这一修复方向。

上述行号为首次审阅时位置。最终源码确认 `voice.cpp:337` 调用 `vTaskDelete(nullptr)`；所有前台 Upload/Status/Retry 及恢复/周期 Status 均传 `&request_scope`，页面下载新增可选 scope 并传至 VoiceHttp 的同凭据快照校验。普通同 scope 路由切换仍允许。

已亲读失败与成功证据：
- `/tmp/my-bot-async-read-corrupt-red.log` 的场景表面断言曾通过，但 host 最后报告 `TASK_ERROR: FreeRTOS task returned` 并非零退出；修复后 corrupt-start 正常完成且没有网络请求。
- `/tmp/my-bot-async-read-scope-red.log` 明确 `expected=4 actual=5`，对应期待 UploadFailed 却到 Stored，即旧录音已被新凭据上传；修复后拒绝上传、保留原 scope pending，恢复原 enrollment 后可重试。
- `/tmp/my-bot-async-read-actor.log` 全17个独立进程通过且全部任务 join。检查新 host 仅让 scope-ready 的外部音频边界快速填满到 Ready；生产 phase/gate/scope校验不被替换，恢复后等待新的 generation 再模拟新页面点击。本人另外亲自执行最终二进制的 `corrupt-start`、`scope-ready`，均 PASS。
- `/tmp/my-bot-async-read-remote-final.log` 通过；阅读新增前台请求与真实 page transport 绑定测试，scope mismatch 时不创建 HTTP client。
- `/tmp/my-bot-async-read-build-final.log` 最终IDF固件链接及镜像生成成功，app余量11%。未烧录。

## 实际阅读与检查

已阅读本轮裁决、目标 actor/mailbox/journal/receipt/scope/settings/transport/HTTP/UI/character/logic/公共头及 CMake 接线，逐段检查 actor 相对基线的差异；对照本轮基线而非把既有动画、通知和计时变更归因本任务。后端基线目录未含 server/store，因此以本轮边界任务所有权及目标函数差异限定审阅：排除头解析、认证顺序、参数化 Inbox 查询，没有扩展审核既有通知实现。

- actor 独占前台状态与 journal 写入；mailbox worker 的工作项和值结果独立拥有缓冲，只做网络；network lane 在 HTTP 析构返回后释放，不等待 actor 消费结果。
- 本地 mark_read 原子合并 receipt 与清 pending；随后 close 的 clear_pending 为无写入 no-op。失败保留页，16/17 在 journal 层不驱逐。Read 确认只删除精确 scope+id，删除失败保留重试，Recording/Preparing 不消费确认而避免非必要 NVS 写入。
- Inbox 消费核对 Idle/generation/revision/config epoch/scope 和本地 receipt；配置变更清旧未读入口，scope 更新在 handle_hold 前。两个 lane 各自释放，设置 Pending 不再开始新网络工作，正常输入不受 mailbox lane 阻塞。
- 新服务只在已认证 Inbox 解析最多16项/527字节排除头；重复header、畸形及超限拒绝；SQL 参数绑定且保留 device/status/read/rowid 条件。无头行为不变，排除不写已读。
- 核对真实 UI 保存失败截图，并从字体 cmap 计算确认「保存失败」四字均存在。错误标题可见，原图和已读按钮仍可见。
- 亲读 permanent-build.log 的15个独立进程结果及对应 scenarios/fixture/UI/host 源码：真实 LVGL 事件连接生产 voice_action/actor；不是旧 action stub。亲自执行现有 `/tmp/my-bot-async-read-probe/permanent/build/actor read-hold`，PASS，Read 未放行时可录音，所有host任务join。
- 亲读 core、journal（886 assertions）、旧UI、完整IDF、真实HTTPS23项、remote边界测试日志。以上其余结果是亲读产物，不声称全部由本人复跑。

## 注释审查产物

遵循独立 comment-reviewer 指令，仅提出注释改动，不修改业务。审核者实际产品删改0行；主线程已应用建议，实际删除测试行尾注释3行，新增产品注释5行保留。已读回确认三个旁白均删除。应用的差异：

```diff
--- voice_bridge/tests/test_inbox_exclusions.py
+++ voice_bridge/tests/test_inbox_exclusions.py
@@
-    assert exchange(receiver, [A])[1]["request_id"] == B  # dismissing B is not Read
+    assert exchange(receiver, [A])[1]["request_id"] == B
@@
-    storage.put("d" * 32, DEVICE, wav())  # not replied
+    storage.put("d" * 32, DEVICE, wav())
@@
-    replied(storage, B)  # intentionally reverse lexicographic ID order
+    replied(storage, B)
```

前一条的实际行为只是重复 GET，并没有模拟 UI Close；后两条是可由步骤与断言直接读出的旁白。没有需要借此改变逻辑的 MUST KILL 结构目标。

保留例外：journal_v1 的版本/字段布局两行、scope v1 编码一行属于持久化格式约束；NVS错误/readback两行有真实SDK依据（nvs.h:37、:340 的 REMOVE_FAILED 契约说明值可能已写入且初始化后完成更新），并且明确不声称物理断电测试。scope独立hash向量来源、边界测试替身范围说明保留，避免读者误以为运行了真实ESP HTTP。settings测试既有四处注释及UI旧namespace注释不属于本轮新增，未清理。未对既有通知、字体生成注释、README prose作全库清理。

## 验收边界与缺口

- 当前 sdkconfig 为 `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`；两个 HTTP 实例可同时握手，新增 mailbox 任务/队列也增加内部内存占用。没有真实硬件并发TLS峰值、录音连续性或栈余量证据，不能由host测试/链接成功推断可部署。本项目前是实机验收缺口，不是已证实内存故障。
- Inbox 2048 TX 与最大527字节头在边界替身验证；真实ESP-IDF发送、LAN与公网代理透传仍未实测。后端真实TLS测试证明Python Handler路径，不能替代设备TLS。
- host NVS覆盖返回值与跨进程提交，无法证明满配置24KiB分区、闪存撕裂/断电恢复；日志已诚实区分。坏日志启动已补会拒绝任务直接返回的host回归；物理芯片的异常路径没有实测。
- 最终17项actor suite已补可变scope的Ready录音和坏journal启动；仍不覆盖第17条真实UI容量失败、保留旧页面后的完整scope切换交互、两个lane同时占用时的真实设置交错。journal/gate/HTTP单元与源码检查覆盖部分性质，不能等同所有贯通场景。
- enrollment相同的LAN/公网同库是部署约束，复用相同token/CA指向另一库不能识别；旧scope仍占总16条，CA/token轮换隔离旧条目。均是裁决接受边界，不另报缺陷。

已完成一次定向Nowledge历史搜索（my-bot 圆屏 已读 异步 actor），仅用于确认既有显式已读背景，不作实现成功证据。
