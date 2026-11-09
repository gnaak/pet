"""테마 미리보기 - 부드러운(안티에일리어싱) 그림으로 화면 3종 × 라이트/다크를 그린다.

- 우파루파는 도형으로 4배 크게 그린 뒤 줄여서 테두리가 부드럽다 (axolotl_frames 는 기기용 데이터 만들 때도 쓴다)
- 글꼴: 숫자 Rajdhani, 한글 Pretendard (둘 다 SIL OFL, tools/fonts)
- 주가 · 날씨는 PC 에서 받은 실제 값 (scratch 경로의 q_*.json, 없으면 예시 값)

실행: python tools/themes.py [주가 json 폴더]  →  tools/themes.png
"""

import json
import math
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "tools" / "fonts"
KST = timezone(timedelta(hours=9))
SS = 4  # 슈퍼샘플 배율

MODES = {
    "dark": dict(bg=(0, 0, 0), fg=(242, 242, 245), sub=(150, 150, 160), grid=(48, 48, 56)),
    "light": dict(bg=(250, 247, 242), fg=(28, 28, 34), sub=(110, 110, 120), grid=(222, 218, 210)),
}
UP, DOWN = (240, 68, 82), (49, 130, 246)  # 오르면 빨강 · 내리면 파랑 (국내 증권 앱 관례)


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


NUM = lambda s: font("Rajdhani-Bold.ttf", s)       # noqa: E731
NUM2 = lambda s: font("Rajdhani-SemiBold.ttf", s)  # noqa: E731
KO_B = lambda s: font("Pretendard-Bold.otf", s)    # noqa: E731
KO_S = lambda s: font("Pretendard-SemiBold.otf", s)  # noqa: E731
KO_M = lambda s: font("Pretendard-Medium.otf", s)  # noqa: E731


# ── 우파루파 (128x112) ───────────────────────────────────
AW, AH = 128, 112
OUT = (201, 120, 150)
BODY = (255, 223, 231)
BELLY = (248, 201, 213)
GILL = (255, 170, 196)
GILL_TIP = (255, 210, 222)
INK = (42, 28, 34)
BLUSH = (255, 170, 190)


def _pts_ellipse(cx, cy, rx, ry, rot=0.0, n=64):
    c, s = math.cos(rot), math.sin(rot)
    out = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        out.append(((cx + x * c - y * s) * SS, (cy + x * s + y * c) * SS))
    return out


def _pts_superellipse(cx, cy, a, b, p=2.4, n=96):
    out = []
    for i in range(n):
        t = 2 * math.pi * i / n
        ct, st = math.cos(t), math.sin(t)
        x = a * math.copysign(abs(ct) ** (2 / p), ct)
        y = b * math.copysign(abs(st) ** (2 / p), st)
        out.append(((cx + x) * SS, (cy + y) * SS))
    return out


def _smooth(points, steps=8):
    """Catmull-Rom 로 점들을 부드러운 선으로"""
    pts = [points[0]] + points + [points[-1]]
    out = []
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for k in range(steps):
            t = k / steps
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) * SS for j in (0, 1)))
    out.append((points[-1][0] * SS, points[-1][1] * SS))
    return out


TOP, BOTTOM = 15.0, 103.0  # 몸 위 · 아래 끝


def _se_half(y, cy, a, b, p):
    t = abs(y - cy) / b
    return 0.0 if t >= 1 else a * (1 - t ** p) ** (1 / p)


def body_half_width(y):
    """인형처럼 위는 둥근 지붕, 아래로 갈수록 자연스럽게 통통해지는 달걀 모양 (허리가 들어가지 않는 하나의 곡선)"""
    cy, b = 60.0, 44.0
    p = 2.65 if y < cy else 2.3  # 위쪽은 덜 둥글게(평평하게), 아래쪽은 둥글게
    a = 39.5 + 0.13 * (y - cy)  # 아래로 갈수록 폭이 조금씩 넓어진다
    return _se_half(y, cy, a, b, p)


def _pts_body(cx, n=120):
    ys = [TOP + (BOTTOM - TOP) * i / n for i in range(n + 1)]
    right = [((cx + body_half_width(y)) * SS, y * SS) for y in ys]
    left = [((cx - body_half_width(y)) * SS, y * SS) for y in reversed(ys)]
    return right + left


def axolotl(face="idle", sway=0.0):
    """사진 속 인형 - 콩알 눈 · 아래로 통통한 몸 · 펠트 방울 아가미 · 가는 물결 입"""
    big = Image.new("RGBA", (AW * SS, AH * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    cx = AW / 2
    edge = (226, 160, 178)          # 몸 테두리 (밝은 배경 · 검은 배경 둘 다에서 실루엣이 보이게 옅게)
    body_c, shade_c = (253, 229, 235), (249, 218, 227)
    gill_c, gill_rim, gill_hi = (243, 160, 182), (226, 128, 156), (249, 196, 210)
    lw = int(1.7 * SS)

    # 팔 · 발 (몸 뒤에 그려서 동그란 끝만 보이게)
    for side in (-1, 1):
        ay = 67  # 팔은 아가미와 조금 떨어져서 - 옆으로 뻗지 않고 아래로 비스듬히 떨어진다
        rot = math.radians(125 if side < 0 else 55)
        px = cx + side * (body_half_width(ay) - 1.4) + 3.0 * math.cos(rot)  # 몸 안쪽으로 더 숨겨 끝만 보이게
        py = ay + 3.0 * math.sin(rot)
        d.polygon(_pts_ellipse(px, py, 7.0, 4.6, rot), fill=body_c, outline=edge, width=lw)
    for x in (cx - 23, cx + 23):  # 발은 양쪽으로 벌려서
        d.polygon(_pts_ellipse(x, 101, 8.5, 6.2), fill=body_c, outline=edge, width=lw)

    # 아가미 - 머리 뒤에 달린 펠트 방울 세 개. 몸보다 먼저 그려서 머리 밖으로 나온 부분만 보인다
    for i, gy in enumerate((35, 44, 53)):
        for side in (-1, 1):
            gx = cx + side * (body_half_width(gy) - 0.6) + side * (math.sin(math.radians(sway)) * 1.2 * (i + 1) / 3)
            r = 5.3 - i * 0.25
            d.polygon(_pts_ellipse(gx, gy, r, r), fill=gill_c, outline=gill_rim, width=int(1.2 * SS))
            d.polygon(_pts_ellipse(gx + side * 1.8, gy - 2.0, r * 0.38, r * 0.3), fill=gill_hi)

    # 몸 + 아래쪽 그늘
    body = _pts_body(cx)
    d.polygon(body, fill=body_c)
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).polygon(body, fill=255)
    from PIL import ImageChops, ImageFilter
    soft = Image.new("L", big.size, 0)
    ImageDraw.Draw(soft).polygon(_pts_ellipse(cx, 112, 56, 26), fill=255)
    soft = soft.filter(ImageFilter.GaussianBlur(6 * SS))  # 그늘은 띠가 아니라 번지게 (색은 그대로, 투명도만 번짐)
    big.paste(Image.new("RGBA", big.size, shade_c + (255,)), (0, 0), ImageChops.multiply(soft, mask))
    d.polygon(_pts_ellipse(cx - 13, 28, 11, 5.5, -0.2), fill=(255, 243, 246))  # 위쪽 윤기
    d.polygon(body, outline=edge, width=lw)

    # 눈 - 콩알처럼 작고 멀리
    ex, ey = (cx - 23, cx + 23), 46
    if face in ("idle", "sad"):
        for x in ex:
            d.polygon(_pts_ellipse(x, ey + (0.7 if face == "sad" else 0), 1.6, 1.9), fill=INK)
    elif face == "blink":
        for x in ex:
            d.line(_smooth([(x - 2.1, ey), (x, ey + 1.3), (x + 2.1, ey)]), fill=INK, width=int(1.5 * SS), joint="curve")
    elif face == "happy":
        for x in ex:
            d.line(_smooth([(x - 2.3, ey + 1.2), (x, ey - 1.3), (x + 2.3, ey + 1.2)]), fill=INK, width=int(1.6 * SS), joint="curve")

    # 입 - 눈 바로 아래에서 눈 아래까지 가는 물결 (슬플 때는 처진다)
    my = -12.0  # 눈과 가깝게 올린 만큼
    if face == "sad":
        mouth = [(cx - 12, 71 + my), (cx - 6, 68.4 + my), (cx, 67.8 + my), (cx + 6, 68.4 + my), (cx + 12, 71 + my)]
    else:
        mouth = [(cx - 23, 67.0 + my), (cx - 17.5, 69.3 + my), (cx - 8.5, 70.0 + my), (cx, 69.1 + my),
                 (cx + 8.5, 70.0 + my), (cx + 17.5, 69.3 + my), (cx + 23, 67.0 + my)]
    d.line(_smooth(mouth), fill=INK, width=int(1.6 * SS), joint="curve")
    return big.resize((AW, AH), Image.LANCZOS)


OUT_NAME = "axo_new.png"


def preview_axolotl():
    """큰 그림 하나 + 표정 다섯 개 + 화면 속 (다크 · 라이트) → tools/axo_new.png"""
    faces = [("idle", 0), ("idle", 5), ("blink", 0), ("happy", 0), ("sad", 0)]
    W = 4 * AW + 40 + 2 * 240 + 40
    sheet = Image.new("RGB", (W, 4 * AH + 40 + AH + 30), (90, 90, 96))
    big = Image.new("RGBA", (AW, AH), (250, 247, 242, 255))
    big.alpha_composite(axolotl())
    sheet.paste(big.resize((AW * 4, AH * 4), Image.LANCZOS).convert("RGB"), (20, 20))
    for i, (f, s) in enumerate(faces):
        tile = Image.new("RGBA", (AW, AH), (0, 0, 0, 255))
        tile.alpha_composite(axolotl(f, s))
        sheet.paste(tile.convert("RGB"), (20 + i * (AW + 8), 4 * AH + 40))
    now = datetime.now(KST)
    for j, mode in enumerate(("dark", "light")):
        sheet.paste(screen_pet(mode, now, (7.0, 4.8, 12.2)).convert("RGB"), (4 * AW + 40 + j * 250, 20))
    sheet.save(ROOT / "tools" / OUT_NAME)


# ── 날씨 아이콘 ──────────────────────────────────────────
def weather_icon(kind, size=26, mode="dark"):
    s = size * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if kind == "moon":
        d.ellipse([s * .14, s * .14, s * .86, s * .86], fill=(255, 214, 102))
        bg = MODES[mode]["bg"]
        d.ellipse([s * .34, s * .02, s * 1.02, s * .7], fill=bg + (255,))
    elif kind == "sun":
        d.ellipse([s * .28, s * .28, s * .72, s * .72], fill=(255, 190, 60))
        for i in range(8):
            a = math.pi * i / 4
            d.line([(s / 2 + math.cos(a) * s * .33, s / 2 + math.sin(a) * s * .33),
                    (s / 2 + math.cos(a) * s * .46, s / 2 + math.sin(a) * s * .46)], fill=(255, 190, 60), width=int(s * .07))
    return im.resize((size, size), Image.LANCZOS)


# ── 화면 그리기 ──────────────────────────────────────────
def text(d, xy, s, f, fill, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def screen_pet(mode, now, weather):
    m = MODES[mode]
    im = Image.new("RGBA", (240, 240), m["bg"] + (255,))
    d = ImageDraw.Draw(im)
    text(d, (120, 16), now.strftime("%H:%M"), NUM(64), m["fg"], "mt")
    wd = "월화수목금토일"[now.weekday()]
    text(d, (120, 86), f"{now.month}월 {now.day}일 {wd}요일", KO_S(15), m["sub"], "mt")
    # 날씨 한 줄 - 아이콘 · 지금 기온 · 최저/최고
    icon = weather_icon("moon" if now.hour >= 18 or now.hour < 6 else "sun", 24, mode)
    t_now, t_lo, t_hi = weather
    part1, part2 = f"{t_now:.0f}°", f"{t_lo:.0f}° / {t_hi:.0f}°"
    w1 = d.textlength(part1, font=NUM(24))
    w2 = d.textlength(part2, font=NUM2(17))
    total = 24 + 6 + w1 + 10 + w2
    x = (240 - total) / 2
    im.alpha_composite(icon, (int(x), 106))
    text(d, (x + 30, 118), part1, NUM(24), m["fg"], "lm")
    text(d, (x + 30 + w1 + 10, 119), part2, NUM2(17), m["sub"], "lm")
    im.alpha_composite(axolotl("idle"), ((240 - AW) // 2, 128))
    return im


def screen_stock(mode, q, title, subtitle, price_text, unit, footer):
    m = MODES[mode]
    im = Image.new("RGBA", (240, 240), m["bg"] + (255,))
    d = ImageDraw.Draw(im)
    price, prev, closes = q["price"], q["prev"], q["closes"]
    diff = price - prev
    up = diff >= 0
    col = UP if up else DOWN

    if title.isascii():
        text(d, (16, 14), title, NUM(30), m["fg"])
    else:
        text(d, (16, 16), title, KO_B(22), m["fg"])
    text(d, (16, 48), subtitle, KO_M(12), m["sub"])
    pet = axolotl("happy" if up else "sad").resize((60, 52), Image.LANCZOS)
    im.alpha_composite(pet, (168, 8))

    text(d, (14, 66), price_text, NUM(54), m["fg"])
    if unit:
        text(d, (16 + d.textlength(price_text, font=NUM(54)), 101), unit, KO_S(18), m["fg"], "ls")
    pct = diff / prev * 100
    arrow = "▲" if up else "▼"
    amount = f"{abs(diff):,.0f}" if unit else f"{abs(diff):.2f}"
    text(d, (16, 128), f"{arrow} {amount}   {pct:+.2f}%", KO_S(16), col)

    x0, x1, y0, y1 = 16, 224, 156, 210
    if q.get("candles"):
        # 1분봉 캔들 - 최근 40개, 캔들마다 오르면 빨강 · 내리면 파랑. 전날 종가는 범위 안일 때만 점선
        cs = q["candles"][-CANDLES:]
        lo, hi = min(c[2] for c in cs), max(c[1] for c in cs)
        span = (hi - lo) or 1
        Y = lambda v: y1 - (v - lo) / span * (y1 - y0)  # noqa: E731
        if lo <= prev <= hi:
            py = round(Y(prev))
            for x in range(x0, x1, 6):
                d.line([(x, py), (x + 2, py)], fill=m["grid"], width=1)
        left = x0 + (x1 - x0 - CANDLES * 5) // 2 + (CANDLES - len(cs)) * 5
        for k, (o, h, l, c) in enumerate(cs):
            cx = left + k * 5
            colr = UP if c >= o else DOWN
            d.line([(cx + 1, round(Y(h))), (cx + 1, round(Y(l)))], fill=colr, width=1)
            top, bot = round(Y(max(o, c))), round(Y(min(o, c)))
            d.rectangle([cx, top, cx + 2, max(bot, top)], fill=colr)
        text(d, (x1, 146), "1분봉", KO_M(11), m["sub"], "rs")
    else:
        # 장중 그래프 - 전날 종가 점선 + 선 + 아래 옅은 면
        lo, hi = min(closes + [prev]), max(closes + [prev])
        span = (hi - lo) or 1
        pts = [(x0 + (x1 - x0) * i / (len(closes) - 1), y1 - (c - lo) / span * (y1 - y0)) for i, c in enumerate(closes)]
        py = y1 - (prev - lo) / span * (y1 - y0)
        for x in range(x0, x1, 6):
            d.line([(x, py), (x + 3, py)], fill=m["grid"], width=1)
        big = Image.new("RGBA", (240 * SS, 240 * SS), (0, 0, 0, 0))
        bd = ImageDraw.Draw(big)
        bd.polygon([(x * SS, y * SS) for x, y in pts] + [(x1 * SS, y1 * SS), (x0 * SS, y1 * SS)], fill=col + (46,))
        bd.line([(x * SS, y * SS) for x, y in pts], fill=col + (255,), width=int(2 * SS), joint="curve")
        im.alpha_composite(big.resize((240, 240), Image.LANCZOS))
    text(d, (16, 222), footer, KO_M(11), m["sub"])
    return im


CANDLES = 40


def load_quote(folder, symbol, fallback):
    """m_<종목>.json (하루 시세: 전날 종가) + c_<종목>.json (1분봉) 이 있으면 그것으로"""
    mp, cp = Path(folder) / f"m_{symbol}.json", Path(folder) / f"c_{symbol}.json"
    if not (mp.exists() and cp.exists()):
        return fallback
    meta = json.loads(mp.read_text())["chart"]["result"][0]["meta"]
    q = json.loads(cp.read_text())["chart"]["result"][0]["indicators"]["quote"][0]
    candles = [(o, h, l, c) for o, h, l, c in zip(q["open"], q["high"], q["low"], q["close"]) if None not in (o, h, l, c)]
    return dict(price=meta["regularMarketPrice"], prev=meta["chartPreviousClose"], closes=[c[3] for c in candles],
                candles=candles, time=datetime.fromtimestamp(meta["regularMarketTime"], KST))


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    now = datetime.now(KST)
    soxl = load_quote(folder, "SOXL", dict(price=45.97, prev=40.87, closes=[41, 43, 44, 45, 45.97], time=now))
    hynix = load_quote(folder, "000660.KS", dict(price=1136000, prev=1132000, closes=[1130000, 1136000], time=now))
    weather = (7.0, 4.8, 12.2)

    cols = []
    for mode in ("dark", "light"):
        cols.append([
            screen_pet(mode, now, weather),
            screen_stock(mode, soxl, "SOXL", "반도체 3배 레버리지 · 미국", f"${soxl['price']:.2f}", "",
                         f"{soxl['time'].month}월 {soxl['time'].day}일 {soxl['time']:%H:%M} 기준 · 장 마감"),
            screen_stock(mode, hynix, "SK하이닉스", "000660 · 코스피", f"{hynix['price']:,.0f}", "원",
                         f"{hynix['time'].month}월 {hynix['time'].day}일 {hynix['time']:%H:%M} 기준"),
        ])
    gap, scale = 16, 2
    sheet = Image.new("RGB", (3 * 240 + 4 * gap, 2 * 240 + 3 * gap), (90, 90, 96))
    for r, row in enumerate(cols):
        for c, im in enumerate(row):
            sheet.paste(im.convert("RGB"), (gap + c * (240 + gap), gap + r * (240 + gap)))
    sheet.resize((sheet.width * scale, sheet.height * scale), Image.LANCZOS).save(ROOT / "tools" / "themes_v2.png")
    print("tools/themes.png (위 다크 · 아래 라이트 / 왼쪽부터 펫 · SOXL · SK하이닉스)")


if __name__ == "__main__":
    main()
