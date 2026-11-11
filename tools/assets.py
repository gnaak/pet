"""기기에 넣을 그림 · 글자 데이터를 만든다 → src/assets.h

- 우파루파 · 날씨 아이콘: 부드러운(투명도 있는) 그림을 팔레트(최대 255색 + 투명도)로 줄여 담는다.
  기기는 켜질 때 · 라이트/다크가 바뀔 때 팔레트를 배경색과 섞어 쓴다
- 글자: 쓰는 글자만 골라 8비트 투명도로 담는다 (숫자 Rajdhani, 한글 Pretendard - 둘 다 SIL OFL)
- 설정 페이지 테마 그림(PNG)과 머리 아이콘

실행: python tools/assets.py  (그림 모양은 tools/themes.py 가 정한다)
"""

import base64
import io
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import themes  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "tools" / "fonts"
out = [
    "// tools/assets.py 가 만든 파일 - 손으로 고치지 않는다",
    "#pragma once",
    "#include <Arduino.h>",
    "",
    "struct Glyph { uint16_t cp; uint8_t w, h; int8_t x, y; uint8_t adv; uint32_t off; };",
    "struct Font { const Glyph* glyphs; uint16_t count; const uint8_t* data; int8_t ascent, descent; };",
    "struct SpriteSet { const uint8_t* pix; uint16_t w, h; uint8_t frames; const uint16_t* pal; const uint8_t* alpha; uint16_t palSize; };",
    "",
]
sizes = {}


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def bytes_c(name, data, per_line=32):
    lines = [f"const uint8_t {name}[{len(data)}] PROGMEM = {{"]
    for i in range(0, len(data), per_line):
        lines.append("  " + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    lines.append("};")
    return lines


# ── 그림 (팔레트 + 투명도) ────────────────────────────────
def emit_sprites(name, frames):
    w, h = frames[0].size
    strip = Image.new("RGBA", (w * len(frames), h), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        strip.paste(f, (i * w, 0))
    px = strip.load()
    for y in range(h):
        for x in range(strip.width):
            if px[x, y][3] < 6:
                px[x, y] = (0, 0, 0, 0)
    q = strip.quantize(colors=255, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette(rawmode="RGBA")
    n = len(pal) // 4
    idx = q.load()
    data = bytearray()
    for i in range(len(frames)):
        for y in range(h):
            for x in range(w):
                data.append(idx[i * w + x, y])
    used = max(data) + 1
    out.append(f"const uint16_t {name}_PAL[{used}] PROGMEM = {{" +
               ",".join(f"0x{rgb565(*pal[k * 4:k * 4 + 3]):04X}" for k in range(used)) + "};")
    out.append(f"const uint8_t {name}_ALPHA[{used}] PROGMEM = {{" + ",".join(str(pal[k * 4 + 3]) for k in range(used)) + "};")
    out.extend(bytes_c(f"{name}_PIX", data))
    out.append(f"const SpriteSet {name} = {{{name}_PIX, {w}, {h}, {len(frames)}, {name}_PAL, {name}_ALPHA, {used}}};")
    out.append("")
    sizes[name] = len(data) + used * 3


# ── 글자 ─────────────────────────────────────────────────
def emit_font(name, file, size, chars):
    f = ImageFont.truetype(str(FONTS / file), size)
    ascent, descent = f.getmetrics()
    glyphs, data = [], bytearray()
    for ch in sorted(set(chars)):
        x0, y0, x1, y1 = f.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        im = Image.new("L", (w, h), 0)
        if w and h:
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255, anchor="ls")
        glyphs.append((ord(ch), w, h, x0, y0, round(f.getlength(ch)), len(data)))
        data += im.tobytes()
    out.extend(bytes_c(f"{name}_DATA", data))
    out.append(f"const Glyph {name}_GLYPHS[{len(glyphs)}] PROGMEM = {{")
    out.extend(f"  {{{cp}, {w}, {h}, {x}, {y}, {a}, {o}}}," for cp, w, h, x, y, a, o in glyphs)
    out.append("};")
    out.append(f"const Font {name} = {{{name}_GLYPHS, {len(glyphs)}, {name}_DATA, {ascent}, {descent}}};")
    out.append("")
    sizes[name] = len(data) + len(glyphs) * 12


# ── 날씨 아이콘 (26px, 투명 배경) ─────────────────────────
WEATHER_KINDS = ["sun", "moon", "sun_cloud", "moon_cloud", "cloud", "fog", "rain", "snow", "thunder"]


def weather_icon(kind, size=26):
    S = size * 4
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    sun, moon = (255, 190, 60), (255, 214, 102)
    cloud, cloud_dark = (214, 220, 232), (168, 176, 192)

    def draw_sun(cx, cy, r):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=sun)
        for i in range(8):
            a = math.pi * i / 4
            d.line([(cx + math.cos(a) * r * 1.45, cy + math.sin(a) * r * 1.45),
                    (cx + math.cos(a) * r * 1.95, cy + math.sin(a) * r * 1.95)], fill=sun, width=int(S * .07))

    def draw_moon(cx, cy, r):
        m = Image.new("L", (S, S), 0)
        md = ImageDraw.Draw(m)
        md.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        md.ellipse([cx - r * .35, cy - r * 1.25, cx + r * 1.45, cy + r * .55], fill=0)
        im.paste(Image.new("RGBA", (S, S), moon + (255,)), (0, 0), m)

    def draw_cloud(ox, oy, s, col):
        for cx, cy, r in ((.36, .62, .2), (.56, .52, .26), (.74, .64, .18)):
            d.ellipse([ox + (cx - r) * s, oy + (cy - r) * s, ox + (cx + r) * s, oy + (cy + r) * s], fill=col)
        d.rounded_rectangle([ox + .16 * s, oy + .6 * s, ox + .92 * s, oy + .82 * s], radius=.11 * s, fill=col)

    if kind == "sun":
        draw_sun(S / 2, S / 2, S * .22)
    elif kind == "moon":
        draw_moon(S / 2, S / 2, S * .36)
    elif kind in ("sun_cloud", "moon_cloud"):
        (draw_sun if kind == "sun_cloud" else draw_moon)(S * .38, S * .36, S * (.17 if kind == "sun_cloud" else .26))
        draw_cloud(S * .08, S * .1, S * .92, cloud)
    elif kind == "cloud":
        draw_cloud(0, S * .02, S, cloud)
    elif kind == "fog":
        for i, y in enumerate((.36, .54, .72)):
            d.rounded_rectangle([S * (.12 + .08 * (i % 2)), S * y, S * (.88 - .08 * (i % 2)), S * (y + .09)], radius=S * .045, fill=cloud)
    elif kind in ("rain", "snow", "thunder"):
        draw_cloud(0, -S * .14, S, cloud_dark if kind == "thunder" else cloud)
        if kind == "rain":
            for x in (.34, .54, .74):
                d.line([(S * x, S * .72), (S * (x - .07), S * .92)], fill=(96, 165, 250), width=int(S * .07))
        elif kind == "snow":
            for x, y in ((.34, .78), (.54, .88), (.74, .78)):
                d.ellipse([S * (x - .05), S * (y - .05), S * (x + .05), S * (y + .05)], fill=(240, 244, 255))
        else:
            d.polygon([(S * .56, S * .62), (S * .4, S * .84), (S * .52, S * .84), (S * .44, S * .98), (S * .66, S * .74),
                       (S * .54, S * .74), (S * .62, S * .62)], fill=sun)
    return im.resize((size, size), Image.LANCZOS)


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def main():
    # 우파루파 - 큰 것 (펫 테마) · 작은 것 (주가 테마 오른쪽 위)
    emit_sprites("AXO", [themes.axolotl("idle"), themes.axolotl("idle", 5), themes.axolotl("blink"), themes.axolotl("happy")])
    emit_sprites("AXO_MINI", [themes.axolotl(f).resize((46, 40), Image.LANCZOS) for f in ("happy", "sad")])
    emit_sprites("WICON", [weather_icon(k) for k in WEATHER_KINDS])
    out.append("enum WeatherKind : uint8_t { " + ", ".join(f"W_{k.upper()}" for k in WEATHER_KINDS) + " };")
    out.append("")

    # 글자 - 화면에 나오는 글자만
    date_chars = "0123456789월일화수목금토요 "
    foot_chars = "0123456789월일:기준장마감· 불러오는중못받았어요다시시도해요분봉프리애터데이켓"
    emit_font("F_CLOCK", "Rajdhani-Bold.ttf", 64, "0123456789:-")
    emit_font("F_PRICE", "Rajdhani-Bold.ttf", 32, "0123456789.,$")
    emit_font("F_NAME_EN", "Rajdhani-Bold.ttf", 18, "SOXL")
    emit_font("F_TEMP", "Rajdhani-Bold.ttf", 24, "0123456789°-")
    emit_font("F_TEMP2", "Rajdhani-SemiBold.ttf", 17, "0123456789°/ -")
    emit_font("F_DATE", "Pretendard-SemiBold.otf", 15, date_chars)
    emit_font("F_CHG", "Pretendard-SemiBold.otf", 14, "▲▼0123456789.,+-% ")
    emit_font("F_NAME_KO", "Pretendard-SemiBold.otf", 14, "SK하이닉스")
    emit_font("F_WON", "Pretendard-SemiBold.otf", 15, "원")
    emit_font("F_FOOT", "Pretendard-Medium.otf", 11, foot_chars)

    # 설정 페이지 - 테마 그림 (다크, 112px) · 머리 아이콘
    from datetime import datetime
    now = datetime.now(themes.KST)
    import random
    def walk(start, step, n=44, seed=1):
        r, v, out = random.Random(seed), start, []
        for _ in range(n):
            o = v
            v = v + r.uniform(-step, step * 1.1)
            out.append((o, max(o, v) + r.uniform(0, step * .5), min(o, v) - r.uniform(0, step * .5), v))
        return out
    q_us = dict(price=45.97, prev=40.87, closes=[], candles=walk(45.0, 0.12), time=now)
    q_kr = dict(price=1136000, prev=1132000, closes=[], candles=walk(1130000, 1500, seed=4), time=now)
    shots = [
        themes.screen_pet("dark", now, (7.0, 4.8, 12.2)),
        themes.screen_stock2("dark", q_us, "SOXL", "$45.97", "", "예시"),
        themes.screen_stock2("dark", q_kr, "SK하이닉스", "1,136,000", "원", "예시"),
    ]
    for i, s in enumerate(shots):
        data = png_bytes(s.convert("RGB").resize((112, 112), Image.LANCZOS))
        out.extend(bytes_c(f"THUMB{i}", data))
        sizes[f"THUMB{i}"] = len(data)
    icon = themes.axolotl("idle").resize((32, 28), Image.LANCZOS)
    uri = "data:image/png;base64," + base64.b64encode(png_bytes(icon)).decode()
    out.append(f'const char AXO_ICON[] PROGMEM = "{uri}";')
    sizes["AXO_ICON"] = len(uri)

    (ROOT / "src" / "assets.h").write_text("\n".join(out) + "\n", encoding="utf-8")
    total = sum(sizes.values())
    print("src/assets.h", f"{total / 1024:.0f}KB", {k: f"{v / 1024:.1f}KB" for k, v in sizes.items()})


if __name__ == "__main__":
    main()
