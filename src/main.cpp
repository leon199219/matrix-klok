// Matrix-Klok — ESP32 + WS2812B 8x32 + DS3231 (RTC in UTC) + NTP + WiFiManager + OTA
#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "config.h"
#include "fonts.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ---------------------------------------------------------------- globals
CRGB leds[NUM_LEDS];
RTC_DS3231 rtc;
WiFiManager wm;
Preferences prefs;
WebServer web(80);
bool webStarted = false;
uint32_t restartAtMs = 0;      // >0 = herstart gepland
bool apToggleReq = false;      // web-verzoek: setup-AP aan/uit
void startPortal(); void stopPortal();
// dagelijkse aan/uit-timer (lokale tijd, systeemtijd komt van NTP of DS3231)
bool timerOn = false;
uint16_t timerOnMin = 7 * 60;    // display aan om 07:00
uint16_t timerOffMin = 23 * 60;  // display uit om 23:00
const uint32_t WAKE_MS = 30000;  // knop tijdens uit-periode = 30 s tonen

bool rtcPresent = false;
bool rtcValid = false;           // RTC heeft geldige tijd
volatile bool ntpSyncFlag = false;
bool ntpEverSynced = false;
uint32_t lastNtpSyncMs = 0;
uint32_t lastRtcToSysMs = 0;
bool portalActive = false;
bool otaStarted = false;
bool wifiWasConnected = false;
bool otaBusy = false;

// SQW 1 Hz
volatile uint32_t sqwEdgeMs = 0;
volatile uint32_t sqwEdgeCount = 0;
void IRAM_ATTR sqwIsr() { sqwEdgeMs = millis(); sqwEdgeCount = sqwEdgeCount + 1; }

// settings
uint8_t briIdx = DEFAULT_BRI_IDX;
uint8_t colorMode = 0;
bool hourlyShows = true;
#define SHOW_COUNT 13   // aantal uurshows (rotatie)
uint8_t showIdx = 0;

enum ColorMode { CM_RAINBOW, CM_CYCLE, CM_WARM, CM_CYAN, CM_ORANGE, CM_RED, CM_GREEN, CM_PURPLE, CM_BLUE, CM_COUNT };
static const char* COLOR_NAMES[CM_COUNT] = {"REGENBOOG","KLEURWISSEL","WARM WIT","CYAAN","ORANJE","ROOD","GROEN","PAARS","BLAUW"};

enum Screen { SC_TIME, SC_SECS, SC_DATE, SC_TEMP, SC_COUNT };
uint8_t screen = SC_TIME, prevScreen = SC_TIME;
uint32_t screenChangedMs = 0;
int8_t slideDir = 1;
uint32_t lastInteractionMs = 0;

// ---------------------------------------------------------------- matrix mapping
// x: 0..31 links->rechts, y: 0..7 boven->onder
uint16_t XY(int x, int y) {
#if MIRROR_X
  x = MATRIX_W - 1 - x;
#endif
#if ORIGIN_BOTTOM_LEFT
  int py = MATRIX_H - 1 - y;     // rij geteld vanaf onder
#else
  int py = y;
#endif
#if COLUMN_MAJOR
  int inner = py;
#if SERPENTINE
  if (x & 1) inner = MATRIX_H - 1 - py;
#endif
  return x * MATRIX_H + inner;
#else
  int inner = x;
#if SERPENTINE
  if (py & 1) inner = MATRIX_W - 1 - x;
#endif
  return py * MATRIX_W + inner;
#endif
}

// clip-venster
int clipX0 = 0, clipX1 = MATRIX_W - 1, clipY0 = 0, clipY1 = MATRIX_H - 1;
void resetClip() { clipX0 = 0; clipX1 = MATRIX_W - 1; clipY0 = 0; clipY1 = MATRIX_H - 1; }
inline void setPix(int x, int y, const CRGB& c) {
  if (x < clipX0 || x > clipX1 || y < clipY0 || y > clipY1) return;
  leds[XY(x, y)] = c;
}
inline void addPix(int x, int y, const CRGB& c) {
  if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) return;
  leds[XY(x, y)] += c;
}

// ---------------------------------------------------------------- kleuren
CRGB colorAt(int x, int y) {
  uint32_t ms = millis();
  switch (colorMode) {
    case CM_RAINBOW: return CHSV((uint8_t)(ms / 120 + x * 6 + y * 2), 230, 255);
    case CM_CYCLE:   return CHSV((uint8_t)(ms / 200), 240, 255);
    case CM_WARM:    return CRGB(255, 150, 60);
    case CM_CYAN:    return CHSV(128, 255, 255);
    case CM_ORANGE:  return CHSV(22, 255, 255);
    case CM_RED:     return CHSV(0, 255, 255);
    case CM_GREEN:   return CHSV(96, 255, 255);
    case CM_PURPLE:  return CHSV(200, 255, 255);
    default:         return CHSV(160, 255, 255);
  }
}
CRGB accentColor() {   // contrastkleur voor labels
  if (colorMode == CM_RAINBOW || colorMode == CM_CYCLE) return CHSV((uint8_t)(millis() / 120 + 128), 200, 255);
  return CRGB(255, 255, 255);
}

// ---------------------------------------------------------------- tekenen
// 5x7 cijfer op (x,y). d=10 -> '-'
void drawDigit5x7(int d, int x, int y, uint8_t scale = 255) {
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 5; c++)
      if (DIGIT5x7[d][r] & (1 << (4 - c))) {
        CRGB col = colorAt(x + c, y + r);
        if (scale < 255) col.nscale8(scale);
        setPix(x + c, y + r, col);
      }
}

int textWidth(const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) { w += findGlyph(*p)->w; if (p[1]) w += 1; }
  return w;
}
// kleur: nullptr -> colorAt
int drawText(const char* s, int x, int y, const CRGB* fixed = nullptr) {
  for (const char* p = s; *p; p++) {
    const Glyph* g = findGlyph(*p);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < g->w; c++)
        if (g->r[r] & (1 << (g->w - 1 - c))) setPix(x + c, y + r, fixed ? *fixed : colorAt(x + c, y + r));
    x += g->w + 1;
  }
  return x;
}

// 8x8 iconen: '.'=uit, letters = kleur
void drawIcon(const char* const rows[8], int ox, int oy, uint8_t layerMax = 9) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      char ch = rows[r][c];
      CRGB col;
      switch (ch) {
        case 'w': col = CRGB(90, 90, 90); break;
        case 'r': col = CRGB(255, 20, 0); break;
        case 'b': col = CRGB(0, 120, 255); break;
        case '0': case '1': case '2': case '3':
          if (ch - '0' > layerMax) continue;
          col = CRGB(0, 160, 255); break;
        default: continue;
      }
      setPix(ox + c, oy + r, col);
    }
}
static const char* const ICON_THERMO[8] = {
  "...ww...", "..w..w..", "..wrrw..", "..wrrw..", "..wrrw..", ".wrrrrw.", ".wrrrrw.", "..wwww.."};
static const char* const ICON_WIFI[8] = {
  ".333333.", "3......3", "..2222..", ".2....2.", "...11...", "..1..1..", "........", "...00..."};

// ---------------------------------------------------------------- tijd
bool clockValid() { return time(nullptr) > 1700000000; }

void setSystemTimeFromRtc() {
  if (!rtcPresent || !rtcValid) return;
  DateTime n = rtc.now();                 // RTC staat in UTC
  struct timeval tv = {(time_t)n.unixtime(), 0};
  settimeofday(&tv, nullptr);
  lastRtcToSysMs = millis();
}

void ntpCallback(struct timeval* tv) { ntpSyncFlag = true; }

bool getLocal(struct tm& t) {
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return clockValid();
}

// fractie van de lopende seconde 0..999 (SQW indien actief, anders systeemklok)
bool sqwActive() { return sqwEdgeCount > 2 && (millis() - sqwEdgeMs) < 2500; }
uint16_t subSecondMs() {
  if (sqwActive()) { uint32_t d = millis() - sqwEdgeMs; return d > 999 ? 999 : d; }
  struct timeval tv; gettimeofday(&tv, nullptr);
  return tv.tv_usec / 1000;
}

bool isNight(const struct tm& t) {
#if NIGHT_DIM_ENABLED
  if (!clockValid()) return false;
  int h = t.tm_hour;
  if (NIGHT_START_HOUR > NIGHT_END_HOUR) return h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR;
  return h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR;
#else
  return false;
#endif
}


void saveSettings();

// true = display hoort nu uit te staan volgens de timer
bool timerSaysOff(const struct tm& t) {
  if (!timerOn || !clockValid() || timerOnMin == timerOffMin) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  if (timerOffMin > timerOnMin) return m >= timerOffMin || m < timerOnMin;  // uit over middernacht heen
  return m >= timerOffMin && m < timerOnMin;
}

// ---------------------------------------------------------------- web-GUI
String hhmm(uint16_t m) { char b[6]; snprintf(b, sizeof(b), "%02u:%02u", m / 60, m % 60); return String(b); }
int parseHHMM(const String& v) {
  int h, m; if (sscanf(v.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}
void handleRoot() {
  struct tm t; bool valid = getLocal(t);
  char now[24] = "onbekend";
  if (valid) strftime(now, sizeof(now), "%d-%m-%Y %H:%M:%S", &t);
  String h = F("<!doctype html><html lang=nl><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Matrix-Klok</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:420px;margin:24px auto;padding:0 16px}"
    "h1{font-size:1.4em}.card{background:#1e1e1e;border-radius:12px;padding:16px;margin:12px 0}"
    "label{display:block;margin:10px 0 4px}input[type=time]{font-size:1.2em;padding:6px;border-radius:8px;border:1px solid #444;background:#222;color:#eee}"
    "button{margin-top:16px;font-size:1.1em;padding:10px 18px;border:0;border-radius:8px;background:#0a84ff;color:#fff}"
    ".muted{color:#999;font-size:.9em}</style></head><body><h1>Matrix-Klok</h1>");
  h += "<div class=card>Tijd: <b>" + String(now) + "</b><br><span class=muted>Bron: ";
  h += ntpEverSynced ? "NTP" : (rtcValid ? "DS3231" : "geen");
  h += "</span><br>Display nu: <b>";
  h += (valid && timerSaysOff(t)) ? "uit (timer)" : "aan";
  h += "</b></div><form method=post action=/save class=card><h2 style='font-size:1.1em;margin-top:0'>Aan/uit-timer</h2>"
       "<label><input type=checkbox name=tmr value=1";
  if (timerOn) h += " checked";
  h += "> Timer gebruiken</label><label>Aan om</label><input type=time name=on value=" + hhmm(timerOnMin) +
       "><label>Uit om</label><input type=time name=off value=" + hhmm(timerOffMin) +
       "><p class=muted>Elke dag. In de uit-periode is het display zwart; een knop indrukken toont de klok 30 seconden.</p>"
       "<label><input type=checkbox name=show value=1";
  if (hourlyShows) h += " checked";
  h += "> Uurshows (animatie elk heel uur)</label>"
       "<button type=submit>Opslaan</button></form>"
       "<div class=card><h2 style='font-size:1.1em;margin-top:0'>Systeem</h2>"
       "<form method=post action=/ap style='display:inline'><button type=submit style='background:#555'>Setup-access-point aanzetten</button></form> "
       "<form method=post action=/restart style='display:inline' onsubmit=\"return confirm('Klok herstarten?')\">"
       "<button type=submit style='background:#d33'>Herstarten</button></form>"
       "<p class=muted>Het access point heet " AP_NAME ". Zolang het aan staat zie je hier de wifi-setup; daar zit ook een Exit-knop om het weer uit te zetten. Lang drukken op de linkerknop zet het ook aan of uit.</p></div>"
       "</body></html>";
  web.send(200, "text/html", h);
}
void handleSave() {
  timerOn = web.hasArg("tmr");
  hourlyShows = web.hasArg("show");
  int on = parseHHMM(web.arg("on")), off = parseHHMM(web.arg("off"));
  if (on >= 0) timerOnMin = on;
  if (off >= 0) timerOffMin = off;
  saveSettings();
  web.sendHeader("Location", "/");
  web.send(303);
}
void handleApi() {
  struct tm t; bool valid = getLocal(t);
  String j = "{\"timer\":" + String(timerOn ? "true" : "false") + ",\"on\":\"" + hhmm(timerOnMin) +
             "\",\"off\":\"" + hhmm(timerOffMin) + "\",\"display_off\":" + String((valid && timerSaysOff(t)) ? "true" : "false") + "}";
  web.send(200, "application/json", j);
}
void handleRestart() {
  web.send(200, "text/html", "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='15;url=/'>"
           "<body style='font-family:system-ui;background:#111;color:#eee;text-align:center;padding-top:40px'>"
           "Klok herstart... deze pagina laadt over 15 s opnieuw.</body>");
  restartAtMs = millis() + 800;
}
void handleAp() {
  web.send(200, "text/html", "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='5;url=/'>"
           "<body style='font-family:system-ui;background:#111;color:#eee;text-align:center;padding-top:40px'>"
           "Setup-access-point " AP_NAME " gaat aan. Deze pagina toont zo de wifi-setup (met Exit om het uit te zetten).</body>");
  apToggleReq = true;
}
void startWeb() {
  static bool routes = false;
  if (!routes) {
    web.on("/", HTTP_GET, handleRoot);
    web.on("/save", HTTP_POST, handleSave);
    web.on("/api/timer", HTTP_GET, handleApi);
    web.on("/restart", HTTP_POST, handleRestart);
    web.on("/ap", HTTP_POST, handleAp);
    routes = true;
  }
  web.begin();
  webStarted = true;
}

// ---------------------------------------------------------------- knoppen
struct Button {
  uint8_t pin; bool stable = false, lastRead = false; uint32_t lastChange = 0, pressStart = 0;
  bool longFired = false, vlongFired = false;
  bool evShort = false, evLong = false, evVLong = false;
  void begin(uint8_t p) { pin = p; pinMode(pin, INPUT_PULLUP); }
  bool raw() { return digitalRead(pin) == LOW; }
  void update() {
    evShort = evLong = evVLong = false;
    bool r = raw(); uint32_t now = millis();
    if (r != lastRead) { lastRead = r; lastChange = now; }
    if (now - lastChange > 25 && r != stable) {
      stable = r;
      if (stable) { pressStart = now; longFired = vlongFired = false; }
      else if (!longFired) evShort = true;
    }
    if (stable) {
      if (!longFired && now - pressStart > LONG_PRESS_MS) { longFired = true; evLong = true; }
      if (!vlongFired && now - pressStart > WIFI_RESET_MS) { vlongFired = true; evVLong = true; }
    }
  }
};
Button bL, bM, bR;

// ---------------------------------------------------------------- settings
void loadSettings() {
  prefs.begin("klok", true);
  briIdx = prefs.getUChar("bri", DEFAULT_BRI_IDX);
  colorMode = prefs.getUChar("col", CM_RAINBOW);
  hourlyShows = prefs.getBool("show", true);
  showIdx = prefs.getUChar("sidx", 0) % SHOW_COUNT;
  timerOn = prefs.getBool("tmr", false);
  timerOnMin = prefs.getUShort("ton", 7 * 60);
  timerOffMin = prefs.getUShort("toff", 23 * 60);
  prefs.end();
  if (timerOnMin >= 1440) timerOnMin = 7 * 60;
  if (timerOffMin >= 1440) timerOffMin = 23 * 60;
  if (briIdx >= sizeof(BRIGHTNESS_STEPS)) briIdx = DEFAULT_BRI_IDX;
  if (colorMode >= CM_COUNT) colorMode = CM_RAINBOW;
}
void saveSettings() {
  prefs.begin("klok", false);
  prefs.putUChar("bri", briIdx); prefs.putUChar("col", colorMode);
  prefs.putBool("show", hourlyShows); prefs.putUChar("sidx", showIdx);
  prefs.putBool("tmr", timerOn); prefs.putUShort("ton", timerOnMin); prefs.putUShort("toff", timerOffMin);
  prefs.end();
}

// ---------------------------------------------------------------- overlays / berichten
enum OverlayType { OV_NONE, OV_TEXT, OV_BRIGHT };
OverlayType overlay = OV_NONE;
char overlayText[48];
uint32_t overlayStart = 0, overlayDur = 0;
CRGB overlayColor = CRGB::White;

void showMessage(const char* s, CRGB col = CRGB::White) {
  strncpy(overlayText, s, sizeof(overlayText) - 1); overlayText[sizeof(overlayText) - 1] = 0;
  overlay = OV_TEXT; overlayStart = millis(); overlayColor = col;
  int w = textWidth(overlayText);
  overlayDur = (w <= MATRIX_W) ? 1600 : (uint32_t)(MATRIX_W + w) * 45 + 300;
}
void drawOverlay() {
  uint32_t e = millis() - overlayStart;
  if (overlay == OV_TEXT) {
    int w = textWidth(overlayText);
    int x = (w <= MATRIX_W) ? (MATRIX_W - w) / 2 : MATRIX_W - (int)(e / 45);
    bool useMode = overlayColor == CRGB(0, 0, 0);
    drawText(overlayText, x, 1, useMode ? nullptr : &overlayColor);
  } else if (overlay == OV_BRIGHT) {
    const int n = sizeof(BRIGHTNESS_STEPS);
    for (int i = 0; i < n; i++) {
      int h = 1 + i, x = 4 + i * 4;
      for (int dx = 0; dx < 3; dx++)
        for (int dy = 0; dy < h; dy++) {
          CRGB c = (i <= briIdx) ? colorAt(x + dx, 7 - dy) : CRGB(20, 20, 20);
          setPix(x + dx, 7 - dy, c);
        }
    }
    // zonnetje links
    CRGB s(255, 200, 0);
    setPix(1, 3, s); setPix(1, 4, s); setPix(0, 2, s); setPix(2, 2, s); setPix(0, 5, s); setPix(2, 5, s);
  }
}

// ---------------------------------------------------------------- schermen
uint8_t shownDigits[4] = {255, 255, 255, 255}, oldDigits[4] = {0, 0, 0, 0};
uint32_t digitChangeMs[4] = {0, 0, 0, 0};
const uint32_t ROLL_MS = 450;

void drawTimeScreen(int ox, const struct tm& t, bool valid) {
  static const int DX[4] = {3, 9, 17, 23};
  uint32_t now = millis();
  if (!valid) {
    if ((now / 500) % 2 == 0) {
      for (int i = 0; i < 4; i++) drawDigit5x7(10, ox + DX[i], 0);
      setPix(ox + 15, 2, colorAt(15, 2)); setPix(ox + 15, 4, colorAt(15, 4));
    }
    return;
  }
  uint8_t d[4] = {(uint8_t)(t.tm_hour / 10), (uint8_t)(t.tm_hour % 10), (uint8_t)(t.tm_min / 10), (uint8_t)(t.tm_min % 10)};
  for (int i = 0; i < 4; i++) {
    if (shownDigits[i] == 255) { shownDigits[i] = d[i]; digitChangeMs[i] = 0; }
    else if (d[i] != shownDigits[i]) { oldDigits[i] = shownDigits[i]; shownDigits[i] = d[i]; digitChangeMs[i] = now + i * 0; }
  }
  int cy0 = clipY0, cy1 = clipY1;
  clipY0 = 0; clipY1 = 6;   // cijfers in rij 0..6, rij 7 = secondenbalk
  for (int i = 0; i < 4; i++) {
    uint32_t e = now - digitChangeMs[i];
    if (digitChangeMs[i] && e < ROLL_MS) {
      float p = e / (float)ROLL_MS;
      p = p * p * (3 - 2 * p);                 // smoothstep
      int off = (int)roundf(p * 8);
      drawDigit5x7(oldDigits[i], ox + DX[i], off);        // oud rolt omlaag weg
      drawDigit5x7(shownDigits[i], ox + DX[i], off - 8);  // nieuw komt van boven
    } else {
      drawDigit5x7(shownDigits[i], ox + DX[i], 0);
    }
  }
  clipY0 = cy0; clipY1 = cy1;
  // dubbele punt pulseert op de seconde (SQW)
  float f = subSecondMs() / 1000.0f;
  uint8_t v = 60 + (uint8_t)(195 * (1 - f) * (1 - f));
  CRGB c1 = colorAt(15, 2); c1.nscale8(v);
  CRGB c2 = colorAt(15, 4); c2.nscale8(v);
  setPix(ox + 15, 2, c1); setPix(ox + 15, 4, c2);
  // secondenbalk onderin
  int sec = t.tm_sec;
  float prog = (sec + f) * MATRIX_W / 60.0f;
  for (int x = 0; x < MATRIX_W; x++) {
    if (x < (int)prog) { CRGB c = colorAt(x, 7); c.nscale8(70); setPix(ox + x, 7, c); }
    else if (x == (int)prog) { CRGB c = colorAt(x, 7); c.nscale8((uint8_t)(70 + 185 * (prog - x))); setPix(ox + x, 7, c); }
  }
}

void drawSecsScreen(int ox, const struct tm& t, bool valid) {
  char buf[12];
  if (valid) snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else strcpy(buf, "--:--:--");
  int w = textWidth(buf);
  drawText(buf, ox + (MATRIX_W - w) / 2, 1);
  // pulserende stip onderin op SQW-tik
  uint8_t v = 255 - (uint8_t)min(255, subSecondMs() / 3);
  CRGB c = accentColor(); c.nscale8(v);
  setPix(ox + 15, 7, c); setPix(ox + 16, 7, c);
}

static const char* DAYS_NL[7] = {"ZO", "MA", "DI", "WO", "DO", "VR", "ZA"};
void drawDateScreen(int ox, const struct tm& t, bool valid) {
  char d[4], rest[16];
  if (valid) { strcpy(d, DAYS_NL[t.tm_wday]); snprintf(rest, sizeof(rest), "%d-%d", t.tm_mday, t.tm_mon + 1); }
  else { strcpy(d, "--"); strcpy(rest, "--"); }
  int w = textWidth(d) + 3 + textWidth(rest);
  int x = ox + (MATRIX_W - w) / 2;
  CRGB a = accentColor();
  x = drawText(d, x, 1, &a);
  x += 2;
  drawText(rest, x, 1);
}

void drawTempScreen(int ox) {
  drawIcon(ICON_THERMO, ox + 0, 0);
  char buf[12];
  if (rtcPresent) snprintf(buf, sizeof(buf), "%.1f*C", rtc.getTemperature());
  else strcpy(buf, "--*C");
  int w = textWidth(buf);
  drawText(buf, ox + 9 + (MATRIX_W - 9 - w) / 2, 1);
}

void drawScreen(uint8_t s, int ox, const struct tm& t, bool valid) {
  switch (s) {
    case SC_TIME: drawTimeScreen(ox, t, valid); break;
    case SC_SECS: drawSecsScreen(ox, t, valid); break;
    case SC_DATE: drawDateScreen(ox, t, valid); break;
    case SC_TEMP: drawTempScreen(ox); break;
  }
}

// ---------------------------------------------------------------- animaties
// --- vuurwerk
struct Particle { float x, y, vx, vy; uint8_t hue; uint8_t life; bool rocket; };
Particle parts[64];
void fxReset() { for (auto& p : parts) p.life = 0; }
void spawnRocket() {
  for (auto& p : parts) if (!p.life) {
    p = {(float)random(3, MATRIX_W - 3), (float)MATRIX_H, random(-20, 21) / 100.0f, -random(30, 45) / 100.0f, (uint8_t)random(256), 255, true};
    return;
  }
}
void fxFireworks() {
  fadeToBlackBy(leds, NUM_LEDS, 45);
  if (random(100) < 7) spawnRocket();
  for (auto& p : parts) {
    if (!p.life) continue;
    p.x += p.vx; p.y += p.vy;
    if (p.rocket) {
      p.vy += 0.012f;
      addPix((int)p.x, (int)p.y, CRGB(255, 180, 80));
      if (p.vy > -0.06f || p.y < 1.5f) {          // knal!
        p.life = 0;
        float cx = p.x, cy = p.y; uint8_t h = p.hue;
        int n = 0;
        for (auto& q : parts) if (!q.life && n < 14) {
          float a = random(628) / 100.0f, s = random(15, 55) / 100.0f;
          q = {cx, cy, cosf(a) * s, sinf(a) * s * 0.7f, (uint8_t)(h + random(-12, 12)), 255, false};
          n++;
        }
      }
    } else {
      p.vy += 0.02f; p.vx *= 0.96f;
      p.life = p.life > 10 ? p.life - 10 : 0;
      addPix((int)roundf(p.x), (int)roundf(p.y), CHSV(p.hue, 200, p.life));
    }
    if (p.x < -2 || p.x > MATRIX_W + 2 || p.y > MATRIX_H + 2) p.life = 0;
  }
}
// --- matrix-regen
float dropY[MATRIX_W], dropV[MATRIX_W];
void fxRainInit() { for (int x = 0; x < MATRIX_W; x++) { dropY[x] = -random(0, 16); dropV[x] = random(8, 25) / 100.0f; } }
void fxRain() {
  fadeToBlackBy(leds, NUM_LEDS, 50);
  for (int x = 0; x < MATRIX_W; x++) {
    dropY[x] += dropV[x];
    int y = (int)dropY[x];
    if (y >= 0 && y < MATRIX_H) { leds[XY(x, y)] = CRGB(150, 255, 150); if (y > 0) leds[XY(x, y - 1)] |= CRGB(0, 200, 0); }
    if (dropY[x] > MATRIX_H + random(0, 6)) { dropY[x] = -random(0, 6); dropV[x] = random(8, 25) / 100.0f; }
  }
}
// --- sterretjes
void fxSparkles() {
  fadeToBlackBy(leds, NUM_LEDS, 28);
  for (int i = 0; i < 3; i++) if (random(100) < 60) leds[random(NUM_LEDS)] = CHSV(random(256), 160, 255);
  if (random(100) < 10) leds[random(NUM_LEDS)] = CRGB::White;
}
// --- show-helpers (bounds-check, via XY() dus orientatie/MIRROR_X blijft kloppen)
inline void putPix(int x, int y, const CRGB& c) {
  if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) return;
  leds[XY(x, y)] = c;
}
// --- regenboog-golven (plasma)
void fxRainbowWaves(uint32_t t) {
  for (int x = 0; x < MATRIX_W; x++)
    for (int y = 0; y < MATRIX_H; y++) {
      uint16_t v = sin8(x * 16 + t / 6) + sin8(y * 32 + t / 9) + sin8((x + y) * 10 + t / 5);
      putPix(x, y, CHSV((uint8_t)(v / 3 + t / 20), 240, 255));
    }
}
// --- vallende sneeuw met hoopjes
struct Flake { float x, y, v, drift; };
Flake flakes[28]; uint8_t snowPile[MATRIX_W];
void fxSnowInit() {
  for (auto& f : flakes) f = {(float)random(MATRIX_W), -(float)random(0, 16), random(5, 14) / 100.0f, random(-6, 7) / 100.0f};
  memset(snowPile, 0, sizeof(snowPile));
}
void fxSnow(uint32_t t) {
  FastLED.clear();
  for (int x = 0; x < MATRIX_W; x++)
    for (int k = 0; k < snowPile[x]; k++) putPix(x, MATRIX_H - 1 - k, CRGB(110, 120, 140));
  for (auto& f : flakes) {
    f.y += f.v; f.x += f.drift + sinf((t / 400.0f) + f.y) * 0.04f;
    if (f.x < 0) f.x += MATRIX_W; if (f.x >= MATRIX_W) f.x -= MATRIX_W;
    int x = (int)f.x, y = (int)f.y;
    if (y >= MATRIX_H - 1 - snowPile[x]) {          // geland
      if (snowPile[x] < 2 && random(100) < 35) snowPile[x]++;
      f = {(float)random(MATRIX_W), -(float)random(0, 4), random(5, 14) / 100.0f, random(-6, 7) / 100.0f};
      continue;
    }
    putPix(x, y, CRGB(220, 230, 255));
  }
}
// --- Pac-Man: eerst vlucht een spookje, dan jaagt Pac-Man op blauwe spookjes
static const uint8_t PAC_OPEN[7]   = {0x1C, 0x3E, 0x70, 0x60, 0x70, 0x3E, 0x1C};  // bit6 = links
static const uint8_t PAC_CLOSED[7] = {0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C};
static const uint8_t GHOST[7]      = {0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x7F, 0x55};
void drawSprite7(const uint8_t* rows, int ox, bool flip, const CRGB& c) {
  for (int r = 0; r < 7; r++)
    for (int b = 0; b < 7; b++)
      if (rows[r] & (0x40 >> b)) putPix(ox + (flip ? 6 - b : b), r, c);
}
void drawGhost(int ox, const CRGB& body, bool scared, bool lookLeft) {
  drawSprite7(GHOST, ox, false, body);
  if (scared) { putPix(ox + 2, 2, CRGB(255, 180, 150)); putPix(ox + 4, 2, CRGB(255, 180, 150)); return; }
  putPix(ox + 1, 2, CRGB::White); putPix(ox + 2, 2, CRGB::White);
  putPix(ox + 4, 2, CRGB::White); putPix(ox + 5, 2, CRGB::White);
  putPix(ox + (lookLeft ? 1 : 2), 3, CRGB(0, 0, 255)); putPix(ox + (lookLeft ? 4 : 5), 3, CRGB(0, 0, 255));
}
void fxPacman(uint32_t t) {
  FastLED.clear();
  const uint32_t half = SHOW_DURATION_MS / 2;
  bool mouth = (t / 120) & 1;
  const uint8_t* pac = mouth ? PAC_OPEN : PAC_CLOSED;
  if (t < half) {                                   // naar rechts: Pac-Man achter rood spookje
    int span = MATRIX_W + 7 + 18;
    int px = -7 + (int)((uint64_t)t * span / half) - 10;
    for (int x = px + 7; x < MATRIX_W; x += 3) if (x >= 0) putPix(x, 3, CRGB(255, 180, 120));  // bolletjes
    drawGhost(px + 11, CRGB(255, 0, 0), false, false);
    drawSprite7(pac, px, false, CRGB(255, 220, 0));
  } else {                                          // naar links: Pac-Man jaagt blauwe spookjes
    uint32_t tt = t - half;
    int span = MATRIX_W + 7 + 26;
    int gx = MATRIX_W - (int)((uint64_t)tt * span / half);
    bool blink = tt > half - 1200 && ((tt / 150) & 1);
    CRGB blue = blink ? CRGB(200, 200, 255) : CRGB(30, 30, 255);
    drawGhost(gx, blue, true, true);
    drawGhost(gx + 9, blue, true, true);
    drawSprite7(pac, gx + 19, true, CRGB(255, 220, 0));
  }
}
// --- stuiterende ballen
struct Ball { float x, y, vx, vy; uint8_t hue; };
Ball balls[3];
void fxBallsInit() {
  for (int i = 0; i < 3; i++)
    balls[i] = {(float)random(2, MATRIX_W - 2), (float)random(0, 3), (random(2) ? 1 : -1) * random(15, 35) / 100.0f, 0, (uint8_t)(i * 85 + random(30))};
}
void fxBalls(uint32_t t) {
  fadeToBlackBy(leds, NUM_LEDS, 70);
  for (auto& b : balls) {
    b.vy += 0.035f; b.x += b.vx; b.y += b.vy;
    if (b.y > MATRIX_H - 1) { b.y = MATRIX_H - 1; b.vy = -b.vy * 0.92f; if (fabsf(b.vy) < 0.35f) b.vy = -0.75f; }
    if (b.x < 0) { b.x = 0; b.vx = -b.vx; }
    if (b.x > MATRIX_W - 1) { b.x = MATRIX_W - 1; b.vx = -b.vx; }
    putPix((int)roundf(b.x), (int)roundf(b.y), CHSV(b.hue + t / 30, 220, 255));
  }
}
// --- vuur (Fire2012, onderkant = rij MATRIX_H-1)
uint8_t heat[MATRIX_W][MATRIX_H];
// rustiger vuur: halve update-snelheid, lagere vlammen, geen witte toppen, ~55% helderheid
void fxFire(uint32_t) {
  static uint8_t tick = 0;
  if (++tick & 1) {                                  // elke 2e frame rekenen = minder flikkering
    for (int x = 0; x < MATRIX_W; x++) {
      for (int i = 0; i < MATRIX_H; i++) heat[x][i] = qsub8(heat[x][i], random8(8, 30));   // sterkere afkoeling = lagere vlammen
      for (int i = MATRIX_H - 1; i >= 2; i--) heat[x][i] = (heat[x][i - 1] + heat[x][i - 2] + heat[x][i - 2]) / 3;
      if (random8() < 90) { int i = random8(2); heat[x][i] = qadd8(heat[x][i], random8(90, 160)); }
    }
  }
  for (int x = 0; x < MATRIX_W; x++)
    for (int i = 0; i < MATRIX_H; i++) {
      CRGB c = HeatColor(scale8(heat[x][i], 170));   // max ~170 = rood/oranje, geen wit
      c.nscale8(145);                                // ~57% helderheid
      putPix(x, MATRIX_H - 1 - i, c);
    }
}
// --- generieke bitmap (bit w-1 = links)
void drawBits(const uint16_t* rows, int n, int w, int ox, int oy, const CRGB& c) {
  for (int r = 0; r < n; r++)
    for (int b = 0; b < w; b++)
      if (rows[r] & (1 << (w - 1 - b))) putPix(ox + b, oy + r, c);
}
// --- ruimteschip door sterrenveld
struct Star { float x; uint8_t y; float v; };
Star stars[30];
void fxSpaceInit() { for (auto& st : stars) st = {(float)random(MATRIX_W), (uint8_t)random(MATRIX_H), random(10, 80) / 100.0f}; }
static const uint16_t SHIP[5] = {0x10, 0x1C, 0x1F, 0x1C, 0x10};   // 5 breed, neus rechts
void fxSpace(uint32_t t) {
  fadeToBlackBy(leds, NUM_LEDS, 90);
  float warp = 1.0f + (t > 3000 && t < 6000 ? (t - 3000) / 700.0f : (t >= 6000 ? max(0.0f, 4.3f - (t - 6000) / 500.0f) : 0));
  for (auto& st : stars) {
    st.x -= st.v * warp;
    if (st.x < 0) { st = {(float)MATRIX_W + random(0, 4), (uint8_t)random(MATRIX_H), random(10, 80) / 100.0f}; }
    uint8_t v = 60 + (uint8_t)(st.v * 240);
    putPix((int)st.x, st.y, CRGB(v, v, v));
    if (warp > 1.8f) putPix((int)st.x + 1, st.y, CRGB(v / 3, v / 3, v / 2));
  }
  int sy = 2 + (int)roundf(sinf(t / 500.0f) * 1.2f);
  int sx = 4 + (int)roundf(sinf(t / 1300.0f) * 2.0f);
  drawBits(SHIP, 5, 5, sx, sy - 1, CRGB(160, 170, 200));
  putPix(sx + 3, sy + 1, CRGB(0, 180, 255));                      // cockpit
  CRGB fl = random8() < 128 ? CRGB(255, 120, 0) : CRGB(255, 40, 0);
  putPix(sx - 1, sy + 1, fl); if (random8() < 140) putPix(sx - 2, sy + 1, CRGB(120, 20, 0));
}
// --- regen met (milde) bliksem
struct Drop { float x, y, v; };
Drop rdrops[26]; uint8_t flashLeft = 0; int boltX = 0;
void fxStormInit() { for (auto& d : rdrops) d = {(float)random(MATRIX_W + 8), -(float)random(0, 10), random(25, 45) / 100.0f}; flashLeft = 0; }
void fxStorm(uint32_t) {
  fadeToBlackBy(leds, NUM_LEDS, 120);
  if (!flashLeft && random16() < 500) { flashLeft = 6; boltX = random(4, MATRIX_W - 4); }
  if (flashLeft) {
    uint8_t g = (flashLeft > 3) ? 28 : 12;                 // mild: dim grijs-blauw scherm
    for (int i = 0; i < NUM_LEDS; i++) leds[i] += CRGB(g, g, g + 8);
    int x = boltX;
    for (int y = 0; y < MATRIX_H; y++) { putPix(x, y, CRGB(150, 150, 180)); if (random8() < 110) x += random8(2) ? 1 : -1; }
    flashLeft--;
  }
  for (auto& d : rdrops) {
    d.y += d.v; d.x -= d.v * 0.35f;
    if (d.y >= MATRIX_H) d = {(float)random(MATRIX_W + 8), -(float)random(0, 4), random(25, 45) / 100.0f};
    putPix((int)d.x, (int)d.y, CRGB(40, 80, 200));
    putPix((int)(d.x + 0.35f), (int)d.y - 1, CRGB(10, 25, 70));
  }
}
// --- Tetris (zwaartekracht naar links: blokken schuiven van rechts, kolommen van 8 worden gewist)
uint8_t tBoard[MATRIX_W][MATRIX_H];          // 0 = leeg, anders hue+1-index
int8_t tCells[4][2]; int tX, tY; uint8_t tCol; uint32_t tLast; int tClearX = -1; uint32_t tClearT;
static const int8_t TETRO[7][4][2] = {
  {{0,0},{1,0},{2,0},{3,0}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{2,0},{1,1}}, {{0,0},{1,0},{2,0},{2,1}},
  {{0,0},{1,0},{2,0},{0,1}}, {{1,0},{2,0},{0,1},{1,1}}, {{0,0},{1,0},{1,1},{2,1}}};
static const uint8_t TCOLS[7] = {128, 64, 200, 22, 160, 96, 0};
bool tFits(int px, int py) {
  for (auto& c : tCells) {
    int x = px + c[0], y = py + c[1];
    if (y < 0 || y >= MATRIX_H || x < 0) return false;
    if (x < MATRIX_W && tBoard[x][y]) return false;
  }
  return true;
}
void tSpawn() {
  int k = random(7), rot = random(4);
  tCol = k;
  for (int i = 0; i < 4; i++) {
    int x = TETRO[k][i][0], y = TETRO[k][i][1];
    for (int r = 0; r < rot; r++) { int nx = y, ny = -x; x = nx; y = ny; }
    tCells[i][0] = x; tCells[i][1] = y;
  }
  int mx = 99, my = 99;
  for (auto& c : tCells) { mx = min(mx, (int)c[0]); my = min(my, (int)c[1]); }
  int h = 0;
  for (auto& c : tCells) { c[0] -= mx; c[1] -= my; h = max(h, (int)c[1]); }
  // kies de y waarbij het blok het verst naar links komt (vult gaten)
  int bestY = 0, bestX = 999;
  for (int y = 0; y + h < MATRIX_H; y++) {
    int x = MATRIX_W;
    if (!tFits(x, y)) continue;
    while (tFits(x - 1, y)) x--;
    if (x < bestX || (x == bestX && random8() < 100)) { bestX = x; bestY = y; }
  }
  tX = MATRIX_W; tY = bestY;
  if (bestX > MATRIX_W - 4) memset(tBoard, 0, sizeof(tBoard));   // vol: opnieuw
}
void fxTetrisInit() { memset(tBoard, 0, sizeof(tBoard)); tClearX = -1; tLast = 0; tSpawn(); }
void fxTetris(uint32_t t) {
  if (tClearX >= 0) {
    if (t - tClearT > 250) {                        // kolom wissen en rest opschuiven
      for (int x = tClearX; x < MATRIX_W - 1; x++) memcpy(tBoard[x], tBoard[x + 1], MATRIX_H);
      memset(tBoard[MATRIX_W - 1], 0, MATRIX_H);
      tClearX = -1;
    }
  } else if (t - tLast >= 35) {
    tLast = t;
    if (tFits(tX - 1, tY)) tX--;
    else {
      for (auto& c : tCells) { int x = tX + c[0], y = tY + c[1]; if (x < MATRIX_W) tBoard[x][y] = tCol + 1; }
      for (int x = 0; x < MATRIX_W; x++) {
        bool full = true;
        for (int y = 0; y < MATRIX_H; y++) if (!tBoard[x][y]) { full = false; break; }
        if (full) { tClearX = x; tClearT = t; break; }
      }
      tSpawn();
    }
  }
  FastLED.clear();
  for (int x = 0; x < MATRIX_W; x++)
    for (int y = 0; y < MATRIX_H; y++)
      if (tBoard[x][y]) putPix(x, y, x == tClearX ? CRGB(200, 200, 200) : (CRGB)CHSV(TCOLS[tBoard[x][y] - 1], 230, 200));
  if (tClearX < 0) for (auto& c : tCells) putPix(tX + c[0], tY + c[1], CHSV(TCOLS[tCol], 230, 255));
}
// --- snake
int8_t snX[64], snY[64]; uint8_t snLen; int8_t apX, apY, snDx, snDy; uint32_t snLast;
bool snHits(int x, int y, bool skipTail) {
  for (int i = 0; i < snLen - (skipTail ? 1 : 0); i++) if (snX[i] == x && snY[i] == y) return true;
  return false;
}
void snApple() { do { apX = random(MATRIX_W); apY = random(MATRIX_H); } while (snHits(apX, apY, false)); }
void fxSnakeInit() {
  snLen = 4; for (int i = 0; i < snLen; i++) { snX[i] = 6 - i; snY[i] = 4; }
  snDx = 1; snDy = 0; snLast = 0; snApple();
}
void fxSnake(uint32_t t) {
  if (t - snLast >= 75) {
    snLast = t;
    // kies richting: richting appel als vrij, anders een andere vrije richting
    const int8_t D[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    int best = -1, bestD = 9999;
    for (int k = 0; k < 4; k++) {
      if (D[k][0] == -snDx && D[k][1] == -snDy) continue;
      int nx = (snX[0] + D[k][0] + MATRIX_W) % MATRIX_W, ny = (snY[0] + D[k][1] + MATRIX_H) % MATRIX_H;
      if (snHits(nx, ny, true)) continue;
      int dx = abs(nx - apX); dx = min(dx, MATRIX_W - dx);
      int dy = abs(ny - apY); dy = min(dy, MATRIX_H - dy);
      int d = dx + dy + (random8() < 25 ? 2 : 0);
      if (d < bestD) { bestD = d; best = k; }
    }
    if (best < 0) fxSnakeInit();
    else {
      snDx = D[best][0]; snDy = D[best][1];
      int nx = (snX[0] + snDx + MATRIX_W) % MATRIX_W, ny = (snY[0] + snDy + MATRIX_H) % MATRIX_H;
      bool eat = (nx == apX && ny == apY);
      if (eat && snLen < 64) snLen++;
      for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
      snX[0] = nx; snY[0] = ny;
      if (eat) { if (snLen >= 40) fxSnakeInit(); else snApple(); }
    }
  }
  FastLED.clear();
  putPix(apX, apY, CRGB(255, 0, 0));
  for (int i = snLen - 1; i >= 0; i--) putPix(snX[i], snY[i], i == 0 ? CRGB(180, 255, 80) : (CRGB)CHSV(96, 255, 230 - min(i * 6, 170)));
}
// --- kloppend hart dat langs schuift
static const uint16_t HEART_BIG[8]   = {0x0C6, 0x1EF, 0x1FF, 0x1FF, 0x0FE, 0x07C, 0x038, 0x010};  // 9 breed
static const uint16_t HEART_SMALL[6] = {0x36, 0x7F, 0x7F, 0x3E, 0x1C, 0x08};                       // 7 breed
void fxHeart(uint32_t t) {
  fadeToBlackBy(leds, NUM_LEDS, 60);
  uint32_t ph = t % 1000;                                   // lub-dub
  bool big = (ph < 120) || (ph > 250 && ph < 370);
  int cx = MATRIX_W + 5 - (int)((uint64_t)t * (MATRIX_W + 10) / SHOW_DURATION_MS);   // midden van het hart
  if (big) drawBits(HEART_BIG, 8, 9, cx - 4, 0, CRGB(255, 0, 40));
  else     drawBits(HEART_SMALL, 6, 7, cx - 3, 1, CRGB(170, 0, 30));
  if (random8() < 70) putPix(random(MATRIX_W), random(MATRIX_H), CHSV(240 + random8(20), 160, 140));
}
static const char* SHOW_NAMES[SHOW_COUNT] = {"VUURWERK", "MATRIX", "STERREN", "REGENBOOG", "SNEEUW", "PACMAN", "BALLEN", "VUUR",
                                             "RUIMTE", "ONWEER", "TETRIS", "SNAKE", "HART"};

bool showActive = false; uint32_t showStart = 0; uint8_t showType = 0;
void startShow(uint8_t type) {
  showActive = true; showStart = millis(); showType = type % SHOW_COUNT;
  Serial.printf("Show: %s\n", SHOW_NAMES[showType]);
  fxReset(); fxRainInit(); fxSnowInit(); fxBallsInit(); memset(heat, 0, sizeof(heat)); fxSpaceInit(); fxStormInit(); fxTetrisInit(); fxSnakeInit(); FastLED.clear();
}
void runShowFrame() {
  uint32_t t = millis() - showStart;
  switch (showType) {
    case 0: fxFireworks(); break;
    case 1: fxRain(); break;
    case 2: fxSparkles(); break;
    case 3: fxRainbowWaves(t); break;
    case 4: fxSnow(t); break;
    case 5: fxPacman(t); break;
    case 6: fxBalls(t); break;
    case 7: fxFire(t); break;
    case 8: fxSpace(t); break;
    case 9: fxStorm(t); break;
    case 10: fxTetris(t); break;
    case 11: fxSnake(t); break;
    default: fxHeart(t); break;
  }
  if (millis() - showStart > SHOW_DURATION_MS) { showActive = false; FastLED.clear(); }
}

// --- opstart-intro (blokkerend, ~2 s)
void bootIntro() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1100) {           // regenboog-golf vanuit het midden
    float r = (millis() - t0) / 1100.0f * 17;
    FastLED.clear();
    for (int x = 0; x < MATRIX_W; x++) {
      float dist = fabsf(x - 15.5f);
      if (dist < r) for (int y = 0; y < MATRIX_H; y++) {
        uint8_t v = (uint8_t)constrain(255 - (r - dist) * 30, 40, 255);
        leds[XY(x, y)] = CHSV(x * 8 + y * 6, 255, v);
      }
    }
    FastLED.show(); delay(16);
  }
  t0 = millis();
  while (millis() - t0 < 1000) {           // oplossen in sterretjes + "HALLO"
    fadeToBlackBy(leds, NUM_LEDS, 40);
    resetClip();
    CRGB w(255, 255, 255);
    w.nscale8((uint8_t)min(255UL, (millis() - t0) / 3));
    drawText("HALLO", (MATRIX_W - textWidth("HALLO")) / 2, 1, &w);
    if (random(100) < 50) leds[random(NUM_LEDS)] = CHSV(random(256), 200, 255);
    FastLED.show(); delay(16);
  }
  FastLED.clear(); FastLED.show();
}

// --- wifi-setup-animatie (wifi-icoon + lopende tekst)
void drawWifiSetup(uint32_t e) {
  drawIcon(ICON_WIFI, 0, 0, (millis() / 300) % 4);
  const char* msg = "WIFI: " AP_NAME;
  int w = textWidth(msg);
  int span = MATRIX_W - 9;
  int x = 9 + span - (int)((e / 45) % (span + w + 8));
  clipX0 = 9;
  drawText(msg, x, 1);
  clipX0 = 0;
}

// --- testpatroon voor de matrix-indeling
void testPatternMode() {
  Serial.println("TESTPATROON: links/rechts = volgende patroon, midden = verder opstarten");
  uint8_t step = 0;
  FastLED.setBrightness(20);
  while (true) {
    bL.update(); bM.update(); bR.update();
    if (bM.evShort || bM.evLong) break;
    if (bR.evShort) step = (step + 1) % 3;
    if (bL.evShort) step = (step + 2) % 3;
    FastLED.clear(); resetClip();
    uint32_t ms = millis();
    if (step == 0) {
      // hoeken via XY: LB rood, RB groen, LO blauw, RO wit + tekst die leesbaar moet zijn
      setPix(0, 0, CRGB::Red); setPix(31, 0, CRGB::Green); setPix(0, 7, CRGB::Blue); setPix(31, 7, CRGB::White);
      CRGB c(255, 120, 0); drawText("TEST 1", 6, 1, &c);
    } else if (step == 1) {
      // ruwe LED-volgorde: index 0 altijd rood, loper volgt de bedrading
      int i = (ms / 25) % NUM_LEDS;
      for (int k = 0; k < 12; k++) leds[(i - k + NUM_LEDS) % NUM_LEDS] = CHSV(96, 255, 255 - k * 20);
      leds[0] = CRGB::Red;
    } else {
      // kolom loopt links->rechts, rij boven->onder (via XY)
      int x = (ms / 80) % MATRIX_W, y = (ms / 400) % MATRIX_H;
      for (int yy = 0; yy < MATRIX_H; yy++) setPix(x, yy, CRGB(0, 0, 255));
      for (int xx = 0; xx < MATRIX_W; xx++) setPix(xx, y, CRGB(255, 0, 80));
    }
    FastLED.show(); delay(16);
  }
  FastLED.clear(); FastLED.show();
}

// ---------------------------------------------------------------- wifi / ntp / ota
void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([]() { otaBusy = true; WiFi.setSleep(false); FastLED.clear(); FastLED.show(); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int tot) {
    // alleen hertekenen als de balk een stap verder is: FastLED.show() zet ~8 ms
    // de interrupts uit, en dat bij elk 1 KB-blok brak de wifi-verbinding af
    static int lastN = -1;
    int n = tot ? (int)((uint64_t)p * MATRIX_W / tot) : 0;
    if (n == lastN) return;
    lastN = n;
    FastLED.clear(); resetClip();
    CRGB c(0, 120, 255); drawText("OTA", 10, 0, &c);
    for (int x = 0; x < n; x++) leds[XY(x, 7)] = CRGB(0, 255, 60);
    FastLED.setBrightness(20); FastLED.show();
  });
  ArduinoOTA.onEnd([]() { otaBusy = false; });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; });
  ArduinoOTA.begin();
  otaStarted = true;
}

void onWifiConnected() {
  Serial.printf("WiFi verbonden: %s\n", WiFi.localIP().toString().c_str());
  static bool sntpStarted = false;        // SNTP blijft draaien bij herverbinden
  if (!sntpStarted) {
    sntp_set_time_sync_notification_cb(ntpCallback);
    sntp_set_sync_interval(NTP_RESYNC_MS);  // elke ~6 uur opnieuw
    configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
    sntpStarted = true;
  }
  if (!otaStarted) startOta();
  char buf[40];
  snprintf(buf, sizeof(buf), "WIFI OK %s", WiFi.localIP().toString().c_str());
  showMessage(buf, CRGB(0, 200, 80));
}

void startPortal() {
  if (webStarted) { web.stop(); webStarted = false; }   // poort 80 vrijgeven voor het portaal
  std::vector<const char*> menu = {"wifi", "info", "exit"};
  wm.setMenu(menu);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);
  wm.setTitle("Matrix-Klok");
  wm.startConfigPortal(AP_NAME);
  portalActive = true;
  Serial.println("Config-portal gestart: " AP_NAME);
}

void stopPortal() {
  wm.stopConfigPortal();
  portalActive = false;
  WiFi.mode(WIFI_STA);
  Serial.println("Config-portal gestopt");
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nMatrix-Klok start");

  bL.begin(PIN_BTN_LEFT); bM.begin(PIN_BTN_MID); bR.begin(PIN_BTN_RIGHT);
  loadSettings();

  FastLED.addLeds<WS2812B, PIN_MATRIX, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(LED_VOLTS, LED_MAX_MA);
  FastLED.setBrightness(BRIGHTNESS_STEPS[briIdx]);
  FastLED.clear(true);

  setenv("TZ", TZ_INFO, 1); tzset();

  delay(30);
  if (bL.raw()) testPatternMode();      // LINKS ingedrukt bij opstarten = testpatroon

  // RTC
  Wire.begin(PIN_SDA, PIN_SCL);
  rtcPresent = rtc.begin(&Wire);
  if (rtcPresent) {
    rtcValid = !rtc.lostPower();
    rtc.disable32K();
    rtc.writeSqwPinMode(DS3231_SquareWave1Hz);
    pinMode(PIN_SQW, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SQW), sqwIsr, FALLING);  // dalende flank = seconde-update
    if (rtcValid) setSystemTimeFromRtc();
    Serial.printf("DS3231 gevonden, %s, %.2f C\n", rtcValid ? "tijd geldig" : "STROOM VERLOREN", rtc.getTemperature());
  } else {
    Serial.println("DS3231 NIET gevonden");
  }

  bootIntro();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // geen modem-sleep: stabielere OTA/TCP
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (wm.getWiFiIsSaved()) {
    WiFi.begin();                       // opgeslagen gegevens, niet-blokkerend
    Serial.println("WiFi: verbinden met opgeslagen netwerk");
  } else {
    startPortal();
  }
  lastInteractionMs = millis();
}

// ---------------------------------------------------------------- loop
void handleButtons() {
  bL.update(); bM.update(); bR.update();
  bool any = bL.evShort || bR.evShort || bM.evShort || bL.evLong || bM.evLong || bR.evLong;
  if (any) lastInteractionMs = millis();
  if (showActive && any) { showActive = false; FastLED.clear(); return; }  // show onderbreken

  auto nextScreen = [](int dir) {
    uint8_t s = screen;
    do { s = (s + SC_COUNT + dir) % SC_COUNT; } while (s == SC_TEMP && !rtcPresent);
    prevScreen = screen; screen = s; screenChangedMs = millis(); slideDir = dir;
  };
  if (bR.evShort) nextScreen(+1);
  if (bL.evShort) nextScreen(-1);

  if (bM.evShort) {
    briIdx = (briIdx + 1) % sizeof(BRIGHTNESS_STEPS);
    overlay = OV_BRIGHT; overlayStart = millis(); overlayDur = 1300;
    saveSettings();
  }
  if (bM.evLong) {
    colorMode = (colorMode + 1) % CM_COUNT;
    saveSettings();
    showMessage(COLOR_NAMES[colorMode], CRGB(0, 0, 0));   // zwart = in de nieuwe kleurmodus
  }
  if (bM.evVLong) {
    showMessage("WIFI WISSEN", CRGB(255, 60, 0));
    wm.resetSettings();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);
    startPortal();
  }
  if (bL.evLong) {
    // lang links = setup-access-point aan/uit (uurshows staan nu in de web-GUI)
    if (portalActive) { stopPortal(); showMessage("AP UIT", CRGB(255, 200, 0)); }
    else { startPortal(); showMessage("AP AAN", CRGB(255, 200, 0)); }
  }
  if (bR.evLong) { startShow(showIdx); showIdx = (showIdx + 1) % SHOW_COUNT; saveSettings(); }   // volgende show in de rij
}

void handleTime() {
  if (ntpSyncFlag) {
    ntpSyncFlag = false;
    ntpEverSynced = true; lastNtpSyncMs = millis();
    if (rtcPresent) {
      rtc.adjust(DateTime((uint32_t)time(nullptr)));   // UTC naar DS3231
      rtcValid = true;
    }
    Serial.println("NTP-sync OK, RTC bijgewerkt");
  }
  // zonder (recente) NTP: elke 10 min systeemklok bijtrekken met de RTC
  bool ntpFresh = ntpEverSynced && (millis() - lastNtpSyncMs) < NTP_RESYNC_MS + 3600000UL;
  if (!ntpFresh && rtcValid && millis() - lastRtcToSysMs > 600000UL) setSystemTimeFromRtc();
}

void handleWifi() {
  if (portalActive) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED && !wm.getConfigPortalActive()) portalActive = false;
  }
  bool conn = WiFi.status() == WL_CONNECTED;
  if (conn && !wifiWasConnected) onWifiConnected();
  wifiWasConnected = conn;
  if (otaStarted) ArduinoOTA.handle();
  if (conn && !portalActive && !webStarted) startWeb();   // poort 80 pas na het setup-portaal
  if (webStarted) web.handleClient();
  if (apToggleReq) { apToggleReq = false; if (!portalActive) { startPortal(); showMessage("AP AAN", CRGB(255, 200, 0)); } }
  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) { FastLED.clear(true); delay(50); ESP.restart(); }
}

void render() {
  struct tm t; bool valid = getLocal(t);
  uint32_t now = millis();

  // automatisch terug naar tijd
  if (screen != SC_TIME && now - lastInteractionMs > SCREEN_TIMEOUT_MS) {
    prevScreen = screen; screen = SC_TIME; screenChangedMs = now; slideDir = -1;
  }

  // uurshow
  static int lastShowHour = -1;
  if (valid) {
    if (lastShowHour < 0) lastShowHour = t.tm_hour;
    if (t.tm_min == 0 && t.tm_hour != lastShowHour) {
      lastShowHour = t.tm_hour;
      bool nightBlock = (NIGHT_NO_SHOWS && isNight(t)) || timerSaysOff(t);
      if (hourlyShows && !nightBlock && screen == SC_TIME) { startShow(showIdx); showIdx = (showIdx + 1) % SHOW_COUNT; saveSettings(); }
    }
  }

  uint8_t bri = BRIGHTNESS_STEPS[briIdx];
  if (isNight(t)) bri = min<uint8_t>(bri, NIGHT_BRIGHTNESS);
  FastLED.setBrightness(bri);

  // timer: display uit, behalve kort na een knopdruk of tijdens wifi-setup
  if (valid && timerSaysOff(t) && !portalActive && now - lastInteractionMs > WAKE_MS) {
    showActive = false; overlay = OV_NONE;
    FastLED.clear(); FastLED.show(); return;
  }

  resetClip();
  if (showActive) { runShowFrame(); FastLED.show(); return; }

  FastLED.clear();
  if (overlay != OV_NONE && now - overlayStart < overlayDur) { drawOverlay(); FastLED.show(); return; }
  overlay = OV_NONE;

  // wifi-setup-animatie: elke 20 s ongeveer 8 s
  if (portalActive && (now / 1000) % 20 < 8) {
    drawWifiSetup(now % 20000);
    FastLED.show(); return;
  }

  uint32_t se = now - screenChangedMs;
  const uint32_t SLIDE_MS = 280;
  if (se < SLIDE_MS && prevScreen != screen) {
    float p = se / (float)SLIDE_MS; p = p * p * (3 - 2 * p);
    int off = (int)(p * MATRIX_W);
    drawScreen(prevScreen, -off * slideDir, t, valid);
    drawScreen(screen, (MATRIX_W - off) * slideDir, t, valid);
  } else {
    drawScreen(screen, 0, t, valid);
  }
  FastLED.show();
}

void loop() {
  handleWifi();
  if (otaBusy) return;
  handleButtons();
  handleTime();
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= 20) { lastFrame = millis(); render(); }  // ~50 fps
  delay(1);
}
