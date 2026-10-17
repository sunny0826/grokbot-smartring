# 手机网页配网设计

2026-10-17，用户明确要求在已验收的0.8.0圆屏配网之上增加手机网页配网。
基线HEAD183a400及当前大量未提交实现；保留已有改动，无提交/推送授权。

## 用户流程

Wi-Fi图标进入“手机配网/圆屏输入”方式页。手机配网先初扫，再显示临时WPA2热点的
加入二维码；下一页是固定本地网页URL二维码，两页都有文字备用及退出。
手机连接后在网页选网、输入密码，连接成功保存；失败可重试，原圆屏键盘保留。
不依赖App、外网、DNS自动弹窗或WebBluetooth。给操作手机本身的热点配网，
用另一台手机或圆屏键盘。

## 选型

按architect/arena执行两个同宿主模型独立候选、候选后独立评判；协调者已完整阅读。
两者收敛到相同主结构，并比较连续APSTA和提交后纯STA等不同结构，不称跨模型实证胜出。
选B为实施规格，吸收A初扫后公布入口、30秒成功窗口、不持锁退出HTTP的约束。
根增加双页二维码，避免手输临时密码和URL；评判支持。保留现有STOP+event fence，
试连/重扫时热点可短断，同一会话保持热点名称/密钥/IP；网页只重试GET，圆屏始终显示结果。
不为了宣称不断线而替换已验收的旧事件隔离机制。

## 所有权与接口

gadget任务仍唯一拥有scan/join/save、临时AP配置和HTTP生命周期；voice安全边界暂停。
Screen/Phone/USB共享lease。Phone进入Connected后仍驱动owner，30秒结果窗口或
绝对5分钟期限后先使会话失效，解锁退出HTTP，再关闭AP并恢复最终应保留的STA，最后释放lease。
保存成功后的清理不得回滚旧凭据。HTTP和LVGL回调只发意图/读快照。

共享接口在wifi_setup.h：Method、PhonePhase、只供屏幕的PhoneDisplay，以及
WifiPhoneCommand。Phone POST在同一mutex内检查operation、revision、阶段、
待处理命令和scan_revision/index/password，接受时立即推进revision（包括cancel/close）。
重复旧版本拒绝；不用request_id或payload摘要缓存。网页不自动重试POST，冲突只GET刷新，
不把版本变化冒充“自己的请求已被接受”，只显示设备权威状态。

HTTP采用本地IDF5.5.3 esp_http_server，不复制HTTP parser；open_fn校验AP本地IP/peer，
每个handler再校验Host/Origin/session token。监听器本身仍是wildcard，准确声明业务准入边界。
AP/STA子网重叠时撤销网页服务并使用圆屏结果。随机热点密码与128位token分开生成，
token不进URL、密码不进日志/存储/返回；SSID以HEX传输并由textContent显示。
静态HTML/CSS/JS嵌入flash，不加载CDN，body有界。每个socket通过recv/send override
使用从接入开始的3秒绝对期限，单次等待不超过1秒，覆盖header/body/response；
持续少量进展不会续期，避免IDF parser或partial send拖住httpd_stop。

## 分工和依赖

共享契约先固定，再分离写入：backend拥有wifi_setup.cpp、driver.cpp/.h、owner.h及其测试；
web拥有HTTP/协议与静态资源及对应测试；UI拥有圆屏方式/二维码页面与真实LVGL测试。
根拥有共享wifi_setup.h和http.h、CMake/font/sdkconfig、版本、文档、总构建和硬件。
不并行写共享build或操作设备，不回滚他人改动。使用现有工作区保留未提交基线。

先证明生产owner/driver与协议边界，再真实浏览器和LVGL，最后build容量、app-flash与
原网络上线。真实手机扫码、无互联网提示/射频重连和异常矩阵仍以实际操作为准。
