/*
  X70 JOYSTICK BOX  -  Vidpro control box with an ESP32 inside
  Reads the original proportional stick and the two push buttons and sends them to the head over Wi-Fi.
  It finds the head by itself (same Wi-Fi network). Power: any USB port or phone charger.

  Wiring (ESP32 dev board):
    3V3  <- yellow (top sensor)  + white (side sensor)
    GND  <- black  (top sensor)  + red   (side sensor) + the black wire of each button
    D34  <- blue   (top sensor, middle tab)
    D35  <- green  (side sensor, middle tab)
    D25  <- red wire of the POWER button      -> go to preset 1
    D26  <- red wire of the CW/CCW button     -> face tracking allowed / blocked
  IMPORTANT: the stick's + wires go to 3V3, never to 5V / VIN.

  First power-up: leave the stick alone for 3 seconds (it learns the centre), then push it fully
  left, right, up and down once (it learns the range and remembers it).
  Reset what it learned: hold both buttons for 5 seconds.

  Wi-Fi: nothing to type here. On first start the box makes its own Wi-Fi "X70-Joystick" (password x70head123).
  Join it, open http://192.168.4.1 and enter the same network the head is on.
  To talk straight to the head with no router, enter the head's own Wi-Fi "X70-Head" there.

  Updates: the "Update both now" button on the head's page updates this box too.
  Board: ESP32 Dev Module, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)".
*/
#define FW_BUILD 0          // stamped by the automatic build - leave as it is
const char *FW_BASE = "https://github.com/bigmes-rgb/x70-head/releases/latest/download/";
const char *FW_FILE = "x70_joystick.bin";
const char *AP_NAME = "X70-Joystick";
const char *DEFAULT_PASS = "x70head123";   // device password until you change it on the box's page

// Stick directions are settings on the box's own page (http://x70joystick.local), not in this code.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

const int PIN_A = 34;        // blue  (top sensor)
const int PIN_B = 35;        // green (side sensor)
const int PIN_BTN1 = 25;     // POWER button
const int PIN_BTN2 = 26;     // CW/CCW button
const int PIN_LED = 2;       // on-board LED: on = connected to Wi-Fi
const int HEAD_PORT = 52380;
const int MIN_SPAN = 250;    // smallest stick range believed before it has learned the real one (ADC counts)

WiFiUDP udp;
WiFiUDP cmdUdp;                          // the head tells the box to update on port 52379
String wifiSsid, wifiPass, devPass;      // stored in the box (its own page)
bool apMode = false, updateRequested = false;
bool swapAxes = true, flipLR = false, flipUD = false;   // stick direction settings (box page)
IPAddress headIP;                        // learned from the head's answers, or looked up by name
uint32_t headAckMs = 0;                  // last time the head answered
int headBuild = -1;
String fwMsg = "";
WebServer web(80);
HTTPUpdateServer webUpdater;
Preferences prefs;

struct AxisIn { int pin; int centre; int lo; int hi; const char *key; };   // lo / hi = learned travel each side of centre
AxisIn A = {PIN_A, 2048, MIN_SPAN, MIN_SPAN, "a"}, B = {PIN_B, 2048, MIN_SPAN, MIN_SPAN, "b"};
struct Btn { int pin; bool last; uint32_t lastEvt, lastEdge; };
Btn b1 = {PIN_BTN1, false, 0, 0}, b2 = {PIN_BTN2, false, 0, 0};
uint32_t evtId = 0;
bool spanDirty = false; uint32_t spanDirtyMs = 0;

int readAvg(int pin) { long s = 0; for (int i = 0; i < 32; i++) s += analogRead(pin); return (int)(s / 32); }

void loadSpan(AxisIn &x) { String k = x.key; x.lo = prefs.getInt((k + "lo").c_str(), MIN_SPAN); x.hi = prefs.getInt((k + "hi").c_str(), MIN_SPAN); }
void saveSpan(const AxisIn &x) { String k = x.key; prefs.putInt((k + "lo").c_str(), x.lo); prefs.putInt((k + "hi").c_str(), x.hi); }

// Stick position from -1 to 1. Learns the furthest it has ever been pushed each way.
float readAxis(AxisIn &x) {
  int d = readAvg(x.pin) - x.centre;
  if (d > x.hi)  { x.hi = d;  spanDirty = true; spanDirtyMs = millis(); }
  if (-d > x.lo) { x.lo = -d; spanDirty = true; spanDirtyMs = millis(); }
  float v = d >= 0 ? (float)d / (x.hi * 0.97f) : (float)d / (x.lo * 0.97f);     // full output just before the mechanical end
  return constrain(v, -1.0f, 1.0f);
}

void sendTo(IPAddress to, const char *msg) { udp.beginPacket(to, HEAD_PORT); udp.write((const uint8_t *)msg, strlen(msg)); udp.endPacket(); }
bool headKnown() { return headIP[0] != 0; }
void sendToHead(const char *msg) {            // straight to the head once its address is known...
  if (headKnown()) sendTo(headIP, msg);
  if (!headKnown() || millis() - headAckMs > 5000) {   // ...and to everyone on the network while it is not answering
    IPAddress bc = WiFi.localIP(); bc[3] = 255; sendTo(bc, msg);
  }
}
void sendEvent(const char *what) {            // sent three times in case one is lost; the head acts once per id
  char m[32]; snprintf(m, sizeof m, "B,%lu,%s", (unsigned long)++evtId, what);
  for (int i = 0; i < 3; i++) { sendToHead(m); delay(15); }
  Serial.printf("button -> %s\n", what);
}

// Works for push-and-release buttons and for click-in / click-out buttons: one press = one action.
bool pressed(Btn &b) {
  bool now = digitalRead(b.pin) == LOW;
  if (now == b.last) return false;
  uint32_t t = millis();
  if (t - b.lastEdge < 60) return false;      // contact bounce
  bool quickRelease = !now && (t - b.lastEvt < 1500);
  b.last = now; b.lastEdge = t;
  if (quickRelease) return false;             // the release of a normal press is not a second press
  b.lastEvt = t;
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN1, INPUT_PULLUP); pinMode(PIN_BTN2, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_A, ADC_11db); analogSetPinAttenuation(PIN_B, ADC_11db);
  prefs.begin("x70joy", false);
  loadSpan(A); loadSpan(B);
  delay(800);
  long ca = 0, cb = 0; for (int i = 0; i < 20; i++) { ca += readAvg(PIN_A); cb += readAvg(PIN_B); delay(20); }
  A.centre = ca / 20; B.centre = cb / 20;
  b1.last = digitalRead(PIN_BTN1) == LOW; b2.last = digitalRead(PIN_BTN2) == LOW;
  evtId = esp_random() & 0xFFFFF;
  Serial.printf("\n=== X70 joystick box ===\nCentre: top sensor %d, side sensor %d (expect roughly 1500 to 2600)\n", A.centre, B.centre);
  if (A.centre < 300 || A.centre > 3800 || B.centre < 300 || B.centre > 3800)
    Serial.println(F("A centre reading is far off: check that sensor's three wires (3V3 / signal / GND)."));
  wifiSsid = prefs.getString("ssid", ""); wifiPass = prefs.getString("wpass", ""); devPass = prefs.getString("dpass", DEFAULT_PASS);
  swapAxes = prefs.getBool("swap", true); flipLR = prefs.getBool("flx", false); flipUD = prefs.getBool("fly", false);
  Serial.printf("Build %d\n", FW_BUILD);
  WiFi.setHostname("x70joystick");
  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); digitalWrite(PIN_LED, i % 2); }
  }
  if (WiFi.status() != WL_CONNECTED) {
    apMode = true; WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME, devPass.c_str());
    Serial.printf("No Wi-Fi yet. Join the Wi-Fi \"%s\" (password %s), then open http://192.168.4.1\n", AP_NAME, devPass.c_str());
  } else Serial.printf("Connected. Box page: http://%s  (or http://x70joystick.local)\n", WiFi.localIP().toString().c_str());
  ArduinoOTA.setHostname("x70joystick");
  ArduinoOTA.setPassword(devPass.c_str());
  ArduinoOTA.begin();
  cmdUdp.begin(52379);
  web.on("/", []() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font:16px sans-serif;max-width:420px;margin:auto;padding:16px'><h3>X70 joystick box</h3><p>Build ");
    h += FW_BUILD; h += apMode ? F(" &middot; not on a Wi-Fi yet") : F(" &middot; connected");
    h += (headAckMs && millis() - headAckMs < 6000) ? F(" &middot; head answering") : F(" &middot; head NOT answering");
    if (fwMsg.length()) { h += "<br>"; h += fwMsg; }
    h += F("</p><p><b>Stick directions</b> (tap to change, works straight away)<br>"
           "<a href='/dir?k=swap'>Swap left/right with up/down</a><br><a href='/dir?k=flx'>Reverse left/right</a><br><a href='/dir?k=fly'>Reverse up/down</a>");
    h += F("</p><form method=post action=/wifisave>Wi-Fi name (same as the head)<br><input name=s style='width:100%;font-size:16px' value='");
    h += wifiSsid;
    h += F("'><br><br>Wi-Fi password<br><input name=p type=password style='width:100%;font-size:16px'><br><br>"
           "Device password (leave empty to keep)<br><input name=d style='width:100%;font-size:16px'><br><br>"
           "<button style='font-size:16px;padding:10px 20px'>Save and restart</button></form>"
           "<p><a href='/fwgo'>Update this box from GitHub</a> &middot; <a href='/update'>Upload a .bin by hand</a></p>");
    web.send(200, "text/html", h);
  });
  web.on("/dir", []() {
    String k = web.arg("k");
    if (k == "swap") { swapAxes = !swapAxes; prefs.putBool("swap", swapAxes); }
    if (k == "flx")  { flipLR = !flipLR; prefs.putBool("flx", flipLR); }
    if (k == "fly")  { flipUD = !flipUD; prefs.putBool("fly", flipUD); }
    web.sendHeader("Location", "/"); web.send(302, "text/plain", "");
  });
  web.on("/wifisave", HTTP_POST, []() {
    String s = web.arg("s"), p = web.arg("p"), d = web.arg("d");
    s.trim();
    if (s.length()) { prefs.putString("ssid", s); if (p.length() || s != wifiSsid) prefs.putString("wpass", p); }
    if (d.length() >= 8) prefs.putString("dpass", d);
    web.send(200, "text/html", "<body style='font:16px sans-serif;padding:16px'>Saved. The box is restarting and will join that Wi-Fi. If it cannot, it brings back its own X70-Joystick Wi-Fi.");
    delay(800); ESP.restart();
  });
  web.on("/fwgo", []() { updateRequested = true; web.send(200, "text/html", "<body style='font:16px sans-serif;padding:16px'>Updating. The box restarts in about a minute. <a href='/'>Back</a>"); });
  webUpdater.setup(&web, "/update", "admin", devPass.c_str());
  web.begin();
}

void selfUpdate() {                            // downloads and installs the newest joystick firmware, then restarts
  WiFiClientSecure c; c.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  sendToHead("J,0.000,0.000");
  t_httpUpdate_return r = httpUpdate.update(c, String(FW_BASE) + FW_FILE);
  if (r == HTTP_UPDATE_FAILED) { fwMsg = "Update failed: " + httpUpdate.getLastErrorString(); Serial.println(fwMsg); }
}

void loop() {
  static uint32_t lastSend = 0, lastPrint = 0, lastTry = 0, bothSince = 0, lastBeat = 0, lastLookup = 0;
  static bool wasMoving = false, stickBusy = false; static int zerosLeft = 0;
  uint32_t now = millis();
  ArduinoOTA.handle();
  web.handleClient();
  bool wifi = !apMode && WiFi.status() == WL_CONNECTED;
  digitalWrite(PIN_LED, wifi ? HIGH : (now / (apMode ? 120 : 300)) % 2);   // slow blink = looking for Wi-Fi, fast blink = setup mode
  if (!apMode && !wifi && now - lastTry > 8000) { lastTry = now; WiFi.reconnect(); }
  if (wifi && now - lastBeat > 2000) { lastBeat = now; char m[24]; snprintf(m, sizeof m, "H,%d", FW_BUILD); sendToHead(m); }   // "I'm here, build N"
  if (cmdUdp.parsePacket() > 0) {
    char b[16]; int n = cmdUdp.read((uint8_t *)b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    if (b[0] == 'U') updateRequested = true;
    else if (b[0] == 'A') { headIP = cmdUdp.remoteIP(); headAckMs = now; headBuild = atoi(b + 2); }   // the head answered a heartbeat
  }
  // head not answering: look it up by its name (x70head.local). Only while the stick is at rest, it takes a moment.
  if (wifi && !stickBusy && (headAckMs == 0 || now - headAckMs > 10000) && (lastLookup == 0 || now - lastLookup > 15000)) {
    lastLookup = now;
    IPAddress ip = MDNS.queryHost("x70head", 1500);
    if (ip[0] != 0) { headIP = ip; Serial.printf("Found the head by name at %s\n", ip.toString().c_str()); }
  }
  if (updateRequested && wifi) { updateRequested = false; selfUpdate(); }

  if (now - lastSend >= 50) {                                     // 20 times a second
    lastSend = now;
    float a = readAxis(A), b = readAxis(B);
    float x = swapAxes ? a : b, y = swapAxes ? b : a;           // side sensor = left/right, top sensor = up/down
    if (flipLR) x = -x;
    if (flipUD) y = -y;
    bool moving = fabsf(x) > 0.05f || fabsf(y) > 0.05f;
    if (wasMoving && !moving) zerosLeft = 5;                      // tell the head clearly that the stick was let go
    if (wifi && (moving || zerosLeft > 0)) {
      char m[32]; snprintf(m, sizeof m, "J,%.3f,%.3f", moving ? x : 0.0f, moving ? y : 0.0f);
      sendToHead(m);
      if (!moving) zerosLeft--;
    }
    wasMoving = moving; stickBusy = moving || zerosLeft > 0;
    if (now - lastPrint > 500) { lastPrint = now; Serial.printf("x %+.2f  y %+.2f  %s  %s\n", x, y, wifi ? "wifi ok" : "NO WIFI",
                                                                  (headAckMs && now - headAckMs < 6000) ? "head ok" : "HEAD NOT ANSWERING"); }
  }
  if (spanDirty && now - spanDirtyMs > 3000) { spanDirty = false; saveSpan(A); saveSpan(B); }   // remember the learned range

  bool p1 = pressed(b1), p2 = pressed(b2);
  if (wifi && p1) sendEvent("P,0");                               // POWER button: go to preset 1
  if (wifi && p2) sendEvent("T");                                 // CW/CCW button: face tracking allowed / blocked
  bool both = digitalRead(PIN_BTN1) == LOW && digitalRead(PIN_BTN2) == LOW;
  if (!both) bothSince = now;
  else if (now - bothSince > 5000) {                              // both held 5 s: forget the learned range
    bothSince = now; A.lo = A.hi = B.lo = B.hi = MIN_SPAN; saveSpan(A); saveSpan(B);
    Serial.println(F("Stick range forgotten. Push it fully each way once."));
  }
}
