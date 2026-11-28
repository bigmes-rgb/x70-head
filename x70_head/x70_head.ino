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

#include <esp_wifi.h>
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
// The two sticks, kept apart: [0] = the page, [1] = the joystick box; pan and tilt speed wanted by each, and when it
// last spoke. Kept apart so that one stick at rest cannot cancel the other (a box stick resting a hair off-centre
// sends "zero" many times a second).
float stickCmd[2][2] = {{0, 0}, {0, 0}}; uint32_t stickMs[2] = {0, 0};
bool stickFromBox = false;               // who is moving the head by stick right now
void stickClear() { stickCmd[0][0] = stickCmd[0][1] = stickCmd[1][0] = stickCmd[1][1] = 0; }
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
const int NEV = 96;
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
  if (!on) { stickClear(); for (Axis *x : AX) { if (x->st) x->st->forceStop(); x->v = x->a = 0; x->going = false; x->webCmd = x->viscaCmd = 0; } }
  digitalWrite(EN_PIN, on ? LOW : HIGH);
  motorsOn = on;
}

// ---------------- motion ----------------
void stopAll() { stickClear(); for (Axis *x : AX) { x->webCmd = x->viscaCmd = 0; if (x->going) { x->going = false; if (x->st) x->st->stopMove(); } } }

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
    stickClear();
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
  // Which stick drives: the page when it is being pushed, otherwise the joystick box. A stick that has gone quiet
  // (page closed, box out of range) counts as let go.
  {
    bool alive[2] = {stickMs[0] != 0 && now - stickMs[0] < DEADMAN_MS, stickMs[1] != 0 && now - stickMs[1] < DEADMAN_MS};
    int who = (alive[0] && (stickCmd[0][0] != 0 || stickCmd[0][1] != 0)) ? 0 : (alive[1] ? 1 : -1);
    pan.webCmd = who < 0 ? 0 : stickCmd[who][0]; tilt.webCmd = who < 0 ? 0 : stickCmd[who][1];
    if (pan.webCmd != 0 || tilt.webCmd != 0) stickFromBox = who == 1;
  }
  bool manual = (int32_t)(manualUntil - now) > 0;
  if ((pan.viscaCmd != 0 || tilt.viscaCmd != 0) && now - viscaLastMs > VISCA_DEADMAN_MS) pan.viscaCmd = tilt.viscaCmd = 0;
  for (Axis *xp : AX) {
    Axis &x = *xp;
    if (!x.st) continue;
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
  static bool obsWas = false;
  if (!viscaClient || !viscaClient.connected()) {
    if (obsWas) { obsWas = false; viscaClient.stop(); pan.viscaCmd = tilt.viscaCmd = 0; len = 0; ev("OBS disconnected (it reconnects when replies are late)"); }     // tracker went away: stop
    WiFiClient c = viscaTcp.accept();
    if (c) { obsWas = true; viscaClient = c; viscaClient.setNoDelay(true); len = 0; ev("OBS connected from %s", c.remoteIP().toString().c_str()); }
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
void joyInput(float x, float y, int who) {              // x, y from -1 to 1; who: 0 = the page, 1 = the joystick box
  auto curve = [](float u) { float m = fabsf(u); if (m < 0.06f) return 0.0f; m = (m - 0.06f) / 0.94f; return (u < 0 ? -1.0f : 1.0f) * m * m; };   // even build-up: fine near the middle without a sudden surge at the end of the stick's travel
  stickCmd[who][0] = curve(constrain(x, -1.0f, 1.0f)) * maxSps() * joyScale; stickCmd[who][1] = curve(constrain(y, -1.0f, 1.0f)) * maxSps() * joyScale;
  stickMs[who] = lastWebMs = millis();
  if (stickCmd[who][0] != 0 || stickCmd[who][1] != 0) { manualUntil = lastWebMs + MANUAL_HOLD_MS; lastInputMs = lastWebMs; }
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
    if (sscanf(b, "J,%f,%f", &x, &y) == 2) { joyInput(x, y, 1); joySeenMs = millis(); }
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
#include "pages.h"   // WAITPAGE, PAGE (the controls) and TUNE (tracking settings)

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
    joyInput(web.arg("x").toFloat(), web.arg("y").toFloat(), 0);
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

// The radio must never doze between packets: a dozing radio hears a burst (the stick being moved) but misses single
// packets (heartbeats, OBS commands), which shows up as the joystick box "going offline" and OBS reconnecting.
// The setting is lost whenever the radio restarts, so it is applied again after every join.
void radioAwake() { WiFi.setSleep(false); esp_wifi_set_ps(WIFI_PS_NONE); }
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
    if (WiFi.status() == WL_CONNECTED) { radioAwake(); return true; }
    WiFi.disconnect(true); delay(200); WiFi.mode(WIFI_STA); WiFi.setSleep(false);
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
  static uint32_t lastTick = 0, lastPass = 0, lastBusyEv = 0;
  uint32_t now = millis();
  if (lastPass != 0 && now - lastPass > 250 && (lastBusyEv == 0 || now - lastBusyEv > 5000)) { lastBusyEv = now; ev("head was busy for %u ms (nothing heard meanwhile)", (unsigned)(now - lastPass)); }
  lastPass = now;
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
    if (jon != jonWas) { jonWas = jon; if (jon) ev("joystick box online (build %d)", joyBuild); else ev("joystick box OFFLINE: silent 6 s, Wi-Fi %d dBm", (int)WiFi.RSSI()); }
    bool wf = WiFi.getMode() != WIFI_STA || WiFi.status() == WL_CONNECTED;
    if (wf != wifiWas) { wifiWas = wf; if (wf) { radioAwake(); ev("Wi-Fi back, %d dBm", (int)WiFi.RSSI()); } else ev("Wi-Fi LOST: head stops"); }
    // Once, a little after start-up: the facts about the radio link.
    static bool linkSaid = false;
    if (!linkSaid && wf && now > 8000 && WiFi.getMode() == WIFI_STA) { linkSaid = true; wifi_ps_type_t ps = WIFI_PS_NONE; esp_wifi_get_ps(&ps); ev("Wi-Fi %s %d dBm ch %d, doze %s", WiFi.SSID().substring(0, 12).c_str(), (int)WiFi.RSSI(), (int)WiFi.channel(), ps == WIFI_PS_NONE ? "off" : "ON"); }
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
