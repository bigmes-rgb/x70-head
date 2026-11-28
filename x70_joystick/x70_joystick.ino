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
String wifiSsid, wifiPass, wifiSsid2, wifiPass2, devPass;      // stored in the box (its own page)
bool apMode = false, updateRequested = false;
uint32_t wifiRestartAt = 0;
bool swapAxes = true, flipLR = false, flipUD = false;   // stick direction settings (box page)
bool trkInvert = false;                                 // false: CW/CCW button pressed in = face tracking allowed
bool trkWanted = true; uint32_t trkEdgeMs = 0;
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
  trkWanted = b2.last != trkInvert;
  evtId = esp_random() & 0xFFFFF;
  Serial.printf("\n=== X70 joystick box ===\nCentre: top sensor %d, side sensor %d (expect roughly 1500 to 2600)\n", A.centre, B.centre);
  if (A.centre < 300 || A.centre > 3800 || B.centre < 300 || B.centre > 3800)
    Serial.println(F("A centre reading is far off: check that sensor's three wires (3V3 / signal / GND)."));
  wifiSsid = prefs.getString("ssid", ""); wifiPass = prefs.getString("wpass", ""); wifiSsid2 = prefs.getString("ssid2", ""); wifiPass2 = prefs.getString("wpass2", ""); devPass = prefs.getString("dpass", DEFAULT_PASS);
  swapAxes = prefs.getBool("swap", true); flipLR = prefs.getBool("flx", false); flipUD = prefs.getBool("fly", false);
  trkInvert = prefs.getBool("trkinv", false);
  Serial.printf("Build %d\n", FW_BUILD);
  WiFi.setHostname("x70joystick");
  joinSavedWifi();
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
    h += F("</p><p><b>Face tracking button (CW/CCW)</b><br>Now: tracking ");
    h += trkWanted ? F("allowed") : F("blocked");
    h += F("<br><a href='/dir?k=trk'>Reverse this button</a>");
    h += F("</p><p><b>Addresses</b><br>This box: ");
    h += apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    h += F("<br>Head: ");
    if (headKnown()) { h += F("<a href='http://"); h += headIP.toString(); h += F("/'>"); h += headIP.toString(); h += F("</a>"); } else h += F("not found yet");
    h += F("</p><form method=post action=/wifisave><i>Easier: set the Wi-Fi on the head's page; it passes it on to this box.</i><br><br>Wi-Fi name (same as the head)<br><input name=s style='width:100%;font-size:16px' value='");
    h += wifiSsid;
    h += F("'><br><br>Wi-Fi password (leave empty to keep)<br><input name=p type=password style='width:100%;font-size:16px'><br><br><b>Second network (optional)</b>, for example the church if the first is home. It joins whichever is in range.<br>Wi-Fi name 2<br><input name=s2 style='width:100%;font-size:16px' value='");
    h += wifiSsid2;
    h += F("'><br><br>Wi-Fi password 2 (leave empty to keep)<br><input name=p2 type=password style='width:100%;font-size:16px'><br><br>"
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
    if (k == "trk")  { trkInvert = !trkInvert; prefs.putBool("trkinv", trkInvert); trkWanted = b2.last != trkInvert; sendEvent(trkWanted ? "T,1" : "T,0"); }
    web.sendHeader("Location", "/"); web.send(302, "text/plain", "");
  });
  web.on("/wifisave", HTTP_POST, []() {
    String s = web.arg("s"), p = web.arg("p"), d = web.arg("d");
    s.trim();
    String s2 = web.arg("s2"), p2 = web.arg("p2"); s2.trim();
    if (s.length()) { prefs.putString("ssid", s); if (p.length() || s != wifiSsid) prefs.putString("wpass", p); }
    if (web.hasArg("s2")) { prefs.putString("ssid2", s2); if (p2.length() || s2 != wifiSsid2) prefs.putString("wpass2", p2); }   // empty name = forget network 2
    if (d.length() >= 8) prefs.putString("dpass", d);
    web.send(200, "text/html", waitPage("Saved. The joystick box is restarting", "Still not back? If you moved it to a different Wi-Fi, put this phone or PC on that Wi-Fi and open x70joystick.local. If the name or password was wrong, join the Wi-Fi called X70-Joystick and open 192.168.4.1 to correct it.", "/", 45));
    delay(800); ESP.restart();
  });
  web.on("/fwgo", []() { updateRequested = true; web.send(200, "text/html", waitPage("Updating the joystick box", "This is taking longer than normal. Leave it powered, then open x70joystick.local again.", "/", 150)); });
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
  if (wifi && now - lastBeat > 2000) { lastBeat = now; char m[24]; snprintf(m, sizeof m, "H,%d,%d", FW_BUILD, trkWanted ? 1 : 0); sendToHead(m); }   // "I'm here, build N, tracking switch"
  if (cmdUdp.parsePacket() > 0) {
    char b[360]; int n = cmdUdp.read((uint8_t *)b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    if (b[0] == 'U' && b[1] == 0) updateRequested = true;
    else if (b[0] == 'W' && b[1] == '\t' && wifiRestartAt == 0) {             // the head passes on its Wi-Fi details and device password
      String f[5]; int k = 0; for (char *q = b + 2; *q && k < 5; q++) { if (*q == '\t') k++; else f[k] += *q; }
      if (k == 4 && f[0].length()) {
        prefs.putString("ssid", f[0]); prefs.putString("wpass", f[1]); prefs.putString("ssid2", f[2]); prefs.putString("wpass2", f[3]);
        if (f[4].length() >= 8) prefs.putString("dpass", f[4]);
        wifiRestartAt = now + 1500;
      }
    }
    else if (b[0] == 'A') { headIP = cmdUdp.remoteIP(); headAckMs = now; headBuild = atoi(b + 2); }   // the head answered a heartbeat
  }
  // head not answering: look it up by its name (x70head.local). Only while the stick is at rest, it takes a moment.
  if (wifi && !stickBusy && (headAckMs == 0 || now - headAckMs > 10000) && (lastLookup == 0 || now - lastLookup > 15000)) {
    lastLookup = now;
    IPAddress ip = MDNS.queryHost("x70head", 1500);
    if (ip[0] != 0) { headIP = ip; Serial.printf("Found the head by name at %s\n", ip.toString().c_str()); }
  }
  if (wifiRestartAt != 0 && (int32_t)(now - wifiRestartAt) >= 0) ESP.restart();
  if (updateRequested && wifi) { updateRequested = false; selfUpdate(); }

  if (now - lastSend >= 25) {                                     // 40 times a second, for a quick response
    lastSend = now;
    float a = readAxis(A), b = readAxis(B);
    float x = swapAxes ? a : b, y = swapAxes ? b : a;           // side sensor = left/right, top sensor = up/down
    if (flipLR) x = -x;
    if (flipUD) y = -y;
    bool moving = fabsf(x) > 0.05f || fabsf(y) > 0.05f;
    if (wasMoving && !moving) zerosLeft = 10;                      // tell the head clearly that the stick was let go
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

  bool p1 = pressed(b1);
  if (wifi && p1) sendEvent("P,0");                               // POWER button: go to preset 1
  // CW/CCW button is a switch: pressed in = face tracking allowed, out = blocked (reversible on the box page)
  bool in2 = digitalRead(PIN_BTN2) == LOW;
  if (in2 != b2.last && now - trkEdgeMs > 60) {
    b2.last = in2; trkEdgeMs = now; trkWanted = in2 != trkInvert;
    if (wifi) sendEvent(trkWanted ? "T,1" : "T,0");
  }
  bool both = digitalRead(PIN_BTN1) == LOW && digitalRead(PIN_BTN2) == LOW;
  if (!both) bothSince = now;
  else if (now - bothSince > 5000) {                              // both held 5 s: forget the learned range
    bothSince = now; A.lo = A.hi = B.lo = B.hi = MIN_SPAN; saveSpan(A); saveSpan(B);
    Serial.println(F("Stick range forgotten. Push it fully each way once."));
  }
}
