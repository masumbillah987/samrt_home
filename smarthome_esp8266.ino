#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ESPAsyncTCP.h>
#include <AsyncMqttClient.h>
#include <user_interface.h>

// ========== EEPROM ADDRESSES ==========
#define EEPROM_SIZE 1024
#define WIFI_SSID_ADDR 0
#define WIFI_PASS_ADDR 32
#define STATE_DATA_ADDR 64
#define IR_CONFIG_ADDR 128
#define MQTT_CONFIG_ADDR 200
#define WIFI_SAFE_ADDR 500

char wifi_ssid[32];
char wifi_pass[32];

// ===== MQTT CONFIG STRUCTURE =====
struct MQTTConfig {
  char host[64];
  uint16_t port;
  char user[32];
  char pass[32];
  char statusTopic[64];
  char controlTopic[64];
  uint32_t checksum;
} mqttConfig;

// ===== STATE STRUCTURE =====
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
bool apEnabled = true;

// ===== PINS =====
#define ZC_PIN D1
#define TRIAC_PIN D6
#define IR_PIN D5
#define LIGHT1_PIN D2
#define LIGHT2_PIN D8

// ===== SPEED =====
volatile int delayMicros = 5000;
volatile int effectiveDelayMicros = 5000;
const int MIN_DELAY = 0;
const int MAX_DELAY = 4000;
bool fanEnabled = false;

// ===== LIGHT STATES =====
bool light1State = false;
bool light2State = false;

// ===== TRIAC STATE MACHINE =====
enum TriacState { IDLE, WAIT_DELAY, OUTPUT_ON };
TriacState triacState = IDLE;
volatile unsigned long lowStartMicros = 0;

// ===== EEPROM SMART TIMER =====
bool stateSavePending = false;
unsigned long lastStateChangeTime = 0;
const unsigned long EEPROM_WRITE_DELAY = 10000;

// ===== STATE CHANGE FLAG =====
volatile bool stateChangedByIR = false;
bool hasPendingLongPoll = false;
unsigned long longPollStartTime = 0;
const unsigned long LONG_POLL_TIMEOUT = 20000;

// ===== LIVE IR CAPTURE =====
volatile bool remoteLearningActive = false;
volatile uint32_t lastCapturedIRCode = 0;
volatile bool newIRCodeCaptured = false;

// ===== IR =====
IRrecv irrecv(IR_PIN);
decode_results results;

// ===== SERVER =====
ESP8266WebServer server(80);

// ===== MQTT CLIENT =====
AsyncMqttClient mqttClient;
unsigned long lastMqttRetryTime = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
bool mqttConnected = false;

// ===== TIMING =====
unsigned long lastIRCheckTime = 0;
const unsigned long IR_CHECK_INTERVAL = 50;
unsigned long lastMDNSUpdateTime = 0;
const unsigned long MDNS_UPDATE_INTERVAL = 1000;

// ===== MEMORY SAVE FEATURE =====
bool memorySaveEnabled = false;
bool stateDirty = false;

// ===== WIFI MODE FEATURE =====
bool wifiSafeEnabled = false;
unsigned long wifiConnectStartTime = 0;
const unsigned long WIFI_TIMEOUT = 10000;

// ===== NON-BLOCKING RESET =====
bool resetPending = false;
unsigned long resetStartTime = 0;
const unsigned long RESET_DELAY_MS = 500;

// ===== CLAMP FUNCTION =====
void clampDelayMicros() {
  if (delayMicros < MIN_DELAY) delayMicros = MIN_DELAY;
  if (delayMicros > MAX_DELAY) delayMicros = MAX_DELAY;
}

// Forward declarations
String getStateJSON();
void publishMqttStatus();
void connectToMqtt();
void saveStateToEEPROM();
void saveWiFiSafeToEEPROM();
void masterReset();
String webpageMain();
String wifiPage();
String remotePage();
String settingsPage();
void updateEffectiveDelay();
void apiSetSpeed();

// ===== CHECKSUM FUNCTIONS =====
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

uint32_t calculateMQTTChecksum(MQTTConfig& data) {
  uint32_t sum = 0;
  for (int i = 0; i < 64; i++) sum += data.host[i];
  sum += data.port;
  for (int i = 0; i < 32; i++) sum += data.user[i];
  for (int i = 0; i < 32; i++) sum += data.pass[i];
  for (int i = 0; i < 64; i++) sum += data.statusTopic[i];
  for (int i = 0; i < 64; i++) sum += data.controlTopic[i];
  return sum * 13;
}

// ===== LOAD / SAVE MQTT CONFIG =====
void loadMQTTConfig() {
  for (int i = 0; i < sizeof(MQTTConfig); i++) {
    ((byte*)&mqttConfig)[i] = EEPROM.read(MQTT_CONFIG_ADDR + i);
  }
  uint32_t calc = calculateMQTTChecksum(mqttConfig);
  if (calc != mqttConfig.checksum) {
    strcpy(mqttConfig.host, "broker.hivemq.com");
    mqttConfig.port = 1883;
    strcpy(mqttConfig.user, "Masum_Home");
    strcpy(mqttConfig.pass, "12345678");
    strcpy(mqttConfig.statusTopic, "1712674082/status");
    strcpy(mqttConfig.controlTopic, "1712674082/control");
    mqttConfig.checksum = calculateMQTTChecksum(mqttConfig);
    for (int i = 0; i < sizeof(MQTTConfig); i++) {
      EEPROM.write(MQTT_CONFIG_ADDR + i, ((byte*)&mqttConfig)[i]);
    }
    EEPROM.commit();
    Serial.println("[MQTT] Default config written");
  }
}

void loadState() {
  for (int i = 0; i < sizeof(StateData); i++) {
    ((byte*)&stateData)[i] = EEPROM.read(STATE_DATA_ADDR + i);
  }
  uint32_t calc = calculateChecksum(stateData);
  if (calc == stateData.checksum) {
    delayMicros = stateData.delayMicros;
    clampDelayMicros();
    effectiveDelayMicros = delayMicros;
    fanEnabled = stateData.fanEnabled;
    light1State = stateData.light1State;
    light2State = stateData.light2State;
    Serial.println("[EEPROM] State Loaded");
  }
}

void triggerStateStorage() {
  if (!memorySaveEnabled) return;
  stateDirty = true;
}

void commitStateIfPending() {
  if (stateSavePending && (millis() - lastStateChangeTime >= EEPROM_WRITE_DELAY)) {
    stateSavePending = false;
    StateData newData;
    newData.delayMicros = delayMicros;
    newData.fanEnabled = fanEnabled;
    newData.light1State = light1State;
    newData.light2State = light2State;
    newData.checksum = calculateChecksum(newData);
    bool changed = false;
    for (int i = 0; i < sizeof(StateData); i++) {
      byte oldByte = EEPROM.read(STATE_DATA_ADDR + i);
      byte newByte = ((byte*)&newData)[i];
      if (oldByte != newByte) {
        EEPROM.write(STATE_DATA_ADDR + i, newByte);
        changed = true;
      }
    }
    if (changed) {
      EEPROM.commit();
      memcpy(&stateData, &newData, sizeof(StateData));
      Serial.println("[PRO-SAVED] State safely written");
    }
  }
}

void saveStateToEEPROM() {
  StateData newData;
  newData.delayMicros = delayMicros;
  newData.fanEnabled = fanEnabled;
  newData.light1State = light1State;
  newData.light2State = light2State;
  newData.checksum = calculateChecksum(newData);
  for (int i = 0; i < sizeof(StateData); i++) {
    EEPROM.write(STATE_DATA_ADDR + i, ((byte*)&newData)[i]);
  }
  EEPROM.commit();
  memcpy(&stateData, &newData, sizeof(StateData));
  Serial.println("[POWER-LOSS] State saved to EEPROM");
}

void loadIRConfig() {
  for (int i = 0; i < sizeof(IRConfigData); i++) {
    ((byte*)&irConfig)[i] = EEPROM.read(IR_CONFIG_ADDR + i);
  }
  uint32_t calc = calculateIRChecksum(irConfig);
  if (calc != irConfig.checksum) {
    irConfig.speedDown = 0x807F0AF5;
    irConfig.speedUp = 0x807F8A75;
    irConfig.fanToggle = 0x807F30CF;
    irConfig.light1Toggle = 0x807F728D;
    irConfig.light2Toggle = 0x807FB04F;
    saveIRConfig();
    Serial.println("[IR] Using default codes");
  }
}

void saveIRConfig() {
  irConfig.checksum = calculateIRChecksum(irConfig);
  bool changed = false;
  for (int i = 0; i < sizeof(IRConfigData); i++) {
    byte oldByte = EEPROM.read(IR_CONFIG_ADDR + i);
    byte newByte = ((byte*)&irConfig)[i];
    if (oldByte != newByte) {
      EEPROM.write(IR_CONFIG_ADDR + i, newByte);
      changed = true;
    }
  }
  if (changed) {
    EEPROM.commit();
    Serial.println("[SAVED] IR Config");
  }
}

void loadWiFi() {
  for (int i = 0; i < 32; i++) wifi_ssid[i] = EEPROM.read(WIFI_SSID_ADDR + i);
  for (int i = 0; i < 32; i++) wifi_pass[i] = EEPROM.read(WIFI_PASS_ADDR + i);
}

void saveWiFi(String ssid, String pass) {
  bool changed = false;
  for (int i = 0; i < 32; i++) {
    char currentS = (i < ssid.length()) ? ssid[i] : 0;
    char currentP = (i < pass.length()) ? pass[i] : 0;
    if (EEPROM.read(WIFI_SSID_ADDR + i) != currentS) {
      EEPROM.write(WIFI_SSID_ADDR + i, currentS);
      changed = true;
    }
    if (EEPROM.read(WIFI_PASS_ADDR + i) != currentP) {
      EEPROM.write(WIFI_PASS_ADDR + i, currentP);
      changed = true;
    }
  }
  if (changed) EEPROM.commit();
}

void loadWiFiSafe() {
  byte val = EEPROM.read(WIFI_SAFE_ADDR);
  wifiSafeEnabled = (val == 1);
}

void saveWiFiSafeToEEPROM() {
  byte val = wifiSafeEnabled ? 1 : 0;
  if (EEPROM.read(WIFI_SAFE_ADDR) != val) {
    EEPROM.write(WIFI_SAFE_ADDR, val);
    EEPROM.commit();
  }
}

void applySavedStates() {
  digitalWrite(LIGHT1_PIN, light1State ? HIGH : LOW);
  digitalWrite(LIGHT2_PIN, light2State ? HIGH : LOW);
}

// ===== UPDATE EFFECTIVE DELAY =====
void updateEffectiveDelay() {
  if (!fanEnabled) {
    effectiveDelayMicros = MAX_DELAY;
    return;
  }
  effectiveDelayMicros = delayMicros;
}

// ===== ZERO CROSS ISR =====
void IRAM_ATTR zcISR() {
  if (digitalRead(ZC_PIN) == LOW) {
    lowStartMicros = micros();
  }
}

// ===== NON-BLOCKING TRIAC STATE MACHINE =====
void handleTriacStateMachine() {
  if (!fanEnabled) {
    digitalWrite(TRIAC_PIN, LOW);
    triacState = IDLE;
    return;
  }
  
  bool zc = digitalRead(ZC_PIN);
  unsigned long now = micros();

  switch (triacState) {
    case IDLE:
      if (zc == LOW) {
        lowStartMicros = now;
        triacState = WAIT_DELAY;
      }
      break;

    case WAIT_DELAY:
      if (zc == LOW) {
        if (now - lowStartMicros >= effectiveDelayMicros) {
          digitalWrite(TRIAC_PIN, HIGH);
          triacState = OUTPUT_ON;
        }
      } else {
        digitalWrite(TRIAC_PIN, HIGH);
        triacState = OUTPUT_ON;
      }
      break;

    case OUTPUT_ON:
      if (zc == HIGH) {
        digitalWrite(TRIAC_PIN, LOW);
        triacState = IDLE;
      }
      break;
  }
}

// ===== JSON STATE STRING =====
String getStateJSON() {
  int percent = 0;
  if (fanEnabled) {
    int safeDelay = constrain(effectiveDelayMicros, MIN_DELAY, MAX_DELAY);
    percent = map(safeDelay, MAX_DELAY, MIN_DELAY, 0, 100);
  }
  String json = "{";
  json += "\"fanEnabled\":" + String(fanEnabled ? "true" : "false") + ",";
  json += "\"light1State\":" + String(light1State ? "true" : "false") + ",";
  json += "\"light2State\":" + String(light2State ? "true" : "false") + ",";
  json += "\"fanSpeed\":" + String(percent);
  json += "}";
  return json;
}

void sendLongPollResponse(bool changed) {
  int percent = 0;
  if (fanEnabled) {
    int safeDelay = constrain(effectiveDelayMicros, MIN_DELAY, MAX_DELAY);
    percent = map(safeDelay, MAX_DELAY, MIN_DELAY, 0, 100);
  }
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

void publishMqttStatus() {
  if (mqttClient.connected()) {
    mqttClient.publish(mqttConfig.statusTopic, 1, false, getStateJSON().c_str());
  }
}

// ===== WEB PAGES =====
String wifiPage() {
  return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>*{margin:0;padding:0;box-sizing:border-box;}body{background:#0f172a;font-family:'Segoe UI',Arial,sans-serif;color:#fff;height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}.card{background:#1e293b;border-radius:18px;padding:30px;width:100%;max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title{font-size:24px;font-weight:bold;text-align:center;margin-bottom:20px;color:#60a5fa;}input{width:100%;padding:12px;margin:10px 0;border:2px solid #334155;background:#0f172a;border-radius:10px;color:#fff;}input:focus{outline:none;border-color:#60a5fa;}label{display:block;font-weight:600;margin-top:10px;}button{width:100%;padding:12px;background:#f59e0b;color:white;border:none;border-radius:10px;margin-top:20px;cursor:pointer;}.back-btn{background:#475569;margin-top:10px;}</style></head><body><div class='card'><div class='title'>WiFi Settings</div><form action='/save'><label>SSID</label><input type='text' name='s' placeholder='Network name' required><label>Password</label><input type='password' name='p' placeholder='Password' required><button type='submit'>Save</button></form><button class='back-btn' onclick=\"location.href='/'\">Back</button></div></body></html>";
}

String remotePage() {
  return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>*{margin:0;padding:0;box-sizing:border-box;}body{background:#0f172a;font-family:'Segoe UI',Arial,sans-serif;color:#fff;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}.card{background:#1e293b;border-radius:18px;padding:25px;width:100%;max-width:400px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title{font-size:22px;font-weight:bold;text-align:center;margin-bottom:5px;color:#60a5fa;}.desc{font-size:13px;color:#a3e635;text-align:center;margin-bottom:15px;font-weight:600;}.input-wrapper{position:relative;margin-bottom:12px;}input{width:100%;padding:10px;margin:4px 0 0 0;border:2px solid #334155;background:#0f172a;border-radius:10px;font-size:14px;color:#fff;font-family:monospace;}input:focus{outline:none;border-color:#3b82f6;}.focused-field{border-color:#22c55e !important;box-shadow:0 0 10px rgba(34,197,94,0.3);}label{display:block;font-weight:600;font-size:13px;color:#e2e8f0;}button{width:100%;padding:12px;background:#8b5cf6;color:white;border:none;border-radius:10px;margin-top:10px;cursor:pointer;}.back-btn{background:#475569;margin-top:8px;}.master-btn{background:#ef4444;margin-top:15px;}</style><script>let activeInputId='ft';function setTarget(id){document.querySelectorAll('input').forEach(el=>el.classList.remove('focused-field'));activeInputId=id;document.getElementById(id).classList.add('focused-field');}async function poolLiveRemoteCodes(){while(true){try{let res=await fetch('/api/get_live_ir');if(res.ok){let data=await res.json();if(data.captured&&data.code!=='0x0'){document.getElementById(activeInputId).value=data.code;}}}catch(e){}await new Promise(r=>setTimeout(r,300));}}let masterClicks=0;let masterTimer=null;function masterKeyClick(){masterClicks++;if(masterClicks===1){masterTimer=setTimeout(()=>{masterClicks=0;},5000);}if(masterClicks>=3){clearTimeout(masterTimer);masterClicks=0;if(confirm('RESET all settings except MQTT?')){fetch('/api/masterreset').then(()=>{alert('Device rebooting...');setTimeout(()=>{location.href='/';},2000);});}}}window.addEventListener('load',()=>{setTarget('ft');poolLiveRemoteCodes();});</script></head><body><div class='card'><div class='title'>Remote Setup</div><div class='desc'>⚡ Tap an input, then press remote button!</div><form action='/save_remote'><label>Fan Toggle Code</label><div class='input-wrapper'><input type='text' id='ft' name='ft' onfocus='setTarget(\"ft\")' value='0x" + String(irConfig.fanToggle, HEX) + "' readonly required></div><label>Fan Speed Up Code</label><div class='input-wrapper'><input type='text' id='su' name='su' onfocus='setTarget(\"su\")' value='0x" + String(irConfig.speedUp, HEX) + "' readonly required></div><label>Fan Speed Down Code</label><div class='input-wrapper'><input type='text' id='sd' name='sd' onfocus='setTarget(\"sd\")' value='0x" + String(irConfig.speedDown, HEX) + "' readonly required></div><label>Light 1 Toggle Code</label><div class='input-wrapper'><input type='text' id='l1' name='l1' onfocus='setTarget(\"l1\")' value='0x" + String(irConfig.light1Toggle, HEX) + "' readonly required></div><label>Light 2 Toggle Code</label><div class='input-wrapper'><input type='text' id='l2' name='l2' onfocus='setTarget(\"l2\")' value='0x" + String(irConfig.light2Toggle, HEX) + "' readonly required></div><button type='submit'>Save IR Configuration</button><button type='button' class='back-btn' onclick=\"location.href='/'\">Back</button><button type='button' class='master-btn' onclick='masterKeyClick()'>MASTER KEY (3x in 5s)</button></form></div></body></html>";
}

String settingsPage() {
  return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>*{margin:0;padding:0;box-sizing:border-box;}body{background:#0f172a;font-family:'Segoe UI',Arial,sans-serif;color:#fff;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}.card{background:#1e293b;border-radius:18px;padding:25px;width:100%;max-width:400px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title{font-size:24px;font-weight:bold;text-align:center;margin-bottom:20px;color:#60a5fa;}.button-group{display:flex;flex-direction:column;gap:15px;}button{padding:14px;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;transition:all 0.2s;}.btn-primary{background:#8b5cf6;color:white;}.btn-toggle{background:#475569;color:#94a3b8;}.active-on{background:#22c55e !important;color:white !important;box-shadow:0 4px 12px rgba(34,197,94,0.3);}.back-btn{background:#475569;color:white;margin-top:10px;}</style><script>let memoryEnabled=false;let wifiModeEnabled=false;async function toggleMemory(){let newState=!memoryEnabled;let endpoint=newState?'/api/memoryon':'/api/memoryoff';await fetch(endpoint);memoryEnabled=newState;const btn=document.getElementById('memBtn');if(memoryEnabled) btn.classList.add('active-on'); else btn.classList.remove('active-on');}async function toggleWiFiMode(){let newState=!wifiModeEnabled;let endpoint=newState?'/api/wifisafeon':'/api/wifisafeoff';await fetch(endpoint);wifiModeEnabled=newState;const btn=document.getElementById('wifiModeBtn');if(wifiModeEnabled) btn.classList.add('active-on'); else btn.classList.remove('active-on');}async function updateStatuses(){let mem=await(await fetch('/api/memorystatus')).json();let wfm=await(await fetch('/api/wifisafestatus')).json();memoryEnabled=mem.enabled;wifiModeEnabled=wfm.enabled;if(memoryEnabled) document.getElementById('memBtn').classList.add('active-on');if(wifiModeEnabled) document.getElementById('wifiModeBtn').classList.add('active-on');}window.addEventListener('load',()=>{updateStatuses();});</script></head><body><div class='card'><div class='title'>⚙️ Settings</div><div class='button-group'><button class='btn-primary' onclick=\"location.href='/remote'\">📡 Remote Settings</button><button id='memBtn' class='btn-toggle' onclick='toggleMemory()'>💾 Memory Save</button><button id='wifiModeBtn' class='btn-toggle' onclick='toggleWiFiMode()'>📶 WiFi Mode</button><button class='back-btn' onclick=\"location.href='/'\">← Back to Home</button></div></div></body></html>";
}

// Main page – now with Done button and auto-redirect logic
String webpageMain() {
  if (mqttConnected) {
    // MQTT connected: show success message with Done button + auto-redirect script
    return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{background:#0f172a;font-family:'Segoe UI',Arial,sans-serif;color:#fff;height:100vh;display:flex;align-items:center;justify-content:center;text-align:center;margin:0;}.card{background:#1e293b;border-radius:18px;padding:30px;max-width:90%;box-shadow:0 8px 32px rgba(0,0,0,0.4);}.title{font-size:28px;font-weight:bold;color:#22c55e;margin-bottom:10px;}.sub{font-size:16px;color:#94a3b8;margin-bottom:20px;}.done-btn{background:#22c55e;color:white;border:none;padding:12px 24px;border-radius:40px;font-size:18px;font-weight:bold;cursor:pointer;margin-top:10px;}</style><script>function isLocalNetwork(){let host=window.location.hostname;return (host==='smarthome.local' || host.startsWith('192.168.') || host.startsWith('10.') || host.startsWith('172.') || host==='localhost');}function redirectToGithub(){localStorage.setItem('smarthome_redirect_done','true');window.location.href='https://masumbillah987.github.io/samrt_home/';}if(isLocalNetwork()){let redirectFlag=localStorage.getItem('smarthome_redirect_done');if(redirectFlag==='true'){window.location.href='https://masumbillah987.github.io/samrt_home/';}}</script></head><body><div class='card'><div class='title'>✓ Connected successfully</div><div class='sub'>Connect your device with home wifi now...</div><button class='done-btn' onclick='redirectToGithub()'>Done</button></div></body></html>";
  } else {
    // MQTT not connected: full control panel with auto-reload when MQTT connects
    return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'><style>*{margin:0;padding:0;box-sizing:border-box;}body{background:#0f172a;font-family:'Segoe UI',Arial,sans-serif;color:#fff;height:100vh;width:100vw;overflow:hidden;display:flex;justify-content:center;align-items:center;padding:10px;}.container{width:100%;max-width:420px;height:100%;display:flex;flex-direction:column;justify-content:space-between;padding:5px 0;}.header{text-align:center;font-size:24px;font-weight:bold;letter-spacing:1px;color:#fff;flex:0 0 auto;}.card{background:#1e293b;border-radius:16px;padding:15px;margin:5px 0;box-shadow:0 8px 32px rgba(0,0,0,0.4);color:#fff;display:flex;flex-direction:column;justify-content:center;}.fan-card{flex:0 1 auto;min-height:200px;max-height:55vh;overflow-y:auto;}.lights-card{flex:0 0 auto;}.wifi-card{flex:0 0 auto;background:transparent;box-shadow:none;padding:0;margin:5px 0 0 0;}.card-title{font-size:16px;font-weight:600;margin-bottom:10px;color:#60a5fa;}.button-group{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:8px 0;}button{padding:12px 15px;border:none;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;text-transform:uppercase;letter-spacing:0.5px;user-select:none;}.btn-state{background:#475569;color:#94a3b8;}.active-on{background:#22c55e !important;color:white !important;box-shadow:0 4px 12px rgba(34,197,94,0.3);}.active-off{background:#ef4444 !important;color:white !important;box-shadow:0 4px 12px rgba(239,68,68,0.2);}.btn-row{display:grid;grid-template-columns:1fr 1fr;gap:10px;}.btn-wifi{background:#f59e0b;color:white;width:100%;padding:12px;box-shadow:0 4px 12px rgba(245,158,11,0.2);}.btn-remote{background:#8b5cf6;color:white;width:100%;padding:12px;box-shadow:0 4px 12px rgba(139,92,246,0.2);}.fan-container{display:flex;justify-content:center;align-items:center;flex:1;min-height:60px;}.fan-icon{width:80px;height:80px;fill:#475569;transition:fill 0.3s ease;}.fan-icon.spinning{animation:spin 0.8s linear infinite;fill:#22c55e;}@keyframes spin{0%{transform:rotate(0deg);}100%{transform:rotate(360deg);}}.status{text-align:center;font-size:14px;font-weight:600;color:#60a5fa;margin:5px 0;}.slider-container{padding:10px 0;display:flex;flex-direction:column;gap:5px;}.slider-label{display:flex;justify-content:space-between;font-size:12px;color:#94a3b8;}input[type=range]{-webkit-appearance:none;width:100%;background:transparent;}input[type=range]:focus{outline:none;}input[type=range]::-webkit-slider-runnable-track{width:100%;height:8px;cursor:pointer;background:#334155;border-radius:10px;}input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;height:20px;width:20px;border-radius:50%;background:#22c55e;cursor:pointer;margin-top:-6px;box-shadow:0 0 8px rgba(34,197,94,0.5);}input[type=range]:disabled::-webkit-slider-thumb{background:#475569;box-shadow:none;cursor:not-allowed;}.light-item{background:#0f172a;border-radius:12px;padding:10px 12px;margin:5px 0;border:1px solid #334155;display:flex;flex-direction:column;}.light-label{font-size:14px;font-weight:600;color:#e2e8f0;margin-bottom:5px;}.light-active{color:#22c55e !important;}</style><script>let currentSpeed=50;let isUpdatingSlider=false;let memoryEnabled=false;let wifiModeEnabled=false;async function updateState(action){try{const response=await fetch('/api/'+action);if(response.ok){const data=await response.json();updateUI(data);}}catch(e){console.error('Error:',e);}}async function handleSliderChange(val){if(isUpdatingSlider)return;isUpdatingSlider=true;let target=parseInt(val);document.getElementById('speedLabel').textContent=target+'%';try{const response=await fetch('/api/speed?value='+target);if(response.ok){const data=await response.json();updateUI(data);}}catch(e){console.error('Error:',e);}isUpdatingSlider=false;}function updateUI(data){currentSpeed=data.fanSpeed;document.getElementById('fanStatus').textContent='Fan is '+(data.fanEnabled?'ON':'OFF');document.getElementById('light1Status').textContent='Light 1: '+(data.light1State?'ON':'OFF');document.getElementById('light2Status').textContent='Light 2: '+(data.light2State?'ON':'OFF');if(!isUpdatingSlider){document.getElementById('fanSlider').value=data.fanSpeed;document.getElementById('speedLabel').textContent=data.fanSpeed+'%';}document.getElementById('fanSlider').disabled=!data.fanEnabled;const fanIcon=document.getElementById('fanIcon');if(data.fanEnabled){fanIcon.classList.add('spinning');document.getElementById('btnFanOn').classList.add('active-on');document.getElementById('btnFanOff').classList.remove('active-off');}else{fanIcon.classList.remove('spinning');document.getElementById('btnFanOn').classList.remove('active-on');document.getElementById('btnFanOff').classList.add('active-off');}if(data.light1State){document.getElementById('light1Status').classList.add('light-active');document.getElementById('btnL1On').classList.add('active-on');document.getElementById('btnL1Off').classList.remove('active-off');}else{document.getElementById('light1Status').classList.remove('light-active');document.getElementById('btnL1On').classList.remove('active-on');document.getElementById('btnL1Off').classList.add('active-off');}if(data.light2State){document.getElementById('light2Status').classList.add('light-active');document.getElementById('btnL2On').classList.add('active-on');document.getElementById('btnL2Off').classList.remove('active-off');}else{document.getElementById('light2Status').classList.remove('light-active');document.getElementById('btnL2On').classList.remove('active-on');document.getElementById('btnL2Off').classList.add('active-off');}}async function listenForIRChanges(){while(true){try{const response=await fetch('/api/changed');if(response.ok){const data=await response.json();if(data.changed){updateUI(data);}}}catch(e){await new Promise(r=>setTimeout(r,2000));}}}async function toggleMemory(){let newState=!memoryEnabled;let endpoint=newState?'/api/memoryon':'/api/memoryoff';await fetch(endpoint);memoryEnabled=newState;document.getElementById('memBtn').classList.toggle('active-on',memoryEnabled);}async function toggleWiFiMode(){let newState=!wifiModeEnabled;let endpoint=newState?'/api/wifisafeon':'/api/wifisafeoff';await fetch(endpoint);wifiModeEnabled=newState;document.getElementById('wifiModeBtn').classList.toggle('active-on',wifiModeEnabled);}async function checkMqttAndReload(){try{let res=await fetch('/api/mqttstatus');let data=await res.json();if(data.connected){location.reload();}}catch(e){}}window.addEventListener('load',()=>{fetch('/api/memorystatus').then(r=>r.json()).then(d=>{memoryEnabled=d.enabled;if(memoryEnabled) document.getElementById('memBtn').classList.add('active-on');});fetch('/api/wifisafestatus').then(r=>r.json()).then(d=>{wifiModeEnabled=d.enabled;if(wifiModeEnabled) document.getElementById('wifiModeBtn').classList.add('active-on');});fetch('/api/status').then(r=>r.json()).then(d=>{updateUI(d);});listenForIRChanges();setInterval(checkMqttAndReload,3000);});</script></head><body><div class='container'><div class='header'>Smart Home</div><div class='card fan-card'><div class='card-title'>Fan Control</div><div class='fan-container'><svg id='fanIcon' class='fan-icon' viewBox='0 0 100 100'><circle cx='50' cy='50' r='6' fill='white'/><path d='M50 10 Q70 30 50 50 Q30 30 50 10'/><path d='M90 50 Q70 70 50 50 Q70 30 90 50'/><path d='M50 90 Q30 70 50 50 Q70 70 50 90'/><path d='M10 50 Q30 30 50 50 Q30 70 10 50'/></svg></div><div class='status' id='fanStatus'>Fan is OFF</div><div class='button-group'><button id='btnFanOn' class='btn-state' onclick='updateState(\"on\")'>On</button><button id='btnFanOff' class='btn-state' onclick='updateState(\"off\")'>Off</button></div><div class='slider-container'><div class='slider-label'><span>Speed</span><span id='speedLabel'>50%</span></div><input type='range' id='fanSlider' min='0' max='100' value='50' oninput='handleSliderChange(this.value)'></div></div><div class='card lights-card'><div class='card-title'>Lights Control</div><div class='light-item'><div class='light-label' id='light1Status'>Light 1: OFF</div><div class='button-group'><button id='btnL1On' class='btn-state' onclick='updateState(\"l1on\")'>On</button><button id='btnL1Off' class='btn-state' onclick='updateState(\"l1off\")'>Off</button></div></div><div class='light-item'><div class='light-label' id='light2Status'>Light 2: OFF</div><div class='button-group'><button id='btnL2On' class='btn-state' onclick='updateState(\"l2on\")'>On</button><button id='btnL2Off' class='btn-state' onclick='updateState(\"l2off\")'>Off</button></div></div></div><div class='card' style='padding:10px; margin-top:5px;'><div class='card-title'>System Settings</div><div class='button-group'><button id='memBtn' class='btn-state' onclick='toggleMemory()'>Memory Save</button><button id='wifiModeBtn' class='btn-state' onclick='toggleWiFiMode()'>WiFi Mode</button></div></div><div class='card wifi-card'><div class='btn-row'><button class='btn-wifi' onclick=\"location.href='/wifi'\">WiFi Setup</button><button class='btn-remote' onclick=\"location.href='/settings'\">Settings</button></div></div></div></body></html>";
  }
}

// ===== ROUTES =====
void root() {
  String page = webpageMain();
  server.setContentLength(page.length());
  server.send(200, "text/html", "");
  const size_t CHUNK = 512;
  for (size_t i = 0; i < page.length(); i += CHUNK) {
    server.sendContent(page.substring(i, i + CHUNK));
    yield();
  }
  server.sendContent("");
}
void apiStatus() { server.send(200, "application/json", getStateJSON()); }
void apiChanged() {
  if (stateChangedByIR) {
    stateChangedByIR = false;
    sendLongPollResponse(true);
  } else {
    hasPendingLongPoll = true;
    longPollStartTime = millis();
  }
}
void apiOn() { fanEnabled = true; updateEffectiveDelay(); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiOff() { fanEnabled = false; updateEffectiveDelay(); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiSetSpeed() {
  if (server.hasArg("value")) {
    int targetPercent = server.arg("value").toInt();
    targetPercent = constrain(targetPercent, 0, 100);
    delayMicros = map(targetPercent, 0, 100, MAX_DELAY, MIN_DELAY);
    clampDelayMicros();
    updateEffectiveDelay();
    triggerStateStorage();
    publishMqttStatus();
    server.send(200, "application/json", getStateJSON());
  } else {
    server.send(400, "application/json", "{\"error\":\"Missing value\"}");
  }
}
void apiSpeedUp() {
  if (delayMicros > MIN_DELAY) delayMicros -= 500;
  clampDelayMicros();
  updateEffectiveDelay();
  triggerStateStorage();
  publishMqttStatus();
  server.send(200, "application/json", getStateJSON());
}
void apiSpeedDown() {
  if (delayMicros < MAX_DELAY) delayMicros += 500;
  clampDelayMicros();
  updateEffectiveDelay();
  triggerStateStorage();
  publishMqttStatus();
  server.send(200, "application/json", getStateJSON());
}
void apiL1On() { light1State = true; digitalWrite(LIGHT1_PIN, HIGH); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL1Off() { light1State = false; digitalWrite(LIGHT1_PIN, LOW); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL2On() { light2State = true; digitalWrite(LIGHT2_PIN, HIGH); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void apiL2Off() { light2State = false; digitalWrite(LIGHT2_PIN, LOW); triggerStateStorage(); publishMqttStatus(); server.send(200, "application/json", getStateJSON()); }
void wifi() { server.send(200, "text/html", wifiPage()); }
void remote() { lastCapturedIRCode = 0; newIRCodeCaptured = false; remoteLearningActive = true; server.send(200, "text/html", remotePage()); }
void apiGetLiveIR() { String json = "{ \"captured\":" + String(newIRCodeCaptured ? "true" : "false") + ", \"code\":\"0x" + String(lastCapturedIRCode, HEX) + "\" }"; newIRCodeCaptured = false; server.send(200, "application/json", json); }
void handleMemoryOn() { memorySaveEnabled = true; saveStateToEEPROM(); server.send(200, "application/json", "{\"success\":true}"); }
void handleMemoryOff() { memorySaveEnabled = false; server.send(200, "application/json", "{\"success\":true}"); }
void handleMemoryStatus() { String json = "{\"enabled\":" + String(memorySaveEnabled ? "true" : "false") + "}"; server.send(200, "application/json", json); }
void handleWiFiSafeOn() { wifiSafeEnabled = true; saveWiFiSafeToEEPROM(); wifiConnectStartTime = millis(); wifi_station_disconnect(); wifi_station_connect(); server.send(200, "application/json", "{\"success\":true}"); }
void handleWiFiSafeOff() { wifiSafeEnabled = false; saveWiFiSafeToEEPROM(); wifi_station_disconnect(); wifiConnectStartTime = 0; server.send(200, "application/json", "{\"success\":true}"); }
void handleWiFiSafeStatus() { String json = "{\"enabled\":" + String(wifiSafeEnabled ? "true" : "false") + "}"; server.send(200, "application/json", json); }
void apiMqttStatus() { String json = "{\"connected\":" + String(mqttConnected ? "true" : "false") + "}"; server.send(200, "application/json", json); }

void masterReset() {
  for (int i = 0; i < sizeof(StateData); i++) EEPROM.write(STATE_DATA_ADDR + i, 0);
  for (int i = 0; i < sizeof(IRConfigData); i++) EEPROM.write(IR_CONFIG_ADDR + i, 0);
  for (int i = 0; i < 32; i++) EEPROM.write(WIFI_SSID_ADDR + i, 0);
  for (int i = 0; i < 32; i++) EEPROM.write(WIFI_PASS_ADDR + i, 0);
  EEPROM.write(WIFI_SAFE_ADDR, 0);
  EEPROM.commit();
  server.send(200, "text/plain", "Resetting...");
  resetPending = true;
  resetStartTime = millis();
}
void handleMasterReset() { masterReset(); }

void save() {
  String s = server.arg("s");
  String p = server.arg("p");
  saveWiFi(s, p);
  strncpy(wifi_ssid, s.c_str(), 32);
  strncpy(wifi_pass, p.c_str(), 32);
  wifi_station_disconnect();
  struct station_config conf;
  memset(&conf, 0, sizeof(conf));
  strncpy((char*)conf.ssid, wifi_ssid, 32);
  strncpy((char*)conf.password, wifi_pass, 32);
  wifi_station_set_config(&conf);
  wifi_station_connect();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}
void saveRemote() {
  irConfig.fanToggle = strtoul(server.arg("ft").c_str(), NULL, 16);
  irConfig.speedUp = strtoul(server.arg("su").c_str(), NULL, 16);
  irConfig.speedDown = strtoul(server.arg("sd").c_str(), NULL, 16);
  irConfig.light1Toggle = strtoul(server.arg("l1").c_str(), NULL, 16);
  irConfig.light2Toggle = strtoul(server.arg("l2").c_str(), NULL, 16);
  saveIRConfig();
  remoteLearningActive = false;
  server.send(200, "text/html", "IR Config Saved. <a href='/'>Back to Home</a>");
}
void settings() { server.send(200, "text/html", settingsPage()); }

// ===== MQTT EVENT HANDLERS =====
void onMqttConnect(bool sessionPresent) {
  Serial.println("[MQTT] Connected to Broker!");
  mqttConnected = true;
  mqttClient.subscribe(mqttConfig.controlTopic, 1);
  publishMqttStatus();
}
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("[MQTT] Disconnected.");
  mqttConnected = false;
}
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  char message[len + 1];
  memcpy(message, payload, len);
  message[len] = '\0';
  String cmd = String(message);
  Serial.print("[MQTT] Received: ");
  Serial.println(cmd);
  bool changed = false;
  if (cmd.startsWith("SPEED_")) {
    int val = cmd.substring(6).toInt();
    delayMicros = map(val, 0, 100, MAX_DELAY, MIN_DELAY);
    clampDelayMicros();
    changed = true;
  } else if (cmd == "FAN_ON") {
    fanEnabled = true;
    changed = true;
  } else if (cmd == "FAN_OFF") {
    fanEnabled = false;
    changed = true;
  } else if (cmd == "FAN_UP") {
    if (delayMicros > MIN_DELAY) delayMicros -= 500;
    clampDelayMicros();
    changed = true;
  } else if (cmd == "FAN_DOWN") {
    if (delayMicros < MAX_DELAY) delayMicros += 500;
    clampDelayMicros();
    changed = true;
  } else if (cmd == "L1_ON") {
    light1State = true;
    digitalWrite(LIGHT1_PIN, HIGH);
    changed = true;
  } else if (cmd == "L1_OFF") {
    light1State = false;
    digitalWrite(LIGHT1_PIN, LOW);
    changed = true;
  } else if (cmd == "L2_ON") {
    light2State = true;
    digitalWrite(LIGHT2_PIN, HIGH);
    changed = true;
  } else if (cmd == "L2_OFF") {
    light2State = false;
    digitalWrite(LIGHT2_PIN, LOW);
    changed = true;
  } else if (cmd == "REFRESH_STATE" || cmd == "GET_STATUS") {
    publishMqttStatus();
    changed = false;
  }
  if (changed) {
    updateEffectiveDelay();
    triggerStateStorage();
    stateChangedByIR = true;
    publishMqttStatus();
  }
}
void connectToMqtt() {
  Serial.println("[MQTT] Connecting to Broker...");
  if (strlen(mqttConfig.user) > 0) {
    mqttClient.setCredentials(mqttConfig.user, mqttConfig.pass);
  }
  mqttClient.setServer(mqttConfig.host, mqttConfig.port);
  mqttClient.connect();
}

// ===== IR HANDLING =====
void handleIR() {
  if (!irrecv.decode(&results)) return;
  uint32_t code = results.value;
  irrecv.resume();
  if (code == 0xFFFFFFFF || code == 0x0) return;
  if (remoteLearningActive) {
    lastCapturedIRCode = code;
    newIRCodeCaptured = true;
    return;
  }
  bool targetHit = false;
  if (code == irConfig.speedDown && delayMicros > MIN_DELAY) {
    delayMicros -= 500;
    clampDelayMicros();
    targetHit = true;
    Serial.println("[IR] Speed Down");
  } else if (code == irConfig.speedUp && delayMicros < MAX_DELAY) {
    delayMicros += 500;
    clampDelayMicros();
    targetHit = true;
    Serial.println("[IR] Speed Up");
  } else if (code == irConfig.fanToggle) {
    fanEnabled = !fanEnabled;
    targetHit = true;
    Serial.println("[IR] Fan Toggle");
  } else if (code == irConfig.light1Toggle) {
    light1State = !light1State;
    digitalWrite(LIGHT1_PIN, light1State);
    targetHit = true;
    Serial.println("[IR] Light 1 Toggle");
  } else if (code == irConfig.light2Toggle) {
    light2State = !light2State;
    digitalWrite(LIGHT2_PIN, light2State);
    targetHit = true;
    Serial.println("[IR] Light 2 Toggle");
  }
  if (targetHit) {
    updateEffectiveDelay();
    triggerStateStorage();
    stateChangedByIR = true;
    publishMqttStatus();
  }
}

// ===== NON‑BLOCKING WIFI START =====
void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass);
  wifi_station_set_auto_connect(0);
  struct station_config conf;
  memset(&conf, 0, sizeof(conf));
  strncpy((char*)conf.ssid, wifi_ssid, 32);
  strncpy((char*)conf.password, wifi_pass, 32);
  wifi_station_set_config(&conf);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== SMART HOME STARTING ===");
  EEPROM.begin(EEPROM_SIZE);
  pinMode(ZC_PIN, INPUT);
  pinMode(TRIAC_PIN, OUTPUT);
  pinMode(LIGHT1_PIN, OUTPUT);
  pinMode(LIGHT2_PIN, OUTPUT);
  digitalWrite(LIGHT1_PIN, LOW);
  digitalWrite(LIGHT2_PIN, LOW);
  digitalWrite(TRIAC_PIN, LOW);
  attachInterrupt(digitalPinToInterrupt(ZC_PIN), zcISR, CHANGE);
  irrecv.enableIRIn();
  loadWiFi();
  loadMQTTConfig();
  loadState();
  loadIRConfig();
  loadWiFiSafe();
  startWiFi();
  applySavedStates();
  updateEffectiveDelay();
  MDNS.begin("smarthome");
  MDNS.addService("http", "tcp", 80);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  server.on("/", root);
  server.on("/api/status", apiStatus);
  server.on("/api/changed", apiChanged);
  server.on("/api/on", apiOn);
  server.on("/api/off", apiOff);
  server.on("/api/speed", apiSetSpeed);
  server.on("/api/up", apiSpeedUp);
  server.on("/api/down", apiSpeedDown);
  server.on("/api/l1on", apiL1On);
  server.on("/api/l1off", apiL1Off);
  server.on("/api/l2on", apiL2On);
  server.on("/api/l2off", apiL2Off);
  server.on("/wifi", wifi);
  server.on("/remote", remote);
  server.on("/settings", settings);
  server.on("/api/get_live_ir", apiGetLiveIR);
  server.on("/save", save);
  server.on("/save_remote", saveRemote);
  server.on("/api/memoryon", handleMemoryOn);
  server.on("/api/memoryoff", handleMemoryOff);
  server.on("/api/memorystatus", handleMemoryStatus);
  server.on("/api/wifisafeon", handleWiFiSafeOn);
  server.on("/api/wifisafeoff", handleWiFiSafeOff);
  server.on("/api/wifisafestatus", handleWiFiSafeStatus);
  server.on("/api/mqttstatus", apiMqttStatus);
  server.on("/api/masterreset", handleMasterReset);
  server.begin();
  Serial.println("=== READY ===");
}

void loop() {
  unsigned long currentMillis = millis();

  if (resetPending && (currentMillis - resetStartTime >= RESET_DELAY_MS)) {
    ESP.restart();
  }

  handleTriacStateMachine();

  if (currentMillis - lastIRCheckTime >= IR_CHECK_INTERVAL) {
    lastIRCheckTime = currentMillis;
    handleIR();
  }
  commitStateIfPending();

  if (memorySaveEnabled && stateDirty) {
    unsigned long now = micros();
    if (now - lowStartMicros > 30000) {
      saveStateToEEPROM();
      stateDirty = false;
    }
  }

  if (wifiSafeEnabled && WiFi.status() != WL_CONNECTED) {
    if (wifiConnectStartTime == 0) wifiConnectStartTime = millis();
    if (millis() - wifiConnectStartTime > WIFI_TIMEOUT) {
      Serial.println("[WiFi] ❌ Not connected within 10s – disabling WiFi Mode");
      wifiSafeEnabled = false;
      saveWiFiSafeToEEPROM();
      wifi_station_disconnect();
      wifiConnectStartTime = 0;
    }
  }
  if (WiFi.status() == WL_CONNECTED) wifiConnectStartTime = 0;

  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (currentMillis - lastMqttRetryTime >= MQTT_RETRY_INTERVAL) {
      lastMqttRetryTime = currentMillis;
      connectToMqtt();
    }
  }

  if (hasPendingLongPoll) {
    if (stateChangedByIR) {
      stateChangedByIR = false;
      sendLongPollResponse(true);
    } else if (currentMillis - longPollStartTime >= LONG_POLL_TIMEOUT) {
      sendLongPollResponse(false);
    }
  }

  server.handleClient();

  if (currentMillis - lastMDNSUpdateTime >= MDNS_UPDATE_INTERVAL) {
    lastMDNSUpdateTime = currentMillis;
    MDNS.update();
  }
}
