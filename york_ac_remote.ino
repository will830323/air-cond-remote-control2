/*
  York AC Remote – ESP32 Wi-Fi to IR bridge
  ------------------------------------------
  Your phone opens http://york-ac.local (or the IP shown in Serial Monitor),
  which serves the remote-control app. Each button press is sent to this
  ESP32, which transmits the matching infrared code to the York aircond.

  Libraries (Arduino IDE > Library Manager):
    - IRremoteESP8266  (v2.8.5 or newer – York protocol was added in 2.8.5)
  Board: any ESP32 dev board (ESP32 core 2.x or 3.x)

  Wiring (see README.md):
    IR LED (via NPN transistor)  -> GPIO 4
    IR receiver VS1838B / TSOP38238 OUT -> GPIO 14 (optional, for auto-detect
                                          and to follow the physical remote)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRutils.h>
#include "web_ui.h"

// ======================= USER SETTINGS =======================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "york-ac";          // -> http://york-ac.local
const char* AP_SSID       = "York-AC-Setup";    // fallback hotspot if Wi-Fi fails
const char* AP_PASSWORD   = "yorkac123";        // min 8 chars

const uint16_t IR_LED_PIN  = 4;
const uint16_t IR_RECV_PIN = 14;
const char*    DEFAULT_PROTOCOL = "YORK";       // change in the app's Settings
// =============================================================

const char* FW_VERSION = "1.0";

WebServer server(80);
Preferences prefs;
IRac ac(IR_LED_PIN);
IRrecv irrecv(IR_RECV_PIN, 1024, 50, true);
decode_results results;

struct AppState {
  bool power = false;
  String mode = "cool";     // cool | dry | fan | auto | heat
  int temp = 24;
  String fan = "auto";      // auto | low | medium | high
  bool swing = false;
  bool turbo = false;
  bool econo = false;
  bool sleep = false;
  int timerMin = 0;         // off timer length (0 = none)
} st;

String roomName = "Air Conditioner";
decode_type_t protocol = decode_type_t::YORK;
int16_t model = -1;
stdAc::state_t lastSent;          // what the AC is believed to be in now
unsigned long timerEndMs = 0;     // 0 = no timer running
unsigned long lastTxMs = 0;

// learn / auto-detect
bool learnMode = false;
unsigned long learnUntil = 0;
String learnStatus = "idle", learnProto = "", learnRaw = "";
bool learnSupported = false;
uint16_t learnLen = 0;

// ---------------------------------------------------------------- helpers
stdAc::opmode_t toOpmode(const String& m) {
  if (m == "dry")  return stdAc::opmode_t::kDry;
  if (m == "fan")  return stdAc::opmode_t::kFan;
  if (m == "auto") return stdAc::opmode_t::kAuto;
  if (m == "heat") return stdAc::opmode_t::kHeat;
  return stdAc::opmode_t::kCool;
}
String fromOpmode(stdAc::opmode_t m) {
  switch (m) {
    case stdAc::opmode_t::kDry:  return "dry";
    case stdAc::opmode_t::kFan:  return "fan";
    case stdAc::opmode_t::kAuto: return "auto";
    case stdAc::opmode_t::kHeat: return "heat";
    default:                     return "cool";
  }
}
stdAc::fanspeed_t toFan(const String& f) {
  if (f == "low")    return stdAc::fanspeed_t::kLow;
  if (f == "medium") return stdAc::fanspeed_t::kMedium;
  if (f == "high")   return stdAc::fanspeed_t::kHigh;
  return stdAc::fanspeed_t::kAuto;
}
String fromFan(stdAc::fanspeed_t f) {
  switch (f) {
    case stdAc::fanspeed_t::kMin:
    case stdAc::fanspeed_t::kLow:        return "low";
    case stdAc::fanspeed_t::kMedium:     return "medium";
    case stdAc::fanspeed_t::kMediumHigh:
    case stdAc::fanspeed_t::kHigh:
    case stdAc::fanspeed_t::kMax:        return "high";
    default:                             return "auto";
  }
}

stdAc::state_t buildIrState() {
  stdAc::state_t s;
  IRac::initState(&s);
  s.protocol = protocol;
  s.model    = model;
  s.power    = st.power;
  s.mode     = toOpmode(st.mode);
  s.celsius  = true;
  s.degrees  = st.temp;
  s.fanspeed = toFan(st.fan);
  s.swingv   = st.swing ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
  s.swingh   = stdAc::swingh_t::kOff;
  s.turbo    = st.turbo;
  s.econo    = st.econo;
  s.quiet    = false;
  s.light    = true;
  s.beep     = true;
  s.sleep    = st.sleep ? 0 : -1;
  s.clock    = -1;
  return s;
}

// IRac lists YORK as supported but has no York branch in sendAc(),
// so York is driven directly through its own class.
IRYorkAc yorkAc(IR_LED_PIN);

bool sendYork(const stdAc::state_t& desired) {
  // York uses a power *toggle* bit: only flip it when power actually changes.
  bool knownPrev = (lastSent.protocol == decode_type_t::YORK);
  bool toggle = knownPrev ? (desired.power != lastSent.power) : true;
  yorkAc.stateReset();
  yorkAc.setMode(IRYorkAc::convertMode(desired.mode));   // mode before fan
  yorkAc.setFan(IRYorkAc::convertFan(desired.fanspeed));
  yorkAc.setTemp((uint8_t)desired.degrees);
  yorkAc.setPowerToggle(toggle);
  yorkAc.calcChecksum();
  yorkAc.send();
  return true;
}

bool transmit() {
  stdAc::state_t desired = buildIrState();
  irrecv.pause();                       // don't decode our own LED
  bool ok = (protocol == decode_type_t::YORK) ? sendYork(desired)
                                              : ac.sendAc(desired, &lastSent);
  irrecv.resume();
  lastTxMs = millis();
  if (ok) lastSent = desired;
  Serial.printf("[IR] %s %s mode=%s %dC fan=%s -> %s\n",
                typeToString(protocol).c_str(), st.power ? "ON" : "OFF",
                st.mode.c_str(), st.temp, st.fan.c_str(), ok ? "sent" : "FAILED");
  return ok;
}

void saveState() {
  prefs.putBool("power", st.power);
  prefs.putString("mode", st.mode);
  prefs.putInt("temp", st.temp);
  prefs.putString("fan", st.fan);
  prefs.putBool("swing", st.swing);
  prefs.putBool("turbo", st.turbo);
  prefs.putBool("econo", st.econo);
  prefs.putBool("sleep", st.sleep);
}

void loadAll() {
  prefs.begin("yorkac", false);
  roomName = prefs.getString("name", roomName);
  String p = prefs.getString("proto", DEFAULT_PROTOCOL);
  decode_type_t t = strToDecodeType(p.c_str());
  protocol = (t != decode_type_t::UNKNOWN && IRac::isProtocolSupported(t)) ? t : decode_type_t::YORK;
  model = prefs.getShort("model", -1);
  st.power = prefs.getBool("power", false);
  st.mode  = prefs.getString("mode", "cool");
  st.temp  = prefs.getInt("temp", 24);
  st.fan   = prefs.getString("fan", "auto");
  st.swing = prefs.getBool("swing", false);
  st.turbo = prefs.getBool("turbo", false);
  st.econo = prefs.getBool("econo", false);
  st.sleep = prefs.getBool("sleep", false);
}

String jsonStr(const String& s) {
  String o = "\"";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

String stateJson() {
  long left = 0;
  if (timerEndMs) left = (long)(timerEndMs - millis()) / 1000;
  if (left < 0) left = 0;
  String j = "{";
  j += "\"power\":" + String(st.power ? "true" : "false");
  j += ",\"mode\":" + jsonStr(st.mode);
  j += ",\"temp\":" + String(st.temp);
  j += ",\"fan\":" + jsonStr(st.fan);
  j += ",\"swing\":" + String(st.swing ? "true" : "false");
  j += ",\"turbo\":" + String(st.turbo ? "true" : "false");
  j += ",\"econo\":" + String(st.econo ? "true" : "false");
  j += ",\"sleep\":" + String(st.sleep ? "true" : "false");
  j += ",\"timer\":" + String(timerEndMs ? st.timerMin : 0);
  j += ",\"timerLeft\":" + String(left);
  j += ",\"protocol\":" + jsonStr(typeToString(protocol));
  j += ",\"model\":" + String(model);
  j += ",\"name\":" + jsonStr(roomName);
  j += ",\"ip\":" + jsonStr(WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  j += ",\"fw\":" + jsonStr(FW_VERSION);
  j += "}";
  return j;
}

bool argBool(const char* k, bool cur) {
  if (!server.hasArg(k)) return cur;
  String v = server.arg(k);
  return v == "1" || v == "true" || v == "on";
}

// ---------------------------------------------------------------- web API
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/html", INDEX_HTML);
}

void handleGetState() { server.send(200, "application/json", stateJson()); }

void handlePostState() {
  st.power = argBool("power", st.power);
  if (server.hasArg("mode")) st.mode = server.arg("mode");
  if (server.hasArg("temp")) st.temp = constrain(server.arg("temp").toInt(), 16, 30);
  if (server.hasArg("fan"))  st.fan  = server.arg("fan");
  st.swing = argBool("swing", st.swing);
  st.turbo = argBool("turbo", st.turbo);
  st.econo = argBool("econo", st.econo);
  st.sleep = argBool("sleep", st.sleep);
  if (server.hasArg("timer")) {
    int t = constrain(server.arg("timer").toInt(), 0, 24 * 60);
    if (t != st.timerMin || (t > 0 && !timerEndMs)) {   // timer changed / (re)started
      st.timerMin = t;
      timerEndMs = (t > 0 && st.power) ? millis() + (unsigned long)t * 60000UL : 0;
    }
  }
  if (!st.power) { timerEndMs = 0; st.timerMin = 0; }
  if (st.power && st.timerMin > 0 && !timerEndMs)
    timerEndMs = millis() + (unsigned long)st.timerMin * 60000UL;

  bool ok = transmit();
  saveState();
  if (!ok) { server.send(500, "text/plain", "IR send failed for protocol " + typeToString(protocol)); return; }
  server.send(200, "application/json", stateJson());
}

void handleConfig() {
  if (server.hasArg("name")) {
    roomName = server.arg("name").substring(0, 24);
    prefs.putString("name", roomName);
  }
  if (server.hasArg("protocol")) {
    decode_type_t t = strToDecodeType(server.arg("protocol").c_str());
    if (t == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(t)) {
      server.send(400, "text/plain", "Unsupported protocol: " + server.arg("protocol"));
      return;
    }
    if (t != protocol) { protocol = t; model = -1; prefs.putShort("model", model); }
    prefs.putString("proto", typeToString(protocol));
    lastSent = stdAc::state_t();   // forget previous state for new protocol
  }
  if (server.hasArg("model")) { model = server.arg("model").toInt(); prefs.putShort("model", model); }
  server.send(200, "application/json", stateJson());
}

void handleLearnStart() {
  learnMode = true;
  learnUntil = millis() + 20000;
  learnStatus = "listening"; learnProto = ""; learnRaw = ""; learnLen = 0; learnSupported = false;
  server.send(200, "application/json", "{\"status\":\"listening\"}");
}

void handleLearnGet() {
  String j = "{\"status\":" + jsonStr(learnStatus);
  j += ",\"protocol\":" + jsonStr(learnProto);
  j += ",\"supported\":" + String(learnSupported ? "true" : "false");
  j += ",\"raw\":" + jsonStr(learnRaw);
  j += ",\"len\":" + String(learnLen) + "}";
  server.send(200, "application/json", j);
}

// ---------------------------------------------------------------- IR receive
void handleIrReceive() {
  if (!irrecv.decode(&results)) return;
  if (millis() - lastTxMs < 800) { irrecv.resume(); return; }   // our own echo

  decode_type_t t = results.decode_type;
  bool acProto = (t != decode_type_t::UNKNOWN) && IRac::isProtocolSupported(t);

  if (learnMode) {
    learnLen = getCorrectedRawLength(&results);
    if (t == decode_type_t::UNKNOWN) {
      if (learnLen > 20) learnStatus = "unknown";      // real signal, unknown protocol
    } else {
      learnStatus = "found";
      learnProto = typeToString(t);
      learnSupported = acProto;
      learnRaw = resultToHexidecimal(&results);
      if (acProto) {
        protocol = t;
        prefs.putString("proto", learnProto);
        stdAc::state_t s;
        if (IRAcUtils::decodeToState(&results, &s)) {
          model = s.model; prefs.putShort("model", model);
        }
      }
      learnMode = false;
      Serial.printf("[LEARN] Detected %s (%s)\n", learnProto.c_str(), acProto ? "A/C supported" : "not A/C");
    }
  } else if (acProto && t == protocol) {
    // Someone used the physical remote – keep the app in sync.
    stdAc::state_t s;
    if (IRAcUtils::decodeToState(&results, &s, &lastSent)) {
      st.power = s.power;
      if (s.mode != stdAc::opmode_t::kOff) st.mode = fromOpmode(s.mode);
      st.temp  = constrain((int)s.degrees, 16, 30);
      st.fan   = fromFan(s.fanspeed);
      st.swing = s.swingv != stdAc::swingv_t::kOff;
      st.turbo = s.turbo;
      st.econo = s.econo;
      st.sleep = s.sleep >= 0;
      if (!st.power) { timerEndMs = 0; st.timerMin = 0; }
      lastSent = s;
      saveState();
      Serial.println("[IR] Synced from physical remote");
    }
  }
  irrecv.resume();
}

// ---------------------------------------------------------------- setup/loop
void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(400); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi OK. Open http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("Wi-Fi failed. Hotspot '%s' (pw %s) -> http://%s\n",
                  AP_SSID, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nYork AC Remote bridge");
  loadAll();
  lastSent.protocol = decode_type_t::UNKNOWN;
  yorkAc.begin();
  irrecv.enableIRIn();
  startWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/state", HTTP_POST, handlePostState);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/learn", HTTP_POST, handleLearnStart);
  server.on("/api/learn", HTTP_GET, handleLearnGet);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.printf("Protocol: %s\n", typeToString(protocol).c_str());
}

void loop() {
  server.handleClient();
  handleIrReceive();

  if (learnMode && millis() > learnUntil) { learnMode = false; if (learnStatus == "listening") learnStatus = "timeout"; }

  if (timerEndMs && (long)(millis() - timerEndMs) >= 0) {
    Serial.println("[TIMER] Switching off");
    timerEndMs = 0; st.timerMin = 0; st.power = false;
    transmit(); saveState();
  }

  // reconnect if the router dropped us
  static unsigned long lastCheck = 0;
  if (WiFi.getMode() == WIFI_STA && millis() - lastCheck > 15000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }
}
