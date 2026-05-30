#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// ===== EEPROM =====
#define EEPROM_SIZE 512
#define WIFI_SSID_ADDR 0
#define WIFI_PASS_ADDR 32
#define STATE_DATA_ADDR 64

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

// ===== AUTO-SAVE TIMER =====
unsigned long lastAutoSaveTime = 0;
const unsigned long AUTO_SAVE_INTERVAL = 5000;

// ===== STATE CHANGE FLAG - LONG-POLLING ENGINE =====
volatile bool stateChangedByIR = false;
bool hasPendingLongPoll = false;
unsigned long longPollStartTime = 0;
const unsigned long LONG_POLL_TIMEOUT = 20000; // Hold connection for 20 seconds max

// ===== IR =====
IRrecv irrecv(IR_PIN);
decode_results results;

// ===== SERVER =====
ESP8266WebServer server(80);

// ===== TIMING VARIABLES =====
unsigned long lastIRCheckTime = 0;
const unsigned long IR_CHECK_INTERVAL = 50;  // Check IR every 50ms

unsigned long lastMDNSUpdateTime = 0;
const unsigned long MDNS_UPDATE_INTERVAL = 1000;  // Update mDNS every 1s

// ===== CALCULATE CHECKSUM =====
uint32_t calculateChecksum(StateData& data) {
  uint32_t sum = 0;
  sum += data.delayMicros;
  sum += data.fanEnabled ? 1 : 0;
  sum += data.light1State ? 2 : 0;
  sum += data.light2State ? 4 : 0;
  return sum * 31;
}

// ===== LOAD STATE FROM EEPROM =====
void loadState() {
  EEPROM.begin(EEPROM_SIZE);
  
  for(int i = 0; i < sizeof(StateData); i++) {
    ((byte*)&stateData)[i] = EEPROM.read(STATE_DATA_ADDR + i);
  }
  
  uint32_t calculatedChecksum = calculateChecksum(stateData);
  if(calculatedChecksum == stateData.checksum) {
    delayMicros = stateData.delayMicros;
    fanEnabled = stateData.fanEnabled;
    light1State = stateData.light1State;
    light2State = stateData.light2State;
  }
}

// ===== SAVE STATE TO EEPROM =====
void saveState() {
  stateData.delayMicros = delayMicros;
  stateData.fanEnabled = fanEnabled;
  stateData.light1State = light1State;
  stateData.light2State = light2State;
  stateData.checksum = calculateChecksum(stateData);
  
  for(int i = 0; i < sizeof(StateData); i++) {
    EEPROM.write(STATE_DATA_ADDR + i, ((byte*)&stateData)[i]);
  }
  EEPROM.commit();
  
  Serial.println("[SAVED] State to EEPROM");
  lastAutoSaveTime = millis();
}

// ===== APPLY SAVED OUTPUTS =====
void applySavedStates() {
  digitalWrite(LIGHT1_PIN, light1State ? HIGH : LOW);
  digitalWrite(LIGHT2_PIN, light2State ? HIGH : LOW);
  Serial.println("Saved outputs applied from EEPROM");
}

// ===== LOAD WIFI =====
void loadWiFi(){
 EEPROM.begin(EEPROM_SIZE);
 for(int i=0;i<32;i++) wifi_ssid[i]=EEPROM.read(WIFI_SSID_ADDR+i);
 for(int i=0;i<32;i++) wifi_pass[i]=EEPROM.read(WIFI_PASS_ADDR+i);
}

// ===== SAVE WIFI =====
void saveWiFi(String ssid,String pass){
 for(int i=0;i<32;i++) EEPROM.write(WIFI_SSID_ADDR+i,ssid[i]);
 for(int i=0;i<32;i++) EEPROM.write(WIFI_PASS_ADDR+i,pass[i]);
 EEPROM.commit();
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

// ===== MAIN PAGE - WITH SLIDER & ASH TO GREEN TOGGLE COLORS =====
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
 
 p += ".btn-wifi {background:#f59e0b; color:white; width:100%; padding:12px; box-shadow:0 4px 12px rgba(245, 158, 11, 0.2);}";
 p += ".btn-wifi:hover {background:#d97706;}";
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
 p += "let currentSpeed = 50;";
 p += "let isUpdatingSlider = false;";
 
 p += "async function updateState(action) {";
 p += "try {";
 p += "const response = await fetch('/api/' + action);";
 p += "if(response.ok) {";
 p += "const data = await response.json();";
 p += "updateUI(data);";
 p += "}";
  p += "} catch(e) {console.error('Error:', e);}";
 p += "}";
 
 // FIXED SLIDER IMPLEMENTATION (Runs sequentially using await)
 p += "async function handleSliderChange(val) {";
 p += "if(isUpdatingSlider) return;";
 p += "isUpdatingSlider = true;";
 p += "let target = parseInt(val);";
 p += "let diff = target - currentSpeed;";
 p += "let steps = Math.round(Math.abs(diff) / 5.5);"; 
 p += "let action = diff > 0 ? 'up' : 'down';";
 p += "document.getElementById('speedLabel').textContent = target + '%';";
 p += "for(let i=0; i<steps; i++) {";
 p += "  await fetch('/api/' + action);";
 p += "}";
 p += "const res = await fetch('/api/status');";
 p += "const data = await res.json();";
 p += "updateUI(data);";
 p += "isUpdatingSlider = false;";
 p += "}";
 
 p += "function updateUI(data) {";
 p += "currentSpeed = data.fanSpeed;";
 p += "document.getElementById('fanStatus').textContent = 'Fan is ' + (data.fanEnabled ? 'ON' : 'OFF');";
 p += "document.getElementById('light1Status').textContent = 'Light 1: ' + (data.light1State ? 'ON' : 'OFF');";
 p += "document.getElementById('light2Status').textContent = 'Light 2: ' + (data.light2State ? 'ON' : 'OFF');";
 
 p += "if(!isUpdatingSlider) {";
 p += "document.getElementById('fanSlider').value = data.fanSpeed;";
 p += "document.getElementById('speedLabel').textContent = data.fanSpeed + '%';";
 p += "}";
 p += "document.getElementById('fanSlider').disabled = !data.fanEnabled;";
 
 p += "const fanIcon = document.getElementById('fanIcon');";
 p += "if(data.fanEnabled) {";
 p += "fanIcon.classList.add('spinning');";
 p += "document.getElementById('btnFanOn').classList.add('active-on');";
 p += "document.getElementById('btnFanOff').classList.remove('active-off');";
 p += "} else {";
 p += "fanIcon.classList.remove('spinning');";
 p += "document.getElementById('btnFanOn').classList.remove('active-on');";
 p += "document.getElementById('btnFanOff').classList.add('active-off');";
 p += "}";
 
 p += "if(data.light1State) {";
 p += "document.getElementById('light1Status').classList.add('light-active');";
 p += "document.getElementById('btnL1On').classList.add('active-on');";
 p += "document.getElementById('btnL1Off').classList.remove('active-off');";
 p += "} else {";
 p += "document.getElementById('light1Status').classList.remove('light-active');";
 p += "document.getElementById('btnL1On').classList.remove('active-on');";
 p += "document.getElementById('btnL1Off').classList.add('active-off');";
 p += "}";
 
 p += "if(data.light2State) {";
 p += "document.getElementById('light2Status').classList.add('light-active');";
 p += "document.getElementById('btnL2On').classList.add('active-on');";
 p += "document.getElementById('btnL2Off').classList.remove('active-off');";
 p += "} else {";
 p += "document.getElementById('light2Status').classList.remove('light-active');";
 p += "document.getElementById('btnL2On').classList.remove('active-on');";
 p += "document.getElementById('btnL2Off').classList.add('active-off');";
 p += "}";
 p += "}";
 
 p += "async function listenForIRChanges() {";
 p += "while(true) {";
 p += "try {";
 p += "const response = await fetch('/api/changed');";
 p += "if(response.ok) {";
 p += "const data = await response.json();";
 p += "if(data.changed) {";
 p += "updateUI(data);";
 p += "}";
 p += "}";
 p += "} catch(e) {";
 p += "await new Promise(r => setTimeout(r, 2000));";
 p += "}";
 p += "}";
 p += "}";

 p += "window.addEventListener('load', () => {";
 p += "fetch('/api/status').then(r => r.json()).then(data => { updateUI(data); listenForIRChanges(); }).catch(e => console.error(e));";
 p += "});";
 p += "</script>";
 p += "</head><body>";

 p += "<div class='container'>";
 p += "<div class='header'>Smart Home</div>";

 // FAN CARD
 p += "<div class='card fan-card'>";
 p += "<div class='card-title'>Fan Control</div>";
 p += "<div class='fan-container'>";
 p += "<svg id='fanIcon' class='fan-icon' viewBox='0 0 100 100'>";
 p += "<circle cx='50' cy='50' r='6' fill='white'/>";
 p += "<path d='M50 10 Q70 30 50 50 Q30 30 50 10'/>";
 p += "<path d='M90 50 Q70 70 50 50 Q70 30 90 50'/>";
 p += "<path d='M50 90 Q30 70 50 50 Q70 70 50 90'/>";
 p += "<path d='M10 50 Q30 30 50 50 Q30 70 10 50'/>";
 p += "</svg>";
 p += "</div>";
 p += "<div class='status' id='fanStatus'>Fan is OFF</div>";
 p += "<div class='button-group'>";
 p += "<button id='btnFanOn' class='btn-state' onclick='updateState(\"on\")'>On</button>";
 p += "<button id='btnFanOff' class='btn-state' onclick='updateState(\"off\")'>Off</button>";
 p += "</div>";
 
 p += "<div class='slider-container'>";
 p += "<div class='slider-label'><span>Speed</span><span id='speedLabel'>50%</span></div>";
 p += "<input type='range' id='fanSlider' min='0' max='100' value='50' onchange='handleSliderChange(this.value)'>";
 p += "</div>";
 p += "</div>";

 // LIGHTS CARD
 p += "<div class='card lights-card'>";
 p += "<div class='card-title'>Lights Control</div>";
 p += "<div class='light-item'>";
 p += "<div class='light-label' id='light1Status'>Light 1: OFF</div>";
 p += "<div class='button-group'>";
 p += "<button id='btnL1On' class='btn-state' onclick='updateState(\"l1on\")'>On</button>";
 p += "<button id='btnL1Off' class='btn-state' onclick='updateState(\"l1off\")'>Off</button>";
 p += "</div>";
 p += "</div>";
 p += "<div class='light-item'>";
 p += "<div class='light-label' id='light2Status'>Light 2: OFF</div>";
 p += "<div class='button-group'>";
 p += "<button id='btnL2On' class='btn-state' onclick='updateState(\"l2on\")'>On</button>";
 p += "<button id='btnL2Off' class='btn-state' onclick='updateState(\"l2off\")'>Off</button>";
 p += "</div>";
 p += "</div>";
 p += "</div>";

 // WIFI SETTINGS CARD
 p += "<div class='card wifi-card'>";
 p += "<button class='btn-wifi' onclick=\"location.href='/wifi'\">WiFi Settings</button>";
 p += "</div>";

 p += "</div>";
 p += "</body></html>";
 return p;
}

// ===== WIFI PAGE =====
String wifiPage(){
 String p="<!DOCTYPE html><html><head>";
 p+="<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>";
 p+="<style>";
 p+="* {margin:0; padding:0; box-sizing:border-box;}";
 p+="body {background:#0f172a; font-family:'Segoe UI', Arial, sans-serif; color:#fff; height:100vh; width:100vw; overflow:hidden; display:flex; align-items:center; justify-content:center; padding:20px;}";
 p+=".card {background:#1e293b; border-radius:18px; padding:30px; width:100%; max-width:360px; box-shadow:0 8px 32px rgba(0,0,0,0.4);}";
 p+=".title {font-size:24px; font-weight:bold; text-align:center; margin-bottom:20px; color:#60a5fa;}";
 p+="input {width:100%; padding:12px; margin:10px 0; border:2px solid #334155; background:#0f172a; border-radius:10px; font-size:14px; transition:0.3s; color:#fff;}";
 p+="input:focus {outline:none; border-color:#60a5fa; box-shadow:0 0 10px rgba(96, 165, 250, 0.2);}";
 p+="label {display:block; font-weight:600; margin-top:10px; color:#e2e8f0;}";
 p+="button {width:100%; padding:12px; background:#f59e0b; color:white; border:none; border-radius:10px; font-size:15px; font-weight:600; margin-top:20px; cursor:pointer; transition:0.3s; text-transform:uppercase; letter-spacing:1px;}";
 p+="button:hover {background:#d97706; box-shadow:0 6px 20px rgba(245, 158, 11, 0.4);}";
 p+="</style></head><body>";
 p+="<div class='card'>";
 p+="<div class='title'>WiFi Settings</div>";
 p+="<form action='/save'>";
 p+="<label>SSID</label>";
 p+="<input type='text' name='s' placeholder='Network name' required>";
 p+="<label>Password</label>";
 p+="<input type='password' name='p' placeholder='Password' required>";
 p+="<button type='submit'>Save</button>";
 p+="</form>";
 p+="</div>";
 p+="</body></html>";
 return p;
}

// ===== ROUTES =====
void root(){
  server.send(200,"text/html",webpage());
}

// ===== API ROUTES =====
void apiStatus(){
  server.send(200, "application/json", getStateJSON());
}

void apiChanged(){
  if (stateChangedByIR) {
    stateChangedByIR = false;
    sendLongPollResponse(true);
  } else {
    hasPendingLongPoll = true;
    longPollStartTime = millis();
  }
}

void apiOn(){
  fanEnabled=true;
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiOff(){
  fanEnabled=false;
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiSpeedUp(){
  if(delayMicros>MIN_DELAY)delayMicros-=500;
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiSpeedDown(){
  if(delayMicros<MAX_DELAY)delayMicros+=500;
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiL1On(){
  light1State=true;
  digitalWrite(LIGHT1_PIN,HIGH);
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiL1Off(){
  light1State=false;
  digitalWrite(LIGHT1_PIN,LOW);
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiL2On(){
  light2State=true;
  digitalWrite(LIGHT2_PIN,HIGH);
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void apiL2Off(){
  light2State=false;
  digitalWrite(LIGHT2_PIN,LOW);
  saveState();
  server.send(200, "application/json", getStateJSON());
}

void wifi(){
  server.send(200,"text/html",wifiPage());
}

void save(){
 String s=server.arg("s");
 String p=server.arg("p");
 saveWiFi(s,p);
 server.send(200,"text/html","Saved. Rebooting...");
 ESP.restart();
}

// ===== WIFI START =====
void startWiFi(){
 WiFi.mode(WIFI_AP_STA);
 WiFi.softAP(ap_ssid,ap_pass);
 WiFi.begin(wifi_ssid,wifi_pass);
}

// ===== ZERO CROSS ISR =====
void ICACHE_RAM_ATTR zeroCrossISR(){
 unsigned long now=micros();
 if(now-lastZCTime<ZC_DEBOUNCE)return;
 lastZCTime=now;
 zcDetected=true;
 zcTime=now;
 digitalWrite(TRIAC_PIN,LOW);
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

 bool targetHit = false;

 if(code==0x807F0AF5 && delayMicros>MIN_DELAY){
   delayMicros-=500;
   saveState();
   Serial.println("[IR] Speed Down");
   targetHit = true;
 }
 else if(code==0x807F8A75 && delayMicros<MAX_DELAY){
   delayMicros+=500;
   saveState();
   Serial.println("[IR] Speed Up");
   targetHit = true;
 }
 else if(code==0x807F30CF){
   fanEnabled=!fanEnabled;
   saveState();
   Serial.print("[IR] Fan Toggle: ");
   Serial.println(fanEnabled ? "ON" : "OFF");
   targetHit = true;
 }
 else if(code==0x807F728D){
   light1State=!light1State;
   digitalWrite(LIGHT1_PIN,light1State);
   saveState();
   Serial.print("[IR] Light 1 Toggle: ");
   Serial.println(light1State ? "ON" : "OFF");
   targetHit = true;
 }
 else if(code==0x807FB04F){
   light2State=!light2State;
   digitalWrite(LIGHT2_PIN,light2State);
   saveState();
   Serial.print("[IR] Light 2 Toggle: ");
   Serial.println(light2State ? "ON" : "OFF");
   targetHit = true;
 }

 if(targetHit) {
   stateChangedByIR = true; 
 }
}

// ===== SETUP =====
void setup(){
 Serial.begin(115200);
 Serial.println("\n\n=== SMART HOME STARTING ===");

 pinMode(ZC_PIN,INPUT);
 pinMode(TRIAC_PIN,OUTPUT);
 pinMode(LIGHT1_PIN,OUTPUT);
 pinMode(LIGHT2_PIN,OUTPUT);

 digitalWrite(LIGHT1_PIN,LOW);
 digitalWrite(LIGHT2_PIN,LOW);

 attachInterrupt(digitalPinToInterrupt(ZC_PIN),zeroCrossISR,RISING);

 irrecv.enableIRIn();

 loadWiFi();
 loadState();
 startWiFi();
 applySavedStates();

 MDNS.begin("smarthome");

 server.on("/",root);
 server.on("/api/status",apiStatus);
 server.on("/api/changed",apiChanged);  
 server.on("/api/on",apiOn);
 server.on("/api/off",apiOff);
 server.on("/api/up",apiSpeedUp);
 server.on("/api/down",apiSpeedDown);
 server.on("/api/l1on",apiL1On);
 server.on("/api/l1off",apiL1Off);
 server.on("/api/l2on",apiL2On);
 server.on("/api/l2off",apiL2Off);
 server.on("/wifi",wifi);
 server.on("/save",save);

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
