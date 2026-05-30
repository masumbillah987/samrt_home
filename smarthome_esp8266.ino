#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// ===== ASYNC MQTT LIBRARIES =====
#include <ESPAsyncTCP.h>
#include <AsyncMqttClient.h>

// ===== EEPROM =====
#define EEPROM_SIZE 512
#define WIFI_SSID_ADDR 0
#define WIFI_PASS_ADDR 32
#define STATE_DATA_ADDR 64
#define IR_CONFIG_ADDR 128 

char wifi_ssid[32];
char wifi_pass[32];

// ===== STATE STRUCTURE (persistent data) =====
struct StateData {
  int delayMicros;
  bool fanEnabled;
  bool light1State;
  bool light2State;
  uint32_t checksum;
} stateData;

// ===== IR CONFIG STRUCTURE =====
struct IRConfigData {
  uint32_t fanToggle;
  uint32_t speedUp;
  uint32_t speedDown;
  uint32_t light1Toggle;
  uint32_t light2Toggle;
  uint32_t checksum;
} irConfig;

// ===== HOTSPOT =====
const char* ap_ssid = "Smart Home";
const char* ap_pass = "12345678";

// ===== PINS =====
#define ZC_PIN D1
#define TRIAC_PIN D6
#define IR_PIN D5
#define LIGHT1_PIN D2
#define LIGHT2_PIN D8

// ===== SPEED =====
volatile int delayMicros = 5000;
const int MIN_DELAY = 1500;
const int MAX_DELAY = 9000;
bool fanEnabled = false;

// ===== LIGHT STATES =====
bool light1State = false;
bool light2State = false;

// ===== TRIAC =====
volatile bool zcDetected = false;
volatile bool triacOn = false;
volatile unsigned long zcTime = 0;
volatile unsigned long lastTriacFire = 0;
volatile unsigned long lastZCTime = 0;

const int TRIAC_PULSE = 10;
const int ZC_DEBOUNCE = 5000;

// ===== EEPROM SMART TIMER VARIABLES (10+ Years Life Extender) =====
bool stateSavePending = false;
unsigned long lastStateChangeTime = 0;
const unsigned long EEPROM_WRITE_DELAY = 10000; // স্টেট পরিবর্তনের ১০ সেকেন্ড পর সেভ হবে

// ===== STATE CHANGE FLAG - LONG-POLLING ENGINE =====
volatile bool stateChangedByIR = false;
bool hasPendingLongPoll = false;
unsigned long longPollStartTime = 0;
const unsigned long LONG_POLL_TIMEOUT = 20000; 

// ===== LIVE IR CAPTURE FOR LEARNING ENGINE =====
volatile bool remoteLearningActive = false;
volatile uint32_t lastCapturedIRCode = 0;
volatile bool newIRCodeCaptured = false;

// ===== IR =====
IRrecv irrecv(IR_PIN);
decode_results results;

// ===== SERVER =====
ESP8266WebServer server(80);

// ===== MQTT CLIENT CONFIGURATION (Global Auth Setup) =====
AsyncMqttClient mqttClient;
unsigned long lastMqttRetryTime = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

// 💡 বিশ্বব্যাপী নিরাপদ সংযোগের জন্য আপনার ব্রোকারের বিবরণ এখানে দিন
const char* MQTT_HOST = "broker.hivemq.com"; 
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "Masum_Home"; // আপনার ব্রোকার ইউজারনেম
const char* MQTT_PASS = "12345678"; // আপনার ব্রোকার পাসওয়ার্ড

const char* MQTT_TOPIC_SUB = "myuniquehome/control";
const char* MQTT_TOPIC_PUB = "myuniquehome/status";

// ===== TIMING VARIABLES =====
unsigned long lastIRCheckTime = 0;
const unsigned long IR_CHECK_INTERVAL = 50;  

unsigned long lastMDNSUpdateTime = 0;
const unsigned long MDNS_UPDATE_INTERVAL = 1000;  

// Forward Declaration
String getStateJSON();

// ===== CALCULATE CHECKSUM =====
uint32_t calculateChecksum(StateData& data) {
  uint32_t sum = 0;
  sum += data.delayMicros;
  sum += data.fanEnabled ? 1 : 0;
  sum += data.light1State ? 2 : 0;
  sum += data.light2State ? 4 : 0;
  return sum * 31;
}

uint32_t calculateIRChecksum(IRConfigData& data) {
  uint32_t sum = 0;
  sum += data.fanToggle;
  sum += data.speedUp;
  sum += data.speedDown;
  sum += data.light1Toggle;
  sum += data.light2Toggle;
  return sum * 17;
}

// ===== LOAD STATE FROM EEPROM =====
void loadState() {
  for(int i = 0; i < sizeof(StateData); i++) {
    ((byte*)&stateData)[i] = EEPROM.read(STATE_DATA_ADDR + i);
  }
  uint32_t calculatedChecksum = calculateChecksum(stateData);
  if(calculatedChecksum == stateData.checksum) {
    delayMicros = stateData.delayMicros;
    fanEnabled = stateData.fanEnabled;
    light1State = stateData.light1State;
    light2State = stateData.light2State;
    Serial.println("[EEPROM] State Loaded Successfully");
  }
}

// ===== LOAD IR CONFIG FROM EEPROM =====
void loadIRConfig() {
  for(int i = 0; i < sizeof(IRConfigData); i++) {
    ((byte*)&irConfig)[i] = EEPROM.read(IR_CONFIG_ADDR + i);
  }
  uint32_t calculatedChecksum = calculateIRChecksum(irConfig);
  if(calculatedChecksum != irConfig.checksum) {
    irConfig.speedDown = 0x807F0AF5;
    irConfig.speedUp = 0x807F8A75;
    irConfig.fanToggle = 0x807F30CF;
    irConfig.light1Toggle = 0x807F728D;
    irConfig.light2Toggle = 0x807FB04F;
  }
}

// ===== TRIGGER EEPROM WRITE TIMER =====
void triggerStateStorage() {
  stateSavePending = true;
  lastStateChangeTime = millis();
}

// ===== PRO-SAFE SAVE STATE TO EEPROM (Anti-Wear Engine) =====
void commitStateIfPending() {
  if (stateSavePending && (millis() - lastStateChangeTime >= EEPROM_WRITE_DELAY)) {
    stateSavePending = false;
    
    StateData newData;
    newData.delayMicros = delayMicros;
    newData.fanEnabled = fanEnabled;
    newData.light1State = light1State;
    newData.light2State = light2State;
    newData.checksum = calculateChecksum(newData);

    bool isChanged = false;
    for(int i = 0; i < sizeof(StateData); i++) {
      byte oldByte = EEPROM.read(STATE_DATA_ADDR + i);
      byte newByte = ((byte*)&newData)[i];
      if(oldByte != newByte) {
        EEPROM.write(STATE_DATA_ADDR + i, newByte);
        isChanged = true;
      }
    }

    if(isChanged) {
      EEPROM.commit();
      memcpy(&stateData, &newData, sizeof(StateData));
      Serial.println("[PRO-SAVED] State safely written to Flash Memory.");
    } else {
      Serial.println("[EEPROM] No status change. Commit bypassed to extend life.");
    }
  }
}

// ===== SAVE IR CONFIG TO EEPROM =====
void saveIRConfig() {
  irConfig.checksum = calculateIRChecksum(irConfig);
  bool isChanged = false;
  for(int i = 0; i < sizeof(IRConfigData); i++) {
    byte oldByte = EEPROM.read(IR_CONFIG_ADDR + i);
    byte newByte = ((byte*)&irConfig)[i];
    if(oldByte != newByte) {
      EEPROM.write(IR_CONFIG_ADDR + i, newByte);
      isChanged = true;
    }
  }
  if(isChanged) {
    EEPROM.commit();
    Serial.println("[SAVED] IR Config to EEPROM");
  }
}

// ===== APPLY SAVED OUTPUTS =====
void applySavedStates() {
  digitalWrite(LIGHT1_PIN, light1State ? HIGH : LOW);
  digitalWrite(LIGHT2_PIN, light2State ? HIGH : LOW);
  Serial.println("Saved outputs applied from EEPROM");
}

// ===== LOAD WIFI =====
void loadWiFi(){
 for(int i=0;i<32;i++) wifi_ssid[i]=EEPROM.read(WIFI_SSID_ADDR+i);
 for(int i=0;i<32;i++) wifi_pass[i]=EEPROM.read(WIFI_PASS_ADDR+i);
}

// ===== SAVE WIFI =====
void saveWiFi(String ssid,String pass){
 bool isChanged = false;
 for(int i=0;i<32;i++) {
   char currentS = (i < ssid.length()) ? ssid[i] : 0;
   char currentP = (i < pass.length()) ? pass[i] : 0;
   if(EEPROM.read(WIFI_SSID_ADDR+i) != currentS) { EEPROM.write(WIFI_SSID_ADDR+i, currentS); isChanged = true; }
   if(EEPROM.read(WIFI_PASS_ADDR+i) != currentP) { EEPROM.write(WIFI_PASS_ADDR+i, currentP); isChanged = true; }
 }
 if(isChanged) {
   EEPROM.commit();
 }
}

// ===== GET STATE JSON =====
String getStateJSON() {
  int percent = map(delayMicros, MAX_DELAY, MIN_DELAY, 0, 100);
  String json = "{";
  json += "\"fanEnabled\":" + String(fanEnabled ? "true" : "false") + ",";
  json += "\"light1State\":" + String(light1State ? "true" : "false") + ",";
  json += "\"light2State\":" + String(light2State ? "true" : "false") + ",";
  json += "\"fanSpeed\":" + String(percent);
  json += "}";
  return json;
}

// ===== SEND LONG POLL RESPONSE =====
void sendLongPollResponse(bool changed) {
  int percent = map(delayMicros, MAX_DELAY, MIN_DELAY, 0, 100);
  String json = "{";
  json += "\"fanEnabled\":" + String(fanEnabled ? "true" : "false") + ",";
  json += "\"light1State\":" + String(light1State ? "true" : "false") + ",";
  json += "\"light2State\":" + String(light2State ? "true" : "false") + ",";
  json += "\"fanSpeed\":" + String(percent) + ",";
  json += "\"changed\":" + String(changed ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
  hasPendingLongPoll = false;
}

// ===== MQTT STATUS PUBLISHER =====
void publishMqttStatus() {
  if (mqttClient.connected()) {
    String currentStatus = getStateJSON();
    mqttClient.publish(MQTT_TOPIC_PUB, 1, false, currentStatus.c_str());
  }
}

// ===== MAIN PAGE =====
String webpage() {
 String p = "<!DOCTYPE html><html><head>";
 p += "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>";
 p += "<style>";
 p += "* {margin:0; padding:0; box-sizing:border-box;}";
 p += "body {background:#0f172a; font-family:'Segoe UI', Arial, sans-serif; color:#fff; height:100vh; width:100vw; overflow:hidden; display:flex; justify-content:center; align-items:center; padding:10px;}";
 p += ".container {width:100%; max-width:420px; height:100%; display:flex; flex-direction:column; justify-content:space-between; padding:5px 0;}";
 p += ".header {text-align:center; font-size:24px; font-weight:bold; letter-spacing:1px; color:#fff; flex:0 0 auto;}";
 p += ".card {background:#1e293b; border-radius:16px; padding:15px; margin:5px 0; box-shadow:0 8px 32px rgba(0,0,0,0.4); color:#fff; display:flex; flex-direction:column; justify-content:center;}";
 p += ".fan-card {flex:1 1 auto;}";
 p += ".lights-card {flex:0 0 auto;}";
 p += ".wifi-card {flex:0 0 auto; background:transparent; box-shadow:none; padding:0; margin:5px 0 0 0;}";
 p += ".card-title {font-size:16px; font-weight:600; margin-bottom:10px; color:#60a5fa;}";
 p += ".button-group {display:grid; grid-template-columns:1fr 1fr; gap:10px; margin:8px 0;}";
 p += "button {padding:12px 15px; border:none; border-radius:10px; font-size:14px; font-weight:600; cursor:pointer; transition:all 0.3s ease; text-transform:uppercase; letter-spacing:0.5px; user-select:none;}";
 p += "button:active {transform:scale(0.95);}";
 p += ".btn-state {background:#475569; color:#94a3b8;}"; 
 p += ".active-on {background:#22c55e !important; color:white !important; box-shadow:0 4px 12px rgba(34, 197, 94, 0.3);}"; 
 p += ".active-off {background:#ef4444 !important; color:white !important; box-shadow:0 4px 12px rgba(239, 68, 68, 0.2);}";
 p += ".btn-row {display:grid; grid-template-columns:1fr 1fr; gap:10px;}"; 
 p += ".btn-wifi {background:#f59e0b; color:white; width:100%; padding:12px; box-shadow:0 4px 12px rgba(245, 158, 11, 0.2);}";
 p += ".btn-wifi:hover {background:#d97706;}";
 p += ".btn-remote {background:#3b82f6; color:white; width:100%; padding:12px; box-shadow:0 4px 12px rgba(59, 130, 246, 0.2);}"; 
 p += ".btn-remote:hover {background:#2563eb;}";
 p += ".fan-container {display:flex; justify-content:center; align-items:center; flex:1; min-height:60px;}";
 p += ".fan-icon {width:80px; height:80px; fill:#475569; transition:fill 0.3s ease;}"; 
 p += ".fan-icon.spinning {animation:spin 0.8s linear infinite; fill:#22c55e;}"; 
 p += "@keyframes spin {0% {transform:rotate(0deg);} 100% {transform:rotate(360deg);}}";
 p += ".status {text-align:center; font-size:14px; font-weight:600; color:#60a5fa; margin:5px 0;}";
 p += ".slider-container {padding:10px 0; display:flex; flex-direction:column; gap:5px;}";
 p += ".slider-label {display:flex; justify-content:between; font-size:12px; color:#94a3b8;}";
 p += "input[type=range] {-webkit-appearance:none; width:100%; background:transparent;}";
 p += "input[type=range]:focus {outline:none;}";
 p += "input[type=range]::-webkit-slider-runnable-track {width:100%; height:8px; cursor:pointer; background:#334155; border-radius:10px;}";
 p += "input[type=range]::-webkit-slider-thumb {-webkit-appearance:none; height:20px; width:20px; border-radius:50%; background:#22c55e; cursor:pointer; margin-top:-6px; box-shadow:0 0 8px rgba(34,197,94,0.5); transition:background 0.3s;}";
 p += "input[type=range]:disabled::-webkit-slider-thumb {background:#475569; box-shadow:none; cursor:not-allowed;}";
 p += ".light-item {background:#0f172a; border-radius:12px; padding:10px 12px; margin:5px 0; border:1px solid #334155; display:flex; flex-direction:column;}";
 p += ".light-label {font-size:14px; font-weight:600; color:#e2e8f0; margin-bottom:5px; transition:color 0.3s;}";
 p += ".light-active {color:#22c55e !important;}"; 
 p += "</style>";
 p += "<script>";
 p += "let currentSpeed = 50; let isUpdatingSlider = false;";
 p += "async function updateState(action) {";
 p += "try { const response = await fetch('/api/' + action); if(response.ok) { const data = await response.json(); updateUI(data); } } catch(e) {console.error('Error:', e);}";
 p += "}";
 p += "async function handleSliderChange(val) {";
 p += "if(isUpdatingSlider) return; isUpdatingSlider = true; let target = parseInt(val); let diff = target - currentSpeed; let steps = Math.round(Math.abs(diff) / 5.5); let action = diff > 0 ? 'up' : 'down';";
 p += "document.getElementById('speedLabel').textContent = target + '%';";
 p += "for(let i=0; i<steps; i++) { await fetch('/api/' + action); }";
 p += "const res = await fetch('/api/status'); const data = await res.json(); updateUI(data); isUpdatingSlider = false;";
 p += "}";
 p += "function updateUI(data) {";
 p += "currentSpeed = data.fanSpeed; document.getElementById('fanStatus').textContent = 'Fan is ' + (data.fanEnabled ? 'ON' : 'OFF'); document.getElementById('light1Status').textContent = 'Light 1: ' + (data.light1State ? 'ON' : 'OFF'); document.getElementById('light2Status').textContent = 'Light 2: ' + (data.light2State ? 'ON' : 'OFF');";
 p += "if(!isUpdatingSlider) { document.getElementById('fanSlider').value = data.fanSpeed; document.getElementById('speedLabel').textContent = data.fanSpeed + '%'; }";
 p += "document.getElementById('fanSlider').disabled = !data.fanEnabled;";
 p += "const fanIcon = document.getElementById('fanIcon'); if(data.fanEnabled) { fanIcon.classList.add('spinning'); document.getElementById('btnFanOn').classList.add('active-on'); document.getElementById('btnFanOff').classList.remove('active-off'); } else { fanIcon.classList.remove('spinning'); document.getElementById('btnFanOn').classList.remove('active-on'); document.getElementById('btnFanOff').classList.add('active-off'); }";
 p += "if(data.light1State) { document.getElementById('light1Status').classList.add('light-active'); document.getElementById('btnL1On').classList.add('active-on'); document.getElementById('btnL1Off').classList.remove('active-off'); } else { document.getElementById('light1Status').classList.remove('light-active'); document.getElementById('btnL1On').classList.remove('active-on'); document.getElementById('btnL1Off').classList.add('active-off'); }";
 p += "if(data.light2State) { document.getElementById('light2Status').classList.add('light-active'); document.getElementById('btnL2On').classList.add('active-on'); document.getElementById('btnL2Off').classList.remove('active-off'); } else { document.getElementById('light2Status').classList.remove('light-active'); document.getElementById('btnL2On').classList.remove('active-on'); document.getElementById('btnL2Off').classList.add('active-off'); }";
 p += "}";
 p += "async function listenForIRChanges() { while(true) { try { const response = await fetch('/api/changed'); if(response.ok) { const data = await response.json(); if(data.changed) { updateUI(data); } } } catch(e) { await new Promise(r => setTimeout(r, 2000)); } } }";
 p += "window.addEventListener('load', () => { fetch('/api/status').then(r => r.json()).then(data => { updateUI(data); listenForIRChanges(); }).catch(e => console.error(e)); });";
 p += "</script></head><body>";
 p += "<div class='container'><div class='header'>Smart Home</div>";
 p += "<div class='card fan-card'><div class='card-title'>Fan Control</div><div class='fan-container'><svg id='fanIcon' class='fan-icon' viewBox='0 0 100 100'><circle cx='50' cy='50' r='6' fill='white'/><path d='M50 10 Q70 30 50 50 Q30 30 50 10'/><path d='M90 50 Q70 70 50 50 Q70 30 90 50'/><path d='M50 90 Q30 70 50 50 Q70 70 50 90'/><path d='M10 50 Q30 30 50 50 Q30 70 10 50'/></svg></div><div class='status' id='fanStatus'>Fan is OFF</div><div class='button-group'><button id='btnFanOn' class='btn-state' onclick='updateState(\"on\")'>On</button><button id='btnFanOff' class='btn-state' onclick='updateState(\"off\")'>Off</button></div><div class='slider-container'><div class='slider-label'><span>Speed</span><span id='speedLabel'>50%</span></div><input type='range' id='fanSlider' min='0' max='100' value='50' onchange='handleSliderChange(this.value)'></div></div>";
 p += "<div class='card lights-card'><div class='card-title'>Lights Control</div><div class='light-item'><div class='light-label' id='light1Status'>Light 1: OFF</div><div class='button-group'><button id='btnL1On' class='btn-state' onclick='updateState(\"l1on\")'>On</button><button id='btnL1Off' class='btn-state' onclick='updateState(\"l1off\")'>Off</button></div></div><div class='light-item'><div class='light-label' id='light2Status'>Light 2: OFF</div><div class='button-group'><button id='btnL2On' class='btn-state' onclick='updateState(\"l2on\")'>On</button><button id='btnL2Off' class='btn-state' onclick='updateState(\"l2off\")'>Off</button></div></div></div>";
 p += "<div class='card wifi-card'><div class='btn-row'><button class='btn-wifi' onclick=\"location.href='/wifi'\">WiFi Setup</button><button class='btn-remote' onclick=\"location.href='/remote'\">Remote Setup</button></div></div></div></body></html>";
 return p;
}

// ===== WIFI PAGE =====
String wifiPage(){
 String p="<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'><style>*{margin:0; padding:0; box-sizing:border-box;}body {background:#0f172a; font-family:'Segoe UI', Arial, sans-serif; color:#fff; height:100vh; width:100vw; overflow:hidden; display:flex; align-items:center; justify-content:center; padding:20px;}.card {background:#1e293b; border-radius:18px; padding:30px; width:100%; max-width:360px; box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title {font-size:24px; font-weight:bold; text-align:center; margin-bottom:20px; color:#60a5fa;}input {width:100%; padding:12px; margin:10px 0; border:2px solid #334155; background:#0f172a; border-radius:10px; font-size:14px; transition:0.3s; color:#fff;}input:focus {outline:none; border-color:#60a5fa; box-shadow:0 0 10px rgba(96, 165, 250, 0.2);}label {display:block; font-weight:600; margin-top:10px; color:#e2e8f0;}button {width:100%; padding:12px; background:#f59e0b; color:white; border:none; border-radius:10px; font-size:15px; font-weight:600; margin-top:20px; cursor:pointer; transition:0.3s; text-transform:uppercase; letter-spacing:1px;}button:hover {background:#d97706; box-shadow:0 6px 20px rgba(245, 158, 11, 0.4);}</style></head><body><div class='card'><div class='title'>WiFi Settings</div><form action='/save'><label>SSID</label><input type='text' name='s' placeholder='Network name' required><label>Password</label><input type='password' name='p' placeholder='Password' required><button type='submit'>Save</button></form></div></body></html>";
 return p;
}

// ===== REMOTE CONFIGURATION PAGE =====
String remotePage(){
 String p="<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'><style>*{margin:0; padding:0; box-sizing:border-box;}body {background:#0f172a; font-family:'Segoe UI', Arial, sans-serif; color:#fff; min-height:100vh; width:100vw; display:flex; align-items:center; justify-content:center; padding:20px;}.card {background:#1e293b; border-radius:18px; padding:25px; width:100%; max-width:400px; box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title {font-size:22px; font-weight:bold; text-align:center; margin-bottom:5px; color:#60a5fa;}.desc {font-size:13px; color:#a3e635; text-align:center; margin-bottom:15px; font-weight:600;}.input-wrapper {position:relative; margin-bottom:12px;}input {width:100%; padding:10px; margin:4px 0 0 0; border:2px solid #334155; background:#0f172a; border-radius:10px; font-size:14px; color:#fff; font-family:monospace;}input:focus {outline:none; border-color:#3b82f6;}.focused-field {border-color:#22c55e !important; box-shadow:0 0 10px rgba(34,197,94,0.3);}label {display:block; font-weight:600; font-size:13px; color:#e2e8f0;}button {width:100%; padding:12px; background:#3b82f6; color:white; border:none; border-radius:10px; font-size:14px; font-weight:600; margin-top:10px; cursor:pointer; transition:0.3s; text-transform:uppercase; letter-spacing:1px;}button:hover {background:#2563eb; box-shadow:0 6px 20px rgba(59, 130, 246, 0.4);}.back-btn {background:#475569; margin-top:8px;}.back-btn:hover {background:#334155; box-shadow:none;}</style>";
 p+="<script>let activeInputId = 'ft'; function setTarget(id) { document.querySelectorAll('input').forEach(el=>el.classList.remove('focused-field')); activeInputId = id; document.getElementById(id).classList.add('focused-field'); } async function poolLiveRemoteCodes() { while(true) { try { let res = await fetch('/api/get_live_ir'); if(res.ok) { let data = await res.json(); if(data.captured && data.code !== '0x0') { document.getElementById(activeInputId).value = data.code; } } } catch(e) {} await new Promise(r => setTimeout(r, 300)); } } window.addEventListener('load', () => { setTarget('ft'); poolLiveRemoteCodes(); });</script>";
 p+="</head><body><div class='card'><div class='title'>Remote Setup</div><div class='desc'>⚡ Tap an input field, then press remote button!</div><form action='/save_remote'>";
 p+="<label>Fan Toggle Key Code</label><div class='input-wrapper'><input type='text' id='ft' name='ft' onfocus='setTarget(\"ft\")' value='0x" + String(irConfig.fanToggle, HEX) + "' readonly required></div>";
 p+="<label>Fan Speed Up Key Code</label><div class='input-wrapper'><input type='text' id='su' name='su' onfocus='setTarget(\"su\")' value='0x" + String(irConfig.speedUp, HEX) + "' readonly required></div>";
 p+="<label>Fan Speed Down Key Code</label><div class='input-wrapper'><input type='text' id='sd' name='sd' onfocus='setTarget(\"sd\")' value='0x" + String(irConfig.speedDown, HEX) + "' readonly required></div>";
 p+="<label>Light 1 Toggle Key Code</label><div class='input-wrapper'><input type='text' id='l1' name='l1' onfocus='setTarget(\"l1\")' value='0x" + String(irConfig.light1Toggle, HEX) + "' readonly required></div>";
 p+="<label>Light 2 Toggle Key Code</label><div class='input-wrapper'><input type='text' id='l2' name='l2' onfocus='setTarget(\"l2\")' value='0x" + String(irConfig.light2Toggle, HEX) + "' readonly required></div>";
 p+="<button type='submit'>Save IR Configuration</button><button type='button' class='back-btn' onclick=\"location.href='/'\">Back</button></form></div></body></html>";
 return p;
}

// ===== ROUTES =====
void root(){ server.send(200,"text/html",webpage()); }
void apiStatus(){ server.send(200, "application/json", getStateJSON()); }
void apiChanged(){
  if (stateChangedByIR) {
    stateChangedByIR = false;
    sendLongPollResponse(true);
  } else {
    hasPendingLongPoll = true;
    longPollStartTime = millis();
  }
}

void apiOn(){ fanEnabled=true; triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiOff(){ fanEnabled=false; triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiSpeedUp(){ if(delayMicros>MIN_DELAY)delayMicros-=500; triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiSpeedDown(){ if(delayMicros<MAX_DELAY)delayMicros+=500; triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL1On(){ light1State=true; digitalWrite(LIGHT1_PIN,HIGH); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL1Off(){ light1State=false; digitalWrite(LIGHT1_PIN,LOW); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL2On(){ light2State=true; digitalWrite(LIGHT2_PIN,HIGH); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL2Off(){ light2State=false; digitalWrite(LIGHT2_PIN,LOW); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void wifi(){ server.send(200,"text/html",wifiPage()); }
void remote(){ lastCapturedIRCode = 0; newIRCodeCaptured = false; remoteLearningActive = true; server.send(200,"text/html",remotePage()); }
void apiGetLiveIR(){ String json = "{ \"captured\":" + String(newIRCodeCaptured ? "true" : "false") + ", \"code\":\"0x" + String(lastCapturedIRCode, HEX) + "\" }"; newIRCodeCaptured = false; server.send(200, "application/json", json); }

void save(){ String s=server.arg("s"); String p=server.arg("p"); saveWiFi(s,p); server.send(200,"text/html","Saved. Rebooting..."); ESP.restart(); }
void saveRemote(){
  irConfig.fanToggle = strtoul(server.arg("ft").c_str(), NULL, 16);
  irConfig.speedUp = strtoul(server.arg("su").c_str(), NULL, 16);
  irConfig.speedDown = strtoul(server.arg("sd").c_str(), NULL, 16);
  irConfig.light1Toggle = strtoul(server.arg("l1").c_str(), NULL, 16);
  irConfig.light2Toggle = strtoul(server.arg("l2").c_str(), NULL, 16);
  saveIRConfig(); remoteLearningActive = false;
  server.send(200,"text/html","IR Config Saved. Returning Home...<script>setTimeout(function(){location.href='/';},1500);</script>");
}

// ===== MQTT EVENT HANDLERS =====
void onMqttConnect(bool sessionPresent) {
  Serial.println("[MQTT] Connected to Broker!");
  mqttClient.subscribe(MQTT_TOPIC_SUB, 1); 
  publishMqttStatus();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("[MQTT] Disconnected.");
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  char message[len + 1];
  memcpy(message, payload, len);
  message[len] = '\0';
  String cmd = String(message);

  Serial.print("[MQTT] Received: ");
  Serial.println(cmd);

  bool changed = false;
  
  // এই নতুন অংশটি যোগ করা হলো
  if (cmd.startsWith("SPEED_")) {
    int val = cmd.substring(6).toInt(); 
    // স্লাইডারের মান (0-100) কে ডিলে মাইক্রোসেকেন্ডে কনভার্ট করা
    delayMicros = map(val, 0, 100, MAX_DELAY, MIN_DELAY);
    changed = true;
  }
  else if (cmd == "FAN_ON")   { fanEnabled = true; changed = true; }
  else if (cmd == "FAN_OFF") { fanEnabled = false; changed = true; }
  else if (cmd == "FAN_UP")    { if(delayMicros > MIN_DELAY) { delayMicros -= 500; changed = true; } }
  else if (cmd == "FAN_DOWN") { if(delayMicros < MAX_DELAY) { delayMicros += 500; changed = true; } }
  else if (cmd == "L1_ON")    { light1State = true; digitalWrite(LIGHT1_PIN, HIGH); changed = true; }
  else if (cmd == "L1_OFF")   { light1State = false; digitalWrite(LIGHT1_PIN, LOW); changed = true; }
  else if (cmd == "L2_ON")    { light2State = true; digitalWrite(LIGHT2_PIN, HIGH); changed = true; }
  else if (cmd == "L2_OFF")   { light2State = false; digitalWrite(LIGHT2_PIN, LOW); changed = true; }

  if (changed) {
    triggerStateStorage(); 
    stateChangedByIR = true; 
    publishMqttStatus();     
  }
}

// ===== ZERO CROSS ISR =====
void ICACHE_RAM_ATTR zeroCrossISR(){
 unsigned long now=micros();
 if(now-lastZCTime<ZC_DEBOUNCE)return;
 lastZCTime=now; zcDetected=true; zcTime=now; digitalWrite(TRIAC_PIN,LOW);
}

// ===== TRIAC HANDLING =====
inline void handleTriac(){
 if(!zcDetected) return;
 unsigned long now=micros();

 if(!triacOn && fanEnabled && (now-zcTime)>=delayMicros){
  digitalWrite(TRIAC_PIN,HIGH);
  triacOn=true;
  lastTriacFire=now;
 }

 if(triacOn && (now-lastTriacFire)>=TRIAC_PULSE){
  digitalWrite(TRIAC_PIN,LOW);
  triacOn=false;
  zcDetected=false;
 }
}

// ===== IR HANDLING =====
void handleIR(){
 if(!irrecv.decode(&results)) return;
 uint32_t code = results.value;
 irrecv.resume();

 if(code == 0xFFFFFFFF || code == 0x0) return;
 if(remoteLearningActive) { lastCapturedIRCode = code; newIRCodeCaptured = true; return; }

 bool targetHit = false;
 if(code==irConfig.speedDown && delayMicros>MIN_DELAY){ delayMicros-=500; targetHit = true; Serial.println("[IR] Speed Down"); }
 else if(code==irConfig.speedUp && delayMicros<MAX_DELAY){ delayMicros+=500; targetHit = true; Serial.println("[IR] Speed Up"); }
 else if(code==irConfig.fanToggle){ fanEnabled=!fanEnabled; targetHit = true; Serial.println("[IR] Fan Toggle"); }
 else if(code==irConfig.light1Toggle){ light1State=!light1State; digitalWrite(LIGHT1_PIN,light1State); targetHit = true; Serial.println("[IR] Light 1 Toggle"); }
 else if(code==irConfig.light2Toggle){ light2State=!light2State; digitalWrite(LIGHT2_PIN,light2State); targetHit = true; Serial.println("[IR] Light 2 Toggle"); }

 if(targetHit) {
   triggerStateStorage(); // ১০ সেকেন্ড ডিফার্ড সেভ চালু করবে (মেমোরি প্রোটেকশন)
   stateChangedByIR = true; 
   publishMqttStatus();
 }
}

void startWiFi(){
 WiFi.mode(WIFI_AP_STA);
 WiFi.softAP(ap_ssid,ap_pass);
 WiFi.begin(wifi_ssid,wifi_pass);
}

// ===== CONNECT TO MQTT ASYNC ENGINE =====
void connectToMqtt() {
  Serial.println("[MQTT] Connecting to Broker...");
  
  // সিকিউর গ্লোবাল অ্যাক্সেসের জন্য ক্রেডেন্সিয়াল যুক্ত করা হয়েছে
  mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  
  mqttClient.connect();
}

// ===== SETUP =====
void setup(){
 Serial.begin(115200);
 Serial.println("\n\n=== SMART HOME STARTING ===");

 EEPROM.begin(EEPROM_SIZE); // মেমোরি লাইফ বাড়াতে সেটাপ-এ একবারই ইনিশিয়েশন

 pinMode(ZC_PIN,INPUT); pinMode(TRIAC_PIN,OUTPUT); pinMode(LIGHT1_PIN,OUTPUT); pinMode(LIGHT2_PIN,OUTPUT);
 digitalWrite(LIGHT1_PIN,LOW); digitalWrite(LIGHT2_PIN,LOW);

 attachInterrupt(digitalPinToInterrupt(ZC_PIN),zeroCrossISR,RISING);
 irrecv.enableIRIn();

 loadWiFi(); loadState(); loadIRConfig(); startWiFi(); applySavedStates();
 MDNS.begin("smarthome");

 // MQTT Callback Registrations
 mqttClient.onConnect(onMqttConnect);
 mqttClient.onDisconnect(onMqttDisconnect);
 mqttClient.onMessage(onMqttMessage);
 mqttClient.setServer(MQTT_HOST, MQTT_PORT); 

 server.on("/",root); server.on("/api/status",apiStatus); server.on("/api/changed",apiChanged);  
 server.on("/api/on",apiOn); server.on("/api/off",apiOff); server.on("/api/up",apiSpeedUp); server.on("/api/down",apiSpeedDown);
 server.on("/api/l1on",apiL1On); server.on("/api/l1off",apiL1Off); server.on("/api/l2on",apiL2On); server.on("/api/l2off",apiL2Off);
 server.on("/wifi",wifi); server.on("/remote",remote); server.on("/api/get_live_ir",apiGetLiveIR); server.on("/save",save); server.on("/save_remote",saveRemote); 

 server.begin();
 Serial.println("=== READY ===");
}

// ===== MAIN LOOP =====
void loop(){
 unsigned long currentMillis = millis();

 handleTriac();

 if(currentMillis - lastIRCheckTime >= IR_CHECK_INTERVAL) {
    lastIRCheckTime = currentMillis;
    handleIR();
 }

 // ১০ বছরের লাইফ-টাইম এক্সটেন্ডার ব্যাকগ্রাউন্ড টাস্ক চেক
 commitStateIfPending();

 if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (currentMillis - lastMqttRetryTime >= MQTT_RETRY_INTERVAL) {
      lastMqttRetryTime = currentMillis;
      connectToMqtt();
    }
 }

 if(hasPendingLongPoll) {
    if(stateChangedByIR) {
       stateChangedByIR = false;
       sendLongPollResponse(true);
    } else if (currentMillis - longPollStartTime >= LONG_POLL_TIMEOUT) {
       sendLongPollResponse(false);
    }
 }
 server.handleClient();

 if(currentMillis - lastMDNSUpdateTime >= MDNS_UPDATE_INTERVAL) {
    lastMDNSUpdateTime = currentMillis;
    MDNS.update();
 }
}
