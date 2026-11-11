// pet 펌웨어 - GeekMagic SmallTV-Ultra 에 테마 화면 (펫 · SOXL · SK하이닉스) · 라이트/다크/자동
//   - 와이파이: 기기에 저장된 와이파이로 붙는다. 못 붙으면 "pet-setup" 와이파이를 띄워 폰으로 설정받는다
//   - 업데이트: http://<IP>/update 에서 다음 펌웨어를 선 없이 올린다 (이게 없으면 다음부터 분해해야 한다)
//   - 설정 페이지: http://<IP>/ - 테마 고르기 · 화면 밝기 모드 · 와이파이
//   - 그림 · 글자는 tools/assets.py 가 만든 src/assets.h (부드러운 그림을 투명도째 담아 배경과 섞어 그린다)
//   - 날씨: Open-Meteo(서울, 키 없음), 주가: 토스증권 Open API (차트 · 장 시간 · 기준가는 HTTPS, 체결은 웹소켓)
//
// 화면 핀 · 켜는 순서는 SmallTV-Ultra 한국어 커펌(wonjj6768)의 브리지 펌웨어에서 확인한 값이다.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <SPI.h>
#include <WiFiClientSecureBearSSL.h>
#include <base64.h>
#include <time.h>

#include <algorithm>

#include "assets.h"

namespace {

constexpr const char* VERSION = "v0.21";
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

constexpr int TOSS_ID_AT = 16, TOSS_ID_LEN = 64, TOSS_SECRET_AT = 80, TOSS_SECRET_LEN = 96;
char tossId[TOSS_ID_LEN] = "";          // 토스증권 Open API 키 - 기기에만 저장하고 페이지에 보이지 않는다
char tossSecret[TOSS_SECRET_LEN] = "";

void loadSettings() {
  EEPROM.begin(256);
  if (EEPROM.read(0) != EEPROM_MAGIC) return;
  if (EEPROM.read(1) < TH_COUNT) theme = static_cast<Theme>(EEPROM.read(1));
  if (EEPROM.read(2) <= MODE_DARK) mode = static_cast<Mode>(EEPROM.read(2));
  for (int i = 0; i < TOSS_ID_LEN; i++) tossId[i] = static_cast<char>(EEPROM.read(TOSS_ID_AT + i));
  for (int i = 0; i < TOSS_SECRET_LEN; i++) tossSecret[i] = static_cast<char>(EEPROM.read(TOSS_SECRET_AT + i));
  tossId[TOSS_ID_LEN - 1] = tossSecret[TOSS_SECRET_LEN - 1] = 0;
  if (static_cast<uint8_t>(tossId[0]) == 0xFF) tossId[0] = 0;  // 한 번도 안 쓴 칸
  if (static_cast<uint8_t>(tossSecret[0]) == 0xFF) tossSecret[0] = 0;
}

void saveSettings() {
  EEPROM.write(0, EEPROM_MAGIC);
  EEPROM.write(1, theme);
  EEPROM.write(2, mode);
  for (int i = 0; i < TOSS_ID_LEN; i++) EEPROM.write(TOSS_ID_AT + i, tossId[i]);
  for (int i = 0; i < TOSS_SECRET_LEN; i++) EEPROM.write(TOSS_SECRET_AT + i, tossSecret[i]);
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

// ── 주가 (토스증권 키가 있으면 토스, 없으면 Yahoo · 네이버) ──
struct StockDef {
  const char* symbol;
  bool kr;
  const char* title;
  const char* sub;
};
const StockDef STOCKS[2] = {
    {"SOXL", false, "SOXL", ""},
    {"000660", true, "SK하이닉스", ""},  // 국내는 네이버 증권 (넥스트레이드 프리 · 애프터 포함)
};
// 1분봉 캔들 - 화면에는 최근 30개, 볼린저밴드 계산용으로 앞쪽 19개를 더 들고 있는다
constexpr int CANDLES = 30;  // 작은 화면에서 또렷하게 - 최근 30분
constexpr int BB_N = 20;     // 볼린저밴드: 종가 20개 평균 ± 2 표준편차
constexpr int HIST = CANDLES + BB_N - 1;
constexpr int C_PITCH = 7;  // 캔들 한 칸 7px (몸통 5px + 틈 2px)
struct Candle {
  float o, h, l, c, v;  // 시가 · 고가 · 저가 · 종가 · 거래량
};
// 화면에는 한 종목만 나오니 캔들 칸은 두 종목이 같이 쓴다 (메모리를 아낀다). 한 종목을 받으면 다른 종목 차트는 비운다
Candle chartBuf[HIST];
struct Quote {
  bool ok = false, failed = false;
  float price = 0, prev = 0;  // 보여 줄 가격 · 비교 기준 (등락은 price - prev)
  Candle* candles = chartBuf;
  uint8_t n = 0;
  time_t at = 0;              // 가격 기준 시각
  time_t lastMin = 0;         // 마지막 1분봉의 시작 시각 (실시간 체결이 이 분이면 갱신, 다음 분이면 새 캔들)
  char session[40] = "";      // 아래 줄에 붙는 " · 데이마켓" · " · 프리마켓" · " · 애프터마켓" · " · 장 마감"
  bool live = false;          // 지금 거래 중 - 소켓이 없을 때 1분마다 받는다
  char baseDate[11] = "";     // 기준가를 받은 정규장 날짜
  uint8_t fails = 0;          // 연속으로 못 받은 횟수 - 있으면 실시간을 붙이기 전에 다시 받는다
  unsigned long fetchedAt = 0;
} quotes[2];

constexpr int CH_X0 = 10, CH_X1 = 230, CH_Y0 = 70, CH_Y1 = 192;  // 캔들
constexpr int VOL_Y0 = 198, VOL_Y1 = 215;                         // 거래량 막대 (아래 띠)
int16_t cHi[CANDLES], cLo[CANDLES], cTop[CANDLES], cBot[CANDLES];
int16_t bbU[CANDLES], bbM[CANDLES], bbL[CANDLES];  // 볼린저밴드 위 · 가운데 · 아래 (-1 은 앞쪽 종가가 20개가 안 됨)
uint8_t cVol[CANDLES];                             // 거래량 막대 높이 (px)
bool cUp[CANDLES];
int cLeft = CH_X0, cCount = 0, prevLineY = -1;
bool chartReady = false;
constexpr uint16_t BAND_C = rgb(232, 168, 56);  // 볼린저밴드 - 빨강 · 파랑 캔들과 안 겹치는 호박색

void buildChart(const Quote& q) {
  chartReady = q.n > 0;
  if (!chartReady) return;
  const int vis = min<int>(q.n, CANDLES), off = q.n - vis;  // 화면에 그리는 건 끝에서 30개
  float up[CANDLES], mid[CANDLES], dn[CANDLES];
  float lo = q.candles[off].l, hi = q.candles[off].h;
  for (int k = 0; k < vis; k++) {
    const int j = off + k;
    lo = min(lo, q.candles[j].l);
    hi = max(hi, q.candles[j].h);
    mid[k] = NAN;
    if (j < BB_N - 1) continue;
    float sum = 0, sq = 0;
    for (int m = j - BB_N + 1; m <= j; m++) sum += q.candles[m].c;
    const float mean = sum / BB_N;
    for (int m = j - BB_N + 1; m <= j; m++) sq += (q.candles[m].c - mean) * (q.candles[m].c - mean);
    const float sd = sqrtf(sq / BB_N);
    mid[k] = mean;
    up[k] = mean + 2 * sd;
    dn[k] = mean - 2 * sd;
    lo = min(lo, dn[k]);  // 밴드도 화면 안에 들어오게 범위에 넣는다
    hi = max(hi, up[k]);
  }
  const float span = hi - lo > 0 ? hi - lo : 1;
  auto Y = [&](float v) { return static_cast<int16_t>(lroundf(CH_Y1 - (v - lo) / span * (CH_Y1 - CH_Y0))); };
  float vMax = 0;
  for (int k = 0; k < vis; k++) vMax = max(vMax, q.candles[off + k].v);
  for (int k = 0; k < vis; k++) {
    const Candle& c = q.candles[off + k];
    cVol[k] = vMax > 0 && c.v > 0 ? max(1L, lroundf(c.v / vMax * (VOL_Y1 - VOL_Y0))) : 0;
    cHi[k] = Y(c.h);
    cLo[k] = Y(c.l);
    cTop[k] = Y(max(c.o, c.c));
    cBot[k] = max(Y(min(c.o, c.c)), cTop[k]);
    cUp[k] = c.c >= c.o;
    const bool has = !isnan(mid[k]);
    bbU[k] = has ? Y(up[k]) : -1;
    bbM[k] = has ? Y(mid[k]) : -1;
    bbL[k] = has ? Y(dn[k]) : -1;
  }
  cCount = vis;
  cLeft = CH_X0 + (CH_X1 - CH_X0 - CANDLES * C_PITCH) / 2 + (CANDLES - vis) * C_PITCH;  // 모자라면 오른쪽으로 붙인다
  prevLineY = q.prev >= lo && q.prev <= hi ? Y(q.prev) : -1;  // 전날 종가는 범위 안일 때만
}

// 지금 띠에 걸치는 선분만 그린다 (dotted 면 2칸 그리고 2칸 띄운다)
void seg(int x0, int y0, int x1, int y1, uint16_t c, bool dotted) {
  if (max(y0, y1) < sy0 || min(y0, y1) >= sy0 + STRIP_H) return;
  const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int i = 0;; i++) {
    if (!dotted || (i & 2) == 0) plot(x0, y0, c, 255);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void drawChart() {
  if (!chartReady || VOL_Y1 < sy0 || CH_Y0 - 1 >= sy0 + STRIP_H) return;
  if (prevLineY >= sy0 && prevLineY < sy0 + STRIP_H) {
    for (int x = CH_X0; x < CH_X1; x += 6) {
      for (int dx = 0; dx < 3; dx++) plot(x + dx, prevLineY, col.grid, 255);
    }
  }
  // 볼린저밴드 - 캔들 뒤에 얇게 (위 · 아래는 이어서, 가운데는 점선). 배경과 미리 섞은 색이라 이음매가 진해지지 않는다
  const uint16_t edge = blend(BAND_C, col.bg, 200), center = blend(BAND_C, col.bg, 130);
  for (int k = 1; k < cCount; k++) {
    if (bbM[k - 1] < 0 || bbM[k] < 0) continue;
    const int x0 = cLeft + (k - 1) * C_PITCH + (C_PITCH - 2) / 2, x1 = x0 + C_PITCH;  // 캔들 심지 위치끼리
    seg(x0, bbU[k - 1], x1, bbU[k], edge, false);
    seg(x0, bbL[k - 1], x1, bbL[k], edge, false);
    seg(x0, bbM[k - 1], x1, bbM[k], center, true);
  }
  const int r0 = sy0, r1 = sy0 + STRIP_H;
  for (int k = 0; k < cCount; k++) {
    const int cx = cLeft + k * C_PITCH;
    const uint16_t c = cUp[k] ? UP_C : DOWN_C;  // 캔들마다 오르면 빨강 · 내리면 파랑
    const int body = C_PITCH - 2;
    for (int y = max(r0, static_cast<int>(cHi[k])); y <= min(r1 - 1, static_cast<int>(cLo[k])); y++) plot(cx + body / 2, y, c, 255);
    for (int y = max(r0, static_cast<int>(cTop[k])); y <= min(r1 - 1, static_cast<int>(cBot[k])); y++) {
      for (int dx = 0; dx < body; dx++) plot(cx + dx, y, c, 255);
    }
    // 거래량 - 아래 띠에 같은 색으로 연하게
    for (int y = max(r0, VOL_Y1 - cVol[k] + 1); y <= min(r1 - 1, VOL_Y1); y++) {
      for (int dx = 0; dx < body; dx++) plot(cx + dx, y, c, 110);
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

// 주가 화면 - 1행 가격 · 2행 이름 + 등락 · 나머지는 1분봉 차트 · 맨 아래 기준 시각
void paintStock(const StockDef& d, const Quote& q) {
  const Font& nameFont = d.kr ? F_NAME_KO : F_NAME_EN;
  if (!q.ok) {
    drawSprite(AXO_MINI, 0, 184, 6);
    drawText(nameFont, d.title, 12, topBase(nameFont, 46), col.sub);
    drawText(F_FOOT, q.failed ? "못 받았어요 · 다시 시도해요" : "불러오는 중", 12, topBase(F_FOOT, 224), col.sub);
    return;
  }
  const float diff = q.price - q.prev;
  const bool up = diff >= 0;
  drawSprite(AXO_MINI, up ? 0 : 1, 184, 6);  // 오르면 웃고 내리면 시무룩

  char price[24], amount[24], line[64];
  if (d.kr) {
    withCommas(price, sizeof(price), lroundf(q.price));
    withCommas(amount, sizeof(amount), lroundf(fabsf(diff)));
  } else {
    snprintf(price, sizeof(price), "$%.2f", q.price);
    snprintf(amount, sizeof(amount), "%.2f", fabsf(diff));
  }
  const int pb = topBase(F_PRICE, 8);
  drawText(F_PRICE, price, 12, pb, col.fg);
  if (d.kr) drawText(F_WON, "원", 12 + textWidth(F_PRICE, price) + 2, pb, col.fg);

  drawText(nameFont, d.title, 12, topBase(nameFont, 46), col.sub);
  const int nx = 12 + textWidth(nameFont, d.title) + 10;
  snprintf(line, sizeof(line), "%s %s  %+.2f%%", up ? "▲" : "▼", amount, q.prev ? diff / q.prev * 100 : 0.0f);
  drawText(F_CHG, line, nx, topBase(F_CHG, 47), up ? UP_C : DOWN_C);

  drawChart();

  const struct tm* t = localtime(&q.at);
  snprintf(line, sizeof(line), "1분봉 · %d월 %d일 %02d:%02d:%02d 기준%s", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, q.session);
  drawText(F_FOOT, line, 12, topBase(F_FOOT, 224), col.sub);
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

// 응답 본문을 read(stream) 에 넘긴다. bearer 가 있으면 인증 머리줄을 붙이고, status 에 응답 코드를 돌려준다
template <typename Read>
bool httpsStream(const char* host, const String& path, Read read, const String* postBody = nullptr, const char* bearer = nullptr,
                 int* status = nullptr) {
  if (safeMode || WiFi.status() != WL_CONNECTED) return false;
  BearSSL::WiFiClientSecure client;
  client.setInsecure();  // 인증서 확인 생략 - 공개 시세 · 날씨만 받는다
  // 메모리가 모자라면 받지 않고 넘어간다. client 를 만들 때 BearSSL 전용 스택(약 6KB)은 이미 잡혔고,
  // 남은 것은 받기 버퍼 8KB + 엔진 약 4KB (응답은 모두 8KB 아래로 받는다 - 1분봉은 60분치만.
  // 쓰는 서버들은 작은 조각 전송(MFLN)을 지원하지 않는다)
  if (ESP.getFreeHeap() < 14000 || ESP.getMaxFreeBlockSize() < 9000) {
    lastNetErr = String(host) + " 메모리 부족 (" + ESP.getFreeHeap() + "B)";
    return false;
  }
  client.setBufferSizes(8192, 512);
  HTTPClient http;
  http.useHTTP10(true);  // 조각 전송(chunked) 없이 받아 바로 읽는다
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(client, host, 443, path, true)) return false;
  if (bearer) http.addHeader("Authorization", String("Bearer ") + bearer);
  int code;
  if (postBody) {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    code = http.POST(*postBody);
  } else {
    code = http.GET();
  }
  if (status) *status = code;
  bool ok = false;
  if (code == 200) ok = read(http.getStream());
  if (!ok) lastNetErr = String(host) + " 응답 " + code;
  http.end();
  return ok;
}

bool httpsJson(const char* host, const String& path, JsonDocument& doc, JsonDocument& filter, const String* postBody = nullptr) {
  return httpsStream(host, path, [&](Stream& s) { return !deserializeJson(doc, s, DeserializationOption::Filter(filter)); }, postBody);
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

// ── 토스증권 실시간 체결 (웹소켓) ──────────────────────
// 토큰(24시간)을 받아 wss://openapi-ws.tossinvest.com/ws/v1 에 붙고, 지금 테마 종목의 체결을 구독한다.
// 서버가 작은 조각 전송(MFLN)을 안 해서 받기 버퍼를 8KB 로 둔다 (인증서 묶음 3.7KB · 체결 프레임 200바이트 안팎)
constexpr const char* TOSS_WS_HOST = "openapi-ws.tossinvest.com";
String tossToken = "";
uint16_t wsRx = 6144;  // 소켓 받기 칸 - 인증서 묶음 3.7KB 라 6KB 로 충분, 안 붙으면 8KB 로 올린다
unsigned long tossTokenExp = 0, wsRetryAt = 0, wsPingAt = 0, wsOpenedAt = 0, lastTradeDraw = 0;
BearSSL::WiFiClientSecure* ws = nullptr;
constexpr size_t WSBUF = 1536;
uint8_t* wsBuf = nullptr;  // 소켓이 붙어 있을 때만 잡는다
int wsSym = -1;          // 구독 중인 종목 (STOCKS 번호)
uint32_t wsTrades = 0;   // 이번 연결에서 받은 체결 수
String tossStatus = "키 없음";
bool tradeDirty = false;

bool tossReady() { return tossId[0] && tossSecret[0]; }

bool fetchTossToken() {
  JsonDocument filter;
  filter["access_token"] = true;
  filter["expires_in"] = true;
  JsonDocument doc;
  const String body = String("grant_type=client_credentials&client_id=") + tossId + "&client_secret=" + tossSecret;
  if (!httpsJson("openapi.tossinvest.com", "/oauth2/token", doc, filter, &body) || doc["access_token"].isNull()) {
    tossStatus = "토큰을 못 받았어요 (키 · 허용 IP 확인)";
    return false;
  }
  tossToken = doc["access_token"].as<String>();
  const long life = doc["expires_in"] | 86400L;
  tossTokenExp = millis() + static_cast<unsigned long>(max(60L, life - 600)) * 1000UL;  // 만료 10분 전에 새로
  return true;
}

// ── 토스증권 시세 (REST) ───────────────────────────────
// 차트(1분봉) · 장 시간(장 운영 캘린더) · 비교 기준가(일봉)를 모두 토스에서 받는다.
// 받는 동안은 소켓이 닫혀 있다 (HTTPS 와 소켓을 같이 들 메모리가 없다)
constexpr const char* TOSS_HOST = "openapi.tossinvest.com";

// "2026-11-09T22:28:43.064+09:00" · "2026-11-10T00:00:00-05:00" → 세계 표준시 초 (못 읽으면 0)
time_t parseIso(const char* s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  const long y = Y - (M <= 2);  // 날짜 → 1970-01-01 부터 날 수
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;
  const long doy = (153L * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long t = (era * 146097L + doe - 719468L) * 86400L + h * 3600L + m * 60L + sec;
  const char* z = s + 19;
  while (*z == '.' || isdigit(static_cast<unsigned char>(*z))) z++;  // 소수 초는 버린다
  if (*z == '+' || *z == '-') {
    int oh = 0, om = 0;
    sscanf(z + 1, "%d:%d", &oh, &om);
    t += (*z == '+' ? -1L : 1L) * (oh * 3600L + om * 60L);
  }
  return static_cast<time_t>(t);
}

bool tossTokenOk() { return (tossToken.length() && millis() < tossTokenExp) || fetchTossToken(); }

template <typename Read>
bool tossGet(const String& path, Read read) {
  if (!tossReady() || !tossTokenOk()) return false;
  int code = 0;
  const bool ok = httpsStream(TOSS_HOST, path, read, nullptr, tossToken.c_str(), &code);
  if (code == 401) tossToken = "";  // 토큰이 무효 - 다음에 새로 받는다
  return ok;
}

// "candles": [...] 를 한 칸씩 읽는다 (통째로 담을 메모리가 없다). each 가 false 면 그만 읽는다
template <typename Each>
bool readCandles(Stream& s, JsonDocument& filter, Each each) {
  if (!s.find("\"candles\"") || !s.find("[")) return false;
  do {
    JsonDocument e;
    if (deserializeJson(e, s, DeserializationOption::Filter(filter)) || e["closePrice"].isNull()) break;
    if (!each(e.as<JsonObject>())) break;
  } while (s.findUntil(",", "]"));
  return true;
}

// 장 운영 캘린더 - 전 · 오늘 · 다음 영업일의 세션 시각 (미국: 데이 · 프리 · 정규 · 애프터, 국내 통합: 프리 · 정규 · 애프터)
enum Session : uint8_t { S_CLOSED, S_DAY, S_PRE, S_REG, S_POST };
struct BizDay {
  char date[11] = "";
  uint32_t start[5] = {}, end[5] = {};  // Session 순서 (S_CLOSED 칸은 비운다)
};
struct {
  BizDay days[3];
  int8_t stock = -1;  // 어느 종목 시장의 캘린더인지 (STOCKS 번호)
  unsigned long at = 0;
} cal;

bool fetchCalendar(int i) {
  static const char* const DAYS[3] = {"previousBusinessDay", "today", "nextBusinessDay"};
  static const char* const NAMES[5] = {"", "dayMarket", "preMarket", "regularMarket", "afterMarket"};
  const bool kr = STOCKS[i].kr;
  JsonDocument filter;
  for (const char* d : DAYS) {
    filter["result"][d]["date"] = true;
    for (int k = 1; k < 5; k++) {
      JsonObject x = kr ? filter["result"][d]["integrated"][NAMES[k]].to<JsonObject>() : filter["result"][d][NAMES[k]].to<JsonObject>();
      x["startTime"] = true;
      x["endTime"] = true;
    }
  }
  JsonDocument doc;
  if (!tossGet(String("/api/v1/market-calendar/") + (kr ? "KR" : "US"),
               [&](Stream& s) { return !deserializeJson(doc, s, DeserializationOption::Filter(filter)); })) {
    return false;
  }
  for (int j = 0; j < 3; j++) {
    JsonObject d = doc["result"][DAYS[j]];
    JsonObject src = kr ? d["integrated"] : d;
    strlcpy(cal.days[j].date, d["date"] | "", sizeof(cal.days[j].date));
    for (int k = 1; k < 5; k++) {
      JsonObject x = src[NAMES[k]];  // 휴장이면 null
      cal.days[j].start[k] = static_cast<uint32_t>(parseIso(x["startTime"] | ""));
      cal.days[j].end[k] = static_cast<uint32_t>(parseIso(x["endTime"] | ""));
    }
  }
  if (!cal.days[1].date[0]) return false;
  cal.stock = i;
  cal.at = millis();
  return true;
}

Session sessionAt(int i, time_t t) {
  if (cal.stock != i) return S_CLOSED;
  for (const BizDay& d : cal.days) {
    for (int k = 1; k < 5; k++) {
      if (d.start[k] && t >= static_cast<time_t>(d.start[k]) && t < static_cast<time_t>(d.end[k])) return static_cast<Session>(k);
    }
  }
  return S_CLOSED;
}

const char* sessionLabel(Session s) {
  switch (s) {
    case S_DAY: return " · 데이마켓";
    case S_PRE: return " · 프리마켓";
    case S_REG: return "";
    case S_POST: return " · 애프터마켓";
    default: return " · 장 마감";
  }
}

// 비교 기준가를 잡을 날 - 이미 끝난 정규장 가운데 가장 최근 날
// (데이 · 프리 · 정규 중에는 전 영업일 종가, 정규장이 끝난 뒤 애프터에는 그날 종가와 비교한다)
const char* baseDate(time_t now) {
  const char* date = nullptr;
  uint32_t best = 0;
  for (const BizDay& d : cal.days) {
    const uint32_t e = d.end[S_REG];
    if (e && static_cast<time_t>(e) <= now && e > best) {
      best = e;
      date = d.date;
    }
  }
  return date;
}

// 1분봉 49개 (최신순으로 온다) → 오래된 것부터 chartBuf 에
bool fetchCandles(Quote& q, const char* symbol) {
  JsonDocument filter;
  filter["timestamp"] = filter["openPrice"] = filter["highPrice"] = filter["lowPrice"] = filter["closePrice"] = filter["volume"] = true;
  int got = 0;
  time_t newest = 0;
  const bool ok = tossGet(String("/api/v1/candles?symbol=") + symbol + "&interval=1m&count=" + HIST, [&](Stream& s) {
    return readCandles(s, filter, [&](JsonObject e) {
      if (got == 0) newest = parseIso(e["timestamp"] | "");
      chartBuf[got++] = {parseNum(e["openPrice"] | "0"), parseNum(e["highPrice"] | "0"), parseNum(e["lowPrice"] | "0"),
                         parseNum(e["closePrice"] | "0"), parseNum(e["volume"] | "0")};
      return got < HIST;
    });
  });
  if (!ok || got == 0 || newest == 0) {
    if (got > 0) q.n = 0;  // 받다가 끊겨 칸이 섞였으면 비운다. 연결부터 못 했으면 전 차트를 그대로 둔다
    return false;
  }
  std::reverse(chartBuf, chartBuf + got);
  q.n = got;
  q.lastMin = newest - 60;  // 1분봉 시각은 끝나는 시각 - 실시간 체결(시작 시각 기준)과 맞춘다
  q.price = chartBuf[got - 1].c;
  q.at = min(time(nullptr), newest);
  return true;
}

// 기준가 - 그날 일봉 종가
bool fetchBase(Quote& q, const char* symbol, const char* date) {
  JsonDocument filter;
  filter["timestamp"] = filter["closePrice"] = true;
  float found = 0;
  tossGet(String("/api/v1/candles?symbol=") + symbol + "&interval=1d&count=4", [&](Stream& s) {
    return readCandles(s, filter, [&](JsonObject e) {
      if (strncmp(e["timestamp"] | "", date, 10) == 0) found = parseNum(e["closePrice"] | "0");
      return found == 0;
    });
  });
  if (found <= 0) return false;
  q.prev = found;
  strlcpy(q.baseDate, date, sizeof(q.baseDate));
  return true;
}

bool fetchQuote(int i) {
  Quote& q = quotes[i];
  quotes[1 - i].n = 0;  // 캔들 칸은 같이 쓴다 - 다른 종목 차트는 비운다
  bool ok = timeReady() && tossReady() && fetchCandles(q, STOCKS[i].symbol);
  if (ok && !q.ok) {  // 켜진 뒤 처음이면 캔들부터 바로 그린다 (장 시간 · 기준가는 곧이어 받는다)
    q.ok = true;
    if (q.prev == 0) q.prev = q.price;
    buildChart(q);
    renderAll();
  }
  if (ok && (cal.stock != i || millis() - cal.at > 30UL * 60000)) fetchCalendar(i);  // 못 받으면 전 캘린더를 쓴다
  if (ok) {
    const time_t now = time(nullptr);
    const char* bd = baseDate(now);
    if (bd && (q.prev == 0 || strcmp(bd, q.baseDate) != 0)) fetchBase(q, STOCKS[i].symbol, bd);  // 못 받으면 다음에 다시
    if (q.prev == 0) q.prev = q.price;  // 기준가를 아직 못 받았으면 등락 0 으로
    const Session s = sessionAt(i, now);
    q.live = s != S_CLOSED;
    strcpy(q.session, sessionLabel(s));
  }
  if (!ok) {
    q.failed = !q.ok;
    if (q.fails < 255) q.fails++;
    return false;
  }
  q.ok = true;
  q.failed = false;
  q.fails = 0;
  q.fetchedAt = millis();
  return true;
}

// 다음에 받을 때까지 - 거래 중 1분, 아니면 10분 (소켓이 붙어 있으면 30분마다)
unsigned long quoteInterval(const Quote& q) { return q.live ? 60000 : 600000; }

void wsClose() {
  if (!ws) return;
  ws->stop();
  delete ws;
  ws = nullptr;
  wsSym = -1;
  free(wsBuf);
  wsBuf = nullptr;
}

// 클라이언트 → 서버 프레임은 가린다(mask)
void wsSend(const char* data, size_t len, uint8_t op = 0x1) {
  if (!ws) return;
  uint8_t hdr[8];
  size_t h = 0;
  hdr[h++] = 0x80 | op;
  if (len < 126) {
    hdr[h++] = 0x80 | len;
  } else {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (len >> 8) & 0xFF;
    hdr[h++] = len & 0xFF;
  }
  uint8_t mask[4];
  for (uint8_t& m : mask) m = static_cast<uint8_t>(random(256));
  memcpy(hdr + h, mask, 4);
  h += 4;
  ws->write(hdr, h);
  uint8_t chunk[64];
  for (size_t i = 0; i < len; i += sizeof(chunk)) {
    const size_t n = min(sizeof(chunk), len - i);
    for (size_t k = 0; k < n; k++) chunk[k] = static_cast<uint8_t>(data[i + k]) ^ mask[(i + k) % 4];
    ws->write(chunk, n);
  }
}

void wsSubscribe(int i) {
  char decl[96];
  snprintf(decl, sizeof(decl), "[{\"type\":\"%s\",\"codes\":[\"%s\"]}]", STOCKS[i].kr ? "trade:kr" : "trade:us", STOCKS[i].symbol);
  wsSend(decl, strlen(decl));
  wsSym = i;
}

bool wsOpen(int i) {
  if (ESP.getFreeHeap() < 20000) {
    tossStatus = "메모리 부족 (" + String(ESP.getFreeHeap()) + "B)";
    return false;
  }
  ws = new BearSSL::WiFiClientSecure();
  ws->setInsecure();
  ws->setBufferSizes(wsRx, 512);
  ws->setTimeout(8000);
  if (!ws->connect(TOSS_WS_HOST, 443)) {
    tossStatus = "연결하지 못했어요";
    wsRx = 8192;
    wsClose();
    return false;
  }
  uint8_t rnd[16];
  for (uint8_t& b : rnd) b = static_cast<uint8_t>(random(256));
  ws->print(String("GET /ws/v1 HTTP/1.1\r\nHost: ") + TOSS_WS_HOST +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
            base64::encode(rnd, sizeof(rnd)) + "\r\nAuthorization: Bearer " + tossToken + "\r\n\r\n");
  const String status = ws->readStringUntil('\n');
  if (status.indexOf(" 101") < 0) {
    if (status.indexOf(" 401") >= 0) {
      tossToken = "";  // 토큰이 무효 - 다음에 새로 받는다
      tossStatus = "토큰이 무효예요 (다시 받는 중)";
    } else if (status.indexOf(" 403") >= 0) {
      tossStatus = "허용 IP 가 아니에요 (토스 설정 > Open API > 허용 IP)";
    } else {
      tossStatus = "연결 거절: " + status.substring(0, 24);
    }
    wsClose();
    return false;
  }
  while (ws->connected()) {  // 나머지 응답 머리 줄은 빈 줄까지 버린다
    const String line = ws->readStringUntil('\n');
    if (line.length() <= 1) break;
  }
  wsBuf = static_cast<uint8_t*>(malloc(WSBUF));
  if (!wsBuf) {
    tossStatus = "메모리 부족";
    wsClose();
    return false;
  }
  wsTrades = 0;
  wsOpenedAt = wsPingAt = millis();
  wsSubscribe(i);
  tossStatus = "연결됨";
  return true;
}

// 체결 하나 - 가격과 마지막 1분봉을 갱신한다 (다음 분이면 새 캔들)
void onTrade(int i, float price, float volume, time_t t) {
  Quote& q = quotes[i];
  q.price = price;
  q.at = t;
  const time_t m = t - t % 60;
  if (q.n == 0 || m > q.lastMin) {
    if (q.n == HIST) {
      memmove(q.candles, q.candles + 1, sizeof(Candle) * (HIST - 1));
      q.n--;
    }
    q.candles[q.n++] = {price, price, price, price, volume};
    q.lastMin = m;
  } else if (m == q.lastMin) {
    Candle& c = q.candles[q.n - 1];
    c.h = max(c.h, price);
    c.l = min(c.l, price);
    c.c = price;
    c.v += volume;
  }
  // 시세는 30분마다만 새로 받으니, 애프터 → 데이처럼 세션이 바뀌면 체결 시각으로 바로 고친다
  strcpy(q.session, sessionLabel(sessionAt(i, t)));
  wsTrades++;
  tradeDirty = true;
}

void wsHandleText(size_t n) {
  if (n == 0 || wsBuf[0] != '{') return;
  JsonDocument doc;
  if (deserializeJson(doc, reinterpret_cast<const char*>(wsBuf), n)) return;
  const char* type = doc["type"] | "";
  if (strcmp(type, "message") == 0 && wsSym >= 0) {
    const char* p = doc["data"]["price"] | "";
    const time_t t = parseIso(doc["data"]["timestamp"] | "");
    if (*p) onTrade(wsSym, atof(p), atof(doc["data"]["volume"] | "0"), t ? t : time(nullptr));
  } else if (strcmp(type, "error") == 0) {
    const char* code = doc["error"]["code"] | "";
    if (strcmp(code, "auth-revoked") == 0) tossToken = "";
    tossStatus = String("오류: ") + code;
    wsClose();
    wsRetryAt = millis() + 15000;
  }
}

// 받은 프레임을 처리한다 (한 번에 최대 20개 - 화면 그리기가 밀리지 않게)
void wsPoll() {
  for (int k = 0; k < 20 && ws && ws->available() >= 2; k++) {
    const uint8_t h0 = ws->read(), h1 = ws->read();
    const uint8_t op = h0 & 0x0F;
    size_t n = h1 & 0x7F;
    if (n == 126) {
      n = (ws->read() << 8) | ws->read();
    } else if (n == 127) {
      n = 0;
      for (int b = 0; b < 8; b++) n = (n << 8) | ws->read();
    }
    if (n > WSBUF) {  // 너무 큰 프레임은 버린다
      for (size_t r = 0; r < n && ws->connected(); r++) ws->read();
      continue;
    }
    if (ws->readBytes(wsBuf, n) != n) break;
    if (op == 0x1) {
      wsHandleText(n);
    } else if (op == 0x9) {
      wsSend(reinterpret_cast<const char*>(wsBuf), n, 0xA);  // ping → pong
    } else if (op == 0x8) {
      tossStatus = "서버가 연결을 닫았어요";
      wsClose();
      wsRetryAt = millis() + 5000;
    }
  }
  if (ws && !ws->connected()) {
    tossStatus = "연결이 끊겼어요";
    wsClose();
    wsRetryAt = millis() + 5000;
  }
}

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
bool updating = false;  // 펌웨어를 받는 중 - 소켓 · 받아오기 · 그리기를 멈춘다
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
    d += rowHtml("토스 실시간", !tossReady() ? String("키 없음") : ws ? "연결됨 · 체결 " + String(wsTrades) + "건" : tossStatus);
    if (theme != TH_PET) {
      const Quote& q = quotes[theme - 1];
      char last[8] = "-";
      if (q.n) strftime(last, sizeof(last), "%H:%M", localtime(&q.lastMin));
      d += rowHtml("차트", String(q.n) + "개 · 마지막 " + last);
      d += rowHtml("기준가", q.baseDate[0] ? String(q.baseDate) + " 종가 " + String(q.prev, 2) : String("아직 없음"));
    }
  }
  d += rowHtml("주소", ipText()) + rowHtml("켜진 지", uptimeText()) +
       rowHtml("남은 메모리", String(ESP.getFreeHeap() / 1024) + " KB (한 덩어리 " + String(ESP.getMaxFreeBlockSize() / 1024) + " KB)") +
       rowHtml("펌웨어", VERSION) + rowHtml("마지막 재시작", ESP.getResetReason()) + "</div>";
  if (safeMode) d += "<p class=desc>연속으로 멈춰서 화면 그리기와 받기를 멈췄어요 (안전 모드). 새 펌웨어를 올리거나 전원을 다시 꽂으면 풀려요.</p>";
  if (lastNetErr.length()) d += "<p class=desc>받기 문제: " + lastNetErr + "</p>";
  if (safeMode || ESP.getResetInfoPtr()->reason == REASON_EXCEPTION_RST) d += "<p class=desc>" + ESP.getResetInfo() + "</p>";
  d += "<div class=end><button class='b sub' onclick='wf.hidden=!wf.hidden'>와이파이 바꾸기</button>"
       "<a class='btn sub' href=/update>펌웨어 올리기</a></div>";
  if (!setupMode) {
    d += String("<form method=post action=/toss><label class=check>토스증권 Open API 키 (") + (tossReady() ? "등록됨" : "없음") +
         ") - 기기에만 저장돼요</label><input name=id placeholder='client_id' autocomplete=off>"
         "<input name=secret type=password placeholder='client_secret' autocomplete=off>"
         "<div class=end><button class='b sub'>키 저장</button></div></form>";
  }
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
  wsClose();
  wsRetryAt = 0;
  if (theme != TH_PET) {
    const Quote& q = quotes[theme - 1];
    buildChart(q);
    if (!q.ok || q.n == 0 || millis() - q.fetchedAt > 60000) nextQuote = 0;  // 차트가 비었거나 1분 넘게 지났으면 바로 다시 받는다
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

void handleToss() {
  const String id = server.arg("id"), secret = server.arg("secret");
  if (id.length() == 0 || secret.length() == 0 || id.length() >= TOSS_ID_LEN || secret.length() >= TOSS_SECRET_LEN) {
    server.send(400, "text/plain; charset=utf-8", "client_id 와 client_secret 을 둘 다 넣어 주세요");
    return;
  }
  strncpy(tossId, id.c_str(), TOSS_ID_LEN - 1);
  strncpy(tossSecret, secret.c_str(), TOSS_SECRET_LEN - 1);
  saveSettings();
  tossToken = "";
  wsClose();
  wsRetryAt = 0;
  tossStatus = "키 저장함 - 곧 연결해요";
  sendNotice("토스 키를 저장했어요", "주가 테마에서 실시간 체결을 받기 시작해요.", 3);
}

// 토스 시세 원본 보기 (확인용) - 시세 · 캔들 · 장 캘린더 주소만 연다 (계좌 · 주문은 막는다)
void handleTossRaw() {
  const String p = server.arg("p");
  if (!(p.startsWith("/api/v1/candles?") || p.startsWith("/api/v1/prices?") || p.startsWith("/api/v1/trades?") ||
        p.startsWith("/api/v1/market-calendar/"))) {
    server.send(400, "text/plain; charset=utf-8", "시세 · 캔들 · 장 캘린더 주소만 볼 수 있어요");
    return;
  }
  wsClose();  // 소켓이 쓰던 메모리를 돌려받는다 - 다음 반복에서 다시 붙는다
  String out;
  const bool ok = tossGet(p, [&](Stream& s) {
    char buf[128];
    while (out.length() < 3000) {
      const size_t n = s.readBytes(buf, sizeof(buf));
      if (n == 0) break;
      out.concat(buf, n);
    }
    return true;
  });
  server.send(ok ? 200 : 502, "application/json; charset=utf-8", ok ? out : "{\"error\":\"" + lastNetErr + "\"}");
}

void handleUpdatePage() {
  sendNotice("펌웨어 올리기", "<form method=post enctype=multipart/form-data><input type=file name=firmware accept=.bin>"
                            "<div class=end><button class='b main'>올리기</button></div></form>",
             600);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updating = true;
    wsClose();  // 소켓이 쓰던 메모리(약 15KB)를 먼저 돌려받는다
    const uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    Update.begin(maxSketch, U_FLASH);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning()) Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning()) Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    updating = false;
  }
  yield();
}

void handleUpdateDone() {
  if (Update.hasError() || !Update.isFinished()) {
    server.send(200, "text/plain; charset=utf-8", "Update error: " + Update.getErrorString());
    updating = false;
    return;
  }
  server.send(200, "text/html", "<META http-equiv=\"refresh\" content=\"15;URL=/\">Update Success! Rebooting...");
  restartAt = millis() + 1000;
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

// 주가 테마 + 토스 키가 있으면 소켓을 붙여 둔다
void tossLoop() {
  // 시세를 못 받았으면 소켓을 붙이기 전에 다시 받는다 (붙으면 30분 동안 다시 받지 않는다). 3번 연속 못 받으면 전 차트로 실시간만 잇는다
  const bool want = theme != TH_PET && tossReady() && !safeMode && quotes[theme - 1].ok &&
                    (quotes[theme - 1].fails == 0 || quotes[theme - 1].fails >= 3);
  if (!want) {
    wsClose();
    return;
  }
  const int i = theme - 1;
  if (!ws) {
    if (millis() < wsRetryAt) return;
    if (tossToken.length() == 0 || millis() > tossTokenExp) {
      if (!fetchTossToken()) {
        wsRetryAt = millis() + 60000;
        return;
      }
    }
    if (!wsOpen(i)) {
      wsRetryAt = millis() + 30000;
      return;
    }
  }
  if (wsSym != i) wsSubscribe(i);
  wsPoll();
  if (ws && millis() - wsPingAt > 50000) {
    wsSend("PING", 4);
    wsPingAt = millis();
  }
  if (ws && millis() - wsOpenedAt > 30UL * 60000) {  // 30분마다 시세를 HTTPS 로 새로 받는다
    wsClose();
    nextQuote = 0;
  }
  if (tradeDirty && millis() - lastTradeDraw > 250) {  // 체결은 초당 4번까지만 다시 그린다
    tradeDirty = false;
    lastTradeDraw = millis();
    buildChart(quotes[i]);
    renderRows(0, 240);
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
    if (ws) return;  // 실시간 체결을 받는 중
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

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/theme", handleTheme);
  server.on("/mode", handleMode);
  server.on("/poke", handlePoke);
  server.on("/thumb", handleThumb);
  server.on("/toss", HTTP_POST, handleToss);
  server.on("/toss/raw", HTTP_GET, handleTossRaw);
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
  if (setupMode || updating) return;
  MDNS.update();
  if (safeMode) return;
  static bool cleared = false;
  if (!cleared && millis() > 120000) {  // 2분 버티면 정상으로 보고 멈춤 횟수를 지운다
    clearCrashCount();
    cleared = true;
  }
  tickMinute();
  fetchDue();
  tossLoop();
  if (theme == TH_PET) animatePet();
}
