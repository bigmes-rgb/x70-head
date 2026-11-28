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
void setMotors(bool on);
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
        Serial.printf("%s driver: powered, set up again\n", x->name);
        x->found = true; changed = true;
      }
    } else if (x->found && ++miss[i] >= 2) {
      x->found = false; changed = true;
      Serial.printf("%s driver: not answering (12 V off?)\n", x->name);
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

// The check runs in the background so the page and the motors never wait for GitHub.
volatile int chkState = 0;                     // 0 idle, 1 asking GitHub, 2 answer ready
int chkBuild = -1; char chkErr[120] = ""; bool chkAuto = false;
void checkTask(void *) {
  chkErr[0] = 0; chkBuild = -1;
  {
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h; h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); h.setConnectTimeout(8000); h.setTimeout(8000);
    if (h.begin(c, String(FW_BASE) + "version.txt")) {
      int code = h.GET();
      if (code == 200) chkBuild = h.getString().toInt();
      else { char tls[60] = ""; c.lastError(tls, sizeof tls); snprintf(chkErr, sizeof chkErr, "Could not reach GitHub (%s%s%s)", HTTPClient::errorToString(code).c_str(), tls[0] ? " / " : "", tls); }
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
    t_httpUpdate_return r = httpUpdate.update(c, String(FW_BASE) + FW_FILE);
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
void joyInput(float x, float y) {                       // x, y from -1 to 1
  auto curve = [](float u) { float m = fabsf(u); if (m < 0.06f) return 0.0f; m = (m - 0.06f) / 0.94f; return (u < 0 ? -1.0f : 1.0f) * powf(m, 2.2f); };   // fine control near the middle
  pan.webCmd = curve(constrain(x, -1.0f, 1.0f)) * maxSps(); tilt.webCmd = curve(constrain(y, -1.0f, 1.0f)) * maxSps();
  lastWebMs = millis();
  if (pan.webCmd != 0 || tilt.webCmd != 0) { manualUntil = lastWebMs + MANUAL_HOLD_MS; lastInputMs = lastWebMs; }
}
void recallPreset(int n) {
  if (n >= 0 && n < NPRESET && presetSet[n]) { manualUntil = millis() + MANUAL_HOLD_MS; pan.viscaCmd = tilt.viscaCmd = 0; gotoPos(presetPos[n][0], presetPos[n][1]); }
}
void setTracking(bool on) { trackingOn = on; if (!on) pan.viscaCmd = tilt.viscaCmd = 0; }
// Joystick box packets (text, UDP 52380):  "J,x,y"  stick   |  "B,id,P,n"  recall preset n   |  "B,id,T"  tracking on/off
void joyBoxPoll() {
  static long lastId = -1;
  for (int guard = 0; guard < 8; guard++) {
    int sz = joyUdp.parsePacket();
    if (sz <= 0) return;
    char b[40]; int n = joyUdp.read((uint8_t *)b, sizeof b - 1); if (n <= 0) continue; b[n] = 0;
    float x, y; long id; int k;
    joyIP = joyUdp.remoteIP();
    if (sscanf(b, "J,%f,%f", &x, &y) == 2) { joyInput(x, y); joySeenMs = millis(); }
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
<style>body{margin:0;background:#14171c;color:#e8ecf1;font:16px -apple-system,Segoe UI,Roboto,sans-serif;display:flex;min-height:100vh;align-items:center;justify-content:center}
.c{width:min(88vw,360px);background:#1b2027;border:1px solid #3a4452;border-radius:16px;padding:22px}h2{margin:0 0 12px;font-size:18px;display:flex;gap:10px;align-items:center}
i{width:18px;height:18px;border-radius:50%;border:2px solid #4fc3f7;border-top-color:transparent;animation:s .8s linear infinite;flex:none}
p{color:#9aa5b4;font-size:14px;line-height:1.45}@keyframes s{to{transform:rotate(360deg)}}</style>
<div class=c><h2><i></i><span>%HEAD%</span></h2><p id=m>This page comes back by itself. Nothing to do.</p><p id=n hidden>%NOTE%</p></div>
<script>var t0=Date.now(),g=function(i){return document.getElementById(i)};setTimeout(function k(){var c=new AbortController();setTimeout(function(){c.abort()},2000);
fetch('%PING%',{signal:c.signal,cache:'no-store'}).then(function(r){if(r.ok)location.replace('/');else throw 0}).catch(function(){var s=Math.round((Date.now()-t0)/1000);
g('m').textContent='Waiting for it to come back: '+s+' s';if(s>%LATE%)g('n').hidden=false;setTimeout(k,2000)})},6000)</script>)HTML";
String waitPage(const char *head, const char *note, const char *ping, int late) {
  String h = FPSTR(WAITPAGE); h.replace("%HEAD%", head); h.replace("%NOTE%", note); h.replace("%PING%", ping); h.replace("%LATE%", String(late)); return h;
}
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
#st{font-size:13px;color:#9aa5b4;min-height:54px;white-space:pre-line}
h3{margin:16px 0 4px;font-size:13px;color:#9aa5b4;text-transform:uppercase;letter-spacing:.08em}
label{display:block;margin:10px 0 2px;font-size:13px;color:#cfd6df}input[type=range]{width:100%}
input[type=number]{width:80px;background:#222830;color:#e8ecf1;border:1px solid #3a4452;border-radius:8px;padding:8px;font-size:15px}
details{margin-top:14px;text-align:left;background:#1b2027;border-radius:10px;padding:8px 12px}summary{padding:6px 0}
#warn{color:#f2a93b;font-size:13px}
#up{position:fixed;inset:0;background:rgba(10,12,15,.93);display:flex;align-items:center;justify-content:center;z-index:9}#up[hidden]{display:none}
.uc{width:min(88vw,360px);background:#1b2027;border:1px solid #3a4452;border-radius:16px;padding:20px 20px 14px;text-align:left}
.uc h2{margin:0 0 10px;font-size:18px}.us{display:flex;align-items:center;gap:10px;padding:7px 0;color:#6b7686;flex-wrap:wrap}
.us i{width:18px;height:18px;border-radius:50%;border:2px solid #3a4452;flex:none;box-sizing:border-box}
.us.now{color:#e8ecf1}.us.now i{border-color:#4fc3f7;border-top-color:transparent;animation:sp .8s linear infinite}
.us.ok{color:#9fdcb5}.us.ok i{background:#2e9d5b;border-color:#2e9d5b}.us.bad{color:#ff8a80}.us.bad i{background:#b3261e;border-color:#b3261e}
.bar{flex-basis:100%;height:8px;background:#2a313b;border-radius:4px;overflow:hidden;margin-left:28px}.bar b{display:block;height:100%;width:0;background:#4fc3f7;transition:width .4s}
.spin{display:inline-block;width:11px;height:11px;border-radius:50%;border:2px solid #4fc3f7;border-top-color:transparent;animation:sp .8s linear infinite;vertical-align:-2px}
#um{font-size:13px;color:#9aa5b4;min-height:34px;margin:10px 0 6px}@keyframes sp{to{transform:rotate(360deg)}}
</style></head><body><div id="up" hidden><div class="uc"><h2 id="ut"></h2>
<div class="us" id="u1"><i></i>Restarting into update mode</div>
<div class="us" id="u2"><i></i>Connecting to GitHub</div>
<div class="us" id="u3"><i></i><span>Downloading <span id="upc"></span></span><div class="bar"><b id="ub"></b></div></div>
<div class="us" id="u4"><i></i>Installing and restarting</div>
<div class="us" id="u5"><i></i><span id="u5t">Head running the new build</span></div>
<div class="us" id="u6"><i></i><span id="uj">Joystick box</span></div>
<p id="um"></p><button id="ux" style="width:100%" hidden>Close</button></div></div><div class="w">
<div id="st">connecting...</div><div id="warn"></div>
<div id="pad"><div id="knob"></div></div>
<button id="stop">STOP</button>
<button id="trk" style="width:100%"></button>
<button style="width:100%" onclick="location.href='/tune'">Adjust face tracking (speed, smoothness)</button>
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
<p id="fw" style="font-size:13px;color:#cfd6df;white-space:pre-line"></p><p id="ck" hidden style="font-size:13px;color:#4fc3f7"><i class="spin"></i> Asking GitHub for the newest build...</p>
<div class="g2"><button onclick="$('ck').hidden=false;q('/fw')">Check for update</button><button onclick="upGo()">Update both now</button></div>
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
t+='\n'+(s.src==3?'OBS is sending commands: IGNORED, tracking is blocked':s.src==2?'OBS face tracking is moving the head':s.src==1?'Joystick':(s.on?'Holding':'No commands'))+(s.on?'':' (MOTORS OFF)')+'   Wi-Fi signal: '+(s.rssi>=-60?'strong':s.rssi>=-70?'good':s.rssi>=-80?'weak':'very weak');t+='\nHead '+s.ip+'   Joystick '+(s.jip||'not seen');$('st').textContent=t;
var w='';if(!s.dp)w+='Pan driver not answering. ';if(!s.dt)w+='Tilt driver not answering. ';if(!s.pl)w+='Pan limits not set (slow). ';if(!s.tl)w+='Tilt limits not set (slow). ';$('warn').textContent=w;
if(document.activeElement!=$('max')){$('max').value=s.max}$('mv').textContent=s.max+' RPM motor';
if(document.activeElement!=$('ramp')){$('ramp').value=s.ramp}$('rv').textContent=s.ramp.toFixed(1)+' s';
if(document.activeElement!=$('gp')&&document.activeElement!=$('gt')){$('gp').value=s.gp;$('gt').value=s.gt}
$('trk').textContent=s.trk?'Face tracking allowed (tap to block)':'Face tracking BLOCKED (tap to allow)';$('trk').className=s.trk?'':'sv';
$('mo').textContent=s.on?'Switch motors OFF (camera may sag)':'Switch motors ON';
for(var i=0;i<6;i++){$('p'+i).className=s.ps[i]?'on':''}
$('au').textContent=s.auto?'Automatic updates: ON (tap to turn off)':'Automatic updates: OFF (tap to turn on)';$('au').className=s.auto?'on':'';
if(s.lat>s.fw&&s.fw>0)w+='Update available: build '+s.lat+'. ';$('warn').textContent=w;
if(!U&&s.ust>0)U={from:s.fw,target:0,jon:!!s.jon,t0:Date.now(),step:2,end:''};if(U)upDraw(s);
$('fw').textContent='Head: build '+s.fw+' at '+s.ip+'\nJoystick box: '+(s.jon?'build '+s.jfw+' at '+s.jip:'not seen')+'\nNewest on GitHub: '+(s.lat<0?'not checked yet':'build '+s.lat)+'\nFree memory: '+s.heap+' KB (largest piece '+s.blk+' KB)'+(s.msg?'\n'+s.msg:'');$('ck').hidden=!s.chk}
function q(u){var c=new AbortController(),t=setTimeout(function(){c.abort()},2500);return fetch(u,{signal:c.signal}).then(function(r){return r.json()}).then(show).catch(function(){$('st').textContent='no connection to the head';if(U)upDraw(null)}).then(function(){clearTimeout(t)})}
// ---- live update progress
var U=null;try{U=JSON.parse(sessionStorage.x70up||'null')}catch(e){}if(U&&Date.now()-U.t0>600000)U=null;
function upGo(){if(S.lat>0&&S.fw>=S.lat&&(!S.jon||S.jfw>=S.lat)&&!confirm('Both are already on the newest build. Install it again?'))return;
U={from:S.fw||0,target:S.lat||0,jon:!!S.jon,t0:Date.now(),step:1,end:''};upDraw(S);q('/fwgo')}
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
function lim(a,e){q('/lim?a='+a+'&e='+e)}
for(var i=0;i<6;i++){var b=document.createElement('button');b.id='p'+i;b.textContent=i+1;b.onclick=(function(n){return function(){q('/p?n='+n+(saving?'&save=1':''));saving=false;$('sv').className=''}})(i);$('pr').appendChild(b)}
$('sv').onclick=function(){saving=!saving;$('sv').className=saving?'sv':''};
$('stop').onclick=function(){X=Y=0;setK();q('/stop')};
$('max').onchange=function(){q('/set?max='+this.value)};$('ramp').onchange=function(){q('/set?ramp='+this.value)};
$('mo').onclick=function(){q('/en?on='+(S.on?0:1))};$('trk').onclick=function(){q('/trk')};
setInterval(function(){if(busy)return;var mvg=act||X||Y;if(mvg||last){busy=true;last=mvg?1:0;fetch('/j?x='+X.toFixed(3)+'&y='+Y.toFixed(3)).then(function(r){return r.json()}).then(show).catch(function(){}).then(function(){busy=false})}
else if(++idle>=10){idle=0;busy=true;q('/s').then(function(){busy=false})}},100);
</script></body></html>)HTML";

const char TUNE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>X70: adjust face tracking</title>
<style>
body{margin:0;background:#14171c;color:#e8ecf1;font:15px -apple-system,Segoe UI,Roboto,sans-serif}
.w{max-width:440px;margin:auto;padding:12px}a{color:#4fc3f7}
button{background:#2a313b;color:#e8ecf1;border:1px solid #3a4452;border-radius:10px;padding:12px 8px;font-size:15px;margin:3px 0;width:100%}
button:active{background:#3a4452}.on{background:#1f5f3a}.sv{background:#8a6d1a}
h3{margin:18px 0 4px;font-size:13px;color:#9aa5b4;text-transform:uppercase;letter-spacing:.08em}
label{display:block;margin:12px 0 2px;font-size:13px;color:#cfd6df}label b{float:right;color:#e8ecf1}
input[type=range]{width:100%}small{color:#9aa5b4;display:block}
input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;background:#222830;color:#e8ecf1;border:1px solid #3a4452;border-radius:8px;padding:9px;font-size:16px;margin:2px 0 6px}
.warn{color:#f2a93b;font-size:13px}.ok{color:#6fcf97}.hi{color:#f2a93b}.g3{display:grid;grid-template-columns:1fr;gap:0}details{margin-top:14px;background:#1b2027;border-radius:10px;padding:8px 12px}summary{padding:6px 0}label i{font-style:normal;font-size:12px;margin-left:6px}#os{font-size:13px;color:#9aa5b4;margin:6px 0}
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
<select id="shot"><option value="6">Waist-up shot</option><option value="11" selected>Full-body shot</option><option value="25">Wide stage shot</option></select>
<button id="rec">Load the recommended settings for this shot</button>
<h3>2. Fix what you see</h3>
<button data-fix="calm">Camera swings past the person and back &rarr; make it calmer</button>
<button data-fix="fast">Camera lags behind the person &rarr; make it quicker</button>
<button data-fix="still">Camera twitches when the person barely moves &rarr; make it steadier</button>
<button data-fix="tight">Person drifts too far off-centre before it moves &rarr; make it tighter</button>
<h3>3. Framing and speed</h3><div id="sl"></div>
<details><summary>More settings (rarely needed)</summary><div id="sl2"></div>
<button id="ix"></button><button id="iy"></button></details>
</div></div>
<script>
var $=function(i){return document.getElementById(i)},H={},ws=null,rid=0,cb={},SRC='',FLT='',FS={},FPS=30,tm={};
// key, label, min, max, step, unit, good-from, good-to, advice when below, advice when above, basic(1)/more(0)
var P=[['track_y','Head room: face higher or lower',-0.5,0.5,0.01,'',0.15,0.35,'face near the middle, a lot of space above','face near the top edge',1],
['Kp_x_db','Pan speed',20,80,1,' dB',0,0,'slow, may lag behind','fast, may swing past the person',1],
['Kp_y_db','Tilt speed',20,80,1,' dB',0,0,'slow, may lag behind','fast, may bob up and down',1],
['e_deadband_x','Still zone, left and right',0,20,0.5,' %',1,4,'follows every small move','person can sit far off-centre',1],
['e_deadband_y','Still zone, up and down',0,20,0.5,' %',3,8,'bobs when the person nods','slow to follow up and down',1],
['lock_hold','Stay on the same person when the face is hidden',1,60,1,' s',5,15,'may jump to someone else quickly','may follow nothing for a while',1],
['track_x','Face left or right in the picture',-0.5,0.5,0.01,'',-0.1,0.1,'person sits left','person sits right',0],
['e_nonlinear_x','Soft edge of still zone, left and right',0,20,0.5,' %',2,6,'starts abruptly','vague, slow to start',0],
['e_nonlinear_y','Soft edge of still zone, up and down',0,20,0.5,' %',4,8,'starts abruptly','vague, slow to start',0],
['Ki_x','Pan follow-through',0,1,0.01,'',0.1,0.3,'falls behind steady walking','overshoots when the person stops',0],
['Ki_y','Tilt follow-through',0,1,0.01,'',0.1,0.3,'falls behind','overshoots',0],
['Td_x','Pan quick start',0,1,0.01,' s',0.1,0.4,'slow to react','shaky',0],
['Td_y','Tilt quick start',0,1,0.01,' s',0.1,0.4,'slow to react','shaky',0],
['Tdlpf','Smoothing',0,10,0.1,' s',1,3,'jittery','sluggish',0],
['detect_interval','Look for faces every',0.2,5,0.1,' s',0.5,2,'more CPU','slow to notice a new face',0],
['track_fps','Tracking rate (0 = every frame)',0,60,1,' fps',8,15,'jerky','more CPU',0]];
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
function note(p,v){var g=good(p);return v<g[0]?'<i class="hi">'+p[8]+'</i>':v>g[1]?'<i class="hi">'+p[9]+'</i>':'<i class="ok">good</i>'}
function base(){var fov=+$('shot').value,s=+(H.tsc||0.2);return Math.max(25,Math.min(77,Math.round(20*Math.log10(fov*0.75*FPS/s))))}
function setv(k,v){FS[k]=v;clearTimeout(tm[k]);tm[k]=setTimeout(function(){var o={};o[k]=v;req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:o,overlay:true}).catch(function(e){os('OBS error: '+e)})},120)}
function build(){os('Connected: filter "'+FLT+'" on "'+SRC+'", OBS at '+FPS.toFixed(0)+' fps.');$('ob').hidden=false;var h=['',''];
P.forEach(function(p){var g=good(p);h[p[10]?0:1]+='<label>'+p[1]+' <b id="v_'+p[0]+'"></b></label><input type="range" id="s_'+p[0]+'" min="'+p[2]+'" max="'+p[3]+'" step="'+p[4]+'"><small id="n_'+p[0]+'"></small>'});$('sl').innerHTML=h[0];$('sl2').innerHTML=h[1];
P.forEach(function(p){var e=$('s_'+p[0]);e.oninput=function(){var v=+e.value;show1(p,v);setv(p[0],v)}});paint()}
function show1(p,v){var g=good(p);$('v_'+p[0]).innerHTML=fmt(p,v)+note(p,v);$('n_'+p[0]).textContent='Good: '+fmt(p,g[0])+' to '+fmt(p,g[1])}
function nudge(k,d){var p=P.filter(function(x){return x[0]==k})[0],v=Math.max(p[2],Math.min(p[3],(+FS[k]||0)+d));setv(k,v);$('s_'+k).value=v;show1(p,v)}
var FIX={calm:[['Kp_x_db',-3],['Kp_y_db',-3]],fast:[['Kp_x_db',3],['Kp_y_db',3]],still:[['e_deadband_x',1],['e_deadband_y',1]],tight:[['e_deadband_x',-1],['e_deadband_y',-1]]};
Array.prototype.forEach.call(document.querySelectorAll('[data-fix]'),function(b){b.onclick=function(){FIX[b.dataset.fix].forEach(function(f){nudge(f[0],f[1])});os('Changed. Watch the camera for a minute before changing it again.')}});
$('shot').onchange=function(){KP=base();if(SRC)paint()};
function paint(){KP=base();P.forEach(function(p){var v=FS[p[0]];if(v===undefined)v=0;$('s_'+p[0]).value=v;show1(p,+v)});
$('en').textContent=FS._en?'Plugin tracking is ON (tap to switch off)':'Plugin tracking is OFF (tap to switch on)';$('en').className=FS._en?'on':'sv';
$('ix').textContent='Reverse pan direction: '+(FS.invert_x?'ON':'off');$('iy').textContent='Reverse tilt direction: '+(FS.invert_y?'ON':'off')}
$('en').onclick=function(){var v=!FS._en;req('SetSourceFilterEnabled',{sourceName:SRC,filterName:FLT,filterEnabled:v}).then(function(){FS._en=v;paint()},function(e){os('OBS error: '+e)})};
$('ix').onclick=function(){setv('invert_x',!FS.invert_x);paint()};$('iy').onclick=function(){setv('invert_y',!FS.invert_y);paint()};
// Start values: loop gain about 1 per second for the chosen shot width, the OBS frame rate and the head's speed scale.
function rec(){var fov=+$('shot').value,s=+(H.tsc||0.2),kp=base();KP=kp;
var v={Kp_x_db:kp,Kp_y_db:Math.max(20,kp-3),e_deadband_x:2,e_nonlinear_x:4,e_deadband_y:4,e_nonlinear_y:6,track_x:0,track_y:fov<8?0.2:0.25,Ki_x:0.2,Ki_y:0.2,Td_x:0.25,Td_y:0.25,Tdlpf:2,track_fps:12,lock_hold:10,detect_interval:1,e_deadband_z:50,Ki_z:0,Td_z:0,Kp_z_db:-40};
Object.assign(FS,v);req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:v,overlay:true}).then(function(){paint();os('Recommended settings loaded for this shot.')},function(e){os('OBS error: '+e)})}
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
  char b[980]; String ipS = WiFi.localIP().toString(), jipS = (joySeenMs != 0 && millis() - joySeenMs < 6000) ? joyIP.toString() : String("");
  snprintf(b, sizeof b,
    "{\"on\":%d,\"trk\":%d,\"src\":%d,\"pd\":%.2f,\"td\":%.2f,\"pt\":%.3f,\"tt\":%.3f,\"gp\":%.2f,\"gt\":%.2f,\"pl\":%d,\"tl\":%d,\"dp\":%d,\"dt\":%d,"
    "\"max\":%d,\"ramp\":%.1f,\"tsc\":%.2f,\"tramp\":%.1f,\"rssi\":%d,\"ps\":[%d,%d,%d,%d,%d,%d],\"fw\":%d,\"auto\":%d,\"lat\":%d,\"jfw\":%d,\"jon\":%d,\"ip\":\"%s\",\"jip\":\"%s\",\"ust\":%d,\"upc\":%d,\"utry\":%d,\"chk\":%d,\"heap\":%d,\"blk\":%d,\"msg\":\"%s\"}",
    motorsOn, trackingOn, src, stepsToDeg(pan, pp), stepsToDeg(tilt, tp), pp / STEPS_PER_TURN, tp / STEPS_PER_TURN, pan.gear, tilt.gear,
    limited(pan), limited(tilt), pan.found, tilt.found, (int)maxRpm, ramp, trackScale, trackRamp, (int)WiFi.RSSI(),
    presetSet[0], presetSet[1], presetSet[2], presetSet[3], presetSet[4], presetSet[5],
    FW_BUILD, autoUpdate, latestBuild, joyBuild, (int)(joySeenMs != 0 && millis() - joySeenMs < 6000), ipS.c_str(), jipS.c_str(), updMode ? (updStage ? (int)updStage : 1) : 0, (int)updPct, (int)updTry, chkState == 1, (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getMaxAllocHeap() / 1024), fwMsg.c_str());
  web.send(200, "application/json", b);
}

void webSetup() {
  web.on("/", []() { web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html", PAGE); });   // never show an old copy after an update
  web.on("/s", sendStatus);
  web.on("/tune", []() { web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html", TUNE); });
  web.on("/j", []() {                                   // joystick: x, y from -1 to 1
    joyInput(web.arg("x").toFloat(), web.arg("y").toFloat());
    sendStatus();
  });
  web.on("/stop", []() { stopAll(); manualUntil = millis() + MANUAL_HOLD_MS; sendStatus(); });
  web.on("/trk", []() { trackingOn = !trackingOn; if (!trackingOn) pan.viscaCmd = tilt.viscaCmd = 0; sendStatus(); });
  web.on("/en", []() { setMotors(web.arg("on") == "1"); sendStatus(); });
  web.on("/set", []() {
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
      if (web.hasArg("save")) { presetSet[n] = true; presetPos[n][0] = pan.st->getCurrentPosition(); presetPos[n][1] = tilt.st->getCurrentPosition(); savePreset(n); }
      else recallPreset(n);
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
                 "<h3>X70 head: Wi-Fi</h3><form method=post action=/wifisave>Wi-Fi name<br><input name=s style='width:100%;font-size:16px' value='");
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
    web.send(200, "text/html", waitPage("Saved. The head is restarting", "Still not back? If you moved it to a different Wi-Fi, put this phone or PC on that Wi-Fi and open x70head.local. If the name or password was wrong, join the Wi-Fi called X70-Head and open 192.168.4.1/wifi to correct it.", "/s", 45));
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
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {      // Wi-Fi dropped: stop moving, it rejoins on its own
    static uint32_t lastTry = 0;
    pan.viscaCmd = tilt.viscaCmd = 0;
    if (now - lastTry > 5000) { lastTry = now; WiFi.reconnect(); }
  }
}
