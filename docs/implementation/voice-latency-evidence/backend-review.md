# 后端独立只读复核回执

结论：在本次授权范围内，未发现可触发并可归因于本轮改动的 P1/P2 缺陷。可进入主协调者的集成验收；此结论不代表已验证实体提速或云端执行行为。

审查位置：`/Users/guoxudong/codes/my-bot`，基线 `759f03c`。审查 `voice_bridge/**` 工作区 diff，另读新增 `connections.py`、`tests/test_latency.py`、`LATENCY.md`，并按需读取其既有调用者和测试 fixture。对照 `docs/implementation/voice-latency-next.md` 与 `/tmp/my-bot-latency-work/backend-result.md`。已做一次针对 my-bot voice latency 的 Nowledge 检索，恢复保留 claim、默认关闭正文预送和不以代理指标声称实体提速的先前约束。

## 关键核验

- Store 的状态检查、等待配额登记、Condition.wait 及醒来后的重新读取共用原 RLock。wait 释放锁；状态写入在数据库 context 成功提交后 notify。回滚不会发出提交通知。没有独立事件缓存造成的先检查后订阅窗口。
- 设备归属在开始等待前检查；状态、read 与页面 bytes 在同一锁内读取。response 端点不自动 mark_read；大写 HTTP ID 在路由中规范化为小写。等待退出的 finally 归还每设备名额；stop_responses/close 的标志检查避免醒来再访问已关闭 DB。
- HTTP 的每次请求清除认证结果、附加响应头和完整请求标志，重新读取凭据。TE 被拒绝，重复或非法 Content-Length 被拒绝，成功上传须已读完声明长度，非上传正文被拒绝。仅成功完整已认证且明确 keep-alive 的请求可复用；错误关闭。
- Connections 对登记、TLS socket 替换、复用批准与配额计数使用同一锁；全局 8 / 每设备 2 与总请求线程 16 分离。连接身份变化不转移原名额且关闭；第 64 次请求、空闲读取、绝对寿命都有退出路径。绝对寿命用 shutdown 唤醒 socket 阻塞；移除取消 timer。
- 停服先禁止批准新复用，唤醒 response 等待者，再关闭登记的原始/TLS socket；包括未完成握手与空闲连接。未发现 Connections 锁与 Store 锁形成互相等待的路径。
- 通知正文选项默认关闭且要求既有 trigger 配置。启用时完整短转写与持久化版本/摘要一并发送，超过长度回退 ID，不截断、不泄漏 claim_id。摘要与转写同事务写入，不同正文覆盖被拒绝；旧记录迁移及旧 claim 重试补全元数据。routine 保留随机 claim_id、关联回执、执行前 claim 与权威正文核验约束。

## 独立执行结果

在主工作区执行：

```text
PYTHONPATH=voice_bridge/src voice_bridge/.venv/bin/python -m pytest -q voice_bridge/tests/test_latency.py
32 passed in 22.86s

git diff --check -- voice_bridge
exit 0
```

这 32 项覆盖状态已变化/登记后提交、超时和回滚、设备/全局等待配额、非法头及权限、VPG1 CRC 与 read 快照、等待中回复、旧摘要迁移、上传后复用、逐请求认证撤销、响应头/排除列表清理、持久配额及释放、空闲/绝对寿命/请求次数、服务关闭等待/空闲 TLS/未握手 TCP。它们是已有新增测试的独立重跑，没有新增或修改仓库测试。

## 限制

- 未重跑整个 133 项后端套件；完整套件结果来自实现回执，主协调者仍负责最终集成验收。
- 未连接生产数据库、调用真实 webhook/MCP、部署、更新云端 routine 或操作实体设备。
- 1 秒是 Condition 等待预算，不是包含 Store 锁竞争、磁盘提交或渲染在内的硬实时 HTTP 截止时间。任意 renderer/磁盘永久阻塞不在本次网络关闭保证内。
- 预送正文的“领取前不执行”由 Bot 任务约定维持，本轮没有新增云端工具执行隔离；选项保持默认关闭。后端测试不能证明云端代理始终遵守该约定。

无仓库文件写入、委派、部署或外部消息；仅生成本回执。
