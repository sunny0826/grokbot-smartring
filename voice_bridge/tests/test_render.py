import hashlib
import struct
import zlib

import pytest

from voice_bridge.assets import FONT_SHA256, font_path, preload_font
from voice_bridge.render import RenderPages
from voice_bridge.store import RequestError


def test_chinese_pages_header_crc_and_rgb565():
    renderer = RenderPages(font_path())
    reply = renderer('你' * 128)
    assert reply.warning is None and len(reply.pages) == 3
    for index, data in enumerate(reply.pages):
        magic, width, height, actual_index, count, crc = struct.unpack('<4sHHHHI', data[:16])
        assert (magic, width, height, actual_index, count) == (b'VPG1', 272, 168, index, 3)
        assert len(data) == 91408 and zlib.crc32(data[16:]) == crc
        pixels = struct.unpack('<45696H', data[16:])
        assert len(set(pixels)) > 10
        assert pixels[0] == ((236 >> 3) << 11) | ((231 >> 2) << 5) | (219 >> 3)


def test_missing_glyph_warns_and_does_not_silently_drop():
    renderer = RenderPages(font_path())
    missing = renderer('回复：\U0010ffff')
    assert missing.warning == 'unsupported_characters_replaced'
    assert missing.pages == renderer('回复：□').pages
    assert len(renderer('😀' * 128).pages) <= 8


def test_explicit_newline_page_limit():
    renderer = RenderPages(font_path())
    assert len(renderer('你\n' * 39 + '好').pages) == 8
    with pytest.raises(RequestError, match='reply_too_many_pages'):
        renderer('你\n' * 40 + '好')


def test_font_checksum_and_explicit_cache_preload(tmp_path, monkeypatch):
    monkeypatch.setenv('XDG_CACHE_HOME', str(tmp_path))
    target = font_path()
    target.parent.mkdir()
    target.write_bytes(b'invalid')
    with pytest.raises(ValueError, match='font_checksum_mismatch'):
        RenderPages(target)
    class Response:
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return None
        def read(self, limit):
            return b'bad download'
    monkeypatch.setattr('urllib.request.urlopen', lambda *args, **kwargs: Response())
    with pytest.raises(ValueError, match='font_checksum_mismatch'):
        preload_font()
    assert target.read_bytes() == b'invalid'
