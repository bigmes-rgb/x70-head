/*
  X70 HEAD - pan/tilt firmware  (Vidpro MH-430 stepper conversion, ESP32 + 2x TMC2209)

  What it does
    - Joystick page for a phone or PC:  http://x70head.local   (or the IP address printed on the serial monitor)
    - VISCA for OBS:  "VISCA over TCP" port 5678 (Face Tracker PTZ)  and  "VISCA over IP / UDP" port 52381 (PTZ Controls)
    - Every speed change eases in and out. Travel limits you set from the page. 6 presets.
    - The joystick always wins over face tracking (tracking resumes 3 s after you let go).

  Libraries: TMCStepper and FastAccelStepper.  Board: ESP32 Dev Module, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)".
  Same wiring as the test sketch (soldering map v3).

  Wi-Fi: nothing to type here. On first start the head makes its own Wi-Fi "X70-Head" (password x70head123).
  Join it, open http://192.168.4.1/wifi and enter your network. It is stored in the head, not in this code.

  Updates: GitHub builds this code automatically. The head's page has an "Update" button that
  downloads the newest build for the head and the joystick box.
*/
#define FW_BUILD 0          // stamped by the automatic build - leave as it is
const char *FW_BASE = "https://github.com/bigmes-rgb/x70-head/releases/latest/download/";
const char *FW_FILE = "x70_head.bin";
const char *AP_NAME = "X70-Head";
const char *DEFAULT_PASS = "x70head123";   // device password until you change it on the Wi-Fi page

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <HTTPUpdateServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <TMCStepper.h>
#include <FastAccelStepper.h>

// ---- pins (soldering map v3) ----
const int EN_PIN    = 27;  // both drivers' EN. HIGH = motors off (the 10k holds it HIGH at start-up)
const int TILT_STEP = 32, TILT_DIR = 33;
const int PAN_STEP  = 25, PAN_DIR  = 26;
const int UART_RX   = 16, UART_TX  = 17;

// ---- drivers ----
const float    R_SENSE    = 0.11f;
const uint8_t  TILT_ADDR  = 1, PAN_ADDR = 0;
const uint16_t RUN_MA     = 600;     // motor current while moving (it holds at half of this)
const bool     QUIET_MODE = true;    // true = StealthChop (silent). false = SpreadCycle (noisier, a bit stronger)

// ---- motion ----
const uint16_t MICROSTEPS     = 16;
const float    STEPS_PER_TURN = 200.0f * MICROSTEPS;
const float    SLOW_CAP_RPM   = 20;    // top motor speed while an axis has no travel limits yet
const float    STOP_TIME      = 0.5f;  // seconds to brake from top speed when coming up to a limit
const uint32_t MANUAL_HOLD_MS = 3000;  // joystick beats face tracking for this long after you let go
const uint32_t DEADMAN_MS     = 600;   // joystick page gone quiet -> stop

TMC2209Stepper drvTilt(&Serial2, R_SENSE, TILT_ADDR);
TMC2209Stepper drvPan (&Serial2, R_SENSE, PAN_ADDR);
FastAccelStepperEngine engine;
WebServer  web(80);
HTTPUpdateServer webUpdater;
WiFiServer viscaTcp(5678);
WiFiClient viscaClient;
WiFiUDP    viscaUdp;
WiFiUDP    joyUdp;                 // joystick box, port 52380
Preferences prefs;
String wifiSsid, wifiPass, devPass;      // stored in the device (Wi-Fi page)
int  latestBuild = -1, joyBuild = -1;    // newest build on GitHub; build the joystick box reports
uint32_t joySeenMs = 0;
bool updateRequested = false;
bool autoUpdate = true;                   // check GitHub by itself and install new builds while the head is idle
uint32_t lastInputMs = 0;                // last joystick / OBS command
String fwMsg = "";

struct Axis {
  const char *name; char key;          // key: 'p' or 't' for saved settings
  FastAccelStepper *st; TMC2209Stepper *drv; bool found;
  bool  invert;                        // flips which way is right / up
  float gear;                          // motor turns per one camera turn. 0 = not known
  bool  hasLo, hasHi; long lo, hi;     // travel limits, motor steps
  float webCmd, viscaCmd;              // wanted speed from each source, steps/s, + = right / up
  float v, a;                          // eased speed now (motor steps/s) and its rate of change
  bool  going;                         // driving to a preset
  long  saved;                         // last position written to flash
};
Axis pan  = {"pan",  'p', nullptr, &drvPan,  false, false, 0, false, false, 0, 0, 0, 0, 0, 0, false, 0};
Axis tilt = {"tilt", 't', nullptr, &drvTilt, false, false, 0, false, false, 0, 0, 0, 0, 0, 0, false, 0};
Axis *AX[2] = {&pan, &tilt};

float maxRpm = 120;        // top motor speed (page slider)
float ramp   = 1.0f;       // seconds for a speed change to ease in / out (page slider)
bool  motorsOn = false;
bool  trackingOn = true;   // false = ignore OBS / face tracking (joystick box button or the page)
uint32_t lastWebMs = 0, manualUntil = 0, lastMoveMs = 0;
const int NPRESET = 6;
bool presetSet[NPRESET]; long presetPos[NPRESET][2];

float maxSps() { return maxRpm / 60.0f * STEPS_PER_TURN; }
float sgn(const Axis &x) { return x.invert ? -1.0f : 1.0f; }
bool  limited(const Axis &x) { return x.hasLo && x.hasHi; }
float capSps(const Axis &x) { return (limited(x) ? maxRpm : fminf(maxRpm, SLOW_CAP_RPM)) / 60.0f * STEPS_PER_TURN; }
float stepsToDeg(const Axis &x, long s) { return x.gear > 0 ? s / STEPS_PER_TURN / x.gear * 360.0f : 0; }

// ---------------- saved settings ----------------
String k(const Axis &x, const char *what) { String s; s += x.key; s += what; return s; }
void loadAll() {
  prefs.begin("x70head", false);
  maxRpm = prefs.getFloat("max", 120); ramp = prefs.getFloat("ramp", 1.0f);
  wifiSsid = prefs.getString("ssid", ""); wifiPass = prefs.getString("wpass", ""); devPass = prefs.getString("dpass", DEFAULT_PASS);
  autoUpdate = prefs.getBool("auto", true);
  for (Axis *x : AX) {
    x->invert = prefs.getBool(k(*x, "inv").c_str(), false);
    x->gear   = prefs.getFloat(k(*x, "gear").c_str(), 0);
    x->hasLo  = prefs.getBool(k(*x, "hl").c_str(), false);  x->lo = prefs.getLong(k(*x, "lo").c_str(), 0);
    x->hasHi  = prefs.getBool(k(*x, "hh").c_str(), false);  x->hi = prefs.getLong(k(*x, "hi").c_str(), 0);
    x->saved  = prefs.getLong(k(*x, "pos").c_str(), 0);
  }
  for (int i = 0; i < NPRESET; i++) {
    char key[8];
    snprintf(key, sizeof key, "ps%d", i); presetSet[i] = prefs.getBool(key, false);
    snprintf(key, sizeof key, "pp%d", i); presetPos[i][0] = prefs.getLong(key, 0);
    snprintf(key, sizeof key, "pt%d", i); presetPos[i][1] = prefs.getLong(key, 0);
  }
}
void saveLimits(const Axis &x) {
  prefs.putBool(k(x, "hl").c_str(), x.hasLo); prefs.putLong(k(x, "lo").c_str(), x.lo);
  prefs.putBool(k(x, "hh").c_str(), x.hasHi); prefs.putLong(k(x, "hi").c_str(), x.hi);
}
void savePreset(int i) {
  char key[8];
  snprintf(key, sizeof key, "ps%d", i); prefs.putBool(key, presetSet[i]);
  snprintf(key, sizeof key, "pp%d", i); prefs.putLong(key, presetPos[i][0]);
  snprintf(key, sizeof key, "pt%d", i); prefs.putLong(key, presetPos[i][1]);
}

// ---------------- drivers ----------------
void configure(TMC2209Stepper &d) {      // the settings proven with the test sketch
  d.begin();
  d.I_scale_analog(false);
  d.toff(4);
  d.blank_time(24);
  d.microsteps(MICROSTEPS);
  d.en_spreadCycle(!QUIET_MODE);
  d.pwm_autoscale(true);
  d.TPOWERDOWN(20);
  d.rms_current(RUN_MA, 0.5);
  d.GSTAT(0b111);
}
void setMotors(bool on) {
  if (!on) for (Axis *x : AX) { if (x->st) x->st->forceStop(); x->v = x->a = 0; x->going = false; x->webCmd = x->viscaCmd = 0; }
  digitalWrite(EN_PIN, on ? LOW : HIGH);
  motorsOn = on;
}

// ---------------- motion ----------------
void stopAll() { for (Axis *x : AX) { x->webCmd = x->viscaCmd = 0; if (x->going) { x->going = false; if (x->st) x->st->stopMove(); } } }

long clampToLimits(const Axis &x, long p) {
  if (x.hasLo && p < x.lo) p = x.lo;
  if (x.hasHi && p > x.hi) p = x.hi;
  return p;
}

void gotoPos(long p, long t) {
  if (!motorsOn) return;
  long tgt[2] = {clampToLimits(pan, p), clampToLimits(tilt, t)};
  for (int i = 0; i < 2; i++) {
    Axis &x = *AX[i];
    if (!x.st) continue;
    float sp = fminf(capSps(x), 90.0f / 60.0f * STEPS_PER_TURN);          // presets travel at up to 90 RPM
    x.webCmd = x.viscaCmd = 0; x.v = x.a = 0;
    x.st->setSpeedInHz((uint32_t)sp);
    x.st->setAcceleration((int32_t)(sp / fmaxf(ramp, 0.3f)));
    x.st->setLinearAcceleration((uint32_t)(sp * 0.15f));                   // soft start and soft landing
    x.st->moveTo(tgt[i]);
    x.going = true;
  }
  lastMoveMs = millis();
}

// Runs 100 times a second. Eases each axis toward the speed that was asked for and never passes a limit.
void motionTick(float dt) {
  uint32_t now = millis();
  bool webAlive = now - lastWebMs < DEADMAN_MS;
  bool manual = (int32_t)(manualUntil - now) > 0;
  for (Axis *xp : AX) {
    Axis &x = *xp;
    if (!x.st) continue;
    if (!webAlive) x.webCmd = 0;
    float want = (manual || !trackingOn) ? x.webCmd : (x.webCmd != 0 ? x.webCmd : x.viscaCmd);
    if (!motorsOn) want = 0;
    if (x.going) {
      if (want != 0) { x.going = false; x.v = x.st->getCurrentSpeedInMilliHz() / 1000.0f; x.a = 0; }   // joystick takes over mid-move
      else { if (!x.st->isRunning()) { x.going = false; x.v = x.a = 0; } else lastMoveMs = now; continue; }
    }
    float cap = capSps(x);
    float target = constrain(want * sgn(x), -cap, cap);                    // motor direction
    // critically damped follow: eases in and eases out, and copes with a target that keeps changing (face tracking)
    float w = 4.74f / ramp;                                                // reaches 95 % of the new speed in 'ramp' seconds
    float vBefore = x.v;
    float acc = w * w * (target - x.v) - 2.0f * w * x.a;
    x.a += acc * dt; x.v += x.a * dt;
    // limits: the closer to the end, the slower it may go toward it
    long pos = x.st->getCurrentPosition();
    float brake = maxSps() / STOP_TIME;
    if (x.v > 0 && x.hasHi) { float d = (float)(x.hi - pos) - 48.0f; float lim = d > 0 ? sqrtf(2.0f * brake * d) : 0; if (x.v > lim) { x.v = lim; if (x.a > 0) x.a = 0; } }
    if (x.v < 0 && x.hasLo) { float d = (float)(pos - x.lo) - 48.0f; float lim = d > 0 ? sqrtf(2.0f * brake * d) : 0; if (-x.v > lim) { x.v = -lim; if (x.a < 0) x.a = 0; } }
    float s = fabsf(x.v);
    if (s < 1.0f) {
      if (target == 0) { x.v = x.a = 0; }
      if (x.st->isRunning()) { x.st->setAcceleration((int32_t)brake); x.st->applySpeedAcceleration(); x.st->stopMove(); }
    } else {
      x.st->setAcceleration((int32_t)(fabsf(x.v - vBefore) / dt * 1.5f) + 300);   // glide to this tick's speed, no stair-steps
      x.st->setLinearAcceleration(0);
      if (x.st->setSpeedInMilliHz((uint32_t)(s * 1000.0f)) == 0) { if (x.v > 0) x.st->runForward(); else x.st->runBackward(); }
      lastMoveMs = now;
    }
  }
  // remember where the head is once it has stood still for 2 s (software position memory)
  if (now - lastMoveMs > 2000) for (Axis *x : AX) if (x->st) {
    long p = x->st->getCurrentPosition();
    if (p != x->saved) { x->saved = p; prefs.putLong(k(*x, "pos").c_str(), p); }
  }
}

// ---------------- VISCA ----------------
float viscaSpeed(int s, int top) {             // speed 1..top -> steps/s, fine at the bottom, fast at the top
  s = constrain(s, 1, top);
  float hi = maxSps(), lo = hi / 300.0f;
  return lo * powf(hi / lo, (float)(s - 1) / (float)(top - 1));
}
int viscaPos(const Axis &x) {                  // Sony units: about 0.075 degree each
  if (!x.st) return 0;
  long p = (long)(x.st->getCurrentPosition() * sgn(x));
  return x.gear > 0 ? (int)lroundf(stepsToDeg(x, p) / 0.075f) : (int)(p / 16);
}
long viscaToSteps(const Axis &x, int units) { return (long)lroundf(units * 0.075f / 360.0f * x.gear * STEPS_PER_TURN * sgn(x)); }
int nib4(const uint8_t *p) { return (int16_t)(((p[0] & 15) << 12) | ((p[1] & 15) << 8) | ((p[2] & 15) << 4) | (p[3] & 15)); }

// Handles one VISCA packet (ends with FF). Writes the reply into out, returns its length.
int viscaHandle(const uint8_t *p, int n, uint8_t *out) {
  static const uint8_t done[] = {0x90, 0x41, 0xFF, 0x90, 0x51, 0xFF};      // ACK + completed
  if (n < 3 || p[n - 1] != 0xFF) return 0;
  if (p[1] == 0x09) {                                                       // inquiries
    if (n >= 5 && p[2] == 0x06 && p[3] == 0x12) {
      int a = viscaPos(pan), b = viscaPos(tilt);
      uint8_t r[] = {0x90, 0x50, (uint8_t)((a >> 12) & 15), (uint8_t)((a >> 8) & 15), (uint8_t)((a >> 4) & 15), (uint8_t)(a & 15),
                     (uint8_t)((b >> 12) & 15), (uint8_t)((b >> 8) & 15), (uint8_t)((b >> 4) & 15), (uint8_t)(b & 15), 0xFF};
      memcpy(out, r, sizeof r); return sizeof r;
    }
    if (n >= 5 && p[2] == 0x04 && p[3] == 0x47) { const uint8_t r[] = {0x90, 0x50, 0, 0, 0, 0, 0xFF}; memcpy(out, r, sizeof r); return sizeof r; }   // zoom position
    if (n >= 5 && p[2] == 0x04 && p[3] == 0x00) { const uint8_t r[] = {0x90, 0x50, 0x02, 0xFF}; memcpy(out, r, sizeof r); return sizeof r; }         // power: on
    if (n >= 5 && p[2] == 0x00 && p[3] == 0x02) { const uint8_t r[] = {0x90, 0x50, 0x00, 0x01, 0x05, 0x11, 0x01, 0x00, 0x02, 0xFF}; memcpy(out, r, sizeof r); return sizeof r; }
    const uint8_t r[] = {0x90, 0x60, 0x02, 0xFF}; memcpy(out, r, sizeof r); return sizeof r;                                                       // not supported
  }
  if (p[1] == 0x01) {                                                       // commands
    if (n >= 9 && p[2] == 0x06 && p[3] == 0x01) {                           // pan/tilt drive
      float ps = viscaSpeed(p[4], 0x18), ts = viscaSpeed(p[5], 0x14);
      pan.viscaCmd  = p[6] == 0x01 ? -ps : (p[6] == 0x02 ? ps : 0);         // 01 left, 02 right
      if (p[6] != 0x03 || p[7] != 0x03) lastInputMs = millis();
      tilt.viscaCmd = p[7] == 0x01 ? ts : (p[7] == 0x02 ? -ts : 0);         // 01 up, 02 down
    } else if (n >= 15 && p[2] == 0x06 && (p[3] == 0x02 || p[3] == 0x03)) { // absolute / relative position
      if (pan.gear > 0 && tilt.gear > 0 && pan.st && tilt.st) {
        long a = viscaToSteps(pan, nib4(p + 6)), b = viscaToSteps(tilt, nib4(p + 10));
        if (p[3] == 0x03) { a += pan.st->getCurrentPosition(); b += tilt.st->getCurrentPosition(); }
        gotoPos(a, b);
      }
    } else if (n >= 5 && p[2] == 0x06 && p[3] == 0x04) {                    // home
      gotoPos(0, 0);
    } else if (n >= 7 && p[2] == 0x04 && p[3] == 0x3F && p[5] < NPRESET) {  // presets
      int i = p[5];
      if (p[4] == 0x01 && pan.st && tilt.st) { presetSet[i] = true; presetPos[i][0] = pan.st->getCurrentPosition(); presetPos[i][1] = tilt.st->getCurrentPosition(); savePreset(i); }
      else if (p[4] == 0x02 && presetSet[i]) gotoPos(presetPos[i][0], presetPos[i][1]);
      else if (p[4] == 0x00) { presetSet[i] = false; savePreset(i); }
    }                                                                       // anything else (zoom, focus...): accepted, nothing to do yet
    memcpy(out, done, sizeof done); return sizeof done;
  }
  const uint8_t r[] = {0x90, 0x50, 0xFF}; memcpy(out, r, sizeof r); return sizeof r;   // address set / IF_clear
}

void viscaPoll() {
  static uint8_t buf[32]; static int len = 0;
  uint8_t out[24];
  // TCP (Face Tracker PTZ "VISCA over TCP")
  if (!viscaClient || !viscaClient.connected()) {
    if (viscaClient) { viscaClient.stop(); pan.viscaCmd = tilt.viscaCmd = 0; len = 0; }     // tracker went away: stop
    WiFiClient c = viscaTcp.accept();
    if (c) { viscaClient = c; viscaClient.setNoDelay(true); len = 0; }
  }
  if (viscaClient && viscaClient.connected()) {
    int guard = 64;
    while (viscaClient.available() && guard--) {
      uint8_t b = viscaClient.read();
      if (len < (int)sizeof buf) buf[len++] = b; else len = 0;
      if (b == 0xFF) { int r = viscaHandle(buf, len, out); if (r) viscaClient.write(out, r); len = 0; }
    }
  }
  // UDP (PTZ Controls "VISCA over IP", Sony style: 8-byte header, then the VISCA packet)
  int sz = viscaUdp.parsePacket();
  if (sz >= 9 && sz <= 40) {
    uint8_t pk[40]; int got = viscaUdp.read(pk, sizeof pk);
    if (got >= 9) {
      uint8_t rp[40]; int r = 0; uint16_t type = (pk[0] << 8) | pk[1];
      memcpy(rp, pk, 8);
      if (type == 0x0200) { rp[0] = 0x02; rp[1] = 0x01; rp[8] = 0x01; r = 1; }             // reset sequence number
      else { rp[0] = 0x01; rp[1] = 0x11; r = viscaHandle(pk + 8, got - 8, rp + 8); }
      for (int i = 0, start = 0; i < r; i++) if (rp[8 + i] == 0xFF || type == 0x0200) {     // one datagram per reply (ACK, then completed)
        uint8_t one[40]; int m = i - start + 1;
        memcpy(one, rp, 8); memcpy(one + 8, rp + 8 + start, m); one[2] = 0; one[3] = (uint8_t)m;
        viscaUdp.beginPacket(viscaUdp.remoteIP(), viscaUdp.remotePort()); viscaUdp.write(one, 8 + m); viscaUdp.endPacket();
        start = i + 1;
      }
    }
  }
}

// ---------------- updates from GitHub ----------------
bool headMoving() { for (Axis *x : AX) if (x->st && x->st->isRunning()) return true; return false; }

void checkLatest() {                           // asks GitHub for the newest build number (takes a second or two)
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); h.setTimeout(8000);
  if (h.begin(c, String(FW_BASE) + "version.txt")) {
    int code = h.GET();
    if (code == 200) { latestBuild = h.getString().toInt(); fwMsg = ""; } else fwMsg = "Could not reach GitHub (" + String(code) + ")";
    h.end();
  } else fwMsg = "Could not reach GitHub";
}
void broadcastText(const char *msg, int port) {
  IPAddress ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP() : WiFi.localIP(), bc = ip; bc[3] = 255;
  WiFiUDP u; u.beginPacket(bc, port); u.write((const uint8_t *)msg, strlen(msg)); u.endPacket();
}
void selfUpdate() {                            // downloads and installs the newest head firmware, then restarts
  setMotors(true);                             // keep holding the camera while it downloads
  WiFiClientSecure c; c.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(c, String(FW_BASE) + FW_FILE);
  if (r == HTTP_UPDATE_FAILED) fwMsg = "Update failed: " + httpUpdate.getLastErrorString();
}

// ---------------- joystick input (page and joystick box) ----------------
void joyInput(float x, float y) {                       // x, y from -1 to 1
  auto curve = [](float u) { float m = fabsf(u); if (m < 0.06f) return 0.0f; m = (m - 0.06f) / 0.94f; return (u < 0 ? -1.0f : 1.0f) * powf(m, 2.2f); };   // fine control near the middle
  pan.webCmd = curve(constrain(x, -1.0f, 1.0f)) * maxSps(); tilt.webCmd = curve(constrain(y, -1.0f, 1.0f)) * maxSps();
  lastWebMs = millis();
  if (pan.webCmd != 0 || tilt.webCmd != 0) { manualUntil = lastWebMs + MANUAL_HOLD_MS; lastInputMs = lastWebMs; }
}
void recallPreset(int n) {
  if (n >= 0 && n < NPRESET && presetSet[n]) { manualUntil = millis() + MANUAL_HOLD_MS; pan.viscaCmd = tilt.viscaCmd = 0; gotoPos(presetPos[n][0], presetPos[n][1]); }
}
// Joystick box packets (text, UDP 52380):  "J,x,y"  stick   |  "B,id,P,n"  recall preset n   |  "B,id,T"  tracking on/off
void joyBoxPoll() {
  static long lastId = -1;
  for (int guard = 0; guard < 8; guard++) {
    int sz = joyUdp.parsePacket();
    if (sz <= 0) return;
    char b[40]; int n = joyUdp.read((uint8_t *)b, sizeof b - 1); if (n <= 0) continue; b[n] = 0;
    float x, y; long id; int k;
    if (sscanf(b, "J,%f,%f", &x, &y) == 2) { joyInput(x, y); joySeenMs = millis(); }
    else if (sscanf(b, "H,%d", &k) == 1) { joyBuild = k; joySeenMs = millis(); }      // joystick box heartbeat with its build number
    else if (sscanf(b, "B,%ld,P,%d", &id, &k) == 2) { if (id != lastId) { lastId = id; recallPreset(k); } }
    else if (sscanf(b, "B,%ld,T", &id) == 1 && strchr(b, 'T')) { if (id != lastId) { lastId = id; trackingOn = !trackingOn; if (!trackingOn) pan.viscaCmd = tilt.viscaCmd = 0; } }
  }
}

// ---------------- web page ----------------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><title>X70 Head</title>
<style>
body{margin:0;background:#14171c;color:#e8ecf1;font:15px -apple-system,Segoe UI,Roboto,sans-serif;text-align:center;-webkit-user-select:none;user-select:none}
.w{max-width:440px;margin:auto;padding:12px}
#pad{width:min(78vw,320px);height:min(78vw,320px);margin:10px auto;border-radius:50%;background:#222830;border:2px solid #3a4452;position:relative;touch-action:none}
#knob{width:30%;height:30%;border-radius:50%;background:#4fc3f7;position:absolute;left:35%;top:35%;pointer-events:none}
button{background:#2a313b;color:#e8ecf1;border:1px solid #3a4452;border-radius:10px;padding:12px 6px;font-size:15px;margin:3px}
button:active{background:#3a4452}.g{display:grid;grid-template-columns:repeat(3,1fr)}.g2{display:grid;grid-template-columns:repeat(2,1fr)}
#stop{background:#b3261e;border-color:#b3261e;font-weight:700;width:100%;padding:14px}
.on{background:#1f5f3a}.sv{background:#8a6d1a}
#st{font-size:13px;color:#9aa5b4;min-height:38px;white-space:pre-line}
h3{margin:16px 0 4px;font-size:13px;color:#9aa5b4;text-transform:uppercase;letter-spacing:.08em}
label{display:block;margin:10px 0 2px;font-size:13px;color:#cfd6df}input[type=range]{width:100%}
input[type=number]{width:80px;background:#222830;color:#e8ecf1;border:1px solid #3a4452;border-radius:8px;padding:8px;font-size:15px}
details{margin-top:14px;text-align:left;background:#1b2027;border-radius:10px;padding:8px 12px}summary{padding:6px 0}
#warn{color:#f2a93b;font-size:13px}
</style></head><body><div class="w">
<div id="st">connecting...</div><div id="warn"></div>
<div id="pad"><div id="knob"></div></div>
<button id="stop">STOP</button>
<button id="trk" style="width:100%"></button>
<h3>Presets (tap = go)</h3><div class="g" id="pr"></div>
<button id="sv" style="width:100%">Save a preset: tap here, then a number</button>
<label>Top speed <span id="mv"></span></label><input id="max" type="range" min="10" max="300" step="5">
<label>Ease in / out <span id="rv"></span></label><input id="ramp" type="range" min="0.2" max="3" step="0.1">
<details><summary>Setup: limits, direction, gears</summary>
<p style="font-size:13px;color:#9aa5b4">Drive slowly to the furthest safe point each way and tap the matching button. Until both ends of an axis are set, that axis stays slow.</p>
<div class="g2"><button onclick="lim('p','l')">Left limit here</button><button onclick="lim('p','r')">Right limit here</button>
<button onclick="lim('t','d')">Down limit here</button><button onclick="lim('t','u')">Up limit here</button>
<button onclick="lim('p','c')">Clear pan limits</button><button onclick="lim('t','c')">Clear tilt limits</button>
<button onclick="q('/set?invp=1')">Flip left / right</button><button onclick="q('/set?invt=1')">Flip up / down</button></div>
<button style="width:100%" onclick="if(confirm('Make this position the centre (home)?'))q('/zero')">This position is centre (home)</button>
<p style="font-size:13px;color:#9aa5b4">Gears: motor turns for one full camera turn (0 = unknown).</p>
Pan <input id="gp" type="number" step="0.01"> Tilt <input id="gt" type="number" step="0.01"> <button onclick="q('/set?gp='+$('gp').value+'&gt='+$('gt').value)">Save</button>
<button id="mo" style="width:100%"></button>
</details>
<details><summary>Update and Wi-Fi</summary>
<p id="fw" style="font-size:13px;color:#cfd6df;white-space:pre-line"></p>
<div class="g2"><button onclick="$('fw').textContent='Checking...';q('/fw')">Check for update</button><button onclick="if(confirm('Update the head and the joystick box now? Both restart.')){q('/fwgo')}">Update both now</button></div>
<button id="au" style="width:100%" onclick="q('/set?auto=1')"></button>
<button style="width:100%" onclick="location.href='/wifi'">Wi-Fi and password</button>
</details></div>
<script>
var X=0,Y=0,act=false,busy=false,saving=false,S={},last=0,idle=0;
var pad=document.getElementById('pad'),knob=document.getElementById('knob'),$=function(i){return document.getElementById(i)};
function setK(){knob.style.left=(35+X*35)+'%';knob.style.top=(35-Y*35)+'%'}
function mv(e){var r=pad.getBoundingClientRect(),x=(e.clientX-r.left)/r.width*2-1,y=1-(e.clientY-r.top)/r.height*2,m=Math.hypot(x,y);if(m>1){x/=m;y/=m}X=x;Y=y;setK()}
pad.onpointerdown=function(e){act=true;pad.setPointerCapture(e.pointerId);mv(e)};
pad.onpointermove=function(e){if(act)mv(e)};
pad.onpointerup=pad.onpointercancel=function(){act=false;X=Y=0;setK()};
function show(s){S=s;var t=(s.gp>0?'Pan '+s.pd.toFixed(1)+'°':'Pan '+s.pt.toFixed(2)+' motor turns')+'   '+(s.gt>0?'Tilt '+s.td.toFixed(1)+'°':'Tilt '+s.tt.toFixed(2)+' motor turns');
t+='\n'+(s.on?(s.src==2?'Face tracking / OBS driving':s.src==1?'Joystick':'Holding'):'MOTORS OFF')+'   Wi-Fi '+s.rssi+' dBm';$('st').textContent=t;
var w='';if(!s.dp)w+='Pan driver not answering. ';if(!s.dt)w+='Tilt driver not answering. ';if(!s.pl)w+='Pan limits not set (slow). ';if(!s.tl)w+='Tilt limits not set (slow). ';$('warn').textContent=w;
if(document.activeElement!=$('max')){$('max').value=s.max}$('mv').textContent=s.max+' RPM motor';
if(document.activeElement!=$('ramp')){$('ramp').value=s.ramp}$('rv').textContent=s.ramp.toFixed(1)+' s';
if(document.activeElement!=$('gp')&&document.activeElement!=$('gt')){$('gp').value=s.gp;$('gt').value=s.gt}
$('trk').textContent=s.trk?'Face tracking allowed (tap to block)':'Face tracking BLOCKED (tap to allow)';$('trk').className=s.trk?'':'sv';
$('mo').textContent=s.on?'Switch motors OFF (camera may sag)':'Switch motors ON';
for(var i=0;i<6;i++){$('p'+i).className=s.ps[i]?'on':''}
$('au').textContent=s.auto?'Automatic updates: ON (tap to turn off)':'Automatic updates: OFF (tap to turn on)';$('au').className=s.auto?'on':'';
if(s.lat>s.fw&&s.fw>0)w+='Update available: build '+s.lat+'. ';$('warn').textContent=w;
$('fw').textContent='Head: build '+s.fw+'\nJoystick box: '+(s.jon?'build '+s.jfw:'not seen')+'\nNewest on GitHub: '+(s.lat<0?'not checked yet':'build '+s.lat)+(s.msg?'\n'+s.msg:'')}
function q(u){return fetch(u).then(function(r){return r.json()}).then(show).catch(function(){$('st').textContent='no connection to the head'})}
function lim(a,e){q('/lim?a='+a+'&e='+e)}
for(var i=0;i<6;i++){var b=document.createElement('button');b.id='p'+i;b.textContent=i+1;b.onclick=(function(n){return function(){q('/p?n='+n+(saving?'&save=1':''));saving=false;$('sv').className=''}})(i);$('pr').appendChild(b)}
$('sv').onclick=function(){saving=!saving;$('sv').className=saving?'sv':''};
$('stop').onclick=function(){X=Y=0;setK();q('/stop')};
$('max').onchange=function(){q('/set?max='+this.value)};$('ramp').onchange=function(){q('/set?ramp='+this.value)};
$('mo').onclick=function(){q('/en?on='+(S.on?0:1))};$('trk').onclick=function(){q('/trk')};
setInterval(function(){if(busy)return;var mvg=act||X||Y;if(mvg||last){busy=true;last=mvg?1:0;fetch('/j?x='+X.toFixed(3)+'&y='+Y.toFixed(3)).then(function(r){return r.json()}).then(show).catch(function(){}).then(function(){busy=false})}
else if(++idle>=10){idle=0;busy=true;q('/s').then(function(){busy=false})}},100);
</script></body></html>)HTML";

void sendStatus() {
  uint32_t now = millis();
  int src = 0;
  if (pan.webCmd != 0 || tilt.webCmd != 0) src = 1;
  else if ((pan.viscaCmd != 0 || tilt.viscaCmd != 0 || pan.going || tilt.going) && (int32_t)(manualUntil - now) <= 0) src = 2;
  long pp = pan.st ? (long)(pan.st->getCurrentPosition() * sgn(pan)) : 0, tp = tilt.st ? (long)(tilt.st->getCurrentPosition() * sgn(tilt)) : 0;
  char b[620];
  snprintf(b, sizeof b,
    "{\"on\":%d,\"trk\":%d,\"src\":%d,\"pd\":%.2f,\"td\":%.2f,\"pt\":%.3f,\"tt\":%.3f,\"gp\":%.2f,\"gt\":%.2f,\"pl\":%d,\"tl\":%d,\"dp\":%d,\"dt\":%d,"
    "\"max\":%d,\"ramp\":%.1f,\"rssi\":%d,\"ps\":[%d,%d,%d,%d,%d,%d],\"fw\":%d,\"auto\":%d,\"lat\":%d,\"jfw\":%d,\"jon\":%d,\"msg\":\"%s\"}",
    motorsOn, trackingOn, src, stepsToDeg(pan, pp), stepsToDeg(tilt, tp), pp / STEPS_PER_TURN, tp / STEPS_PER_TURN, pan.gear, tilt.gear,
    limited(pan), limited(tilt), pan.found, tilt.found, (int)maxRpm, ramp, (int)WiFi.RSSI(),
    presetSet[0], presetSet[1], presetSet[2], presetSet[3], presetSet[4], presetSet[5],
    FW_BUILD, autoUpdate, latestBuild, joyBuild, (int)(joySeenMs != 0 && millis() - joySeenMs < 6000), fwMsg.c_str());
  web.send(200, "application/json", b);
}

void webSetup() {
  web.on("/", []() { web.send_P(200, "text/html", PAGE); });
  web.on("/s", sendStatus);
  web.on("/j", []() {                                   // joystick: x, y from -1 to 1
    joyInput(web.arg("x").toFloat(), web.arg("y").toFloat());
    sendStatus();
  });
  web.on("/stop", []() { stopAll(); manualUntil = millis() + MANUAL_HOLD_MS; sendStatus(); });
  web.on("/trk", []() { trackingOn = !trackingOn; if (!trackingOn) pan.viscaCmd = tilt.viscaCmd = 0; sendStatus(); });
  web.on("/en", []() { setMotors(web.arg("on") == "1"); sendStatus(); });
  web.on("/set", []() {
    if (web.hasArg("max"))  { maxRpm = constrain(web.arg("max").toFloat(), 10.0f, 300.0f); prefs.putFloat("max", maxRpm); }
    if (web.hasArg("ramp")) { ramp = constrain(web.arg("ramp").toFloat(), 0.2f, 3.0f); prefs.putFloat("ramp", ramp); }
    if (web.hasArg("invp")) { pan.invert = !pan.invert; prefs.putBool("pinv", pan.invert); }
    if (web.hasArg("invt")) { tilt.invert = !tilt.invert; prefs.putBool("tinv", tilt.invert); }
    if (web.hasArg("gp"))   { pan.gear = fmaxf(0, web.arg("gp").toFloat()); prefs.putFloat("pgear", pan.gear); }
    if (web.hasArg("auto")) { autoUpdate = !autoUpdate; prefs.putBool("auto", autoUpdate); }
    if (web.hasArg("gt"))   { tilt.gear = fmaxf(0, web.arg("gt").toFloat()); prefs.putFloat("tgear", tilt.gear); }
    sendStatus();
  });
  web.on("/lim", []() {                                 // a = p|t, e = l|r|u|d (set that end here) or c (clear)
    Axis &x = web.arg("a") == "p" ? pan : tilt; String e = web.arg("e");
    if (x.st) {
      long pos = x.st->getCurrentPosition();
      if (e == "c") x.hasLo = x.hasHi = false;
      else {
        bool userHigh = (e == "r" || e == "u");                 // right / up = the + end as you see it
        bool motorHigh = x.invert ? !userHigh : userHigh;
        if (motorHigh) { x.hi = pos; x.hasHi = true; if (x.hasLo && x.lo > pos) x.hasLo = false; }
        else           { x.lo = pos; x.hasLo = true; if (x.hasHi && x.hi < pos) x.hasHi = false; }
      }
      saveLimits(x);
    }
    sendStatus();
  });
  web.on("/zero", []() {                                // here becomes 0,0; limits and presets keep their real places
    for (int i = 0; i < 2; i++) {
      Axis &x = *AX[i];
      if (!x.st || x.st->isRunning()) continue;
      long pos = x.st->getCurrentPosition();
      x.lo -= pos; x.hi -= pos; saveLimits(x);
      for (int n = 0; n < NPRESET; n++) { presetPos[n][i] -= pos; savePreset(n); }
      x.st->setCurrentPosition(0);
    }
    sendStatus();
  });
  web.on("/p", []() {
    int n = web.arg("n").toInt();
    if (n >= 0 && n < NPRESET && pan.st && tilt.st) {
      if (web.hasArg("save")) { presetSet[n] = true; presetPos[n][0] = pan.st->getCurrentPosition(); presetPos[n][1] = tilt.st->getCurrentPosition(); savePreset(n); }
      else recallPreset(n);
    }
    sendStatus();
  });
  web.on("/fw", []() { if (headMoving()) fwMsg = "Stop the head first"; else checkLatest(); sendStatus(); });
  web.on("/fwgo", []() {
    if (headMoving()) fwMsg = "Stop the head first";
    else { fwMsg = "Updating... the page comes back in about a minute"; updateRequested = true; }
    sendStatus();
  });
  web.on("/wifi", []() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font:16px sans-serif;max-width:420px;margin:auto;padding:16px'>"
                 "<h3>X70 head: Wi-Fi</h3><form method=post action=/wifisave>Wi-Fi name<br><input name=s style='width:100%;font-size:16px' value='");
    h += wifiSsid;
    h += F("'><br><br>Wi-Fi password<br><input name=p type=password style='width:100%;font-size:16px'><br><br>"
           "Device password (for the X70-Head Wi-Fi and manual updates; leave empty to keep)<br><input name=d style='width:100%;font-size:16px'><br><br>"
           "<button style='font-size:16px;padding:10px 20px'>Save and restart</button></form><p><a href='/'>Back</a> &middot; <a href='/update'>Upload a .bin by hand</a></p>");
    web.send(200, "text/html", h);
  });
  web.on("/wifisave", HTTP_POST, []() {
    String s = web.arg("s"), p = web.arg("p"), d = web.arg("d");
    s.trim();
    if (s.length()) { prefs.putString("ssid", s); if (p.length() || s != wifiSsid) prefs.putString("wpass", p); }
    if (d.length() >= 8) prefs.putString("dpass", d);
    web.send(200, "text/html", "<body style='font:16px sans-serif;padding:16px'>Saved. The head is restarting and will join that Wi-Fi. If it cannot, it brings back its own X70-Head Wi-Fi.");
    delay(800); setMotors(false); ESP.restart();
  });
  webUpdater.setup(&web, "/update", "admin", devPass.c_str());   // upload a .bin from any browser
  web.begin();
}

// ---------------- start-up ----------------
void setup() {
  pinMode(EN_PIN, OUTPUT); digitalWrite(EN_PIN, HIGH);          // motors off until everything is ready
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  delay(300);
  Serial.printf("\n=== X70 head firmware, build %d ===\n", FW_BUILD);
  loadAll();

  for (Axis *x : AX) {
    x->found = x->drv->version() == 0x21;
    Serial.printf("%s driver: %s\n", x->name, x->found ? "found" : "NOT FOUND (12 V on before USB?)");
    if (x->found) configure(*x->drv);
  }
  engine.init();
  pan.st  = engine.stepperConnectToPin(PAN_STEP);
  tilt.st = engine.stepperConnectToPin(TILT_STEP);
  if (pan.st)  { pan.st->setDirectionPin(PAN_DIR);   pan.st->setCurrentPosition(pan.saved); }
  if (tilt.st) { tilt.st->setDirectionPin(TILT_DIR); tilt.st->setCurrentPosition(tilt.saved); }
  if (!pan.st || !tilt.st) Serial.println(F("Step generator could not start - tell Claude."));
  if (pan.found && tilt.found) setMotors(true); else Serial.println(F("Motors stay OFF: a driver is not answering."));

  WiFi.setHostname("x70head");
  bool joined = false;
  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("Joining Wi-Fi \"%s\"", wifiSsid.c_str());
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
    joined = WiFi.status() == WL_CONNECTED;
  }
  if (joined) {
    Serial.printf("\nConnected. Joystick page: http://%s   (or http://x70head.local)\n", WiFi.localIP().toString().c_str());
    Serial.printf("OBS: VISCA over TCP port 5678, or VISCA over IP (UDP) port 52381, address %s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME, devPass.c_str());
    Serial.printf("\nNo Wi-Fi yet. Join the Wi-Fi \"%s\" (password %s), then open http://192.168.4.1/wifi\n", AP_NAME, devPass.c_str());
  }
  ArduinoOTA.setHostname("x70head");                     // also starts the x70head.local name
  ArduinoOTA.setPassword(devPass.c_str());
  ArduinoOTA.onStart([]() { stopAll(); for (Axis *x : AX) if (x->st) x->st->forceStop(); });   // stand still while updating
  ArduinoOTA.begin();
  webSetup();
  viscaTcp.begin();
  viscaUdp.begin(52381);
  joyUdp.begin(52380);
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  if (now - lastTick >= 10) { float dt = (now - lastTick) * 0.001f; if (dt > 0.05f) dt = 0.05f; lastTick = now; motionTick(dt); }
  web.handleClient();
  ArduinoOTA.handle();
  viscaPoll();
  joyBoxPoll();
  // Automatic updates: look at GitHub every 10 minutes, but only while nothing is happening
  // (no joystick or OBS command and no movement for 5 minutes, or the first minutes after power-up).
  static uint32_t lastCheck = 0, lastJoyNudge = 0;
  bool online = WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED;
  bool quiet = !headMoving() && pan.viscaCmd == 0 && tilt.viscaCmd == 0 && pan.webCmd == 0 && tilt.webCmd == 0 &&
               ((lastInputMs == 0 && lastMoveMs == 0) || (now - lastInputMs > 300000UL && now - lastMoveMs > 300000UL));
  if (online && quiet && now > 45000UL && (lastCheck == 0 || now - lastCheck > 600000UL)) {
    lastCheck = now;
    checkLatest();
    if (autoUpdate && FW_BUILD > 0 && latestBuild > FW_BUILD) { Serial.printf("New build %d found: updating\n", latestBuild); updateRequested = true; }
  }
  // joystick box still on an older build (it was off during the last update): tell it again, at most every 5 minutes
  if (online && quiet && autoUpdate && latestBuild > 0 && joyBuild > 0 && joyBuild < latestBuild && now - joySeenMs < 6000 &&
      (lastJoyNudge == 0 || now - lastJoyNudge > 300000UL)) { lastJoyNudge = now; broadcastText("U", 52379); }
  if (updateRequested && !headMoving()) {
    updateRequested = false;
    for (int i = 0; i < 3; i++) { broadcastText("U", 52379); delay(30); }   // tell the joystick box to update itself too
    delay(300);
    selfUpdate();                                                           // restarts on success
  }
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {      // Wi-Fi dropped: stop moving, it rejoins on its own
    static uint32_t lastTry = 0;
    pan.viscaCmd = tilt.viscaCmd = 0;
    if (now - lastTry > 5000) { lastTry = now; WiFi.reconnect(); }
  }
}
