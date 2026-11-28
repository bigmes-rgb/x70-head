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
// Same files on the installer site. Tried first: it is one secure connection to one server, where the release link
// above is passed through two more servers and needs about twice the memory.
const char *FW_SITE = "https://bigmes-rgb.github.io/x70-head/";
const char *AP_NAME = "X70-Head";
const char *DEFAULT_PASS = "x70head123";   // device password until you change it on the Wi-Fi page

#include <stdarg.h>
#include <esp_system.h>
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
const uint16_t MICROSTEPS     = 128;    // fine steps: at 16 each step showed as a tick in the picture when starting and stopping
const float    STEPS_PER_TURN = 200.0f * MICROSTEPS;
const float    SLOW_CAP_RPM   = 20;    // top motor speed while an axis has no travel limits yet
const float    STOP_TIME      = 0.5f;  // seconds to brake from top speed when coming up to a limit
const uint32_t MANUAL_HOLD_MS = 3000;  // joystick beats face tracking for this long after you let go
const uint32_t DEADMAN_MS     = 600;   // joystick page gone quiet -> stop
const uint32_t VISCA_DEADMAN_MS = 3000; // OBS / tracker gone quiet while it had the head moving -> stop

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
String wifiSsid, wifiPass, wifiSsid2, wifiPass2, devPass;      // stored in the device (Wi-Fi page)
int  latestBuild = -1, joyBuild = -1;    // newest build on GitHub; build the joystick box reports
uint32_t joySeenMs = 0;
IPAddress joyIP;                         // where the joystick box was last heard from
bool updateRequested = false;
bool updMode = false;                     // started only to install an update: nothing else runs
volatile int updStage = 0, updPct = 0, updTry = 0;   // 1 connecting, 2 downloading, 3 installed, 4 failed
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
  bool  trk;                           // the move in progress came from face tracking / OBS (uses the tracking ease)
};
Axis pan  = {"pan",  'p', nullptr, &drvPan,  false, false, 0, false, false, 0, 0, 0, 0, 0, 0, false, 0};
Axis tilt = {"tilt", 't', nullptr, &drvTilt, false, false, 0, false, false, 0, 0, 0, 0, 0, 0, false, 0};
Axis *AX[2] = {&pan, &tilt};

float joyScale = 0.5f;     // joystick full deflection = this share of the top speed (page slider)
float maxRpm = 120;        // top motor speed (page slider)
float ramp   = 1.0f;       // seconds for a speed change to ease in / out (page slider)
bool  motorsOn = false;
bool  trackingOn = true;   // false = ignore OBS / face tracking (joystick box button or the page)
float trackScale = 0.2f;   // tracking speed scale: 1.0 = the speeds of a Sony PTZ camera, lower = finer and gentler (tune page)
float trackRamp  = 0.5f;   // seconds for a tracking speed change to ease in / out (tune page)
uint32_t viscaLastMs = 0;  // last pan/tilt command from OBS
// Camera speed in degrees per second for each VISCA speed number, as the Face Tracker plugin assumes it (Sony table).
const float SONY_PAN[25]  = {0, 0.875f, 1.55f, 2.075f, 2.4f, 2.6f, 2.8f, 3.0f, 3.2f, 3.45f, 3.875f, 4.7f, 6.375f, 9.75f, 14.5f, 19.5f, 25.25f, 31.75f, 38.25f, 46.5f, 59.0f, 74.0f, 89.0f, 104.5f, 120.0f};
const float SONY_TILT[19] = {0, 0.875f, 1.55f, 2.075f, 3.225f, 4.75f, 6.175f, 8.0f, 10.0f, 12.25f, 15.0f, 18.5f, 23.5f, 30.5f, 39.0f, 48.5f, 58.75f, 69.25f, 80.0f};
uint32_t lastWebMs = 0, manualUntil = 0, lastMoveMs = 0;
const int NPRESET = 6;
bool presetSet[NPRESET]; long presetPos[NPRESET][2];
char presetName[NPRESET][28];   // shown on the page, e.g. "Pulpit"
int  setupDone = 0;             // guided setup steps finished on the page: 1 = directions checked, 2 = centre set, 4 = feel chosen

float maxSps() { return maxRpm / 60.0f * STEPS_PER_TURN; }
float sgn(const Axis &x) { return x.invert ? -1.0f : 1.0f; }
bool  limited(const Axis &x) { return x.hasLo && x.hasHi; }
float capSps(const Axis &x) { return (limited(x) ? maxRpm : fminf(maxRpm, SLOW_CAP_RPM)) / 60.0f * STEPS_PER_TURN; }
float stepsToDeg(const Axis &x, long s) { return x.gear > 0 ? s / STEPS_PER_TURN / x.gear * 360.0f : 0; }

// ---------------- saved settings ----------------
String k(const Axis &x, const char *what) { String s; s += x.key; s += what; return s; }
void loadAll() {
  prefs.begin("x70head", false);
  setupDone = prefs.getInt("sd", 0);
  joyScale = prefs.getFloat("jsc", 0.5f); maxRpm = prefs.getFloat("max", 120); ramp = prefs.getFloat("ramp", 1.0f);
  trackScale = prefs.getFloat("tsc", 0.2f); trackRamp = prefs.getFloat("tramp", 0.5f);
  wifiSsid = prefs.getString("ssid", ""); wifiPass = prefs.getString("wpass", ""); wifiSsid2 = prefs.getString("ssid2", ""); wifiPass2 = prefs.getString("wpass2", ""); devPass = prefs.getString("dpass", DEFAULT_PASS);
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
    snprintf(key, sizeof key, "pm%d", i); strlcpy(presetName[i], prefs.getString(key, "").c_str(), sizeof presetName[i]);
    snprintf(key, sizeof key, "pp%d", i); presetPos[i][0] = prefs.getLong(key, 0);
    snprintf(key, sizeof key, "pt%d", i); presetPos[i][1] = prefs.getLong(key, 0);
  }
  // Positions are stored in motor steps. If the step size changed with an update, convert everything once.
  int was = prefs.getInt("ustep", 16);
  if (was != MICROSTEPS && was > 0) {
    auto conv = [was](long v) { return (long)((int64_t)v * MICROSTEPS / was); };
    for (Axis *x : AX) {
      x->lo = conv(x->lo); x->hi = conv(x->hi); x->saved = conv(x->saved);
      prefs.putLong(k(*x, "lo").c_str(), x->lo); prefs.putLong(k(*x, "hi").c_str(), x->hi); prefs.putLong(k(*x, "pos").c_str(), x->saved);
    }
    for (int i = 0; i < NPRESET; i++) {
      char key[8];
      presetPos[i][0] = conv(presetPos[i][0]); presetPos[i][1] = conv(presetPos[i][1]);
      snprintf(key, sizeof key, "pp%d", i); prefs.putLong(key, presetPos[i][0]);
      snprintf(key, sizeof key, "pt%d", i); prefs.putLong(key, presetPos[i][1]);
    }
    prefs.putInt("ustep", MICROSTEPS);
  }
}
void saveLimits(const Axis &x) {
  prefs.putBool(k(x, "hl").c_str(), x.hasLo); prefs.putLong(k(x, "lo").c_str(), x.lo);
  prefs.putBool(k(x, "hh").c_str(), x.hasHi); prefs.putLong(k(x, "hi").c_str(), x.hi);
}
// A preset's name as typed on the page: anything that would break the status text is dropped, and it is cut to fit
// without splitting a letter (names may be in Arabic).
void setPresetName(int i, String v) {
  char *d = presetName[i]; size_t n = 0, k = 0;
  for (; k < v.length() && n < sizeof presetName[i] - 1; k++) { unsigned char c = v[k]; if (c >= 0x20 && c != '"' && c != '\\' && c != '<') d[n++] = c; }
  if (k < v.length()) {                                                               // cut short: drop a half letter at the end
    while (n > 0 && ((unsigned char)d[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0 && (unsigned char)d[n - 1] >= 0xC0) n--;
  }
  d[n] = 0;
  char key[8]; snprintf(key, sizeof key, "pm%d", i); prefs.putString(key, d);
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
  d.intpol(true);                        // the driver smooths between steps as well
  d.iholddelay(15);                      // fade to the holding current slowly, no nudge after stopping
  d.en_spreadCycle(!QUIET_MODE);
  d.pwm_autoscale(true);
  d.TPOWERDOWN(20);
  d.rms_current(RUN_MA, 0.5);
  d.GSTAT(0b111);
}
void setMotors(bool on);
// ---------------- event log ----------------
// The last things that happened, with the time since power-up. Shown on the page ("Recent events") so that an odd
// movement can be traced to its cause afterwards.
const int NEV = 48;
char evBuf[NEV][76]; int evHead = 0, evCount = 0;
void ev(const char *fmt, ...) {
  char m[60]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
  uint32_t t = millis() / 1000;
  snprintf(evBuf[evHead], sizeof evBuf[0], "%02u:%02u:%02u  %s", (unsigned)(t / 3600), (unsigned)(t / 60 % 60), (unsigned)(t % 60), m);
  Serial.println(evBuf[evHead]);
  evHead = (evHead + 1) % NEV; if (evCount < NEV) evCount++;
}
bool driversDown = false;                // true while a driver is not answering (no 12 V)
unsigned long lastDrvCheck = 0;
// Runs once a second while the head is still. Lets 12 V and USB be plugged in any order:
// a driver that appears late, or that lost power and came back, is set up again automatically.
void driverWatch() {
  static uint8_t miss[2] = {0, 0};
  bool changed = false; int i = 0;
  for (Axis *x : AX) {
    bool ok = x->drv->version() == 0x21;
    if (ok) {
      miss[i] = 0;
      if (!x->found || (x->drv->GSTAT() & 1)) {
        configure(*x->drv);
        ev("%s driver: powered, set up again", x->name);
        x->found = true; changed = true;
      }
    } else if (x->found && ++miss[i] >= 2) {
      x->found = false; changed = true;
      ev("%s driver: not answering (12 V off?)", x->name);
    }
    i++;
  }
  if (!changed) return;
  if (pan.found && tilt.found) { if (driversDown) { driversDown = false; setMotors(true); Serial.println(F("Motors on.")); } }
  else if (!driversDown) { driversDown = true; setMotors(false); }
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
  if ((pan.viscaCmd != 0 || tilt.viscaCmd != 0) && now - viscaLastMs > VISCA_DEADMAN_MS) pan.viscaCmd = tilt.viscaCmd = 0;
  for (Axis *xp : AX) {
    Axis &x = *xp;
    if (!x.st) continue;
    if (!webAlive) x.webCmd = 0;
    float want = (manual || !trackingOn) ? x.webCmd : (x.webCmd != 0 ? x.webCmd : x.viscaCmd);
    if (!motorsOn) want = 0;
    if (want != 0) x.trk = (x.webCmd == 0);                              // who started this move: tracking or the joystick
    if (x.going) {
      if (want != 0) { x.going = false; x.v = x.st->getCurrentSpeedInMilliHz() / 1000.0f; x.a = 0; }   // joystick takes over mid-move
      else { if (!x.st->isRunning()) { x.going = false; x.v = x.a = 0; } else lastMoveMs = now; continue; }
    }
    float cap = capSps(x);
    float target = constrain(want * sgn(x), -cap, cap);                    // motor direction
    // critically damped follow: eases in and eases out, and copes with a target that keeps changing (face tracking)
    float w = 4.74f / (x.trk ? trackRamp : ramp);                                                // reaches 95 % of the new speed in 'ramp' seconds
    float vBefore = x.v;
    float acc = w * w * (target - x.v) - 2.0f * w * x.a;
    x.a += acc * dt;
    if (!x.trk) {
      // Joystick moves: however hard the stick is thrown over, the speed builds up no faster than this, so a big
      // move starts as gently as a small one. Stopping may be twice as quick, so letting go still stops promptly.
      float aUp = maxSps() * joyScale / fmaxf(0.3f, ramp * 2.0f);
      bool speedingUp = fabsf(target) > fabsf(x.v) && target * x.v >= 0;
      float lim = speedingUp ? aUp : aUp * 2.0f;
      x.a = constrain(x.a, -lim, lim);
    }
    x.v += x.a * dt;
    // limits: the closer to the end, the slower it may go toward it
    long pos = x.st->getCurrentPosition();
    float brake = maxSps() / STOP_TIME;
    if (x.v > 0 && x.hasHi) { float d = (float)(x.hi - pos) - 3.0f * MICROSTEPS; float lim = d > 0 ? sqrtf(2.0f * brake * d) : 0; if (x.v > lim) { x.v = lim; if (x.a > 0) x.a = 0; } }
    if (x.v < 0 && x.hasLo) { float d = (float)(pos - x.lo) - 3.0f * MICROSTEPS; float lim = d > 0 ? sqrtf(2.0f * brake * d) : 0; if (-x.v > lim) { x.v = -lim; if (x.a < 0) x.a = 0; } }
    float s = fabsf(x.v);
    if (s < 1.0f) {
      if (target == 0) { x.v = x.a = 0; }
      if (x.st->isRunning()) { x.st->setAcceleration((int32_t)brake); x.st->applySpeedAcceleration(); x.st->stopMove(); }
    } else {
      x.st->setAcceleration((int32_t)(fabsf(x.v - vBefore) / dt * 1.5f) + 300 * (MICROSTEPS / 16));   // glide to this tick's speed, no stair-steps
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
// VISCA speed number -> motor steps/s. With the gear ratio known the head moves at the Sony table speed times the
// tracking scale, so tracker settings mean the same here as on a real PTZ camera. Without it, the old uncalibrated curve.
float viscaSps(const Axis &x, int s, bool isTilt) {
  if (x.gear <= 0) return viscaSpeed(s, isTilt ? 0x14 : 0x18);
  float dps = (isTilt ? SONY_TILT[constrain(s, 1, 18)] : SONY_PAN[constrain(s, 1, 24)]) * trackScale;
  return dps / 360.0f * x.gear * STEPS_PER_TURN;
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
      float ps = viscaSps(pan, p[4], false), ts = viscaSps(tilt, p[5], true);
      viscaLastMs = millis();
      pan.viscaCmd  = p[6] == 0x01 ? -ps : (p[6] == 0x02 ? ps : 0);         // 01 left, 02 right
      if (p[6] != 0x03 || p[7] != 0x03) lastInputMs = millis();
      tilt.viscaCmd = p[7] == 0x01 ? ts : (p[7] == 0x02 ? -ts : 0);         // 01 up, 02 down
      if (!trackingOn) pan.viscaCmd = tilt.viscaCmd = 0;                    // blocked: the command is thrown away here
    } else if (n >= 15 && p[2] == 0x06 && (p[3] == 0x02 || p[3] == 0x03)) { // absolute / relative position
      if (trackingOn && pan.gear > 0 && tilt.gear > 0 && pan.st && tilt.st) {
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
    if (viscaClient) { viscaClient.stop(); pan.viscaCmd = tilt.viscaCmd = 0; len = 0; ev("OBS disconnected"); }     // tracker went away: stop
    WiFiClient c = viscaTcp.accept();
    if (c) { viscaClient = c; viscaClient.setNoDelay(true); len = 0; ev("OBS connected from %s", c.remoteIP().toString().c_str()); }
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

// The check runs in the background so the page and the motors never wait for GitHub.
volatile int chkState = 0;                     // 0 idle, 1 asking GitHub, 2 answer ready
int chkBuild = -1; char chkErr[120] = ""; bool chkAuto = false;
void checkTask(void *) {
  chkErr[0] = 0; chkBuild = -1;
  for (int source = 0; source < 2 && chkBuild < 0; source++) {
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h; h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); h.setConnectTimeout(8000); h.setTimeout(8000);
    String url = source == 0 ? String(FW_SITE) + "version.txt?t=" + String(esp_random()) : String(FW_BASE) + "version.txt";
    if (h.begin(c, url)) {
      int code = h.GET();
      if (code == 200) { int b = h.getString().toInt(); if (b > 0) { chkBuild = b; chkErr[0] = 0; } }
      if (chkBuild < 0) { char tls[60] = ""; c.lastError(tls, sizeof tls); snprintf(chkErr, sizeof chkErr, "Could not reach GitHub (%s%s%s)", code > 0 ? String(code).c_str() : HTTPClient::errorToString(code).c_str(), tls[0] ? " / " : "", tls); }
      h.end();
    } else snprintf(chkErr, sizeof chkErr, "Could not reach GitHub");
  }
  for (char *q = chkErr; *q; q++) if (*q == '"') *q = '\'';
  chkState = 2;
  vTaskDelete(NULL);
}
void startCheck(bool automatic) {
  if (chkState != 0) return;
  chkAuto = automatic; chkState = 1;
  if (xTaskCreate(checkTask, "check", 12288, NULL, 1, NULL) != pdPASS) { chkState = 0; fwMsg = "Could not check: the head is out of memory. Unplug its power for 5 seconds."; }
}
void sendText(IPAddress to, const char *msg, int port) { WiFiUDP u; u.beginPacket(to, port); u.write((const uint8_t *)msg, strlen(msg)); u.endPacket(); }
void tellJoystick(const char *msg);
void broadcastText(const char *msg, int port) {
  IPAddress ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP() : WiFi.localIP(), bc = ip; bc[3] = 255;
  WiFiUDP u; u.beginPacket(bc, port); u.write((const uint8_t *)msg, strlen(msg)); u.endPacket();
}
// Updates install in a clean restart: the head comes back up with only Wi-Fi and its page running,
// so the download has the whole memory and network to itself, and the page can show live progress.
void requestUpdateRestart() {
  for (Axis *x : AX) if (x->st) { long p = x->st->getCurrentPosition(); if (p != x->saved) { x->saved = p; prefs.putLong(k(*x, "pos").c_str(), p); } }
  prefs.putBool("upd", true);
  delay(400);
  ESP.restart();
}
void updateTask(void *) {
  delay(1500);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) { updStage = 2; updPct = total > 0 ? (int)(100LL * cur / total) : 0; });
  String err;
  for (int attempt = 1; attempt <= 5; attempt++) {
    updTry = attempt; updStage = 1; updPct = 0;
    WiFiClientSecure c; c.setInsecure();
    String url = (attempt % 2) ? String(FW_SITE) + FW_FILE + "?t=" + String(esp_random()) : String(FW_BASE) + FW_FILE;   // installer site first, release link as the fallback
    t_httpUpdate_return r = httpUpdate.update(c, url);
    if (r == HTTP_UPDATE_OK) { updPct = 100; updStage = 3; delay(2500); ESP.restart(); }
    char tls[80] = ""; int te = c.lastError(tls, sizeof tls);       // the exact reason the secure connection gave, for diagnosis
    err = httpUpdate.getLastErrorString(); if (te) { err += " / "; err += tls; }
    err.replace("\"", "'");
    Serial.printf("Update try %d failed: %s\n", attempt, err.c_str());
    delay(4000);
  }
  updStage = 4;
  prefs.putString("updmsg", "Update failed after 5 tries: " + err + " (free memory " + String(ESP.getFreeHeap() / 1024) + " KB)");
  delay(4000);
  ESP.restart();                                 // back to normal running on the build it already has
}
void tellJoystick(const char *msg) {          // straight to the box if its address is known, and to everyone as a fallback
  if (joyIP[0] != 0) sendText(joyIP, msg, 52379);
  broadcastText(msg, 52379);
}

// ---------------- joystick input (page and joystick box) ----------------
bool stickFromBox = false;               // who moved the stick last: the joystick box or the page
void joyInput(float x, float y) {                       // x, y from -1 to 1
  auto curve = [](float u) { float m = fabsf(u); if (m < 0.06f) return 0.0f; m = (m - 0.06f) / 0.94f; return (u < 0 ? -1.0f : 1.0f) * m * m; };   // even build-up: fine near the middle without a sudden surge at the end of the stick's travel
  pan.webCmd = curve(constrain(x, -1.0f, 1.0f)) * maxSps() * joyScale; tilt.webCmd = curve(constrain(y, -1.0f, 1.0f)) * maxSps() * joyScale;
  lastWebMs = millis();
  if (pan.webCmd != 0 || tilt.webCmd != 0) { manualUntil = lastWebMs + MANUAL_HOLD_MS; lastInputMs = lastWebMs; }
}
void recallPreset(int n) {
  ev("preset %d asked for", n + 1);
  if (n >= 0 && n < NPRESET && presetSet[n]) { manualUntil = millis() + MANUAL_HOLD_MS; pan.viscaCmd = tilt.viscaCmd = 0; gotoPos(presetPos[n][0], presetPos[n][1]); }
}
void setTracking(bool on) { if (on != trackingOn) ev("tracking %s by the joystick box", on ? "ALLOWED" : "BLOCKED"); trackingOn = on; if (!on) pan.viscaCmd = tilt.viscaCmd = 0; }
// Joystick box packets (text, UDP 52380):  "J,x,y"  stick   |  "B,id,P,n"  recall preset n   |  "B,id,T"  tracking on/off
void joyBoxPoll() {
  static long lastId = -1;
  for (int guard = 0; guard < 8; guard++) {
    int sz = joyUdp.parsePacket();
    if (sz <= 0) return;
    char b[40]; int n = joyUdp.read((uint8_t *)b, sizeof b - 1); if (n <= 0) continue; b[n] = 0;
    float x, y; long id; int k;
    if (joyUdp.remoteIP() != joyIP) ev("joystick box talking from %s", joyUdp.remoteIP().toString().c_str());
    joyIP = joyUdp.remoteIP();
    if (sscanf(b, "J,%f,%f", &x, &y) == 2) { stickFromBox = true; joyInput(x, y); joySeenMs = millis(); }
    else if (sscanf(b, "H,%d", &k) == 1) {                                           // joystick box heartbeat: build number, tracking switch
      joyBuild = k; joySeenMs = millis();
      int sw = -1; static int lastSw = -1;
      if (sscanf(b, "H,%*d,%d", &sw) == 1 && sw != lastSw) { lastSw = sw; setTracking(sw != 0); }   // follow the switch when it changes (also after a restart)
      char a[16]; snprintf(a, sizeof a, "A,%d", FW_BUILD); sendText(joyIP, a, 52379);  // answer, so the box learns the head's address
    }
    else if (sscanf(b, "B,%ld,P,%d", &id, &k) == 2) { if (id != lastId) { lastId = id; recallPreset(k); } }
    else if (sscanf(b, "B,%ld,T,%d", &id, &k) == 2) { if (id != lastId) { lastId = id; setTracking(k != 0); } }          // tracking switch position
    else if (sscanf(b, "B,%ld,T", &id) == 1 && strchr(b, 'T')) { if (id != lastId) { lastId = id; setTracking(!trackingOn); } }   // older boxes: toggle
  }
}

// ---------------- web page ----------------
const char WAITPAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>X70</title>
<style>body{margin:0;background:#0d0f13;color:#e8ecf1;font:16px -apple-system,Segoe UI,Roboto,sans-serif;display:flex;min-height:100vh;align-items:center;justify-content:center}
.c{width:min(88vw,360px);background:#161a21;border:1px solid #2a313c;border-radius:16px;padding:22px}h2{margin:0 0 12px;font-size:18px;display:flex;gap:10px;align-items:center}
i{width:18px;height:18px;border-radius:50%;border:2px solid #ffb347;border-top-color:transparent;animation:s .8s linear infinite;flex:none}
p{color:#8e98a7;font-size:14px;line-height:1.45}@keyframes s{to{transform:rotate(360deg)}}</style>
<div class=c><h2><i></i><span>%HEAD%</span></h2><p id=m>This page comes back by itself. Nothing to do.</p><p id=n hidden>%NOTE%</p></div>
<script>var t0=Date.now(),g=function(i){return document.getElementById(i)};setTimeout(function k(){var c=new AbortController();setTimeout(function(){c.abort()},2000);
fetch('%PING%',{signal:c.signal,cache:'no-store'}).then(function(r){if(r.ok)location.replace('/');else throw 0}).catch(function(){var s=Math.round((Date.now()-t0)/1000);
g('m').textContent='Waiting for it to come back: '+s+' s';if(s>%LATE%)g('n').hidden=false;setTimeout(k,2000)})},6000)</script>)HTML";
String waitPage(const char *head, const char *note, const char *ping, int late) {
  String h = FPSTR(WAITPAGE); h.replace("%HEAD%", head); h.replace("%NOTE%", note); h.replace("%PING%", ping); h.replace("%LATE%", String(late)); return h;
}
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#0d0f13"><title>X70 Head</title>
<style>
:root{--bg:#0d0f13;--s1:#161a21;--s2:#1e232c;--ln:#2a313c;--tx:#eef1f5;--dm:#8e98a7;--ac:#ffb347;--ok:#4cd08a;--aim:#5cc8ff;--bad:#ff5d52;--r:16px;color-scheme:dark}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-user-select:none;user-select:none;touch-action:manipulation}
.w{max-width:480px;margin:auto;padding:14px max(16px,env(safe-area-inset-left)) 150px max(16px,env(safe-area-inset-right))}
[hidden]{display:none!important}
.n{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums}
.lb{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--dm)}
/* header */
.hd{display:flex;align-items:center;justify-content:space-between;gap:8px}
.hd b{font-size:13px;letter-spacing:.18em}.hd b i{font-style:normal;color:var(--ac)}
.pl{display:flex;gap:6px}.pi{display:flex;align-items:center;gap:5px;font-size:11px;color:var(--dm);background:var(--s1);border:1px solid var(--ln);border-radius:99px;padding:4px 9px}
.pi::before{content:"";width:7px;height:7px;border-radius:50%;background:#4a5261}.pi.ok::before{background:var(--ok)}.pi.wn::before{background:var(--ac)}.pi.ok{color:var(--tx)}
#hero{margin:16px 0 4px;font-size:24px;font-weight:650;letter-spacing:-.01em;line-height:1.15;text-wrap:balance;min-height:28px}
#hero.t{color:var(--ok)}#hero.j{color:var(--ac)}#hero.b{color:var(--bad)}
#sub{color:var(--dm);font-size:13px;min-height:19px}
#al{margin-top:10px;display:flex;flex-direction:column;gap:6px}
.a{display:flex;align-items:center;gap:10px;background:#2a2113;border:1px solid #5b4520;color:#ffd9a0;border-radius:12px;padding:9px 12px;font-size:13px;text-align:left;width:100%}
.a.r{background:#2b1514;border-color:#6b2a25;color:#ffc2bd}.a span{flex:1}.a em{font-style:normal;font-weight:600;white-space:nowrap}
/* cards */
.c{background:var(--s1);border:1px solid var(--ln);border-radius:var(--r);padding:14px;margin-top:14px}
.ch{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px}.ch small{color:var(--dm);font-size:12px}
button{font:inherit;color:inherit;background:var(--s2);border:1px solid var(--ln);border-radius:12px;padding:12px;cursor:pointer}
button:active{filter:brightness(1.25)}button:focus-visible,input:focus-visible{outline:2px solid var(--ac);outline-offset:2px}
.pr{background:var(--ac);border-color:var(--ac);color:#1a1205;font-weight:650}.gh{background:none}.dg{background:var(--bad);border-color:var(--bad);color:#fff;font-weight:650}
.bw{width:100%}.row{display:flex;gap:8px}.row>*{flex:1}
/* stage map */
#map{display:block;width:100%;height:auto;border-radius:10px;background:#0a0c10;touch-action:manipulation}
#mapx{position:relative}#mape{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;text-align:center;padding:16px;color:var(--dm);font-size:13px}
/* pad */
#pad,.pad{width:min(64vw,250px);aspect-ratio:1;margin:6px auto 12px;border-radius:50%;position:relative;touch-action:none;
background:radial-gradient(circle,#232a35 0,#161a21 68%,#11141a 100%);border:1px solid var(--ln);box-shadow:inset 0 0 0 10px #0f1217,inset 0 0 24px #000}
.pad::before,.pad::after{content:"";position:absolute;background:var(--ln)}.pad::before{left:50%;top:14%;bottom:14%;width:1px}.pad::after{top:50%;left:14%;right:14%;height:1px}
.kn{width:34%;height:34%;border-radius:50%;position:absolute;left:33%;top:33%;pointer-events:none;z-index:1;background:radial-gradient(circle at 35% 30%,#ffd08a,var(--ac) 55%,#b87416);box-shadow:0 6px 18px #000a}
.sg{display:grid;grid-template-columns:repeat(3,1fr);background:var(--bg);border:1px solid var(--ln);border-radius:12px;padding:3px}
.sg button{border:0;background:none;padding:9px 4px;border-radius:9px;color:var(--dm);font-size:13px}.sg button.on{background:var(--s2);color:var(--tx);box-shadow:0 0 0 1px var(--ln)}
/* presets */
#pr{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.p{position:relative;text-align:left;padding:10px 10px 12px;min-height:64px;display:flex;flex-direction:column;justify-content:space-between;overflow:hidden}
.p u{text-decoration:none;font-size:11px;color:var(--dm)}.p s{text-decoration:none;font-size:14px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.p.e{border-style:dashed;background:none;color:var(--dm)}.p.e s{font-weight:400}.p.at{border-color:var(--aim);box-shadow:inset 0 0 0 1px var(--aim)}
.p i{position:absolute;right:0;top:0;width:34px;height:30px;display:flex;align-items:center;justify-content:center;font-style:normal;color:var(--dm);font-size:18px;line-height:1}
/* switch */
.sw{display:flex;align-items:center;justify-content:space-between;gap:12px;width:100%;background:none;border:0;padding:4px 0;text-align:left}
.sw span small{display:block;color:var(--dm);font-size:12px}.tg{flex:none;width:50px;height:30px;border-radius:99px;background:#39414e;position:relative;transition:.2s}
.tg::after{content:"";position:absolute;left:3px;top:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}.sw[aria-checked=true] .tg{background:var(--ok)}.sw[aria-checked=true] .tg::after{left:23px}
.lk{display:flex;justify-content:space-between;align-items:center;width:100%;margin-top:10px;text-align:left}.lk::after{content:"›";color:var(--dm);font-size:20px;line-height:1}
/* sliders */
.f{margin-top:14px}.f label{display:flex;justify-content:space-between;font-size:13px}.f label b{font-weight:600}.f small{display:block;color:var(--dm);font-size:12px;margin-top:2px}
input[type=range]{width:100%;accent-color:var(--ac);margin:8px 0 0;height:28px}
input[type=text],input[type=number]{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--ln);border-radius:10px;padding:11px;font:inherit;font-size:16px;-webkit-user-select:text;user-select:text}
/* checklist */
.ck{display:flex;align-items:center;gap:12px;width:100%;text-align:left;background:none;border:0;border-top:1px solid var(--ln);border-radius:0;padding:12px 0}
.ck:first-of-type{border-top:0}.ck i{flex:none;width:26px;height:26px;border-radius:50%;border:2px solid #4a5261;display:flex;align-items:center;justify-content:center;font-style:normal;font-size:12px;color:var(--dm)}
.ck.d i{background:var(--ok);border-color:var(--ok);color:#06210f;font-weight:700}.ck span{flex:1}.ck span small{display:block;color:var(--dm);font-size:12px}.ck::after{content:"›";color:var(--dm);font-size:20px}
.ring{--p:0;width:52px;height:52px;border-radius:50%;background:conic-gradient(var(--ok) calc(var(--p)*1%),#2a313c 0);display:flex;align-items:center;justify-content:center;flex:none}
.ring b{width:42px;height:42px;border-radius:50%;background:var(--s1);display:flex;align-items:center;justify-content:center;font-size:13px}
.hl{display:flex;gap:10px;align-items:flex-start;padding:8px 0;font-size:13px;border-top:1px solid var(--ln)}.hl:first-child{border-top:0}
.hl::before{content:"";flex:none;width:9px;height:9px;border-radius:50%;margin-top:5px;background:var(--ok)}.hl.wn::before{background:var(--ac)}.hl.bd::before{background:var(--bad)}.hl span{flex:1}.hl small{display:block;color:var(--dm)}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:0;font-size:13px}dt{color:var(--dm)}dd{margin:0;text-align:right}
pre{font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;color:#cfd6df;margin:10px 0;max-height:320px;overflow:auto;-webkit-user-select:text;user-select:text}
details summary{list-style:none;display:flex;justify-content:space-between;cursor:pointer}details summary::after{content:"+";color:var(--dm)}details[open] summary::after{content:"–"}
/* fixed bottom */
#bt{position:fixed;left:0;right:0;bottom:0;background:linear-gradient(#0d0f1300,var(--bg) 22%);padding:18px max(16px,env(safe-area-inset-left)) calc(8px + env(safe-area-inset-bottom)) max(16px,env(safe-area-inset-right));z-index:5}
#bt>div{max-width:448px;margin:auto}#stop{width:100%;padding:15px;font-size:17px;letter-spacing:.14em;border-radius:14px;box-shadow:0 8px 24px #ff5d5233}
nav{display:grid;grid-template-columns:repeat(3,1fr);margin-top:8px}nav a{color:var(--dm);text-decoration:none;text-align:center;font-size:12px;padding:6px 0 2px}
nav a svg{display:block;margin:0 auto 2px;width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}nav a.on{color:var(--ac)}nav a em{font-style:normal;display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--ac);margin-left:4px;vertical-align:1px}
/* overlays */
.ov{position:fixed;inset:0;z-index:9;background:#07080bee;display:flex;align-items:flex-end;justify-content:center}
.sh{width:100%;max-width:480px;background:var(--s1);border:1px solid var(--ln);border-bottom:0;border-radius:20px 20px 0 0;padding:18px 16px calc(16px + env(safe-area-inset-bottom));text-align:left;animation:up .22s ease-out}
.sh h2{margin:0 0 6px;font-size:18px}.sh p{margin:0 0 12px;color:var(--dm);font-size:14px}.sh .st{display:flex;flex-direction:column;gap:8px;margin-top:12px}
@keyframes up{from{transform:translateY(24px);opacity:0}}@keyframes sp{to{transform:rotate(360deg)}}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
#wz{background:var(--bg);align-items:stretch;overflow:auto}.wzi{width:100%;max-width:480px;margin:auto;padding:14px 16px calc(18px + env(safe-area-inset-bottom));display:flex;flex-direction:column;min-height:100%}
.wzh{display:flex;align-items:center;gap:12px}.wzh button{padding:8px 12px;border-radius:99px;font-size:13px}.wzp{flex:1;height:4px;border-radius:2px;background:var(--ln);overflow:hidden}.wzp b{display:block;height:100%;background:var(--ac);transition:width .3s}
#wzb{flex:1;display:flex;flex-direction:column}#wzb h2{font-size:22px;margin:18px 0 6px;letter-spacing:-.01em;text-wrap:balance}#wzb p{color:#c3cad4;margin:0 0 8px;font-size:15px}
#wzb .fig{margin:8px auto;width:min(70vw,230px)}#wzb .fig svg{display:block;width:100%;height:auto}.wzf{margin-top:auto;display:flex;flex-direction:column;gap:8px;padding-top:12px}
.rd{display:flex;justify-content:center;gap:18px;font-size:13px;color:var(--dm);margin:2px 0 6px}.rd b{color:var(--tx);font-weight:600}
.ch3{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.ch3 button.on,.opt.on{border-color:var(--ac);box-shadow:inset 0 0 0 1px var(--ac)}
.opt{display:block;width:100%;text-align:left;margin-top:8px}.opt b{display:block}.opt small{color:var(--dm);font-size:13px}
.tip{background:var(--s1);border:1px solid var(--ln);border-radius:12px;padding:10px 12px;font-size:13px;color:var(--dm);margin-top:6px}
/* update */
.us{display:flex;align-items:center;gap:10px;padding:7px 0;color:#6b7686;flex-wrap:wrap}.us i{width:18px;height:18px;border-radius:50%;border:2px solid #3a4452;flex:none}
.us.now{color:var(--tx)}.us.now i{border-color:var(--ac);border-top-color:transparent;animation:sp .8s linear infinite}.us.ok{color:#9fdcb5}.us.ok i{background:var(--ok);border-color:var(--ok)}.us.bad{color:#ff8a80}.us.bad i{background:var(--bad);border-color:var(--bad)}
.bar{flex-basis:100%;height:8px;background:var(--s2);border-radius:4px;overflow:hidden;margin-left:28px}.bar b{display:block;height:100%;width:0;background:var(--ac);transition:width .4s}
.spin{display:inline-block;width:11px;height:11px;border-radius:50%;border:2px solid var(--ac);border-top-color:transparent;animation:sp .8s linear infinite;vertical-align:-2px}
#um{font-size:13px;color:var(--dm);min-height:34px;margin:10px 0 6px}
</style></head><body>
<div id="up" class="ov" hidden><div class="sh"><h2 id="ut"></h2>
<div class="us" id="u1"><i></i>Restarting into update mode</div>
<div class="us" id="u2"><i></i>Connecting to GitHub</div>
<div class="us" id="u3"><i></i><span>Downloading <span id="upc"></span></span><div class="bar"><b id="ub"></b></div></div>
<div class="us" id="u4"><i></i>Installing and restarting</div>
<div class="us" id="u5"><i></i><span id="u5t">Head running the new build</span></div>
<div class="us" id="u6"><i></i><span id="uj">Joystick box</span></div>
<p id="um"></p><button id="ux" class="bw" hidden>Close</button></div></div>
<div id="shv" class="ov" hidden><div class="sh"><h2 id="sht"></h2><p id="shp"></p><input id="shi" type="text" maxlength="14" hidden><div class="st" id="shb"></div></div></div>
<div id="wz" class="ov" hidden><div class="wzi"><div class="wzh"><button id="wzk" class="gh">Back</button><div class="wzp"><b id="wzbar"></b></div><button id="wzx" class="gh">Close</button></div><div id="wzb"></div></div></div>

<div class="w">
<div class="hd"><b>X70 <i>HEAD</i></b><div class="pl"><span class="pi" id="cO">OBS</span><span class="pi" id="cJ">Joystick</span><span class="pi" id="cW">Wi-Fi</span></div></div>
<div id="hero">Connecting…</div><div id="sub"></div><div id="al"></div>

<section id="t-control">
<div class="c"><div class="ch"><span class="lb">Stage map</span><small id="mh">double-tap to aim</small></div>
<div id="mapx"><svg id="map" viewBox="0 0 320 170" role="img" aria-label="Where the camera is aimed within its travel"></svg>
<div id="mape" hidden><span>The map shows where the camera can aim once its travel limits are set.</span><button class="pr" onclick="wzOpen()">Start guided setup</button></div></div></div>

<div class="c"><div class="ch"><span class="lb">Joystick</span><small id="jh"></small></div>
<div id="pad" class="pad"><div class="kn"></div></div>
<div class="sg" id="feel"><button data-v="0.2">Precise</button><button data-v="0.5">Normal</button><button data-v="1">Fast</button></div></div>

<div class="c"><div class="ch"><span class="lb">Presets</span><small>tap to go · ⋯ to edit</small></div><div id="pr"></div></div>

<div class="c"><button class="sw" id="trk" role="switch"><span><b>Follow the presenter</b><small id="trs"></small></span><span class="tg"></span></button>
<button class="lk" onclick="location.href='/tune'">Tracking settings</button></div>
</section>

<section id="t-setup" hidden>
<div class="c"><div style="display:flex;gap:14px;align-items:center;margin-bottom:6px"><div class="ring" id="ring"><b class="n" id="ringn"></b></div><div><b id="sut" style="font-size:17px"></b><div id="sus" style="color:var(--dm);font-size:13px"></div></div></div>
<div id="cks"></div><button class="pr bw" id="sug" style="margin-top:8px"></button></div>
<div class="c"><div class="ch"><span class="lb">Movement feel</span><small id="recn"></small></div>
<div class="ch3" id="fl3"><button data-f="0">Calm</button><button data-f="1">Balanced</button><button data-f="2">Lively</button></div>
<details style="margin-top:12px"><summary><span style="font-size:13px;color:var(--dm)">Exact numbers</span></summary>
<div class="f"><label>Top speed <b class="n" id="mv"></b></label><input id="max" type="range" min="10" max="300" step="5"><small>The ceiling for everything: joystick, presets and tracking.</small></div>
<div class="f"><label>Joystick speed <b class="n" id="jv"></b></label><input id="jsc" type="range" min="0.05" max="1" step="0.05"><small>How fast the camera turns with the stick fully over.</small></div>
<div class="f"><label>Smoothness <b class="n" id="rv"></b></label><input id="ramp" type="range" min="0.2" max="3" step="0.1"><small>Higher = gentler starts and stops, slower to react.</small></div></details></div>
<div class="c"><details><summary><span class="lb">Advanced calibration</span></summary>
<div class="row" style="margin-top:12px"><button onclick="q('/set?invp=1')">Flip left / right</button><button onclick="q('/set?invt=1')">Flip up / down</button></div>
<div class="row" style="margin-top:8px"><button onclick="ask('Clear the pan limits?','The head turns slowly left and right until both are set again.','Clear',function(){q('/lim?a=p&e=c')})">Clear pan limits</button><button onclick="ask('Clear the tilt limits?','The head tilts slowly until both are set again.','Clear',function(){q('/lim?a=t&e=c')})">Clear tilt limits</button></div>
<div class="f"><label>Gears: motor turns for one camera turn</label><div class="row" style="margin-top:8px;align-items:center"><input id="gp" type="number" step="0.01" inputmode="decimal" aria-label="Pan gear"><input id="gt" type="number" step="0.01" inputmode="decimal" aria-label="Tilt gear"><button onclick="q('/set?gp='+$('gp').value+'&gt='+$('gt').value)">Save</button></div><small>Pan, then tilt. 0 = not known. The guided setup can measure these.</small></div></details></div>
</section>

<section id="t-system" hidden>
<div class="c"><div class="ch"><span class="lb">Health check</span><small id="hls"></small></div><div id="hl"></div></div>
<div class="c"><div class="ch"><span class="lb">Software</span><small id="ck" hidden><i class="spin"></i> asking GitHub…</small></div><dl id="fw"></dl>
<div class="row" style="margin-top:12px"><button onclick="$('ck').hidden=false;q('/fw')">Check</button><button class="pr" id="upb" onclick="upGo()">Update both</button></div>
<button class="sw" id="au" role="switch" style="margin-top:12px"><span><b>Update automatically</b><small>Installs new builds when the head is idle.</small></span><span class="tg"></span></button></div>
<div class="c"><div class="ch"><span class="lb">Network</span></div><dl id="nw"></dl><button class="lk" onclick="location.href='/wifi'">Wi-Fi networks and password</button></div>
<div class="c"><details id="evd"><summary><span class="lb">Recent events</span></summary><pre id="evt">loading…</pre><div class="row"><button onclick="evLoad()">Refresh</button><button onclick="evSel()">Select all to copy</button></div></details></div>
<div class="c"><button class="sw" id="mo" role="switch"><span><b>Motors powered</b><small>Off lets you move the camera by hand. It may sag.</small></span><span class="tg"></span></button></div>
</section>
</div>

<div id="bt"><div><button id="stop" class="dg">STOP</button>
<nav><a href="#control" data-t="control"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/></svg>Control</a>
<a href="#setup" data-t="setup"><svg viewBox="0 0 24 24"><path d="M4 7h10M18 7h2M4 17h2M10 17h10"/><circle cx="16" cy="7" r="2"/><circle cx="8" cy="17" r="2"/></svg><span>Setup<em id="nd" hidden></em></span></a>
<a href="#system" data-t="system"><svg viewBox="0 0 24 24"><path d="M3 12h4l2-6 4 12 2-6h6"/></svg><span>System<em id="nh" hidden></em></span></a></nav></div></div>

<script>
var X=0,Y=0,act=false,busy=false,S={},last=0,idle=0,U=null,W=null,NS='http://www.w3.org/2000/svg';
function $(i){return document.getElementById(i)}
function esc(t){return String(t).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function fx(v,d){return (v<0?'−':'')+Math.abs(v).toFixed(d)}
// ---- talking to the head
function q(u){var c=new AbortController(),t=setTimeout(function(){c.abort()},2500);return fetch(u,{signal:c.signal}).then(function(r){return r.json()}).then(show).catch(function(){offline()}).then(function(){clearTimeout(t)})}
function offline(){$('hero').textContent='No connection to the head';$('hero').className='b';$('sub').textContent='Trying again…';if(U)upDraw(null)}
// ---- joystick pads (the main one and the one inside the guided setup share the stick)
function padOn(p){var k=p.firstElementChild;function mv(e){var r=p.getBoundingClientRect(),x=(e.clientX-r.left)/r.width*2-1,y=1-(e.clientY-r.top)/r.height*2,m=Math.hypot(x,y);if(m>1){x/=m;y/=m}X=x;Y=y;k.style.left=(33+X*33)+'%';k.style.top=(33-Y*33)+'%'}
p.onpointerdown=function(e){act=true;p.setPointerCapture(e.pointerId);mv(e)};p.onpointermove=function(e){if(act)mv(e)};
p.onpointerup=p.onpointercancel=function(){act=false;X=Y=0;k.style.left=k.style.top='33%'}}
padOn($('pad'));
// ---- units: degrees when the gears are known, motor turns when not
function deg(a,v){var g=a?S.gt:S.gp;return g>0?v/g*360:null}
function pos(a,v){var d=deg(a,v);return d==null?fx(v,2)+' turns':fx(d,1)+'°'}
function dps(a,rpm){var g=a?S.gt:S.gp;return g>0?rpm*6/g:null}
// ---- sheets (instead of browser pop-ups)
function sheet(o){$('sht').textContent=o.t;$('shp').textContent=o.p||'';var i=$('shi');i.hidden=o.v==null;if(o.v!=null){i.value=o.v;i.placeholder=o.ph||''}
var b=$('shb');b.innerHTML='';o.b.concat([['Cancel','gh']]).forEach(function(a){var e=document.createElement('button');e.textContent=a[0];e.className=a[1]||'';e.onclick=function(){$('shv').hidden=true;if(a[2])a[2](i.value.trim())};b.appendChild(e)});$('shv').hidden=false}
function ask(t,p,l,f){sheet({t:t,p:p,b:[[l,'pr',f]]})}
$('shv').onclick=function(e){if(e.target==this)this.hidden=true};
// ---- stage map
var M={x0:14,x1:306,y0:12,y1:142},mapOK=false;
function sv(n,a,p){var e=document.createElementNS(NS,n);for(var k in a)e.setAttribute(k,a[k]);if(p)p.appendChild(e);return e}
function mapXY(p,t){var l=S.lv;return[M.x0+(p-l[0])/(l[1]-l[0])*(M.x1-M.x0),M.y1-(t-l[2])/(l[3]-l[2])*(M.y1-M.y0)]}
function mapBuild(){var m=$('map');m.innerHTML='';var g=sv('g',{},m);
for(var i=0;i<=8;i++){var x=M.x0+(M.x1-M.x0)*i/8;sv('line',{x1:x,y1:M.y0,x2:x,y2:M.y1,stroke:i==4?'#2f3743':'#1a1f27'},g)}
for(i=0;i<=4;i++){var y=M.y0+(M.y1-M.y0)*i/4;sv('line',{x1:M.x0,y1:y,x2:M.x1,y2:y,stroke:i==2?'#2f3743':'#1a1f27'},g)}
sv('rect',{x:M.x0,y:M.y0,width:M.x1-M.x0,height:M.y1-M.y0,rx:6,fill:'none',stroke:'#3a4452'},g);
['mL','mC','mR'].forEach(function(id,i){var t=sv('text',{id:id,x:[M.x0,160,M.x1][i],y:160,'text-anchor':['start','middle','end'][i],'font-size':10,fill:'#8e98a7','font-family':'ui-monospace,Menlo,monospace'},m)});
sv('g',{id:'mP'},m);var a=sv('g',{id:'mA'},m);sv('circle',{r:13,fill:'#5cc8ff22',stroke:'#5cc8ff','stroke-width':1.5},a);sv('circle',{r:2.5,fill:'#5cc8ff'},a);
[[-19,0,-9,0],[9,0,19,0],[0,-19,0,-9],[0,9,0,19]].forEach(function(l){sv('line',{x1:l[0],y1:l[1],x2:l[2],y2:l[3],stroke:'#5cc8ff','stroke-width':1.5,'stroke-linecap':'round'},a)})}
mapBuild();
function mapDraw(s){mapOK=!!(s.pl&&s.tl&&s.lv[1]>s.lv[0]&&s.lv[3]>s.lv[2]);$('mape').hidden=mapOK;$('map').style.opacity=mapOK?1:.25;$('mh').hidden=!mapOK;$('mA').style.display=mapOK?'':'none';if(!mapOK)return;
var c=mapXY(s.pt,s.tt);$('mA').setAttribute('transform','translate('+c[0].toFixed(1)+','+c[1].toFixed(1)+')');
$('mL').textContent='◂ '+pos(0,s.lv[0]);$('mR').textContent=pos(0,s.lv[1])+' ▸';$('mC').textContent='tilt '+pos(1,s.lv[2])+' to '+pos(1,s.lv[3]);
var g=$('mP');g.innerHTML='';for(var i=0;i<6;i++)if(s.ps[i]){var p=mapXY(s.pp[i][0],s.pp[i][1]);p[0]=Math.max(M.x0,Math.min(M.x1,p[0]));p[1]=Math.max(M.y0,Math.min(M.y1,p[1]));
sv('circle',{cx:p[0],cy:p[1],r:9,fill:'#161a21',stroke:'#ffb347','stroke-width':1.5},g);var t=sv('text',{x:p[0],y:p[1]+3.5,'text-anchor':'middle','font-size':10,'font-weight':700,fill:'#ffb347'},g);t.textContent=i+1}}
var tapT=0,tapX=0,tapY=0;$('map').addEventListener('pointerup',function(e){if(!mapOK)return;var n=Date.now(),r=this.getBoundingClientRect();
if(n-tapT<400&&Math.hypot(e.clientX-tapX,e.clientY-tapY)<30){tapT=0;var x=(e.clientX-r.left)/r.width*320,y=(e.clientY-r.top)/r.height*170,l=S.lv;
var p=l[0]+(Math.max(M.x0,Math.min(M.x1,x))-M.x0)/(M.x1-M.x0)*(l[1]-l[0]),t=l[2]+(M.y1-Math.max(M.y0,Math.min(M.y1,y)))/(M.y1-M.y0)*(l[3]-l[2]);q('/go?p='+p.toFixed(3)+'&t='+t.toFixed(3))}else{tapT=n;tapX=e.clientX;tapY=e.clientY}});
// ---- presets
for(var i=0;i<6;i++){var b=document.createElement('button');b.id='p'+i;b.className='p';b.innerHTML='<u></u><s></s><i>⋯</i>';b.onclick=(function(n){return function(e){if(!S.ps)return;if(!S.ps[n]||e.target.tagName=='I')pEdit(n);else q('/p?n='+n)}})(i);$('pr').appendChild(b)}
function pName(n){return S.pn&&S.pn[n]?S.pn[n]:'Preset '+(n+1)}
function pEdit(n){var set=S.ps[n],sv=function(v){q('/p?n='+n+'&save=1&name='+encodeURIComponent(v))};
if(!set)sheet({t:'Save preset '+(n+1),p:'The camera will come back to exactly where it points now. Give it a name you will recognise.',v:'',ph:'e.g. Pulpit, Choir, Wide',b:[['Save this aim','pr',sv]]});
else sheet({t:pName(n),p:'Change the name here, then choose what to do.',v:S.pn[n]||'',ph:'Name',b:[['Rename','',function(v){q('/p?n='+n+'&name='+encodeURIComponent(v))}],['Replace with where the camera points now','pr',sv],['Delete this preset','',function(){q('/p?n='+n+'&clear=1')}]]})}
// ---- recommended movement feel, worked out from the gears
var FEEL=[['Calm',40,12,1.4],['Balanced',60,20,1.0],['Lively',90,35,0.6]];
function feel(i){var f=FEEL[i],g=S.gp;if(!(g>0))return null;var rpm=Math.max(10,Math.min(300,Math.round(f[1]*g/6/5)*5)),top=rpm*6/g;return{max:rpm,jsc:Math.max(.05,Math.min(1,Math.round(f[2]/top*20)/20)),ramp:f[3]}}
function feelNow(){for(var i=0;i<3;i++){var f=feel(i);if(f&&f.max==S.max&&Math.abs(f.jsc-S.jsc)<.03&&Math.abs(f.ramp-S.ramp)<.06)return i}return -1}
function feelSet(i){var f=feel(i);if(!f){ask('The gears are not known yet','The head needs them to work out speeds in degrees per second. The guided setup measures them in a minute.','Open guided setup',function(){wzOpen(2)});return}
q('/set?max='+f.max+'&jsc='+f.jsc+'&ramp='+f.ramp+'&sd='+(S.sd|4))}
[].forEach.call($('fl3').children,function(b){b.onclick=function(){feelSet(+b.dataset.f)}});
[].forEach.call($('feel').children,function(b){b.onclick=function(){q('/set?jsc='+b.dataset.v)}});
// ---- setup checklist
function steps(s){var sp=s.lm[0]&&s.lm[1]?pos(0,s.lv[0])+' to '+pos(0,s.lv[1]):s.lm[0]||s.lm[1]?'one end set':'not set: pan stays slow',st=s.lm[2]&&s.lm[3]?pos(1,s.lv[2])+' to '+pos(1,s.lv[3]):s.lm[2]||s.lm[3]?'one end set':'not set: tilt stays slow';
return[['Directions','Right is right, up is up',!!(s.sd&1)],['Centre','Where straight ahead is',!!(s.sd&2)],['Gears',s.gp>0&&s.gt>0?s.gp.toFixed(2)+' pan, '+s.gt.toFixed(2)+' tilt motor turns per camera turn':'not known: angles and speeds cannot be shown',s.gp>0&&s.gt>0],
['Left and right limits',sp,!!s.pl],['Up and down limits',st,!!s.tl],['Movement feel',s.sd&4?'chosen':'using the factory values',!!(s.sd&4)]]}
function setupDraw(s){var st=steps(s),n=st.filter(function(x){return x[2]}).length,h='';st.forEach(function(x,i){h+='<button class="ck'+(x[2]?' d':'')+'" onclick="wzOpen('+i+')"><i>'+(x[2]?'✓':i+1)+'</i><span>'+x[0]+'<small>'+esc(x[1])+'</small></span></button>'});
if($('cks').dataset.h!=h){$('cks').innerHTML=h;$('cks').dataset.h=h}$('ring').style.setProperty('--p',n/6*100);$('ringn').textContent=n+'/6';
$('sut').textContent=n==6?'Setup complete':n?'Setup: '+(6-n)+' step'+(n==5?'':'s')+' left':'Let’s set up the head';$('sus').textContent=n==6?'Tap any line to redo it.':'About five minutes. The head stays slow and safe until its limits are set.';
$('sug').textContent=n==6?'Run the guided setup again':n?'Continue':'Start guided setup';$('sug').onclick=function(){var k=0;for(var i=0;i<6;i++)if(!st[i][2]){k=i;break}wzOpen(n==6?0:k)};$('nd').hidden=n==6;return n}
// ---- health check
function health(s){var h=[],g=s.rssi>=-60?'strong':s.rssi>=-70?'good':s.rssi>=-80?'weak':'very weak';
h.push(s.dp&&s.dt?['ok','Motor drivers answering']:['bd',(s.dp?'Tilt':s.dt?'Pan':'Both')+' motor driver not answering','Check the driver board is seated and powered.']);
h.push(s.on?['ok','Motors powered and holding']:['wn','Motors are switched off','Turn them on at the bottom of this page.']);
h.push(s.pl&&s.tl?['ok','Travel limits set']:['wn','Travel limits not set','The head is held to a slow, safe speed. Setup tab.']);
h.push(s.gp>0&&s.gt>0?['ok','Gears known']:['wn','Gears not known','Tracking and presets cannot use real angles. Setup tab.']);
h.push(s.rssi>=-70?['ok','Wi-Fi signal '+g]:['wn','Wi-Fi signal '+g,'Move the access point closer or use the second saved network.']);
h.push(s.jon?['ok','Joystick box connected (build '+s.jfw+')']:['wn','Joystick box not seen','Fine if you are not using it.']);
h.push(s.obs?['ok','OBS connected']:['wn','OBS is not connected','Open OBS and check the Face Tracker connection.']);
h.push(s.blk>=30?['ok','Memory healthy ('+s.heap+' KB free)']:['wn','Memory is low ('+s.heap+' KB free, largest piece '+s.blk+' KB)','Restart the head before a service.']);
h.push(s.lat>s.fw&&s.fw>0?['wn','Update available: build '+s.lat,'Software, below.']:['ok',s.lat<0?'Running build '+s.fw+' (newest not checked yet)':'Software up to date (build '+s.fw+')']);return h}
// ---- everything on the page follows the head's status
function show(s){S=s;var mv=s.src;
var H=s.ust>0?['Updating…','']:!s.on?['Motors are off','b']:mv==1?['You are driving','j']:mv==2?(s.trk&&s.obs?['Following the presenter','t']:['Moving to a preset','']):mv==3?['Tracking is blocked','']:['Holding still',''];
$('hero').textContent=H[0];$('hero').className=H[1];
$('sub').innerHTML='<span class="n">Pan '+pos(0,s.pt)+' · Tilt '+pos(1,s.tt)+'</span>'+(mv==3?' · OBS is sending moves, ignored':s.trk&&s.obs&&mv==0?' · tracking is standing by':'');
$('cO').className='pi '+(s.obs?'ok':'');$('cJ').className='pi '+(s.jon?'ok':'');$('cW').className='pi '+(s.rssi>=-70?'ok':'wn');
var a='';if(!s.dp||!s.dt)a+='<button class="a r" onclick="go(\'system\')"><span>'+(s.dp?'Tilt':s.dt?'Pan':'Both')+' motor driver'+(s.dp||s.dt?' is':'s are')+' not answering.</span><em>Details</em></button>';
if(s.lat>s.fw&&s.fw>0)a+='<button class="a" onclick="go(\'system\')"><span>Build '+s.lat+' is ready to install.</span><em>Update</em></button>';
if($('al').dataset.h!=a){$('al').innerHTML=a;$('al').dataset.h=a}
mapDraw(s);
var top=dps(0,s.max);$('jh').textContent=top?'full stick = '+(top*s.jsc).toFixed(0)+'° per second':'';
[].forEach.call($('feel').children,function(b){b.className=Math.abs(b.dataset.v-s.jsc)<.03?'on':''});
for(var i=0;i<6;i++){var b=$('p'+i),at=s.ps[i]&&Math.abs(s.pp[i][0]-s.pt)<.02&&Math.abs(s.pp[i][1]-s.tt)<.02;b.className='p'+(s.ps[i]?(at?' at':''):' e');b.children[0].textContent=i+1;b.children[1].textContent=s.ps[i]?pName(i):'+ Save here';b.children[2].hidden=!s.ps[i]}
$('trk').setAttribute('aria-checked',!!s.trk);$('trs').textContent=!s.trk?'Blocked: the head ignores OBS. Joystick and presets still work.':s.obs?'Allowed: OBS face tracking can move the head.':'Allowed, but OBS is not connected.';
var n=setupDraw(s),f=feelNow();[].forEach.call($('fl3').children,function(b,i){b.className=i==f?'on':''});$('recn').textContent=s.gp>0?(f<0?'custom':'worked out for your gears'):'needs the gears';
function sl(id,v){if(document.activeElement!=$(id))$(id).value=v}sl('max',s.max);sl('jsc',s.jsc);sl('ramp',s.ramp);
var tp=dps(0,s.max),tt=dps(1,s.max);$('mv').textContent=tp?tp.toFixed(0)+'°/s pan'+(tt?', '+tt.toFixed(0)+'°/s tilt':''):s.max+' RPM at the motor';
$('jv').textContent=tp?(tp*s.jsc).toFixed(0)+'°/s':Math.round(s.jsc*100)+' % of top speed';$('rv').textContent='full speed in '+(s.ramp*2).toFixed(1)+' s';
if(document.activeElement!=$('gp')&&document.activeElement!=$('gt')){$('gp').value=s.gp;$('gt').value=s.gt}
var h=health(s),bad=h.filter(function(x){return x[0]!='ok'}).length,hh='';h.forEach(function(x){hh+='<div class="hl '+x[0]+'"><span>'+esc(x[1])+(x[2]?'<small>'+esc(x[2])+'</small>':'')+'</span></div>'});
if($('hl').dataset.h!=hh){$('hl').innerHTML=hh;$('hl').dataset.h=hh}$('hls').textContent=bad?bad+' to look at':'all good';$('nh').hidden=!(s.lat>s.fw&&s.fw>0)&&s.dp&&s.dt;
var up=s.up||0;$('fw').innerHTML='<dt>Head</dt><dd class="n">build '+s.fw+'</dd><dt>Joystick box</dt><dd class="n">'+(s.jon?'build '+s.jfw:'not seen')+'</dd><dt>Newest on GitHub</dt><dd class="n">'+(s.lat<0?'not checked yet':'build '+s.lat)+'</dd>'+(s.msg?'<dt>Note</dt><dd>'+esc(s.msg)+'</dd>':'');
$('nw').innerHTML='<dt>Head address</dt><dd class="n">'+esc(s.ip)+'</dd><dt>Joystick box</dt><dd class="n">'+(s.jip?esc(s.jip):'not seen')+'</dd><dt>Wi-Fi signal</dt><dd>'+(s.rssi>=-60?'strong':s.rssi>=-70?'good':s.rssi>=-80?'weak':'very weak')+'</dd><dt>Running for</dt><dd class="n">'+Math.floor(up/3600)+' h '+Math.floor(up/60%60)+' min</dd>';
$('ck').hidden=!s.chk;$('au').setAttribute('aria-checked',!!s.auto);$('mo').setAttribute('aria-checked',!!s.on);
if(!U&&s.ust>0)U={from:s.fw,target:0,jon:!!s.jon,t0:Date.now(),step:2,end:''};if(U)upDraw(s);if(W)wzLive()}
// ---- tabs
function go(t){location.hash=t}
function tab(){var t=(location.hash||'#control').slice(1);if(!$('t-'+t))t='control';['control','setup','system'].forEach(function(k){$('t-'+k).hidden=k!=t});[].forEach.call(document.querySelectorAll('nav a'),function(a){a.className=a.dataset.t==t?'on':''});scrollTo(0,0)}
addEventListener('hashchange',tab);tab();
// ---- guided setup
var WZ=['dir','ctr','gear','pan','tilt','feel','done'],SVGH='<svg viewBox="0 0 200 200">';
function arc(k,a,b,r){var d='';for(var i=0;i<=40;i++){var p=pt(k,a+(b-a)*i/40,r);d+=(i?'L':'M')+p[0].toFixed(1)+' '+p[1].toFixed(1)}return d}
function pt(k,a,r){a=a*Math.PI/180;return k?[100+r*Math.cos(a),100-r*Math.sin(a)]:[100+r*Math.sin(a),100-r*Math.cos(a)]}
// the dial: looking down on the head for pan, from the side for tilt. Without gears the angles are a sketch.
function dialDeg(k,v){var d=deg(k,v);if(d!=null)return d;var l=S.lv,m=Math.max(1.5,Math.abs(k?S.tt:S.pt),Math.abs(l[k*2]),Math.abs(l[k*2+1]));return v*(k?60:120)/m}
function dial(k){var s=SVGH+'<circle cx="100" cy="100" r="82" fill="none" stroke="#2a313c" stroke-width="1"/>';for(var a=0;a<360;a+=15){var p=pt(k,a,82),p2=pt(k,a,a%90?77:72);s+='<line x1="'+p[0]+'" y1="'+p[1]+'" x2="'+p2[0]+'" y2="'+p2[1]+'" stroke="#3a4452"/>'}
s+='<path id="dA" fill="none" stroke="#4cd08a" stroke-width="7" stroke-linecap="round" opacity=".9"/><g id="dL"></g><g id="dN"><path d="'+(k?'M100 100L172 100':'M100 100L100 28')+'" stroke="#5cc8ff" stroke-width="2" stroke-dasharray="3 4"/>'+
(k?'<rect x="78" y="86" width="40" height="28" rx="5" fill="#1e232c" stroke="#eef1f5" stroke-width="1.5"/><rect x="118" y="91" width="16" height="18" rx="2" fill="#ffb347"/>':'<rect x="86" y="82" width="28" height="40" rx="5" fill="#1e232c" stroke="#eef1f5" stroke-width="1.5"/><rect x="91" y="66" width="18" height="16" rx="2" fill="#ffb347"/>')+'</g>'+
'<text x="100" y="'+(k?196:14)+'" text-anchor="middle" font-size="10" fill="#8e98a7">'+(k?'side view':'stage')+'</text></svg>';return s}
function dialDraw(k){if(!$('dN'))return;var v=k?S.tt:S.pt,a=dialDeg(k,v),lm=S.lm,lv=S.lv,lo=dialDeg(k,lv[k*2]),hi=dialDeg(k,lv[k*2+1]),hl=lm[k*2],hh=lm[k*2+1];
$('dN').setAttribute('transform','rotate('+(k?-a:a).toFixed(1)+' 100 100)');var f=hl?lo:Math.min(a,hh?hi:a),t=hh?hi:Math.max(a,hl?lo:a);
$('dA').setAttribute('d',t-f>.5?arc(k,f,t,82):'');$('dA').setAttribute('stroke',hl&&hh?'#4cd08a':'#ffb347');var g='';[[hl,lo],[hh,hi]].forEach(function(e){if(e[0]){var p=pt(k,e[1],82);g+='<circle cx="'+p[0].toFixed(1)+'" cy="'+p[1].toFixed(1)+'" r="6" fill="#4cd08a" stroke="#0d0f13" stroke-width="2"/>'}});$('dL').innerHTML=g;
if($('dR'))$('dR').innerHTML='<span>now <b class="n">'+pos(k,v)+'</b></span>'+(hl&&hh?'<span>travel <b class="n">'+pos(k,lv[k*2+1]-lv[k*2]).replace('−','')+'</b></span>':'')}
function wzOpen(i){W={i:i||0,s:0};$('wz').hidden=false;wzDraw()}
function wzGo(d){W.i=Math.max(0,Math.min(6,W.i+d));W.s=0;W.m=null;wzDraw()}
$('wzx').onclick=function(){X=Y=0;W=null;$('wz').hidden=true};$('wzk').onclick=function(){if(W.s>0){W.s--;wzDraw()}else wzGo(-1)};
function wzSet(h,f){$('wzb').innerHTML=h+'<div class="wzf">'+f+'</div>';var p=$('wzb').querySelector('.pad');if(p)padOn(p);$('wzbar').style.width=(W.i/6*100)+'%';$('wzk').style.visibility=W.i||W.s?'visible':'hidden';$('wz').scrollTop=0;wzLive()}
var PAD='<div class="pad"><div class="kn"></div></div>',NX='<button class="pr" onclick="wzGo(1)">',SK='<button class="gh" onclick="wzGo(1)">Skip for now</button>';
function wzDraw(){var k=WZ[W.i],s=S;
if(k=='dir'){var up=W.s==1;wzSet('<span class="lb">Step 1 of 6 · Directions</span><h2>Push the stick '+(up?'UP':'to the RIGHT')+' and watch the camera.</h2><p>Did the camera '+(up?'tilt up':'turn to its right, as seen from behind it')+'?</p>'+PAD,
'<button class="pr" onclick="wzDir(0)">Yes, it went '+(up?'up':'right')+'</button><button onclick="wzDir(1)">No, it went '+(up?'down':'left')+': fix it</button>')}
if(k=='ctr')wzSet('<span class="lb">Step 2 of 6 · Centre</span><h2>Aim at the middle of the stage, level.</h2><p>This becomes “straight ahead”. Angles are counted from here, and presets you already saved keep their real places.</p>'+PAD+'<div class="rd" id="dR0"></div>',
'<button class="pr" onclick="q(\'/zero\').then(function(){return q(\'/set?sd=\'+(S.sd|2))}).then(function(){wzGo(1)})">This is centre</button>'+SK);
if(k=='pan'||k=='tilt'){var a=k=='pan'?0:1,n=a?['DOWN','UP','d','u','down','up']:['LEFT','RIGHT','l','r','left','right'],h0=s.lm[a*2],h1=s.lm[a*2+1],e=W.s;
if(e==0&&h0&&h1&&!W.re){wzSet('<span class="lb">Step '+(4+a)+' of 6 · '+(a?'Up and down':'Left and right')+' limits</span><h2>These limits are set.</h2><p>The head slows down by itself as it gets near them and never drives past.</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>',
NX+'Keep them</button><button onclick="q(\'/lim?a='+(a?'t':'p')+'&e=c\').then(function(){W.re=1;wzDraw()})">Set them again</button>');return}
if(e>1){W.re=0;wzSet('<span class="lb">Step '+(4+a)+' of 6</span><h2>'+(a?'Tilt':'Pan')+' limits saved.</h2><p>Try it: push the stick all the way each side. The head eases to a stop at each end.</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>'+PAD,NX+'Next</button>');return}
wzSet('<span class="lb">Step '+(4+a)+' of 6 · '+(a?'Up and down':'Left and right')+' limits</span><h2>Drive '+n[e]+' to the furthest safe point.</h2><p>Go slowly. Stop before a cable pulls tight or the camera touches anything'+(a?'.':', then leave a finger of slack.')+'</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>'+PAD,
'<button class="pr" onclick="q(\'/lim?a='+(a?'t':'p')+'&e='+n[e+2]+'\').then(function(){W.s++;wzDraw()})">Set the '+n[e+4]+' limit here</button>'+(e==0?SK:''))}
if(k=='gear'){var m=W.m;if(!m){wzSet('<span class="lb">Step 3 of 6 · Gears</span><h2>How far does the camera turn for each motor turn?</h2><p>With this the head knows real angles, so speeds read in degrees per second and tracking is calibrated.</p>'+
'<button class="opt" onclick="W.m={a:0,g:90};wzDraw()"><b>Measure it for me</b><small>Turn the camera through a known angle. The head counts the motor turns and does the maths. About a minute per axis.</small></button>'+
'<button class="opt" onclick="W.m={k:1};wzDraw()"><b>I know the numbers</b><small>Type the motor turns per camera turn for pan and tilt.</small></button>'+(s.gp>0&&s.gt>0?'<div class="tip">Saved now: pan <b class="n">'+s.gp.toFixed(2)+'</b>, tilt <b class="n">'+s.gt.toFixed(2)+'</b></div>':''),(s.gp>0&&s.gt>0?NX+'Keep these</button>':SK));return}
if(m.k){wzSet('<span class="lb">Step 3 of 6 · Gears</span><h2>Motor turns for one full camera turn</h2><p>Pan, then tilt.</p><div class="row"><input id="wgp" type="number" step="0.01" inputmode="decimal" value="'+(s.gp||'')+'" placeholder="Pan"><input id="wgt" type="number" step="0.01" inputmode="decimal" value="'+(s.gt||'')+'" placeholder="Tilt"></div>',
'<button class="pr" onclick="q(\'/set?gp=\'+($(\'wgp\').value||0)+\'&gt=\'+($(\'wgt\').value||0)).then(function(){wzGo(1)})">Save</button><button class="gh" onclick="W.m=null;wzDraw()">Measure instead</button>');return}
var ax=m.a,nm=ax?'tilt':'pan',ch=ax?[20,30,45]:[45,90,180],cs='';ch.forEach(function(c){cs+='<button class="'+(m.g==c?'on':'')+'" onclick="W.m.g='+c+';wzDraw()">'+c+'°</button>'});
if(m.r!=null){wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2 class="n">'+m.r.toFixed(2)+'</h2><p>motor turns for one full camera '+(ax?'tilt circle':'turn')+'. '+(m.r<2||m.r>400?'That looks unlikely. Measure again, moving exactly '+m.g+'°.':'Measured from '+Math.abs(m.d).toFixed(2)+' motor turns over '+m.g+'°.')+'</p>',
'<button class="pr" onclick="wzGearSave()">Save and '+(ax?'finish':'measure tilt')+'</button><button onclick="W.m={a:'+ax+',g:'+m.g+'};wzDraw()">Measure again</button>');return}
if(m.p0==null)wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2>Aim the camera at a starting mark.</h2><p>'+(ax?'Level is easiest: a phone’s level app laid on top of the camera shows 0°.':'Pick something easy to line up with, like the edge of a door, and centre it in the viewfinder.')+'</p><p>Then choose how far you will turn it:</p><div class="ch3">'+cs+'</div>'+PAD,
'<button class="pr" onclick="W.m.p0='+(ax?'S.tt':'S.pt')+';wzDraw()">Start measuring</button><button class="gh" onclick="'+(ax?'wzGo(1)':'W.m={a:1,g:30};wzDraw()')+'">Skip '+nm+'</button>');
else wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2>Now '+(ax?'tilt':'turn')+' the camera exactly '+m.g+'°.</h2><p>'+(ax?'Watch the phone’s level app on top of the camera until it reads '+m.g+'°.':m.g==90?'A quarter turn: from facing the stage to facing the side wall squarely.':m.g==180?'Half a turn: until it faces exactly backwards.':'Half of a quarter turn: from square-on to the corner of a square room.')+'</p>'+PAD+'<div class="rd"><span>motor turns so far <b class="n" id="gN">0.00</b></span></div>',
'<button class="pr" onclick="wzGearDone()">Done, it has turned '+m.g+'°</button><button class="gh" onclick="W.m.p0=null;wzDraw()">Start over</button>')}
if(k=='feel'){var o='',cur=feelNow();if(!(s.gp>0)){wzSet('<span class="lb">Step 6 of 6 · Movement feel</span><h2>The gears are needed for this step.</h2><p>Without them the head cannot work out speeds in degrees per second. You can still set speeds by hand on the Setup tab.</p>','<button class="pr" onclick="W.i=2;W.s=0;W.m=null;wzDraw()">Go to gears</button>'+SK);return}
FEEL.forEach(function(f,i){var v=feel(i);o+='<button class="opt'+(i==cur?' on':'')+'" onclick="feelSet('+i+');setTimeout(wzDraw,400)"><b>'+f[0]+(i==1?' · recommended':'')+'</b><small>'+['Slow, graceful moves for a seated sermon and a tight shot.','A natural pace for most services.','Quick moves for an active stage and a wide shot.'][i]+' Joystick up to '+(v.max*6/s.gp*v.jsc).toFixed(0)+'°/s, top speed '+(v.max*6/s.gp).toFixed(0)+'°/s.</small></button>'});
wzSet('<span class="lb">Step 6 of 6 · Movement feel</span><h2>How should the camera move?</h2><p>Worked out for your gears. Pick one and try it on the stick; change it any time.</p>'+o+PAD,NX+(cur<0?'Keep my own numbers':'Finish')+'</button>')}
if(k=='done'){var st=steps(s),n=st.filter(function(x){return x[2]}).length,l='';st.forEach(function(x){l+='<div class="hl '+(x[2]?'':'wn')+'"><span>'+x[0]+'<small>'+esc(x[1])+'</small></span></div>'});
wzSet('<h2>'+(n==6?'The head is ready.':'Saved. '+(6-n)+' step'+(n==5?'':'s')+' still open.')+'</h2><p>'+(n==6?'It knows its directions, its centre, how far it may travel and how fast to move. Save a few presets next: aim, then tap an empty preset.':'You can finish the open ones any time from the Setup tab.')+'</p><div class="c" style="margin-top:8px">'+l+'</div>','<button class="pr" onclick="$(\'wzx\').onclick();go(\'control\')">Go to the controls</button>')}}
function wzDir(flip){var up=W.s==1,d=function(){if(up)q('/set?sd='+(S.sd|1)).then(function(){wzGo(1)});else{W.s=1;wzDraw()}};if(flip)q('/set?inv'+(up?'t':'p')+'=1').then(function(){ask('Fixed. Try it again.','Push the stick '+(up?'up':'right')+' once more to check.','OK',function(){})});else d()}
function wzGearDone(){var m=W.m,d=(m.a?S.tt:S.pt)-m.p0;if(Math.abs(d)<.15){ask('The motor has hardly moved','Use the stick on this screen to turn the camera '+m.g+'° from the starting mark, then tap Done.','OK',function(){});return}m.d=d;m.r=Math.abs(d)*360/m.g;wzDraw()}
function wzGearSave(){var m=W.m;q('/set?'+(m.a?'gt':'gp')+'='+m.r.toFixed(2)).then(function(){if(m.a)wzGo(1);else{W.m={a:1,g:30};wzDraw()}})}
function wzLive(){if(!W||!S.lm)return;var k=WZ[W.i];if(k=='pan')dialDraw(0);if(k=='tilt')dialDraw(1);if(k=='ctr'&&$('dR0'))$('dR0').innerHTML='<span>pan <b class="n">'+pos(0,S.pt)+'</b></span><span>tilt <b class="n">'+pos(1,S.tt)+'</b></span>';
if(k=='gear'&&$('gN')&&W.m&&W.m.p0!=null)$('gN').textContent=Math.abs((W.m.a?S.tt:S.pt)-W.m.p0).toFixed(2)}
// ---- live update progress
try{U=JSON.parse(sessionStorage.x70up||'null')}catch(e){}if(U&&Date.now()-U.t0>600000)U=null;
function upGo(){var f=function(){U={from:S.fw||0,target:S.lat||0,jon:!!S.jon,t0:Date.now(),step:1,end:''};upDraw(S);q('/fwgo')};
if(S.lat>0&&S.fw>=S.lat&&(!S.jon||S.jfw>=S.lat))ask('Already on the newest build','Both the head and the joystick box are up to date. Install it again anyway?','Install again',f);else f()}
function upDraw(s){var n=Date.now();
if(s&&!U.end){if(s.ust==1)U.step=Math.max(U.step,2);else if(s.ust==2){U.step=3;U.pct=s.upc}else if(s.ust==3)U.step=4;else if(s.ust==4){U.end='bad';U.msg='The download failed 5 times. The head is going back to the build it had.'}
else if(s.fw>U.from||(U.step>=2&&U.target&&s.fw>=U.target)){U.step=5;U.end='ok';U.fw=s.fw}
else if(U.step>=2||n-U.t0>30000){U.end='bad';U.msg=s.msg||'The head came back on the same build.'}}
if(!s&&!U.end&&U.step==3&&U.pct>=95)U.step=4;if(s&&U.end=='bad'&&s.ust==0&&s.msg)U.msg=s.msg;
for(var i=1;i<=5;i++)$('u'+i).className='us '+(U.end=='ok'||i<U.step?'ok':i==U.step?(U.end=='bad'?'bad':'now'):'');
$('upc').textContent=U.step==3?(U.pct||0)+' %':'';if(s&&s.ust==1&&s.utry>1)$('um').dataset.t='Try '+s.utry+' of 5. ';else if(s)$('um').dataset.t='';$('ub').style.width=(U.step>3||U.end=='ok'?100:U.step==3?(U.pct||0):0)+'%';
if(U.end=='ok')$('u5t').textContent='Head running build '+U.fw;
var j='';if(s){if(!U.jon&&!s.jon){j='';$('uj').textContent='Joystick box: not switched on'}else if(s.jon&&(U.target||U.fw)&&s.jfw>=(U.target||U.fw)){j='ok';$('uj').textContent='Joystick box running build '+s.jfw}else if(U.end!='ok'){j='';$('uj').textContent='Joystick box: next, after the head'}else{j=n-U.t0>360000?'bad':'now';$('uj').textContent=s.jon?'Joystick box: on build '+s.jfw+', updating now':'Joystick box: restarting'}$('u6').className='us '+j}
$('ut').textContent=U.end=='ok'?'Update finished':U.end=='bad'?'Update did not finish':'Updating the head';
$('um').textContent=U.end=='ok'?'':U.end=='bad'?U.msg:!s?'The head is restarting. This page reconnects by itself.':n-U.t0>180000?'This is taking longer than normal. Leave everything powered.':($('um').dataset.t||'')+'About a minute for the head, then the joystick box. Nothing else is needed.';
$('ux').hidden=!U.end;if(U.end=='ok'&&$('u6').className=='us now'){$('ut').textContent='Head done, joystick box next';$('um').textContent='The joystick box updates by itself now. You can close this.'}$('up').hidden=false;try{sessionStorage.x70up=JSON.stringify(U)}catch(e){}}
$('ux').onclick=function(){U=null;$('up').hidden=true;try{sessionStorage.removeItem('x70up')}catch(e){}};
// ---- the rest of the controls
function evLoad(){fetch('/log').then(function(r){return r.text()}).then(function(t){$('evt').textContent=t}).catch(function(){$('evt').textContent='could not load'})}
function evSel(){var r=document.createRange();r.selectNodeContents($('evt'));var s=getSelection();s.removeAllRanges();s.addRange(r)}
$('evd').ontoggle=function(){if(this.open)evLoad()};
$('stop').onclick=function(){X=Y=0;q('/stop')};$('trk').onclick=function(){q('/trk')};$('au').onclick=function(){q('/set?auto=1')};
$('mo').onclick=function(){if(S.on)ask('Switch the motors off?','The camera is no longer held and may sag forward. Hold it if it is front-heavy.','Switch off',function(){q('/en?on=0')});else q('/en?on=1')};
$('max').onchange=function(){q('/set?max='+this.value)};$('jsc').onchange=function(){q('/set?jsc='+this.value)};$('ramp').onchange=function(){q('/set?ramp='+this.value)};
setInterval(function(){if(busy)return;var mvg=act||X||Y;if(mvg||last){busy=true;last=mvg?1:0;fetch('/j?x='+X.toFixed(3)+'&y='+Y.toFixed(3)).then(function(r){return r.json()}).then(show).catch(function(){}).then(function(){busy=false})}
else if(++idle>=(W?3:10)){idle=0;busy=true;q('/s').then(function(){busy=false})}},100);
</script></body></html>)HTML";

const char TUNE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>X70: adjust face tracking</title>
<style>
body{margin:0;background:#0d0f13;color:#e8ecf1;font:15px -apple-system,Segoe UI,Roboto,sans-serif}
.w{max-width:440px;margin:auto;padding:12px}a{color:#ffb347}
button{background:#1e232c;color:#e8ecf1;border:1px solid #2a313c;border-radius:10px;padding:12px 8px;font-size:15px;margin:3px 0;width:100%}
button:active{background:#2a313c}.on{background:#1c6b43}.sv{background:#8a6d1a}
h3{margin:18px 0 4px;font-size:13px;color:#8e98a7;text-transform:uppercase;letter-spacing:.08em}
label{display:block;margin:12px 0 2px;font-size:13px;color:#cfd6df}label b{float:right;color:#e8ecf1}
input[type=range]{width:100%}small{color:#8e98a7;display:block}
input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;background:#161a21;color:#e8ecf1;border:1px solid #2a313c;border-radius:8px;padding:9px;font-size:16px;margin:2px 0 6px}
.warn{color:#f2a93b;font-size:13px}.ok{color:#6fcf97}.hi{color:#f2a93b}.g3{display:grid;grid-template-columns:1fr;gap:0}details{margin-top:14px;background:#161a21;border-radius:10px;padding:8px 12px}summary{padding:6px 0}label i{font-style:normal;font-size:12px;margin-left:6px}#os{font-size:13px;color:#8e98a7;margin:6px 0}
</style></head><body><div class="w">
<a href="/">&larr; Head controls</a>
<h3>Head (changes apply at once)</h3>
<small id="adr"></small><div id="hw" class="warn"></div>
<button id="trk"></button>
<label>Gentleness of tracking moves <b id="v_tsc"></b></label><input id="tsc" type="range" min="0.05" max="1" step="0.05">
<small>Left = slower, finer moves. Right = faster, coarser. Good: 0.15 to 0.30. The joystick is not affected.</small>
<label>Soft start and stop <b id="v_tramp"></b></label><input id="tramp" type="range" min="0.2" max="2" step="0.1">
<small>How long a tracking move takes to speed up and slow down. Good: 0.4 to 0.8 s. Longer looks smoother but reacts later.</small>
<h3>OBS Face Tracker PTZ (live)</h3>
<div id="cn">
<small>In OBS: Tools &rarr; WebSocket Server Settings &rarr; enable, and copy the password. Stored only in this browser.</small>
PC address<input id="ip" type="text" placeholder="192.168.1.50" autocapitalize="off" autocomplete="off">
Port<input id="port" type="number" value="4455">
Password<input id="pw" type="password" autocomplete="off">
<button id="go">Connect to OBS</button></div>
<div id="os">Not connected to OBS.</div>
<div id="ob" hidden>
<button id="en"></button>
<h3>1. Start here</h3>
<select id="shot" hidden><option value="11" selected>Full-body shot</option></select>
<button id="rec">Load the recommended settings</button>
<h3>Who to follow</h3>
<button id="lk" class="on">LOCK on the person at the yellow cross</button>
<small>Aim the camera with the joystick so the person is on the yellow cross, then tap. The camera stays with that person and never picks anybody else by itself.</small>
<button id="au2">Follow automatically again (the biggest person)</button>
<div id="fm" class="warn"></div>
<h3>2. How the camera behaves</h3><div id="ch"></div><div id="sl" hidden></div>
<details><summary>Exact numbers (used when a choice above is Custom)</summary><div id="sl2"></div>
<button id="ix"></button><button id="iy"></button></details>
</div></div>
<script>
var $=function(i){return document.getElementById(i)},H={},ws=null,rid=0,cb={},SRC='',FLT='',FS={},FPS=30,tm={};
// key, label, min, max, step, unit, good-from, good-to, advice when below, advice when above, basic(1)/more(0)
var P=[['head_top_pct','Head: distance from the top',5,60,1,' % of height',18,30,'head near the top edge','a lot of space above the head',0],
['zone_w_pct','Rest zone: width',4,80,1,' % of width',15,30,'follows every small move','person can sit far off-centre',0],
['zone_h_pct','Rest zone: height',4,80,1,' % of height',15,30,'bobs when the person nods','slow to follow up and down',0],
['zone_strength','Bringing the person back (0.6 gentle, 1 normal, 1.6 firm)',0.6,1.6,0.1,'',0.8,1.6,'gentle, a fast walker may get away','',0],
['zone_anticipation','When the person walks (0 wait, 1 travel with them)',0,1,0.1,'',0.6,1,'follows after the person has moved','',0],
['pursuit_s','Keep turning after the person walks out of the picture',0,5,0.5,' s',1,3,'stops at once when the person is lost','may turn away from where they stopped',0],
['search_b_pct','Search area: ignore the bottom (congregation)',0,80,1,' % of height',15,40,'people in the seats may be picked','a person low in the picture is missed',0],
['lock_hold','Automatic: wait for a lost person before picking another',1,60,1,' s',5,15,'may jump to someone else quickly','may follow nobody for a while',0],
['head_left_pct','Head: distance from the left',20,80,1,' % of width',45,55,'person sits left','person sits right',0],
['search_t_pct','Search area: ignore the top',0,80,1,' % of height',0,10,'','tall people are missed',0],
['search_l_pct','Search area: ignore the left side',0,80,1,' % of width',0,15,'','person is lost on the left',0],
['search_r_pct','Search area: ignore the right side',0,80,1,' % of width',0,15,'','person is lost on the right',0]];
var C=[['headroom','Head room (space above the head)',[['tight','Tight (closer shots)'],['normal','Normal'],['generous','Generous (wide shots)'],['custom','Custom (number below)']]],
['lead','Space in front of a walking person',[['off','None: always centred'],['some','A little'],['more','More'],['custom','Custom (number below)']]],
['sensitivity','Sensitivity',[['low','Low: calm camera, ignores small movements'],['medium','Medium'],['high','High: follows small movements too'],['custom','Custom (numbers below)']]],
['zone_strength','Speed',[[0.6,'Gentle: slow, smooth catching up'],[1,'Normal'],[1.6,'Firm: for people who move fast']]],
['zone_anticipation','When the person walks',[[1,'Travel with them (like a camera operator)'],[0.6,'Travel with them, a little behind'],[0,'Only move once they are off-centre']]],
['scope','The camera may',[['pt','Turn and tilt'],['pan','Turn only (tilt stays where the joystick left it)']]],
['pursuit_s','If the person walks out of the picture',[[0,'Stop and wait'],[2,'Keep turning that way for 2 seconds'],[4,'Keep turning that way for 4 seconds']]],
['seats','Seats in the bottom of the picture',[['none','None in view'],['low','A few heads'],['medium','Front rows'],['high','Many rows'],['custom','Custom (number below)']]],
['lock_hold','Automatic: if the person is lost',[[5,'Wait 5 seconds, then follow somebody else'],[10,'Wait 10 seconds'],[30,'Wait 30 seconds']]]];
var KP=56;
function q(u){return fetch(u).then(function(r){return r.json()}).then(head).catch(function(){})}
function head(s){H=s;var w='';if(!(s.gp>0)||!(s.gt>0))w='Gear ratios are not set on the head page, so tracking speeds are not calibrated. ';
if(!s.trk)w+='Tracking is blocked (CW/CCW button is out). ';$('hw').textContent=w;$('adr').textContent='Head '+(s.ip||'?')+'   Joystick '+(s.jip||'not seen');
if(document.activeElement!=$('tsc'))$('tsc').value=s.tsc;$('v_tsc').textContent=(+s.tsc).toFixed(2)+' x';
if(document.activeElement!=$('tramp'))$('tramp').value=s.tramp;$('v_tramp').textContent=(+s.tramp).toFixed(1)+' s';
$('trk').textContent=s.trk?'Face tracking allowed (tap to block)':'Face tracking BLOCKED (tap to allow)';$('trk').className=s.trk?'on':'sv'}
$('tsc').oninput=function(){$('v_tsc').textContent=(+this.value).toFixed(2)+' x'};$('tsc').onchange=function(){q('/set?tsc='+this.value).then(function(){if(SRC)paint()})};
$('tramp').oninput=function(){$('v_tramp').textContent=(+this.value).toFixed(1)+' s'};$('tramp').onchange=function(){q('/set?tramp='+this.value)};
$('trk').onclick=function(){q('/trk')};
setInterval(function(){q('/s')},1500);q('/s');
// ---- SHA-256 (the page is plain http, so the browser's own crypto is not available) ----
function sha(str){var m=new TextEncoder().encode(str),l=m.length,n=((l+8)>>6)+1,w=new Uint32Array(n*16),i,j;
for(i=0;i<l;i++)w[i>>2]|=m[i]<<(24-(i%4)*8);w[l>>2]|=0x80<<(24-(l%4)*8);w[n*16-1]=l*8;w[n*16-2]=Math.floor(l/536870912);
var K=[],h=[],c=2,x=0;function fr(v){return((v-Math.floor(v))*4294967296)|0}
while(x<64){var pr=true;for(j=2;j*j<=c;j++)if(c%j==0){pr=false;break}if(pr){if(x<8)h[x]=fr(Math.sqrt(c));K[x++]=fr(Math.cbrt(c))}c++}
var W=new Int32Array(64);function r(v,s){return(v>>>s)|(v<<(32-s))}
for(i=0;i<n;i++){for(j=0;j<16;j++)W[j]=w[i*16+j];
for(j=16;j<64;j++){var a=W[j-15],b=W[j-2];W[j]=(W[j-16]+(r(a,7)^r(a,18)^(a>>>3))+W[j-7]+(r(b,17)^r(b,19)^(b>>>10)))|0}
var A=h[0],B=h[1],C=h[2],D=h[3],E=h[4],F=h[5],G=h[6],I=h[7];
for(j=0;j<64;j++){var t1=(I+(r(E,6)^r(E,11)^r(E,25))+((E&F)^(~E&G))+K[j]+W[j])|0,t2=((r(A,2)^r(A,13)^r(A,22))+((A&B)^(A&C)^(B&C)))|0;
I=G;G=F;F=E;E=(D+t1)|0;D=C;C=B;B=A;A=(t1+t2)|0}
h[0]=(h[0]+A)|0;h[1]=(h[1]+B)|0;h[2]=(h[2]+C)|0;h[3]=(h[3]+D)|0;h[4]=(h[4]+E)|0;h[5]=(h[5]+F)|0;h[6]=(h[6]+G)|0;h[7]=(h[7]+I)|0}
var o='';for(i=0;i<8;i++)for(j=3;j>=0;j--)o+=String.fromCharCode((h[i]>>>(j*8))&255);return btoa(o)}
// ---- OBS WebSocket (protocol 5) ----
function os(t){$('os').textContent=t}
function req(t,d){return new Promise(function(ok,no){if(!ws||ws.readyState!=1){no('not connected');return}var id='r'+(++rid);cb[id]=[ok,no];
ws.send(JSON.stringify({op:6,d:{requestType:t,requestId:id,requestData:d||{}}}))})}
function connect(){var ip=$('ip').value.trim(),port=$('port').value||4455,pw=$('pw').value;if(!ip){os('Enter the PC address.');return}
try{localStorage.setItem('x70obs',JSON.stringify({ip:ip,port:port,pw:pw}))}catch(e){}
if(ws){ws.onclose=null;ws.close()}os('Connecting...');
try{ws=new WebSocket('ws://'+ip+':'+port)}catch(e){os('Cannot open the connection: '+e.message);return}
ws.onmessage=function(m){var j=JSON.parse(m.data);
if(j.op==0){var a=j.d.authentication,id={rpcVersion:1,eventSubscriptions:0};if(a)id.authentication=sha(sha(pw+a.salt)+a.challenge);ws.send(JSON.stringify({op:1,d:id}))}
else if(j.op==2){find()}
else if(j.op==7){var c=cb[j.d.requestId];delete cb[j.d.requestId];if(c){if(j.d.requestStatus.result)c[0](j.d.responseData||{});else c[1](j.d.requestStatus.comment||('error '+j.d.requestStatus.code))}}};
ws.onclose=function(e){$('ob').hidden=true;os(e.code==4009?'OBS rejected the password.':'Not connected to OBS'+(e.code&&e.code!=1000?' (code '+e.code+').':'.'))};
ws.onerror=function(){os('Could not reach OBS at '+ip+':'+port+'. Is the WebSocket server enabled?')}}
function find(){os('Connected. Looking for the Face Tracker PTZ filter...');
req('GetVideoSettings').then(function(v){if(v.fpsDenominator)FPS=v.fpsNumerator/v.fpsDenominator;return req('GetInputList')}).then(function(l){
var ins=l.inputs||[],i=0;function next(){if(i>=ins.length){os('Connected, but no source has a Face Tracker PTZ filter yet.');return}
var nm=ins[i++].inputName;req('GetSourceFilterList',{sourceName:nm}).then(function(f){var fl=(f.filters||[]).filter(function(x){return x.filterKind=='face_tracker_ptz'})[0];
if(!fl){next();return}SRC=nm;FLT=fl.filterName;
req('GetSourceFilterDefaultSettings',{filterKind:'face_tracker_ptz'}).then(function(d){FS=Object.assign({},d.defaultFilterSettings||{},fl.filterSettings||{});FS._en=fl.filterEnabled;build()},function(){FS=Object.assign({},fl.filterSettings||{});FS._en=fl.filterEnabled;build()})},next)}
next()}).catch(function(e){os('OBS error: '+e)})}
function fmt(p,v){return(+v).toFixed(p[4]<0.1?2:(p[4]<1?1:0))+p[5]}
function good(p){if(p[0]=='Kp_x_db')return[KP-5,KP+3];if(p[0]=='Kp_y_db')return[KP-8,KP];return[p[6],p[7]]}
function note(p,v){var g=good(p),t=v<g[0]?p[8]:v>g[1]?p[9]:'';return t?'<i class="hi">'+t+'</i>':'<i class="ok">good</i>'}
function base(){var fov=+$('shot').value,s=+(H.tsc||0.2);return Math.max(25,Math.min(77,Math.round(20*Math.log10(fov*0.75*FPS/s))))}
function fmode(){$('fm').textContent=FS.follow_mode=='manual'?'Manual: staying on the person you locked.':'Automatic: following the biggest person in the search area.'}
$('lk').onclick=function(){setv('follow_mode','manual');setv('lock_request',Date.now()%1000000000);fmode();os('Lock sent. The green box should now be on that person.')};
$('au2').onclick=function(){setv('follow_mode','auto');fmode()};
function setv(k,v){FS[k]=v;clearTimeout(tm[k]);tm[k]=setTimeout(function(){var o={};o[k]=v;req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:o,overlay:true}).catch(function(e){os('OBS error: '+e)})},120)}
function build(){os('Connected: filter "'+FLT+'" on "'+SRC+'", OBS at '+FPS.toFixed(0)+' fps.');$('ob').hidden=false;var h=['',''];
P.forEach(function(p){var g=good(p);h[p[10]?0:1]+='<label>'+p[1]+' <b id="v_'+p[0]+'"></b></label><input type="range" id="s_'+p[0]+'" min="'+p[2]+'" max="'+p[3]+'" step="'+p[4]+'"><small id="n_'+p[0]+'"></small>'});$('sl').innerHTML=h[0];$('sl2').innerHTML=h[1];
P.forEach(function(p){var e=$('s_'+p[0]);e.oninput=function(){var v=+e.value;show1(p,v);setv(p[0],v)}});paint()}
function show1(p,v){var g=good(p);$('v_'+p[0]).innerHTML=fmt(p,v)+note(p,v);$('n_'+p[0]).textContent='Good: '+fmt(p,g[0])+' to '+fmt(p,g[1])}
function nudge(k,d){var p=P.filter(function(x){return x[0]==k})[0],v=Math.max(p[2],Math.min(p[3],(+FS[k]||0)+d));setv(k,v);$('s_'+k).value=v;show1(p,v)}
function buildC(){var h='';C.forEach(function(c){h+='<label>'+c[1]+'</label><select id="c_'+c[0]+'">'+c[2].map(function(o){return '<option value="'+o[0]+'">'+o[1]+'</option>'}).join('')+'</select>'});$('ch').innerHTML=h;
C.forEach(function(c){$('c_'+c[0]).onchange=function(){var v=this.value;setv(c[0],typeof c[2][0][0]=='number'?+v:v)}})}
function paintC(){C.forEach(function(c){var e=$('c_'+c[0]);if(e&&FS[c[0]]!==undefined)e.value=FS[c[0]]})}
$('shot').onchange=function(){KP=base();if(SRC)paint()};
function paint(){fmode();if(!$('c_headroom'))buildC();paintC();KP=base();P.forEach(function(p){var v=FS[p[0]];if(v===undefined)v=0;$('s_'+p[0]).value=v;show1(p,+v)});
$('en').textContent=FS._en?'Plugin tracking is ON (tap to switch off)':'Plugin tracking is OFF (tap to switch on)';$('en').className=FS._en?'on':'sv';
$('ix').textContent='Reverse pan direction: '+(FS.invert_x?'ON':'off');$('iy').textContent='Reverse tilt direction: '+(FS.invert_y?'ON':'off')}
$('en').onclick=function(){var v=!FS._en;req('SetSourceFilterEnabled',{sourceName:SRC,filterName:FLT,filterEnabled:v}).then(function(){FS._en=v;paint()},function(e){os('OBS error: '+e)})};
$('ix').onclick=function(){setv('invert_x',!FS.invert_x);paint()};$('iy').onclick=function(){setv('invert_y',!FS.invert_y);paint()};
// Start values: loop gain about 1 per second for the chosen shot width, the OBS frame rate and the head's speed scale.
function rec(){
var v={zone_mode:true,headroom:'normal',lead:'some',sensitivity:'medium',zone_strength:1,zone_anticipation:1,scope:'pt',pursuit_s:2,lock_hold:10,person_model:'tiny',detector_engine:2};
Object.assign(FS,v);req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:v,overlay:true}).then(function(){paint();os('Recommended settings loaded. The search area was left as it is.')},function(e){os('OBS error: '+e)})}
$('rec').onclick=rec;$('go').onclick=connect;
try{var sv=JSON.parse(localStorage.getItem('x70obs')||'null');if(sv){$('ip').value=sv.ip;$('port').value=sv.port;$('pw').value=sv.pw}}catch(e){}
</script></body></html>)HTML";

void sendStatus() {
  uint32_t now = millis();
  int src = 0;
  if (pan.webCmd != 0 || tilt.webCmd != 0) src = 1;
  else if ((pan.viscaCmd != 0 || tilt.viscaCmd != 0 || pan.going || tilt.going) && (int32_t)(manualUntil - now) <= 0) src = 2;
  if (src == 0 && !trackingOn && viscaLastMs != 0 && now - viscaLastMs < 2000) src = 3;   // OBS is talking but blocked
  long pp = pan.st ? (long)(pan.st->getCurrentPosition() * sgn(pan)) : 0, tp = tilt.st ? (long)(tilt.st->getCurrentPosition() * sgn(tilt)) : 0;
  static char b[1900]; String ipS = WiFi.localIP().toString(), jipS = (joySeenMs != 0 && millis() - joySeenMs < 6000) ? joyIP.toString() : String("");
  snprintf(b, sizeof b,
    "{\"on\":%d,\"trk\":%d,\"src\":%d,\"pd\":%.2f,\"td\":%.2f,\"pt\":%.3f,\"tt\":%.3f,\"gp\":%.2f,\"gt\":%.2f,\"pl\":%d,\"tl\":%d,\"dp\":%d,\"dt\":%d,"
    "\"max\":%d,\"ramp\":%.1f,\"tsc\":%.2f,\"tramp\":%.1f,\"rssi\":%d,\"ps\":[%d,%d,%d,%d,%d,%d],\"fw\":%d,\"auto\":%d,\"lat\":%d,\"jfw\":%d,\"jon\":%d,\"ip\":\"%s\",\"jip\":\"%s\",\"ust\":%d,\"upc\":%d,\"utry\":%d,\"chk\":%d,\"obs\":%d,\"jsc\":%.2f,\"heap\":%d,\"blk\":%d,\"msg\":\"%s\",\"sd\":%d,\"inv\":[%d,%d],\"lm\":[%d,%d,%d,%d],\"lv\":[%.3f,%.3f,%.3f,%.3f],"
    "\"pp\":[[%.3f,%.3f],[%.3f,%.3f],[%.3f,%.3f],[%.3f,%.3f],[%.3f,%.3f],[%.3f,%.3f]],\"pn\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],\"up\":%lu}",
    motorsOn, trackingOn, src, stepsToDeg(pan, pp), stepsToDeg(tilt, tp), pp / STEPS_PER_TURN, tp / STEPS_PER_TURN, pan.gear, tilt.gear,
    limited(pan), limited(tilt), pan.found, tilt.found, (int)maxRpm, ramp, trackScale, trackRamp, (int)WiFi.RSSI(),
    presetSet[0], presetSet[1], presetSet[2], presetSet[3], presetSet[4], presetSet[5],
    FW_BUILD, autoUpdate, latestBuild, joyBuild, (int)(joySeenMs != 0 && millis() - joySeenMs < 6000), ipS.c_str(), jipS.c_str(), updMode ? (updStage ? (int)updStage : 1) : 0, (int)updPct, (int)updTry, chkState == 1, (int)((viscaClient && viscaClient.connected()) || (viscaLastMs != 0 && now - viscaLastMs < 3000)), joyScale, (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getMaxAllocHeap() / 1024), fwMsg.c_str(),
    setupDone, pan.invert, tilt.invert,
    // the ends as the user sees them (left, right, down, up), in motor turns from centre
    pan.invert ? pan.hasHi : pan.hasLo, pan.invert ? pan.hasLo : pan.hasHi, tilt.invert ? tilt.hasHi : tilt.hasLo, tilt.invert ? tilt.hasLo : tilt.hasHi,
    (pan.invert ? -pan.hi : pan.lo) / STEPS_PER_TURN, (pan.invert ? -pan.lo : pan.hi) / STEPS_PER_TURN, (tilt.invert ? -tilt.hi : tilt.lo) / STEPS_PER_TURN, (tilt.invert ? -tilt.lo : tilt.hi) / STEPS_PER_TURN,
    presetPos[0][0] * sgn(pan) / STEPS_PER_TURN, presetPos[0][1] * sgn(tilt) / STEPS_PER_TURN, presetPos[1][0] * sgn(pan) / STEPS_PER_TURN, presetPos[1][1] * sgn(tilt) / STEPS_PER_TURN,
    presetPos[2][0] * sgn(pan) / STEPS_PER_TURN, presetPos[2][1] * sgn(tilt) / STEPS_PER_TURN, presetPos[3][0] * sgn(pan) / STEPS_PER_TURN, presetPos[3][1] * sgn(tilt) / STEPS_PER_TURN,
    presetPos[4][0] * sgn(pan) / STEPS_PER_TURN, presetPos[4][1] * sgn(tilt) / STEPS_PER_TURN, presetPos[5][0] * sgn(pan) / STEPS_PER_TURN, presetPos[5][1] * sgn(tilt) / STEPS_PER_TURN,
    presetName[0], presetName[1], presetName[2], presetName[3], presetName[4], presetName[5], (unsigned long)(millis() / 1000));
  web.send(200, "application/json", b);
}

void webSetup() {
  web.on("/", []() { web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html", PAGE); });   // never show an old copy after an update
  web.on("/s", sendStatus);
  web.on("/tune", []() { web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html", TUNE); });
  web.on("/j", []() {                                   // joystick: x, y from -1 to 1
    stickFromBox = false; joyInput(web.arg("x").toFloat(), web.arg("y").toFloat());
    sendStatus();
  });
  web.on("/stop", []() { ev("STOP pressed on the page"); stopAll(); manualUntil = millis() + MANUAL_HOLD_MS; sendStatus(); });
  web.on("/trk", []() { trackingOn = !trackingOn; ev("tracking %s on the page", trackingOn ? "ALLOWED" : "BLOCKED"); if (!trackingOn) pan.viscaCmd = tilt.viscaCmd = 0; sendStatus(); });
  web.on("/log", []() {
    String t; t.reserve(NEV * 76 + 80);
    uint32_t u = millis() / 1000;
    t += "Head build "; t += FW_BUILD; t += ", running for "; t += String(u / 3600); t += " h "; t += String(u / 60 % 60); t += " min. Newest first; times are since power-up.\n";
    for (int i = 0; i < evCount; i++) { t += evBuf[(evHead - 1 - i + NEV) % NEV]; t += '\n'; }
    web.sendHeader("Cache-Control", "no-store"); web.send(200, "text/plain", t);
  });
  web.on("/en", []() { setMotors(web.arg("on") == "1"); sendStatus(); });
  web.on("/set", []() {
    if (web.hasArg("sd"))   { setupDone = web.arg("sd").toInt() & 7; prefs.putInt("sd", setupDone); }
    if (web.hasArg("jsc"))  { joyScale = constrain(web.arg("jsc").toFloat(), 0.05f, 1.0f); prefs.putFloat("jsc", joyScale); }
    if (web.hasArg("max"))  { maxRpm = constrain(web.arg("max").toFloat(), 10.0f, 300.0f); prefs.putFloat("max", maxRpm); }
    if (web.hasArg("tsc"))   { trackScale = constrain(web.arg("tsc").toFloat(), 0.05f, 1.0f); prefs.putFloat("tsc", trackScale); }
    if (web.hasArg("tramp")) { trackRamp = constrain(web.arg("tramp").toFloat(), 0.2f, 2.0f); prefs.putFloat("tramp", trackRamp); }
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
      if (web.hasArg("name")) setPresetName(n, web.arg("name"));
      if (web.hasArg("save")) { presetSet[n] = true; presetPos[n][0] = pan.st->getCurrentPosition(); presetPos[n][1] = tilt.st->getCurrentPosition(); savePreset(n); ev("preset %d saved on the page", n + 1); }
      else if (web.hasArg("clear")) { presetSet[n] = false; savePreset(n); setPresetName(n, ""); ev("preset %d cleared on the page", n + 1); }
      else if (!web.hasArg("name")) recallPreset(n);
    }
    sendStatus();
  });
  web.on("/go", []() {                                  // aim at a spot on the stage map: p, t in motor turns from centre, as the user sees them
    if (pan.st && tilt.st) {
      ev("stage map: aim asked on the page");
      manualUntil = millis() + MANUAL_HOLD_MS; pan.viscaCmd = tilt.viscaCmd = 0;
      gotoPos((long)lroundf(web.arg("p").toFloat() * STEPS_PER_TURN * sgn(pan)), (long)lroundf(web.arg("t").toFloat() * STEPS_PER_TURN * sgn(tilt)));
    }
    sendStatus();
  });
  web.on("/fw", []() { if (headMoving()) fwMsg = "Stop the head first"; else { fwMsg = ""; startCheck(false); } sendStatus(); });
  web.on("/fwgo", []() {
    if (headMoving()) fwMsg = "Stop the head first";
    else { fwMsg = "Restarting to update"; updateRequested = true; }
    sendStatus();
  });
  web.on("/wifi", []() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font:16px sans-serif;max-width:420px;margin:auto;padding:16px'>"
                 "<h3>X70 head: Wi-Fi</h3><p style='font-size:14px;color:#555'>Saving here also updates the joystick box, as long as it is switched on.</p><form method=post action=/wifisave>Wi-Fi name<br><input name=s style='width:100%;font-size:16px' value='");
    h += wifiSsid;
    h += F("'><br><br>Wi-Fi password (leave empty to keep)<br><input name=p type=password style='width:100%;font-size:16px'><br><br><b>Second network (optional)</b>, for example the church if the first is home. It joins whichever is in range.<br>Wi-Fi name 2<br><input name=s2 style='width:100%;font-size:16px' value='");
    h += wifiSsid2;
    h += F("'><br><br>Wi-Fi password 2 (leave empty to keep)<br><input name=p2 type=password style='width:100%;font-size:16px'><br><br>"
           "Device password (for the X70-Head Wi-Fi and manual updates; leave empty to keep)<br><input name=d style='width:100%;font-size:16px'><br><br>"
           "<button style='font-size:16px;padding:10px 20px'>Save and restart</button></form><p><a href='/'>Back</a> &middot; <a href='/update'>Upload a .bin by hand</a></p>");
    web.send(200, "text/html", h);
  });
  web.on("/wifisave", HTTP_POST, []() {
    String s = web.arg("s"), p = web.arg("p"), d = web.arg("d");
    s.trim();
    String s2 = web.arg("s2"), p2 = web.arg("p2"); s2.trim();
    if (s.length()) { prefs.putString("ssid", s); if (p.length() || s != wifiSsid) prefs.putString("wpass", p); }
    if (web.hasArg("s2")) { prefs.putString("ssid2", s2); if (p2.length() || s2 != wifiSsid2) prefs.putString("wpass2", p2); }   // empty name = forget network 2
    if (d.length() >= 8) prefs.putString("dpass", d);
    // Give the joystick box the same Wi-Fi details and device password, so it only has to be done here.
    // Sent straight to the box (never broadcast), over the Wi-Fi both are on now.
    bool joyHere = joyIP[0] != 0 && joySeenMs != 0 && millis() - joySeenMs < 6000;
    if (joyHere) {
      String m = "W\t" + prefs.getString("ssid", "") + "\t" + prefs.getString("wpass", "") + "\t" + prefs.getString("ssid2", "") + "\t" + prefs.getString("wpass2", "") + "\t" + prefs.getString("dpass", DEFAULT_PASS);
      for (int i = 0; i < 3; i++) { sendText(joyIP, m.c_str(), 52379); delay(60); }
    }
    String note = "Still not back? If you moved it to a different Wi-Fi, put this phone or PC on that Wi-Fi and open x70head.local. If the name or password was wrong, join the Wi-Fi called X70-Head and open 192.168.4.1/wifi to correct it.";
    String head = joyHere ? "Saved on the head and the joystick box. Both are restarting" : "Saved on the head only (joystick box not switched on). Restarting";
    web.send(200, "text/html", waitPage(head.c_str(), note.c_str(), "/s", 45));
    delay(800); setMotors(false); ESP.restart();
  });
  webUpdater.setup(&web, "/update", "admin", devPass.c_str());   // upload a .bin from any browser
  web.begin();
}

bool joinSavedWifi() {                         // two remembered networks: joins whichever is in range, the stronger one first
  const String *ss[2] = {&wifiSsid, &wifiSsid2}, *pp[2] = {&wifiPass, &wifiPass2};
  if (!wifiSsid.length() && !wifiSsid2.length()) return false;
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  int rssi[2] = {-999, -999}, n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) for (int j = 0; j < 2; j++) if (ss[j]->length() && WiFi.SSID(i) == *ss[j] && WiFi.RSSI(i) > rssi[j]) rssi[j] = WiFi.RSSI(i);
  WiFi.scanDelete();
  int first = rssi[1] > rssi[0] ? 1 : 0;
  for (int t = 0; t < 2; t++) {
    int j = t == 0 ? first : 1 - first;
    if (!ss[j]->length()) continue;
    Serial.printf("Joining Wi-Fi \"%s\"%s", ss[j]->c_str(), rssi[j] == -999 ? " (not seen, trying anyway)" : "");
    WiFi.begin(ss[j]->c_str(), pp[j]->c_str());
    for (int i = 0; i < (rssi[j] == -999 ? 16 : 40) && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect(true); delay(200); WiFi.mode(WIFI_STA);
  }
  return false;
}

// ---------------- start-up ----------------
void setup() {
  pinMode(EN_PIN, OUTPUT); digitalWrite(EN_PIN, HIGH);          // motors off until everything is ready
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  delay(300);
  Serial.printf("\n=== X70 head firmware, build %d ===\n", FW_BUILD);
  { static const char *why[] = {"unknown", "power-up", "external reset", "restart by the firmware", "CRASH (panic)", "CRASH (watchdog)", "CRASH (watchdog)", "CRASH (watchdog)", "deep sleep", "POWER DIP (brown-out)", "SDIO"};
    int r = (int)esp_reset_reason(); ev("started, build %d: %s", FW_BUILD, r >= 0 && r <= 10 ? why[r] : "unknown"); }
  loadAll();

  for (Axis *x : AX) {
    x->found = x->drv->version() == 0x21;
    Serial.printf("%s driver: %s\n", x->name, x->found ? "found" : "waiting for 12 V");
    if (x->found) configure(*x->drv);
  }
  engine.init();
  pan.st  = engine.stepperConnectToPin(PAN_STEP);
  tilt.st = engine.stepperConnectToPin(TILT_STEP);
  if (pan.st)  { pan.st->setDirectionPin(PAN_DIR);   pan.st->setCurrentPosition(pan.saved); }
  if (tilt.st) { tilt.st->setDirectionPin(TILT_DIR); tilt.st->setCurrentPosition(tilt.saved); }
  if (!pan.st || !tilt.st) Serial.println(F("Step generator could not start - tell Claude."));
  if (pan.found && tilt.found) setMotors(true); else { driversDown = true; Serial.println(F("Motors off until both drivers answer. Plug in 12 V; no restart needed.")); }

  WiFi.setHostname("x70head");
  bool joined = joinSavedWifi();
  if (joined) ev("Wi-Fi joined, address %s", WiFi.localIP().toString().c_str()); else ev("no Wi-Fi: own network X70-Head started");
  if (joined) {
    Serial.printf("\nConnected. Joystick page: http://%s   (or http://x70head.local)\n", WiFi.localIP().toString().c_str());
    Serial.printf("OBS: VISCA over TCP port 5678, or VISCA over IP (UDP) port 52381, address %s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME, devPass.c_str());
    Serial.printf("\nNo Wi-Fi yet. Join the Wi-Fi \"%s\" (password %s), then open http://192.168.4.1/wifi\n", AP_NAME, devPass.c_str());
  }
  if (prefs.getBool("upd", false)) {                     // this start-up is for installing an update
    prefs.putBool("upd", false);
    if (joined) updMode = true; else prefs.putString("updmsg", "Update failed: the head could not join the Wi-Fi");
  }
  fwMsg = prefs.getString("updmsg", ""); if (fwMsg.length()) prefs.remove("updmsg");
  if (updMode) { webSetup(); xTaskCreate(updateTask, "update", 16384, NULL, 1, NULL); return; }
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
  if (updMode) { web.handleClient(); delay(2); return; }      // update in progress: only the page is served
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
  if (now - lastDrvCheck > 1000UL && !headMoving() && pan.viscaCmd == 0 && tilt.viscaCmd == 0 && pan.webCmd == 0 && tilt.webCmd == 0) {
    lastDrvCheck = now; driverWatch();
  }
  bool quiet = !headMoving() && pan.viscaCmd == 0 && tilt.viscaCmd == 0 && pan.webCmd == 0 && tilt.webCmd == 0 &&
               ((lastInputMs == 0 && lastMoveMs == 0) || (now - lastInputMs > 300000UL && now - lastMoveMs > 300000UL));
  if (online && quiet && now > 45000UL && (lastCheck == 0 || now - lastCheck > 600000UL)) {
    lastCheck = now;
    startCheck(true);
  }
  if (chkState == 2) {                                                     // the answer from GitHub has arrived
    if (chkErr[0]) fwMsg = chkErr; else { latestBuild = chkBuild; fwMsg = ""; }
    if (chkAuto && !chkErr[0] && autoUpdate && FW_BUILD > 0 && latestBuild > FW_BUILD) { Serial.printf("New build %d found: updating\n", latestBuild); updateRequested = true; }
    chkState = 0;
  }
  // joystick box still on an older build (it was off during the last update): tell it again, at most every 5 minutes
  bool still = !headMoving() && pan.viscaCmd == 0 && tilt.viscaCmd == 0 && pan.webCmd == 0 && tilt.webCmd == 0;
  if (online && still && joyBuild > 0 && now - joySeenMs < 6000 &&
      (joyBuild < FW_BUILD || (quiet && autoUpdate && joyBuild < latestBuild)) &&            // the two always run the same build
      (lastJoyNudge == 0 || now - lastJoyNudge > 300000UL)) { lastJoyNudge = now; for (int i = 0; i < 3; i++) { tellJoystick("U"); delay(30); } }
  if (updateRequested && !headMoving() && chkState == 0) {
    updateRequested = false;
    requestUpdateRestart();                    // the head goes first; the joystick box is told once the head is back (below)
  }
  // event log: joystick box coming and going, who is moving the head, Wi-Fi
  static uint32_t lastEvCheck = 0;
  if (now - lastEvCheck > 200) {
    lastEvCheck = now;
    static bool jonWas = false, wifiWas = true; static int srcWas = 0; static uint32_t lastTrkEv = 0;
    bool jon = joySeenMs != 0 && now - joySeenMs < 6000;
    if (jon != jonWas) { jonWas = jon; ev(jon ? "joystick box online (build %d)" : "joystick box OFFLINE", joyBuild); }
    bool wf = WiFi.getMode() != WIFI_STA || WiFi.status() == WL_CONNECTED;
    if (wf != wifiWas) { wifiWas = wf; ev(wf ? "Wi-Fi back" : "Wi-Fi LOST: head stops"); }
    int src = (pan.webCmd != 0 || tilt.webCmd != 0) ? (stickFromBox ? 1 : 2) : ((trackingOn && (pan.viscaCmd != 0 || tilt.viscaCmd != 0) && (int32_t)(manualUntil - now) <= 0) ? 3 : 0);
    if (src != srcWas) {
      if (src == 1) ev("moving: joystick box");
      else if (src == 2) ev("moving: stick on the page");
      else if (src == 3 && (lastTrkEv == 0 || now - lastTrkEv > 15000)) { lastTrkEv = now; ev("moving: OBS tracking"); }
      srcWas = src;
    }
  }
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {      // Wi-Fi dropped: stop moving, it rejoins on its own
    static uint32_t lastTry = 0;
    pan.viscaCmd = tilt.viscaCmd = 0;
    if (now - lastTry > 5000) { lastTry = now; WiFi.reconnect(); }
  }
}
