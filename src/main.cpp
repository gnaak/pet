// pet 펌웨어 - GeekMagic SmallTV-Ultra 에 테마 화면 (펫 · SOXL · SK하이닉스) · 라이트/다크/자동
//   - 와이파이: 기기에 저장된 와이파이로 붙는다. 못 붙으면 "pet-setup" 와이파이를 띄워 폰으로 설정받는다
//   - 업데이트: http://<IP>/update 에서 다음 펌웨어를 선 없이 올린다 (이게 없으면 다음부터 분해해야 한다)
//   - 설정 페이지: http://<IP>/ - 테마 고르기 · 화면 밝기 모드 · 와이파이
//   - 그림 · 글자는 tools/assets.py 가 만든 src/assets.h (부드러운 그림을 투명도째 담아 배경과 섞어 그린다)
//   - 날씨: Open-Meteo(서울), 주가: Yahoo Finance 차트 - 둘 다 키 없이 HTTPS 로 받는다
//
// 화면 핀 · 켜는 순서는 SmallTV-Ultra 한국어 커펌(wonjj6768)의 브리지 펌웨어에서 확인한 값이다.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <SPI.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>

#include "assets.h"

namespace {

constexpr const char* VERSION = "v0.14";
constexpr const char* HOSTNAME = "pet";        // http://pet.local
constexpr const char* SETUP_AP = "pet-setup";  // 와이파이를 못 찾으면 띄우는 와이파이 이름 (비밀번호 없음)
constexpr uint32_t WIFI_WAIT_MS = 20000;

// ── 화면 (ST7789 240x240) ─────────────────────────────
constexpr int8_t LCD_DC = 0;
constexpr int8_t LCD_RST = 2;
constexpr int8_t LCD_BACKLIGHT = 5;  // LOW 일 때 켜진다
constexpr uint32_t LCD_SPI_HZ = 40000000;

Arduino_HWSPI bus(LCD_DC, -1 /* CS 없음 */, &SPI, true);
Arduino_ST7789 lcd(&bus, -1 /* 리셋은 직접 */, 0 /* 회전 */, true /* IPS */, 240, 240);

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GRAY = 0x8410;
constexpr uint16_t GREEN = 0x07E0;
constexpr uint16_t YELLOW = 0xFFE0;

void lcdCommand(uint8_t cmd, std::initializer_list<uint8_t> data = {}) {
  bus.writeCommand(cmd);
  for (uint8_t d : data) bus.write(d);
}

void initDisplay() {
  pinMode(LCD_BACKLIGHT, OUTPUT);
  digitalWrite(LCD_BACKLIGHT, LOW);
  bus.begin(LCD_SPI_HZ, SPI_MODE3);
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, HIGH); delay(120);
  digitalWrite(LCD_RST, LOW);  delay(120);
  digitalWrite(LCD_RST, HIGH); delay(120);

  bus.beginWrite();
  lcdCommand(0x11); delay(120);
  lcdCommand(0xB2, {0x1F, 0x1F, 0x00, 0x33, 0x33});
  lcdCommand(0x35, {0x00});
  lcdCommand(0x36, {0x00});
  lcdCommand(0x3A, {0x05});
  lcdCommand(0xB7, {0x00});
  lcdCommand(0xBB, {0x36});
  lcdCommand(0xC0, {0x2C});
  lcdCommand(0xC2, {0x01});
  lcdCommand(0xC3, {0x13});
  lcdCommand(0xC4, {0x20});
  lcdCommand(0xC6, {0x13});
  lcdCommand(0xD6, {0xA1});
  lcdCommand(0xD0, {0xA4, 0xA1});
  lcdCommand(0xD6, {0xA1});
  lcdCommand(0xE0, {0xD0, 0x08, 0x11, 0x08, 0x0C, 0x15, 0x39, 0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2D});
  lcdCommand(0xE1, {0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0B, 0x16, 0x14, 0x2F, 0x31});
  lcdCommand(0xE4, {0x1D, 0x00, 0x00});
  lcdCommand(0x21);
  lcdCommand(0x29);
  lcdCommand(0x2A, {0x00, 0x00, 0x00, 0xEF});
  lcdCommand(0x2B, {0x00, 0x00, 0x00, 0xEF});
  lcdCommand(0x2C);
  bus.endWrite();
  delay(10);
  lcd.setRotation(0);
  lcd.fillScreen(BLACK);
}

// 켜질 때 · 와이파이 설정 화면에만 쓰는 기본 글꼴 (영어)
void centerText(const String& text, int16_t y, uint8_t size, uint16_t color) {
  const int16_t w = static_cast<int16_t>(text.length()) * 6 * size;
  lcd.setTextSize(size);
  lcd.setTextColor(color, BLACK);
  lcd.setCursor((240 - w) / 2, y);
  lcd.print(text);
}

// ── 설정 (기기에 저장) ─────────────────────────────────
enum Theme : uint8_t { TH_PET, TH_SOXL, TH_HYNIX, TH_COUNT };
enum Mode : uint8_t { MODE_AUTO, MODE_LIGHT, MODE_DARK };
constexpr uint8_t EEPROM_MAGIC = 0x5B;
Theme theme = TH_PET;
Mode mode = MODE_AUTO;

void loadSettings() {
  EEPROM.begin(16);
  if (EEPROM.read(0) != EEPROM_MAGIC) return;
  if (EEPROM.read(1) < TH_COUNT) theme = static_cast<Theme>(EEPROM.read(1));
  if (EEPROM.read(2) <= MODE_DARK) mode = static_cast<Mode>(EEPROM.read(2));
}

void saveSettings() {
  EEPROM.write(0, EEPROM_MAGIC);
  EEPROM.write(1, theme);
  EEPROM.write(2, mode);
  EEPROM.commit();
}

// ── 색 (라이트 · 다크) ─────────────────────────────────
struct Colors {
  uint16_t bg, fg, sub, grid;
};
constexpr Colors DARK = {rgb(0, 0, 0), rgb(242, 242, 245), rgb(150, 150, 160), rgb(48, 48, 56)};
constexpr Colors LIGHT = {rgb(250, 247, 242), rgb(28, 28, 34), rgb(110, 110, 120), rgb(222, 218, 210)};
constexpr uint16_t UP_C = rgb(240, 68, 82);    // 오르면 빨강 (국내 증권 앱 관례)
constexpr uint16_t DOWN_C = rgb(49, 130, 246); // 내리면 파랑
Colors col = DARK;

// a(0~255) 만큼 fg 를 bg 위에 섞는다
uint16_t blend(uint16_t fg, uint16_t bg, uint8_t a) {
  const uint16_t fr = ((fg >> 10) & 0x3E) + 1, fgG = ((fg >> 4) & 0x7E) + 1, fb = ((fg << 1) & 0x3E) + 1;
  const uint16_t br = ((bg >> 10) & 0x3E) + 1, bgG = ((bg >> 4) & 0x7E) + 1, bb = ((bg << 1) & 0x3E) + 1;
  const uint16_t r = ((fr * a) + (br * (255 - a))) >> 9;
  const uint16_t g = ((fgG * a) + (bgG * (255 - a))) >> 9;
  const uint16_t b = ((fb * a) + (bb * (255 - a))) >> 9;
  return (r << 11) | (g << 5) | b;
}

// ── 줄 단위로 합쳐 그리기 ──────────────────────────────
// 화면 전체 버퍼(115KB)를 둘 메모리가 없어서, 8줄씩 띠를 만들어 배경 → 그림 → 글자를 합친 뒤 한 번에 보낸다.
// 그래서 깜빡임이 없고, 겹친 그림도 투명도대로 섞인다.
constexpr int STRIP_H = 4;  // 4줄씩 (띠 버퍼 1.9KB - HTTPS 에 쓸 메모리를 남긴다)
uint16_t strip[240 * STRIP_H];
int sy0 = 0;  // 지금 띠의 맨 윗줄

inline void plot(int x, int y, uint16_t c, uint8_t a) {
  if (a == 0 || x < 0 || x >= 240 || y < sy0 || y >= sy0 + STRIP_H) return;
  uint16_t& p = strip[(y - sy0) * 240 + x];
  p = a == 255 ? c : blend(c, p, a);
}

uint16_t nextCp(const char*& s) {
  const uint8_t c = static_cast<uint8_t>(*s++);
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) return static_cast<uint16_t>(((c & 0x1F) << 6) | (*s++ & 0x3F));
  if ((c & 0xF0) == 0xE0) {
    const uint16_t cp = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
    s += 2;
    return cp;
  }
  return '?';
}

// 플래시(PROGMEM)는 4바이트 단위로만 읽을 수 있다. 2바이트를 바로 읽으면(l16ui) 칩이 멈추므로
// 글자 표는 memcpy_P 로 통째로 복사해서 본다 (v0.12 · v0.13 이 이것 때문에 계속 다시 켜졌다)
inline uint16_t flashWord(const void* p) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  return pgm_read_byte(b) | (pgm_read_byte(b + 1) << 8);
}

bool getGlyph(const Font& f, uint16_t cp, Glyph& g) {
  for (uint16_t i = 0; i < f.count; i++) {
    memcpy_P(&g, &f.glyphs[i], sizeof(Glyph));
    if (g.cp == cp) return true;
  }
  return false;
}

int textWidth(const Font& f, const char* s) {
  int w = 0;
  Glyph g;
  while (*s) {
    if (getGlyph(f, nextCp(s), g)) w += g.adv;
  }
  return w;
}

enum Align : uint8_t { LEFT, CENTER, RIGHT };

// base 는 글자 기준선 y
void drawText(const Font& f, const char* s, int x, int base, uint16_t color, Align al = LEFT) {
  if (base + f.descent < sy0 || base - f.ascent - 4 >= sy0 + STRIP_H) return;
  if (al != LEFT) x -= al == CENTER ? textWidth(f, s) / 2 : textWidth(f, s);
  Glyph g;
  while (*s) {
    if (!getGlyph(f, nextCp(s), g)) continue;
    const int gx = x + g.x, gy = base + g.y;
    const int r0 = max(gy, sy0), r1 = min(gy + g.h, sy0 + STRIP_H);
    for (int yy = r0; yy < r1; yy++) {
      const uint8_t* row = f.data + g.off + static_cast<uint32_t>(yy - gy) * g.w;
      for (int xx = 0; xx < g.w; xx++) {
        const uint8_t a = pgm_read_byte(row + xx);
        if (a) plot(gx + xx, yy, color, a);
      }
    }
    x += g.adv;
  }
}

int topBase(const Font& f, int top) { return top + f.ascent; }                       // 글자 윗변 → 기준선
int midBase(const Font& f, int mid) { return mid + (f.ascent - f.descent) / 2; }     // 글자 가운데 → 기준선

void drawSprite(const SpriteSet& s, uint8_t frame, int x, int y) {
  const int r0 = max(y, sy0), r1 = min(y + static_cast<int>(s.h), sy0 + STRIP_H);
  if (r0 >= r1) return;
  const uint8_t* base = s.pix + static_cast<uint32_t>(frame) * s.w * s.h;
  for (int yy = r0; yy < r1; yy++) {
    const uint8_t* row = base + static_cast<uint32_t>(yy - y) * s.w;
    for (int xx = 0; xx < s.w; xx++) {
      const uint8_t idx = pgm_read_byte(row + xx);
      const uint8_t a = pgm_read_byte(s.alpha + idx);
      if (a) plot(x + xx, yy, flashWord(s.pal + idx), a);
    }
  }
}

// ── 날씨 (Open-Meteo, 서울) ────────────────────────────
struct Weather {
  bool ok = false;
  float now = 0, lo = 0, hi = 0;
  int code = 0;
  bool isDay = true;
  unsigned long at = 0;
} weather;

uint8_t weatherKind() {
  const int c = weather.code;
  if (c == 0) return weather.isDay ? W_SUN : W_MOON;
  if (c <= 2) return weather.isDay ? W_SUN_CLOUD : W_MOON_CLOUD;
  if (c == 3) return W_CLOUD;
  if (c == 45 || c == 48) return W_FOG;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return W_SNOW;
  if (c >= 95) return W_THUNDER;
  return W_RAIN;  // 이슬비 · 비 · 소나기
}

// ── 주가 (Yahoo Finance 차트, 5분 간격 하루치) ──────────
struct StockDef {
  const char* symbol;
  bool kr;
  const char* title;
  const char* sub;
};
const StockDef STOCKS[2] = {
    {"SOXL", false, "SOXL", "반도체 3배 레버리지 · 미국"},
    {"000660", true, "SK하이닉스", "000660 · 코스피"},  // 국내는 네이버 증권 (넥스트레이드 프리 · 애프터 포함)
};
// 1분봉 캔들 - 최근 40개 (100분치를 받아 빈 분을 건너뛰고 끝에서 40개)
constexpr int CANDLES = 40;
constexpr int C_PITCH = 5;  // 캔들 한 칸 5px (몸통 3px + 틈 2px)
struct Candle {
  float o, h, l, c;
};
struct Quote {
  bool ok = false, failed = false, metaOk = false;
  float price = 0, prev = 0;  // 보여 줄 가격 · 비교 기준 (등락은 price - prev)
  Candle candles[CANDLES];
  uint8_t n = 0;
  time_t at = 0;              // 가격 기준 시각
  char session[40] = "";      // 아래 줄에 붙는 " · 프리마켓" · " · 애프터마켓" · " · 장 마감"
  bool live = false;          // 지금 거래 중 (정규장 · 프리 · 애프터) - 1분마다 받는다
  // 미국 (Yahoo) - 하루 시세
  float regPrice = 0, regPrev = 0;
  time_t regTime = 0, preStart = 0, regStart = 0, regEnd = 0, postEnd = 0;
  unsigned long fetchedAt = 0, metaAt = 0;
} quotes[2];

constexpr int CH_X0 = 16, CH_X1 = 224, CH_Y0 = 156, CH_Y1 = 210;
int16_t cHi[CANDLES], cLo[CANDLES], cTop[CANDLES], cBot[CANDLES];
bool cUp[CANDLES];
int cLeft = CH_X0, cCount = 0, prevLineY = -1;
bool chartReady = false;

void buildChart(const Quote& q) {
  chartReady = q.n > 0;
  if (!chartReady) return;
  float lo = q.candles[0].l, hi = q.candles[0].h;
  for (int k = 0; k < q.n; k++) {
    lo = min(lo, q.candles[k].l);
    hi = max(hi, q.candles[k].h);
  }
  const float span = hi - lo > 0 ? hi - lo : 1;
  auto Y = [&](float v) { return static_cast<int16_t>(lroundf(CH_Y1 - (v - lo) / span * (CH_Y1 - CH_Y0))); };
  for (int k = 0; k < q.n; k++) {
    const Candle& c = q.candles[k];
    cHi[k] = Y(c.h);
    cLo[k] = Y(c.l);
    cTop[k] = Y(max(c.o, c.c));
    cBot[k] = max(Y(min(c.o, c.c)), cTop[k]);
    cUp[k] = c.c >= c.o;
  }
  cCount = q.n;
  cLeft = CH_X0 + (CH_X1 - CH_X0 - CANDLES * C_PITCH) / 2 + (CANDLES - q.n) * C_PITCH;  // 모자라면 오른쪽으로 붙인다
  prevLineY = q.prev >= lo && q.prev <= hi ? Y(q.prev) : -1;  // 전날 종가는 범위 안일 때만
}

void drawChart() {
  if (!chartReady || CH_Y1 < sy0 || CH_Y0 - 1 >= sy0 + STRIP_H) return;
  if (prevLineY >= sy0 && prevLineY < sy0 + STRIP_H) {
    for (int x = CH_X0; x < CH_X1; x += 6) {
      for (int dx = 0; dx < 3; dx++) plot(x + dx, prevLineY, col.grid, 255);
    }
  }
  const int r0 = sy0, r1 = sy0 + STRIP_H;
  for (int k = 0; k < cCount; k++) {
    const int cx = cLeft + k * C_PITCH;
    const uint16_t c = cUp[k] ? UP_C : DOWN_C;  // 캔들마다 오르면 빨강 · 내리면 파랑
    for (int y = max(r0, static_cast<int>(cHi[k])); y <= min(r1 - 1, static_cast<int>(cLo[k])); y++) plot(cx + 1, y, c, 255);
    for (int y = max(r0, static_cast<int>(cTop[k])); y <= min(r1 - 1, static_cast<int>(cBot[k])); y++) {
      plot(cx, y, c, 255);
      plot(cx + 1, y, c, 255);
      plot(cx + 2, y, c, 255);
    }
  }
}

// ── 시계 ──────────────────────────────────────────────
bool timeReady() { return time(nullptr) > 1700000000; }
const char* const WEEKDAY[] = {"일", "월", "화", "수", "목", "금", "토"};

bool isLightNow() {
  if (mode == MODE_LIGHT) return true;
  if (mode == MODE_DARK) return false;
  if (!timeReady()) return false;
  const time_t now = time(nullptr);
  const int h = localtime(&now)->tm_hour;
  return h >= 7 && h < 19;  // 자동: 아침 7시 ~ 저녁 7시는 라이트
}

// ── 테마 화면 ─────────────────────────────────────────
uint8_t petFrame = 0;  // 0 기본 · 1 아가미 살랑 · 2 눈 감음 · 3 웃음

void paintPet() {
  char buf[40];
  if (timeReady()) {
    const time_t now = time(nullptr);
    const struct tm* t = localtime(&now);
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    drawText(F_CLOCK, buf, 120, topBase(F_CLOCK, 16), col.fg, CENTER);
    snprintf(buf, sizeof(buf), "%d월 %d일 %s요일", t->tm_mon + 1, t->tm_mday, WEEKDAY[t->tm_wday]);
    drawText(F_DATE, buf, 120, topBase(F_DATE, 86), col.sub, CENTER);
  } else {
    drawText(F_CLOCK, "--:--", 120, topBase(F_CLOCK, 16), col.fg, CENTER);
  }
  if (weather.ok) {
    char p1[12], p2[24];
    snprintf(p1, sizeof(p1), "%d°", static_cast<int>(lroundf(weather.now)));
    snprintf(p2, sizeof(p2), "%d° / %d°", static_cast<int>(lroundf(weather.lo)), static_cast<int>(lroundf(weather.hi)));
    const int w1 = textWidth(F_TEMP, p1), w2 = textWidth(F_TEMP2, p2);
    const int x = (240 - (26 + 6 + w1 + 10 + w2)) / 2;
    drawSprite(WICON, weatherKind(), x, 105);
    drawText(F_TEMP, p1, x + 32, midBase(F_TEMP, 118), col.fg);
    drawText(F_TEMP2, p2, x + 32 + w1 + 10, midBase(F_TEMP2, 119), col.sub);
  }
  drawSprite(AXO, petFrame, (240 - AXO.w) / 2, 128);
}

void withCommas(char* out, size_t n, long v) {
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%ld", v);
  const int len = strlen(tmp);
  size_t o = 0;
  for (int i = 0; i < len && o + 2 < n; i++) {
    out[o++] = tmp[i];
    const int rest = len - 1 - i;
    if (rest > 0 && rest % 3 == 0) out[o++] = ',';
  }
  out[o] = 0;
}

void paintStock(const StockDef& d, const Quote& q) {
  if (d.kr) {
    drawText(F_KOTITLE, d.title, 16, topBase(F_KOTITLE, 16), col.fg);
  } else {
    drawText(F_TITLE, d.title, 16, topBase(F_TITLE, 14), col.fg);
  }
  drawText(F_SUB, d.sub, 16, topBase(F_SUB, 48), col.sub);

  if (!q.ok) {
    drawSprite(AXO_MINI, 0, 168, 8);
    drawText(F_FOOT, q.failed ? "못 받았어요 · 다시 시도해요" : "불러오는 중", 16, topBase(F_FOOT, 222), col.sub);
    return;
  }
  const float diff = q.price - q.prev;
  const bool up = diff >= 0;
  drawSprite(AXO_MINI, up ? 0 : 1, 168, 8);  // 오르면 웃고 내리면 시무룩

  char price[24], amount[24], line[48];
  if (d.kr) {
    withCommas(price, sizeof(price), lroundf(q.price));
    withCommas(amount, sizeof(amount), lroundf(fabsf(diff)));
  } else {
    snprintf(price, sizeof(price), "$%.2f", q.price);
    snprintf(amount, sizeof(amount), "%.2f", fabsf(diff));
  }
  drawText(F_PRICE, price, 14, topBase(F_PRICE, 66), col.fg);
  if (d.kr) drawText(F_WON, "원", 16 + textWidth(F_PRICE, price), 101, col.fg);
  snprintf(line, sizeof(line), "%s %s   %+.2f%%", up ? "▲" : "▼", amount, q.prev ? diff / q.prev * 100 : 0.0f);
  drawText(F_CHG, line, 16, topBase(F_CHG, 128), up ? UP_C : DOWN_C);
  if (chartReady) drawText(F_FOOT, "1분봉", CH_X1, 146, col.sub, RIGHT);
  drawChart();

  const struct tm* t = localtime(&q.at);
  snprintf(line, sizeof(line), "%d월 %d일 %02d:%02d 기준%s", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, q.session);
  drawText(F_FOOT, line, 16, topBase(F_FOOT, 222), col.sub);
}

void paintTheme() {
  if (theme == TH_PET) {
    paintPet();
  } else {
    paintStock(STOCKS[theme - 1], quotes[theme - 1]);
  }
}

// y0 ~ y1 줄을 다시 그린다 (띠 경계에 맞춰)
void renderRows(int y0, int y1) {
  for (sy0 = (y0 / STRIP_H) * STRIP_H; sy0 < y1 && sy0 < 240; sy0 += STRIP_H) {
    for (int i = 0; i < 240 * STRIP_H; i++) strip[i] = col.bg;
    paintTheme();
    lcd.draw16bitRGBBitmap(0, sy0, strip, 240, min(STRIP_H, 240 - sy0));
  }
}

void renderAll() {
  col = isLightNow() ? LIGHT : DARK;
  renderRows(0, 240);
}

// ── HTTPS 로 JSON 받기 ────────────────────────────────
// 메모리가 적어서 필요한 칸만 걸러 받는다 (filter). 서버가 작은 조각(MFLN)을 지원하면 버퍼를 줄인다
String lastNetErr = "";  // 설정 페이지에 보이는 마지막 받기 문제
bool safeMode = false;   // 연속으로 멈춰 다시 켜졌으면 받기를 멈춘다

bool httpsJson(const char* host, const String& path, JsonDocument& doc, JsonDocument& filter) {
  if (safeMode || WiFi.status() != WL_CONNECTED) return false;
  BearSSL::WiFiClientSecure client;
  client.setInsecure();  // 인증서 확인 생략 - 공개 시세 · 날씨만 받는다
  const bool mfln = client.probeMaxFragmentLength(host, 443, 1024);
  // 메모리가 모자라면 받지 않고 넘어간다 (모자란 채로 연결하면 칩이 멈추고 다시 켜진다)
  // 16KB 받기 버퍼 + BearSSL 전용 스택 약 6KB + 여유
  const uint32_t need = mfln ? 12000 : 26000;
  if (ESP.getMaxFreeBlockSize() < need) {
    lastNetErr = String(host) + " 메모리 부족 (" + ESP.getMaxFreeBlockSize() + "B)";
    return false;
  }
  if (mfln) {
    client.setBufferSizes(1024, 1024);
  } else {
    client.setBufferSizes(16384, 512);
  }
  HTTPClient http;
  http.useHTTP10(true);  // 조각 전송(chunked) 없이 받아 바로 읽는다
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(client, host, 443, path, true)) return false;
  const int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (!ok) lastNetErr = String(host) + " 응답 " + code;
  http.end();
  return ok;
}

bool fetchWeather() {
  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["is_day"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  JsonDocument doc;
  const bool ok = httpsJson("api.open-meteo.com",
                            "/v1/forecast?latitude=37.57&longitude=126.98&current=temperature_2m,weather_code,is_day"
                            "&daily=temperature_2m_max,temperature_2m_min&timezone=Asia%2FSeoul&forecast_days=1",
                            doc, filter);
  if (!ok || doc["current"]["temperature_2m"].isNull()) return false;
  weather.now = doc["current"]["temperature_2m"];
  weather.code = doc["current"]["weather_code"];
  weather.isDay = doc["current"]["is_day"].as<int>() == 1;
  weather.lo = doc["daily"]["temperature_2m_min"][0];
  weather.hi = doc["daily"]["temperature_2m_max"][0];
  weather.ok = true;
  weather.at = millis();
  return true;
}

float parseNum(const char* s) {  // "1,114,000" · "-18,000" → 숫자
  char buf[24];
  size_t o = 0;
  for (; s && *s && o + 1 < sizeof(buf); s++) {
    if (*s != ',') buf[o++] = *s;
  }
  buf[o] = 0;
  return atof(buf);
}

// 받은 캔들 배열에서 빈 분을 건너뛰고 끝에서 40개만 담는다
template <typename Get>
void keepLastCandles(Quote& q, int total, Get get) {
  Candle c;
  int valid = 0;
  for (int k = 0; k < total; k++) valid += get(k, c);
  int skip = max(0, valid - CANDLES);
  q.n = 0;
  for (int k = 0; k < total; k++) {
    if (!get(k, c)) continue;
    if (skip > 0) {
      skip--;
      continue;
    }
    q.candles[q.n++] = c;
  }
}

// ── 미국 (Yahoo) - 프리마켓 · 정규장 · 애프터마켓 ─────────
// 하루 시세(30분마다): 정규장 가격 · 전날 종가 · 프리/정규/애프터 시간
bool fetchUsMeta(Quote& q, const char* symbol) {
  JsonDocument filter;
  JsonObject m = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  m["regularMarketPrice"] = true;
  m["chartPreviousClose"] = true;
  m["regularMarketTime"] = true;
  m["currentTradingPeriod"]["pre"]["start"] = true;
  m["currentTradingPeriod"]["regular"]["start"] = true;
  m["currentTradingPeriod"]["regular"]["end"] = true;
  m["currentTradingPeriod"]["post"]["end"] = true;
  JsonDocument doc;
  if (!httpsJson("query1.finance.yahoo.com", String("/v8/finance/chart/") + symbol + "?interval=1d&range=1d", doc, filter)) {
    return false;
  }
  JsonObject meta = doc["chart"]["result"][0]["meta"];
  if (meta["regularMarketPrice"].isNull()) return false;
  q.regPrice = meta["regularMarketPrice"];
  q.regPrev = meta["chartPreviousClose"];
  q.regTime = meta["regularMarketTime"].as<long>();
  q.preStart = meta["currentTradingPeriod"]["pre"]["start"].as<long>();
  q.regStart = meta["currentTradingPeriod"]["regular"]["start"].as<long>();
  q.regEnd = meta["currentTradingPeriod"]["regular"]["end"].as<long>();
  q.postEnd = meta["currentTradingPeriod"]["post"]["end"].as<long>();
  q.metaOk = true;
  q.metaAt = millis();
  return true;
}

bool fetchUs(Quote& q, const char* symbol) {
  if (!q.metaOk || millis() - q.metaAt > 30UL * 60000) {
    if (!fetchUsMeta(q, symbol) && !q.metaOk) return false;
  }
  const time_t now = time(nullptr);
  const bool pre = timeReady() && now >= q.preStart && now < q.regStart;
  const bool reg = timeReady() && now >= q.regStart && now <= q.regEnd;
  const bool post = timeReady() && now > q.regEnd && now <= q.postEnd;
  q.live = pre || reg || post;
  // 거래 중이면 지금까지, 아니면 지난 애프터마켓 끝(정규장 마감 + 4시간)까지 100분치
  const time_t end = (q.live ? now : q.regTime + 4 * 3600) + 60;

  JsonDocument filter;
  JsonObject r = filter["chart"]["result"][0].to<JsonObject>();
  r["meta"]["regularMarketPrice"] = true;
  r["meta"]["regularMarketTime"] = true;
  r["timestamp"] = true;
  JsonObject f = r["indicators"]["quote"][0].to<JsonObject>();
  f["open"] = f["high"] = f["low"] = f["close"] = true;
  JsonDocument doc;
  const String path = String("/v8/finance/chart/") + symbol + "?interval=1m&includePrePost=true&period1=" +
                      String(static_cast<long>(end - 6000)) + "&period2=" + String(static_cast<long>(end));
  time_t lastAt = 0;
  float lastClose = 0;
  if (httpsJson("query1.finance.yahoo.com", path, doc, filter)) {
    JsonObject res = doc["chart"]["result"][0];
    JsonArray o = res["indicators"]["quote"][0]["open"], h = res["indicators"]["quote"][0]["high"],
              l = res["indicators"]["quote"][0]["low"], c = res["indicators"]["quote"][0]["close"];
    JsonArray ts = res["timestamp"];
    keepLastCandles(q, c.size(), [&](int k, Candle& out) {
      if (o[k].isNull() || h[k].isNull() || l[k].isNull() || c[k].isNull()) return false;
      out = {o[k].as<float>(), h[k].as<float>(), l[k].as<float>(), c[k].as<float>()};
      lastAt = ts[k].as<long>();
      lastClose = out.c;
      return true;
    });
    if (!res["meta"]["regularMarketPrice"].isNull()) {
      q.regPrice = res["meta"]["regularMarketPrice"];
      q.regTime = res["meta"]["regularMarketTime"].as<long>();
    }
  }
  if (reg || lastAt == 0) {
    // 정규장 (또는 캔들을 못 받음): 정규장 가격, 전날 종가와 비교
    q.price = q.regPrice;
    q.prev = q.regPrev;
    q.at = q.regTime;
    strcpy(q.session, reg ? "" : " · 장 마감");
  } else {
    // 프리 · 애프터 · 장 밖: 마지막 1분봉 가격, 직전 정규장 종가와 비교
    q.price = lastClose;
    q.prev = q.regPrice;
    q.at = lastAt;
    strcpy(q.session, pre ? " · 프리마켓" : post ? " · 애프터마켓" : " · 장 마감");
  }
  return true;
}

// ── 국내 (네이버 증권) - 정규장 · 넥스트레이드 프리/애프터 ──
bool fetchKr(Quote& q, const char* code) {
  JsonDocument filter;
  JsonObject d = filter["datas"][0].to<JsonObject>();
  d["closePriceRaw"] = true;
  d["compareToPreviousClosePriceRaw"] = true;
  d["marketStatus"] = true;
  d["localTradedAt"] = true;
  d["overMarketPriceInfo"]["tradingSessionType"] = true;
  d["overMarketPriceInfo"]["overMarketStatus"] = true;
  d["overMarketPriceInfo"]["overPrice"] = true;
  d["overMarketPriceInfo"]["compareToPreviousClosePrice"] = true;
  JsonDocument doc;
  if (!httpsJson("polling.finance.naver.com", String("/api/realtime/domestic/stock/") + code, doc, filter)) return false;
  JsonObject s = doc["datas"][0];
  if (s["closePriceRaw"].isNull()) return false;

  const float close = parseNum(s["closePriceRaw"] | "0");
  const float diff = parseNum(s["compareToPreviousClosePriceRaw"] | "0");
  const bool regOpen = strcmp(s["marketStatus"] | "", "OPEN") == 0;
  JsonObject over = s["overMarketPriceInfo"];
  const bool hasOver = !over.isNull() && !over["overPrice"].isNull();
  const bool overOpen = hasOver && strcmp(over["overMarketStatus"] | "", "OPEN") == 0;
  const bool isPre = hasOver && strcmp(over["tradingSessionType"] | "", "PRE_MARKET") == 0;
  if (regOpen || !hasOver) {
    q.price = close;
    q.prev = close - diff;
    strcpy(q.session, regOpen ? "" : " · 장 마감");
  } else {
    // 넥스트레이드 프리 · 애프터 - 네이버처럼 전날 종가와 비교
    const float op = parseNum(over["overPrice"] | "0");
    q.price = op;
    q.prev = op - parseNum(over["compareToPreviousClosePrice"] | "0");
    snprintf(q.session, sizeof(q.session), "%s%s", isPre ? " · 프리마켓" : " · 애프터마켓", overOpen ? "" : " 마감");
  }
  q.live = regOpen || overOpen;

  // 마지막 거래 시각 "2026-11-09T20:00:00+09:00"
  struct tm tt = {};
  int Y, M, D, h, m, sec;
  if (sscanf(s["localTradedAt"] | "", "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) == 6) {
    tt.tm_year = Y - 1900;
    tt.tm_mon = M - 1;
    tt.tm_mday = D;
    tt.tm_hour = h;
    tt.tm_min = m;
    tt.tm_sec = sec;
    tt.tm_isdst = 0;
    q.at = mktime(&tt);  // 기기 시간대가 한국(KST-9)이라 그대로 맞는다
  }

  // 1분봉 - 마지막 거래 시각까지 100분치 (네이버 시간 형식 yyyyMMddHHmm)
  if (q.at > 0) {
    char from[16], to[16];
    const time_t end = q.at + 60, start = end - 6000;
    strftime(from, sizeof(from), "%Y%m%d%H%M", localtime(&start));
    strftime(to, sizeof(to), "%Y%m%d%H%M", localtime(&end));
    JsonDocument cf;
    cf[0]["openPrice"] = cf[0]["highPrice"] = cf[0]["lowPrice"] = cf[0]["currentPrice"] = true;
    JsonDocument cd;
    if (httpsJson("api.stock.naver.com", String("/chart/domestic/item/") + code + "/minute?startDateTime=" + from + "&endDateTime=" + to,
                  cd, cf)) {
      JsonArray arr = cd.as<JsonArray>();
      keepLastCandles(q, arr.size(), [&](int k, Candle& out) {
        JsonObject it = arr[k];
        if (it["currentPrice"].isNull() || it["openPrice"].isNull()) return false;
        out = {it["openPrice"].as<float>(), it["highPrice"].as<float>(), it["lowPrice"].as<float>(), it["currentPrice"].as<float>()};
        return true;
      });
    }
  }
  q.metaOk = true;
  return true;
}

bool fetchQuote(int i) {
  Quote& q = quotes[i];
  const bool ok = STOCKS[i].kr ? fetchKr(q, STOCKS[i].symbol) : fetchUs(q, STOCKS[i].symbol);
  if (!ok) {
    q.failed = !q.ok;
    return false;
  }
  q.ok = true;
  q.failed = false;
  q.fetchedAt = millis();
  return true;
}

// 다음에 받을 때까지 - 거래 중(프리 · 정규 · 애프터) 1분, 아니면 10분
unsigned long quoteInterval(const Quote& q) { return q.live ? 60000 : 600000; }

// ── 연속 멈춤 감지 ────────────────────────────────────
struct RtcState {
  uint32_t magic, crashes;
};
constexpr uint32_t RTC_MAGIC = 0x50455431;
constexpr uint32_t RTC_SLOT = 64;  // RTC 사용자 메모리 256바이트 위치

void checkCrashLoop() {
  RtcState st;
  ESP.rtcUserMemoryRead(RTC_SLOT, reinterpret_cast<uint32_t*>(&st), sizeof(st));
  if (st.magic != RTC_MAGIC) st = {RTC_MAGIC, 0};
  const rst_info* ri = ESP.getResetInfoPtr();
  const bool crashed = ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_SOFT_WDT_RST || ri->reason == REASON_WDT_RST;
  st.crashes = crashed ? st.crashes + 1 : 0;
  ESP.rtcUserMemoryWrite(RTC_SLOT, reinterpret_cast<uint32_t*>(&st), sizeof(st));
  safeMode = st.crashes >= 2;
}

void clearCrashCount() {
  RtcState st = {RTC_MAGIC, 0};
  ESP.rtcUserMemoryWrite(RTC_SLOT, reinterpret_cast<uint32_t*>(&st), sizeof(st));
}

// ── 와이파이 · 웹 ─────────────────────────────────────
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updater;
bool setupMode = false;
unsigned long restartAt = 0;
wl_status_t failStatus = WL_IDLE_STATUS;
String scanned[20];
int scannedCount = 0;

String ipText() { return setupMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }

const char* reasonEn(wl_status_t s) {
  switch (s) {
    case WL_WRONG_PASSWORD: return "wrong password";
    case WL_NO_SSID_AVAIL: return "Wi-Fi not found";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_IDLE_STATUS: return "no saved Wi-Fi";
    default: return "timeout";
  }
}
const char* reasonKo(wl_status_t s) {
  switch (s) {
    case WL_WRONG_PASSWORD: return "비밀번호가 틀렸어요";
    case WL_NO_SSID_AVAIL: return "그 와이파이를 못 찾았어요 (5GHz 이거나 이름이 다르거나 멀리 있어요)";
    case WL_CONNECT_FAILED: return "연결하지 못했어요";
    case WL_IDLE_STATUS: return "저장된 와이파이가 없어요";
    default: return "시간 안에 연결하지 못했어요";
  }
}

void drawSetupScreen() {
  lcd.fillScreen(BLACK);
  centerText("pet " + String(VERSION), 14, 2, GRAY);
  centerText(reasonEn(failStatus), 96, 2, YELLOW);
  if (WiFi.SSID().length()) centerText(WiFi.SSID(), 122, 1, GRAY);
  centerText("Wi-Fi setup", 150, 2, WHITE);
  centerText("join: " + String(SETUP_AP), 180, 2, WHITE);
  centerText("open " + ipText(), 206, 2, WHITE);
}

bool waitConnected(uint32_t ms) {
  const unsigned long start = millis();
  while (millis() - start < ms) {
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) return true;
    if (s == WL_WRONG_PASSWORD || s == WL_NO_SSID_AVAIL) return false;
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectSavedWifi() {
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (WiFi.SSID().length() == 0) {
    failStatus = WL_IDLE_STATUS;
    return false;
  }
  // WiFi.disconnect() 는 저장된 이름 · 비밀번호를 지우므로 쓰지 않는다
  const String savedSsid = WiFi.SSID();
  const String savedPsk = WiFi.psk();
  centerText("Wi-Fi...", 110, 2, WHITE);
  centerText(savedSsid, 140, 1, GRAY);
  WiFi.begin();
  if (waitConnected(WIFI_WAIT_MS)) return true;
  if (WiFi.status() != WL_WRONG_PASSWORD && WiFi.status() != WL_NO_SSID_AVAIL) {
    centerText("Wi-Fi... (2)", 110, 2, WHITE);
    WiFi.setPhyMode(WIFI_PHY_MODE_11G);
    WiFi.begin(savedSsid, savedPsk);
    if (waitConnected(15000)) return true;
  }
  failStatus = WiFi.status();
  return false;
}

void startSetupAp() {
  setupMode = true;
  WiFi.setAutoReconnect(false);
  wifi_station_disconnect();  // 저장된 와이파이는 지우지 않고 붙으려는 시도만 멈춘다
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n && scannedCount < 20; i++) {
    const String name = WiFi.SSID(i);
    if (name.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < scannedCount; j++) dup = dup || scanned[j] == name;
    if (!dup) scanned[scannedCount++] = name;
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP);
}

// ── 설정 페이지 - base 디자인 규칙(DESIGN.md)을 따른다 ──
// 컨트롤 sm(32px) · 버튼 글자 13px · 카드는 선 대신 1px 링 · 색은 상태에만 · 주 버튼은 검정(다크에서 흰색) · 고른 칸은 2px
const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><title>pet</title><style>
:root{--bg:#fff;--card:#fff;--sub:#fafafa;--hover:#f5f5f5;--fg:#171717;--fg2:#4d4d4d;--ph:#808080;--line:#ebebeb;--ring:0 0 0 1px rgba(0,0,0,.08);--pri:#171717;--inv:#fff;--ok:#15803d;--warn:#b45309}
@media(prefers-color-scheme:dark){:root{--bg:#121214;--card:#1c1c20;--sub:#242429;--hover:#2e2e33;--fg:#ededf0;--fg2:#aaaab2;--ph:#7d7d87;--line:#323238;--ring:0 0 0 1px rgba(255,255,255,.1);--pri:#fafafa;--inv:#121214;--ok:#4ade80;--warn:#fbbf24;color-scheme:dark}}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,BlinkMacSystemFont,"Apple SD Gothic Neo",Pretendard,"Malgun Gothic",sans-serif;word-break:keep-all;-webkit-text-size-adjust:100%}
main{max-width:480px;margin:0 auto;padding:0 16px 16px;display:grid;gap:12px}
.top{display:flex;align-items:center;gap:8px;height:48px}
.top img{width:27px;height:24px}
.top b{font-size:14px;font-weight:600;letter-spacing:-.02em}
.grow{flex:1}
.state{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--fg2)}
.dot{width:6px;height:6px;border-radius:50%;background:var(--ok)}.dot.warn{background:var(--warn)}
.card{background:var(--card);box-shadow:var(--ring);border-radius:12px;padding:16px;display:grid;gap:12px}
.head{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:32px}
h2{margin:0;font-size:13px;font-weight:600}
.desc{margin:-8px 0 0;font-size:12px;line-height:1.4;color:var(--fg2)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.tile{display:grid;gap:6px;padding:6px;border-radius:12px;background:var(--card);box-shadow:var(--ring);border:0;font:inherit;font-size:12px;color:var(--fg2);cursor:pointer;text-align:center}
.tile img{width:100%;aspect-ratio:1;border-radius:8px;display:block}
.tile.on{box-shadow:0 0 0 2px var(--pri);color:var(--fg);font-weight:600}
.seg{display:grid;grid-template-columns:repeat(3,1fr);gap:2px;background:var(--sub);box-shadow:var(--ring);border-radius:8px;padding:2px}
.seg button{height:28px;border:0;border-radius:6px;background:none;font:inherit;font-size:13px;color:var(--fg2);cursor:pointer}
.seg button.on{background:var(--card);color:var(--fg);font-weight:600;box-shadow:var(--ring)}
button.b,.btn{display:inline-flex;align-items:center;justify-content:center;height:32px;padding:0 12px;border-radius:8px;font:inherit;font-size:13px;font-weight:500;cursor:pointer;text-decoration:none;white-space:nowrap;user-select:none;transition:background .15s}
.main{background:var(--pri);color:var(--inv);border:0}
.sub{background:var(--card);color:var(--fg);border:1px solid var(--line)}.sub:hover{background:var(--hover)}
input,select{width:100%;height:32px;padding:0 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg);font:inherit;font-size:14px;outline:0}
input:focus,select:focus{border-color:var(--pri)}input::placeholder{color:var(--ph)}
form{display:grid;gap:8px}.end{display:flex;justify-content:flex-end;gap:8px}
.rows{display:grid;gap:8px}
.row{display:flex;justify-content:space-between;gap:12px;font-size:13px}.row span:first-child{color:var(--fg2);flex:none}
.row span:last-child{font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.check{display:flex;align-items:center;gap:6px;font-size:13px;color:var(--fg2)}.check input{width:16px;height:16px;margin:0}
#t{position:fixed;left:50%;bottom:24px;transform:translate(-50%,8px);background:var(--pri);color:var(--inv);font-size:13px;padding:8px 14px;border-radius:12px;opacity:0;transition:opacity .15s,transform .15s;pointer-events:none}
#t.show{opacity:1;transform:translate(-50%,0)}
</style></head><body><main>)HTML";

const char PAGE_END[] PROGMEM = R"HTML(</main><div id=t></div><script>
function toast(m){t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),1800)}
function pick(sel,el){document.querySelectorAll(sel).forEach(x=>x.classList.remove('on'));el.classList.add('on')}
function setTheme(i,el){fetch('/theme?i='+i).then(r=>{if(r.ok){pick('.tile',el);toast('테마를 바꿨어요')}else toast('바꾸지 못했어요')})}
function setMode(m,el){fetch('/mode?m='+m).then(r=>{if(r.ok){pick('.seg button',el);toast('화면 모드를 바꿨어요')}})}
function poke(){fetch('/poke').then(()=>toast('우파루파가 웃었어요'))}
</script></body></html>)HTML";

String uptimeText() {
  const unsigned long m = millis() / 60000;
  return m < 60 ? String(m) + "분" : String(m / 60) + "시간 " + String(m % 60) + "분";
}

String agoText(bool ok, bool failed, unsigned long at) {
  if (!ok) return failed ? "못 받았어요" : "아직 안 받았어요";
  const unsigned long m = (millis() - at) / 60000;
  return m == 0 ? "방금 받음" : String(m) + "분 전에 받음";
}

String rowHtml(const String& label, const String& value) {
  return "<div class=row><span>" + label + "</span><span>" + value + "</span></div>";
}

void handleRoot() {
  server.chunkedResponseModeStart(200, F("text/html; charset=utf-8"));
  server.sendContent_P(PAGE_HEAD);
  server.sendContent(F("<header class=top><img alt='' src='"));
  server.sendContent_P(AXO_ICON);
  server.sendContent(String("'><b>pet</b><span class=grow></span><span class=state>") +
                     (setupMode ? "<i class='dot warn'></i>와이파이 설정 필요" : "<i class=dot></i>연결됨") + "</span></header>");

  if (setupMode) {
    server.sendContent("<section class=card><h2>와이파이에 연결하지 못했어요</h2><p class=desc>" + String(reasonKo(failStatus)) +
                       "</p>" + (WiFi.SSID().length() ? rowHtml("저장된 와이파이", WiFi.SSID()) : String()) + "</section>");
  } else {
    // 테마 - 그림 타일 그리드
    static const char* const NAMES[TH_COUNT] = {"우파루파", "SOXL", "SK하이닉스"};
    String g = "<section class=card><div class=head><h2>테마</h2><button class='b sub' onclick=poke()>웃기기</button></div><div class=grid>";
    for (int i = 0; i < TH_COUNT; i++) {
      g += String("<button class='tile") + (theme == i ? " on" : "") + "' onclick='setTheme(" + i + ",this)'><img alt='' src='/thumb?i=" +
           i + "'>" + NAMES[i] + "</button>";
    }
    g += "</div></section>";
    server.sendContent(g);

    // 화면 모드
    static const char* const MODES[3] = {"자동", "라이트", "다크"};
    String m = "<section class=card><h2>화면 모드</h2><p class=desc>자동은 아침 7시부터 저녁 7시까지 라이트예요.</p><div class=seg>";
    for (int i = 0; i < 3; i++) {
      m += String("<button") + (mode == i ? " class=on" : "") + " onclick='setMode(" + i + ",this)'>" + MODES[i] + "</button>";
    }
    m += "</div></section>";
    server.sendContent(m);
  }

  // 기기
  String d = "<section class=card><h2>기기</h2><div class=rows>";
  if (!setupMode) {
    d += rowHtml("와이파이", WiFi.SSID());
    d += rowHtml("날씨", agoText(weather.ok, false, weather.at));
    d += rowHtml("SOXL", agoText(quotes[0].ok, quotes[0].failed, quotes[0].fetchedAt));
    d += rowHtml("SK하이닉스", agoText(quotes[1].ok, quotes[1].failed, quotes[1].fetchedAt));
  }
  d += rowHtml("주소", ipText()) + rowHtml("켜진 지", uptimeText()) +
       rowHtml("남은 메모리", String(ESP.getFreeHeap() / 1024) + " KB (한 덩어리 " + String(ESP.getMaxFreeBlockSize() / 1024) + " KB)") +
       rowHtml("펌웨어", VERSION) + rowHtml("마지막 재시작", ESP.getResetReason()) + "</div>";
  if (safeMode) d += "<p class=desc>연속으로 멈춰서 화면 그리기와 받기를 멈췄어요 (안전 모드). 새 펌웨어를 올리거나 전원을 다시 꽂으면 풀려요.</p>";
  if (lastNetErr.length()) d += "<p class=desc>받기 문제: " + lastNetErr + "</p>";
  if (safeMode || ESP.getResetInfoPtr()->reason == REASON_EXCEPTION_RST) d += "<p class=desc>" + ESP.getResetInfo() + "</p>";
  d += "<div class=end><button class='b sub' onclick='wf.hidden=!wf.hidden'>와이파이 바꾸기</button>"
       "<a class='btn sub' href=/update>펌웨어 올리기</a></div>";
  d += String("<form id=wf method=post action=/wifi") + (setupMode ? "" : " hidden") + ">";
  if (scannedCount > 0) {
    d += "<select name=pick>";
    for (int i = 0; i < scannedCount; i++) d += "<option>" + scanned[i] + "</option>";
    d += "</select><input name=ssid placeholder='목록에 없으면 이름을 직접 입력'>";
  } else {
    d += "<input name=ssid placeholder='와이파이 이름 (2.4GHz)' required>";
  }
  d += "<input name=pass id=pw type=password placeholder='비밀번호'>"
       "<label class=check><input type=checkbox onclick=\"pw.type=this.checked?'text':'password'\">비밀번호 보기</label>"
       "<div class=end><button class='b main'>저장하고 다시 켜기</button></div></form></section>";
  server.sendContent(d);

  server.sendContent_P(PAGE_END);
  server.chunkedResponseFinalize();
}

void sendNotice(const String& title, const String& body, int backSec) {
  String html = FPSTR(PAGE_HEAD);
  html += "<meta http-equiv=refresh content='" + String(backSec) + ";url=/'>";
  html += "<section class=card><h2>" + title + "</h2><p class=desc>" + body + " " + String(backSec) +
          "초 뒤 처음 페이지로 돌아가요.</p></section>";
  html += FPSTR(PAGE_END);
  server.send(200, "text/html; charset=utf-8", html);
}

void handleWifi() {
  const String typed = server.arg("ssid");
  const String ssid = typed.length() ? typed : server.arg("pick");
  const String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "와이파이 이름이 비었어요");
    return;
  }
  WiFi.persistent(true);
  WiFi.begin(ssid, pass);
  sendNotice("저장했어요", "기기가 다시 켜지면 " + ssid + " 에 붙어요. 화면에 새 주소가 뜨면 그 주소로 들어오세요.", 15);
  restartAt = millis() + 1500;
}

unsigned long nextWeather = 0, nextQuote = 0;
unsigned long happyUntil = 0;

void handleTheme() {
  const int i = server.arg("i").toInt();
  if (i < 0 || i >= TH_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "없는 테마예요");
    return;
  }
  theme = static_cast<Theme>(i);
  saveSettings();
  if (theme != TH_PET) {
    const Quote& q = quotes[theme - 1];
    buildChart(q);
    if (!q.ok || millis() - q.fetchedAt > 60000) nextQuote = 0;  // 1분 넘게 지났으면 바로 다시 받는다
  } else if (!weather.ok) {
    nextWeather = 0;
  }
  server.send(200, "text/plain", "ok");
  renderAll();
}

void handleMode() {
  const int m = server.arg("m").toInt();
  if (m < 0 || m > MODE_DARK) {
    server.send(400, "text/plain; charset=utf-8", "없는 모드예요");
    return;
  }
  mode = static_cast<Mode>(m);
  saveSettings();
  server.send(200, "text/plain", "ok");
  renderAll();
}

void handlePoke() {
  happyUntil = millis() + 2500;
  server.send(200, "text/plain", "ok");
}

void handleThumb() {
  static const uint8_t* const DATA[TH_COUNT] = {THUMB0, THUMB1, THUMB2};
  static const size_t SIZE[TH_COUNT] = {sizeof(THUMB0), sizeof(THUMB1), sizeof(THUMB2)};
  const int i = server.arg("i").toInt();
  if (i < 0 || i >= TH_COUNT) {
    server.send(404);
    return;
  }
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "image/png", reinterpret_cast<const char*>(DATA[i]), SIZE[i]);
}

// ── 움직임 · 받아오기 ─────────────────────────────────
// 우파루파: 0.7초마다 아가미 살랑 · 3~6초마다 눈 깜빡 · /poke 를 받으면 2.5초 웃음
void animatePet() {
  static unsigned long nextStep = 0, blinkUntil = 0, nextBlink = 3000;
  static uint8_t breath = 0;
  const unsigned long now = millis();
  if (now >= nextStep) {
    nextStep = now + 700;
    breath ^= 1;
  }
  if (now >= nextBlink) {
    blinkUntil = now + 160;
    nextBlink = now + 3000 + random(3000);
  }
  const uint8_t frame = now < happyUntil ? 3 : now < blinkUntil ? 2 : breath;
  if (frame == petFrame) return;
  petFrame = frame;
  renderRows(128, 240);
}

// 1분마다 - 시계 · 날짜 다시 그리기, 자동 모드면 라이트/다크 바꾸기
void tickMinute() {
  static int lastMin = -1;
  if (!timeReady()) return;
  const time_t now = time(nullptr);
  const struct tm* t = localtime(&now);
  if (t->tm_min == lastMin) return;
  const bool first = lastMin < 0;
  lastMin = t->tm_min;
  const Colors want = isLightNow() ? LIGHT : DARK;
  if (first || want.bg != col.bg) {
    renderAll();
  } else if (theme == TH_PET) {
    renderRows(0, 104);
  } else {
    renderRows(216, 240);  // 주가 화면 아래 "장 마감" 여부
  }
}

void fetchDue() {
  const unsigned long now = millis();
  if (theme == TH_PET) {
    if (now < nextWeather) return;
    const bool ok = fetchWeather();
    nextWeather = now + (ok ? 15UL * 60000 : 60000);
    if (ok) renderAll();
  } else {
    if (now < nextQuote) return;
    const int i = theme - 1;
    const bool ok = fetchQuote(i);
    nextQuote = millis() + (ok ? quoteInterval(quotes[i]) : 45000);
    buildChart(quotes[i]);
    renderAll();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  checkCrashLoop();
  loadSettings();
  initDisplay();
  centerText("pet " + String(VERSION), 14, 2, GRAY);

  if (!connectSavedWifi()) startSetupAp();

  updater.setup(&server, "/update");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/theme", handleTheme);
  server.on("/mode", handleMode);
  server.on("/poke", handlePoke);
  server.on("/thumb", handleThumb);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();

  if (setupMode) {
    drawSetupScreen();
    return;
  }
  configTime("KST-9", "pool.ntp.org", "time.google.com");
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);

  // 주소는 테마 화면에 없으니 켜질 때 잠깐 보여 준다
  lcd.fillScreen(BLACK);
  centerText("connected", 96, 2, GRAY);
  centerText(ipText(), 124, 2, GREEN);
  centerText("reset: " + ESP.getResetReason(), 170, 1, GRAY);
  if (safeMode) {
    // 두 번 연속 멈췄다 - 테마를 그리지도 받지도 않고, 이 화면에서 업데이트만 기다린다
    centerText("SAFE MODE", 186, 2, YELLOW);
    centerText("upload: " + ipText() + "/update", 210, 1, WHITE);
    return;
  }
  delay(2500);
  // 켜지고 30초 동안은 받지 않는다 - 그 사이에 업데이트를 올릴 수 있게
  nextWeather = millis() + 30000;
  nextQuote = millis() + 30000;
  renderAll();
}

void loop() {
  server.handleClient();
  if (restartAt != 0 && millis() > restartAt) ESP.restart();
  if (setupMode) return;
  MDNS.update();
  if (safeMode) return;
  static bool cleared = false;
  if (!cleared && millis() > 120000) {  // 2분 버티면 정상으로 보고 멈춤 횟수를 지운다
    clearCrashCount();
    cleared = true;
  }
  tickMinute();
  fetchDue();
  if (theme == TH_PET) animatePet();
}
