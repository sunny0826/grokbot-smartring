"""Fixed-font Chinese reply pages in the device's immutable RGB565 format."""
from dataclasses import dataclass
import struct
import hashlib
from pathlib import Path
import zlib

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

from .assets import FONT_SHA256
from .store import RequestError

WIDTH, HEIGHT = 272, 168
FONT_SIZE, LINE_HEIGHT, MARGIN = 22, 28, 12
MAX_PAGES = 8


@dataclass(frozen=True)
class RenderedReply:
    pages: tuple[bytes, ...]
    warning: str | None = None


class RenderPages:
    def __init__(self, font_path):
        if hashlib.sha256(Path(font_path).read_bytes()).hexdigest() != FONT_SHA256:
            raise ValueError("font_checksum_mismatch")
        self.font = ImageFont.truetype(str(font_path), FONT_SIZE)
        with TTFont(font_path) as font:
            self.characters = set(font.getBestCmap())
        if ord('□') not in self.characters:
            raise ValueError("replacement_glyph_missing")

    def __call__(self, text):
        missing = any(c != '\n' and ord(c) not in self.characters for c in text)
        text = ''.join(c if c == '\n' or ord(c) in self.characters else '□' for c in text)
        lines, line = [], ''
        for c in text:
            if c == '\n':
                lines.append(line)
                line = ''
            elif line and self.font.getlength(line + c) > WIDTH - 2 * MARGIN:
                lines.append(line)
                line = c
            else:
                line += c
        lines.append(line)
        per_page = (HEIGHT - 2 * MARGIN) // LINE_HEIGHT
        count = (len(lines) + per_page - 1) // per_page
        if count > MAX_PAGES:
            raise RequestError(400, "reply_too_many_pages")
        pages = []
        for index in range(count):
            canvas = Image.new('RGB', (WIDTH, HEIGHT), (236, 231, 219))
            draw = ImageDraw.Draw(canvas)
            for row, line in enumerate(lines[index * per_page:(index + 1) * per_page]):
                draw.text((MARGIN, MARGIN + row * LINE_HEIGHT), line, font=self.font,
                          fill=(40, 40, 35), anchor='lt')
            payload = bytearray(WIDTH * HEIGHT * 2)
            rgb = canvas.tobytes()
            for i in range(WIDTH * HEIGHT):
                r, g, b = rgb[i * 3:i * 3 + 3]
                struct.pack_into('<H', payload, i * 2, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
            header = struct.pack('<4sHHHHI', b'VPG1', WIDTH, HEIGHT, index, count, zlib.crc32(payload))
            pages.append(header + payload)
        return RenderedReply(tuple(pages), 'unsupported_characters_replaced' if missing else None)
