#!/usr/bin/env python3
"""Package actual host LVGL captures into a self-contained animation preview (Pillow required)."""
import argparse
import base64
import io
import json
from pathlib import Path

from PIL import Image

SKINS = ("grokbot", "pika", "rowlet")
STAGES = ("recording", "uploading", "waiting", "claimed", "replied", "review", "loading")


def encode(frames, animated=False):
    output = io.BytesIO()
    options = dict(save_all=True, append_images=frames[1:], duration=100, loop=0) if animated else {}
    frames[0].save(output, format="WEBP", lossless=True, **options)
    return "data:image/webp;base64," + base64.b64encode(output.getvalue()).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    images = {}
    for skin in SKINS:
        images[skin] = {}
        for stage in STAGES:
            paths = [args.work_dir / "fusion-frames" / f"{skin}-{stage}-{i:02d}.ppm" for i in range(24)]
            frames = [Image.open(path).convert("RGB") for path in paths]
            assert all(frame.size == (360, 360) for frame in frames)
            images[skin][stage] = {"motion": encode(frames, True), "still": encode(frames[12:13])}
        unread = encode([Image.open(args.work_dir / "fusion-frames" / f"{skin}-unread.ppm").convert("RGB")])
        images[skin]["unread"] = {"motion": unread, "still": unread}
    reply = encode([Image.open(args.work_dir / "reply-first.png").convert("RGB")])
    html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>圆屏 · 角色与语音流程</title><style>
*{box-sizing:border-box}body{margin:0;background:#f7f4ec;color:#272720;font:16px system-ui,sans-serif}
main{max-width:960px;margin:0 auto;padding:48px 24px}small{color:#777367;letter-spacing:.06em}
h1{font-size:clamp(28px,5vw,46px);font-weight:650;letter-spacing:-.04em;margin:12px 0}
p{color:#777367;line-height:1.7}nav{display:flex;flex-wrap:wrap;gap:10px;margin:28px 0}
button{font:inherit;cursor:pointer;padding:10px 18px;border:1px solid #cdc6b7;border-radius:30px;background:transparent;color:inherit}
button[aria-pressed=true]{background:#292921;color:#fff;border-color:#292921}#pause{margin-left:auto}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:28px 24px}figure{margin:0;text-align:center}
img{display:block;width:100%;max-width:260px;aspect-ratio:1;border-radius:50%;margin:auto}
figcaption{margin:14px 0 0;font-weight:550}figcaption span{display:block;color:#777367;font-size:14px;font-weight:400;margin-top:5px}
.reading{margin-top:48px;padding-top:32px;border-top:1px solid #ded8cb;display:flex;align-items:center;gap:40px}.reading img{width:220px;margin:0}.reading div{max-width:420px}
footer{font-size:13px;line-height:1.8;color:#777367;margin-top:40px}
@media(max-width:620px){main{padding:32px 16px}.grid{grid-template-columns:repeat(2,1fr);gap:24px 12px}button{padding:8px 12px}.reading{display:block}.reading img{margin:auto}.reading div{text-align:center}}
</style><main><small>MY BOT · 0.12.3 · 2026.10.17</small><h1>点头像，读消息。</h1>
<p>新消息留一个蓝点。进行中，主要看动画。</p>
<nav aria-label="选择角色"><button data-skin="grokbot" aria-pressed="true">Grokbot</button><button data-skin="pika" aria-pressed="false">Pika</button><button data-skin="rowlet" aria-pressed="false">Rowlet</button><button id="pause" aria-pressed="false">暂停动画</button></nav>
<div class="grid">
<figure><img data-stage="loading" alt="读取回复时放大后的角色与环绕动画"><figcaption>正在读取回复<span>动画放大约 45%</span></figcaption></figure>
<figure><img data-stage="recording" alt="录音时角色随音量轻轻伸缩"><figcaption>正在录音<span>跟着声音动</span></figcaption></figure>
<figure><img data-stage="uploading" alt="上传时持续播放原有环绕动画"><figcaption>正在上传<span>Orbit · 环绕</span></figcaption></figure>
<figure><img data-stage="waiting" alt="等待接单时持续播放原有环绕动画"><figcaption>等待接单<span>持续环绕直到状态改变</span></figcaption></figure>
<figure><img data-stage="claimed" alt="处理时变成三个依次脉动的点"><figcaption>正在思考<span>Thinking · 三点轮动</span></figcaption></figure>
<figure><img data-stage="replied" alt="收到回复时弹出原有蓝色通知角标"><figcaption>收到回复<span>Notify · 蓝点弹出</span></figcaption></figure>
<figure><img data-stage="review" alt="需核对时角色静止"><figcaption>需要核对<span>停下来，说明情况</span></figcaption></figure>
</div><section class="reading"><img data-stage="unread" alt="首页只有带未读蓝点的角色，不再显示未读按钮"><div><h2>点击带蓝点的头像，就能查看。</h2><p>不再显示未读按钮。点击区域跟着角色移动，滑动换角色、长按录音照常。</p></div></section>
<section class="reading"><img src="REPLY_IMAGE" alt="实际渲染的中文回复阅读界面"><div><h2>回复到了，就看内容。</h2><p>前台等待时仍会立即打开回复。阅读时，角色让出空间；返回首页后恢复原来的互动。</p></div></section>
<footer>真实固件绘图与 LVGL 界面的主机预览，三种角色 × 七个测试状态，另附首页未读静态截图。每段采样 2.4 秒、每秒 10 帧并循环播放；“收到回复”在每次循环开头重放提示。状态、音量及回复文字为测试数据，不是实机录像，也不表示云端处理耗时或设备帧率。预览图片本身不是可操作的设备界面。<br><a href="reply-animation.md">实现与验证记录</a></footer></main>
<script>
const images=IMAGE_DATA;let skin='grokbot',paused=matchMedia('(prefers-reduced-motion: reduce)').matches;
const pause=document.querySelector('#pause');
function update(){document.querySelectorAll('[data-stage]').forEach(el=>el.src=images[skin][el.dataset.stage][paused?'still':'motion']);pause.textContent=paused?'播放动画':'暂停动画';pause.setAttribute('aria-pressed',String(paused));}
document.querySelectorAll('[data-skin]').forEach(button=>button.addEventListener('click',()=>{skin=button.dataset.skin;document.querySelectorAll('[data-skin]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));update();}));
pause.addEventListener('click',()=>{paused=!paused;update();});update();
</script></html>'''
    html = html.replace("REPLY_IMAGE", reply).replace("IMAGE_DATA", json.dumps(images, separators=(",", ":")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(html)
    print(f"Preview: {args.output} ({args.output.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
