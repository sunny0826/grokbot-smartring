"""Explicit, checksum-pinned font preparation; normal serving only reads cache."""
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request

FONT_URL = ('https://raw.githubusercontent.com/notofonts/noto-cjk/'
            '523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/SimplifiedChinese/'
            'NotoSansCJKsc-Regular.otf')
FONT_SHA256 = '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'


def font_path():
    root = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache'))
    return root / 'my-bot' / 'NotoSansCJKsc-Regular.otf'


def preload_font():
    target = font_path()
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == FONT_SHA256:
        return target
    with urllib.request.urlopen(FONT_URL, timeout=60) as response:
        data = response.read(24 * 1024 * 1024 + 1)
    if hashlib.sha256(data).hexdigest() != FONT_SHA256:
        raise ValueError('font_checksum_mismatch')
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        temporary.replace(target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return target
