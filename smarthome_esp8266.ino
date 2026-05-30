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
  uint32_t checksum; // Simple checksum for validation
} stateData;

// ===== HOTSPOT =====
const char* ap_ssid="Smart Home";
const char* ap_pass="12345678";

// ===== PINS =====
#define ZC_PIN D1
#define TRIAC_PIN D6
#define IR_PIN D5
#define LIGHT1_PIN D2
#define LIGHT2_PIN D8

// ===== SPEED =====
volatile int delayMicros=5000;
const int MIN_DELAY=1500;
const int MAX_DELAY=9000;
bool fanEnabled=false;   // FAN STARTS ON

// ===== LIGHT STATES =====
bool light1State=false;
bool light2State=false;

// ===== TRIAC =====
volatile bool zcDetected=false;
volatile bool triacOn=false;
volatile unsigned long zcTime=0;
volatile unsigned long lastTriacFire=0;
volatile unsigned long lastZCTime=0;

const int TRIAC_PULSE=10;
const int ZC_DEBOUNCE=5000;

// ===== IR =====
IRrecv irrecv(IR_PIN);
decode_results results;

// ===== SERVER =====
ESP8266WebServer server(80);

// ===== CALCULATE CHECKSUM =====
uint32_t calculateChecksum(StateData& data) {
  uint32_t sum = 0;
  sum += data.delayMicros;
  sum += data.fanEnabled ? 1 : 0;
  sum += data.light1State ? 2 : 0;
  sum += data.light2State ? 4 : 0;
  return sum * 31; // Simple checksum
}

// ===== LOAD STATE FROM EEPROM =====
void loadState() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Read state data from EEPROM
  for(int i = 0; i < sizeof(StateData); i++) {
    ((byte*)&stateData)[i] = EEPROM.read(STATE_DATA_ADDR + i);
  }
  
  // Validate checksum
  uint32_t calculatedChecksum = calculateChecksum(stateData);
  if(calculatedChecksum == stateData.checksum) {
    // Data is valid, apply the saved state
    delayMicros = stateData.delayMicros;
    fanEnabled = stateData.fanEnabled;
    light1State = stateData.light1State;
    light2State = stateData.light2State;
    
    Serial.println("State loaded from EEPROM");
    Serial.print("Fan: "); Serial.println(fanEnabled ? "ON" : "OFF");
    Serial.print("Light1: "); Serial.println(light1State ? "ON" : "OFF");
    Serial.print("Light2: "); Serial.println(light2State ? "ON" : "OFF");
    Serial.print("Fan Speed: "); Serial.println(delayMicros);
  } else {
    Serial.println("Invalid state data, using defaults");
  }
}

// ===== SAVE STATE TO EEPROM =====
void saveState() {
  // Update state data
  stateData.delayMicros = delayMicros;
  stateData.fanEnabled = fanEnabled;
  stateData.light1State = light1State;
  stateData.light2State = light2State;
  stateData.checksum = calculateChecksum(stateData);
  
  // Write to EEPROM
  for(int i = 0; i < sizeof(StateData); i++) {
    EEPROM.write(STATE_DATA_ADDR + i, ((byte*)&stateData)[i]);
  }
  EEPROM.commit();
  
  Serial.println("State saved to EEPROM");
}

// ===== APPLY SAVED OUTPUTS =====
void applySavedStates() {
  // Apply light states
  digitalWrite(LIGHT1_PIN, light1State ? HIGH : LOW);
  digitalWrite(LIGHT2_PIN, light2State ? HIGH : LOW);
  
  Serial.println("Saved outputs applied");
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

// ===== MAIN PAGE =====
String webpage() {

 int percent = map(delayMicros, MAX_DELAY, MIN_DELAY, 0, 100);
 String fanAnim = fanEnabled ? "spin 0.8s linear infinite" : "none";

 String p = "<!DOCTYPE html><html><head>";
 p += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
 p += "<style>";
 p += "body{background:#0f172a;font-family:Arial;color:white;margin:0}";
 p += ".top{font-size:24px;padding:15px;background:#020617;text-align:center;position:relative;letter-spacing:1px}";
 // Fixed refresh button using SVG
 p += ".refresh{position:absolute;right:12px;top:12px;background:rgba(255,255,255,0.2);border:none;border-radius:50%;width:28px;height:28px;cursor:pointer;transition:0.3s;display:flex;align-items:center;justify-content:center}";
 p += ".refresh:hover{background:rgba(255,255,255,0.4);transform:rotate(90deg)}";
 p += ".card{background:#1e293b;margin:15px;border-radius:18px;padding:20px;text-align:center;box-shadow:0 4px 12px rgba(0,0,0,0.4);transition:0.3s}";
 p += ".card:hover{box-shadow:0 8px 20px rgba(0,0,0,0.6)}";
 p += "button{width:110px;height:42px;font-size:15px;margin:6px;border:none;border-radius:10px;color:white;cursor:pointer;transition:0.2s}";
 p += "button:hover{opacity:0.85;transform:scale(1.05)}";
 p += ".on{background:#22c55e}";
 p += ".off{background:#ef4444}";
 p += ".sp{background:#3b82f6}";
 p += ".relay{background:#f59e0b}";
 p += ".save{background:#8b5cf6}";
 p += ".fanbox{display:flex;justify-content:center;margin-top:10px}";
 p += ".fan{width:110px;height:110px;animation:" + fanAnim + "}";
 p += "@keyframes spin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}";
 p += ".bar{width:100%;height:18px;background:#334155;border-radius:10px;margin-top:15px}";
 p += ".fill{height:18px;background:#22c55e;border-radius:10px}";
 p += ".status{font-size:18px;margin-top:5px}";
 p += ".wifiButton{background:#3b82f6;padding:8px 18px;border-radius:10px;font-size:14px;cursor:pointer;transition:0.3s}";
 p += ".wifiButton:hover{background:#60a5fa;}";
 p += "</style></head><body>";

 // HEADER
 p += "<div class='top'>Smart Home";
 // SVG refresh icon
 p += "<button class='refresh' onclick='location.reload()'>";
 p += "<svg width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>";
 p += "<path d='M23 4v6h-6'></path>";
 p += "<path d='M1 20a10 10 0 0 1 18-6.5L23 10'></path>";
 p += "</svg>";
 p += "</button>";
 p += "</div>";

 // FAN CARD
 p += "<div class='card'>";
 p += "<div class='fanbox'>";
 p += "<svg class='fan' viewBox='0 0 100 100' fill='#60a5fa'>";
 p += "<circle cx='50' cy='50' r='6' fill='white'/>";
 p += "<path d='M50 10 Q70 30 50 50 Q30 30 50 10'/>";
 p += "<path d='M90 50 Q70 70 50 50 Q70 30 90 50'/>";
 p += "<path d='M50 90 Q30 70 50 50 Q70 70 50 90'/>";
 p += "<path d='M10 50 Q30 30 50 50 Q30 70 10 50'/>";
 p += "</svg>";
 p += "</div>";
 p += "<div class='status'>Fan ";
 p += (fanEnabled ? "ON" : "OFF");
 p += "</div>";
 p += "<br>";
 p += "<button class='on' onclick=\"location.href='/on'\">ON</button>";
 p += "<button class='off' onclick=\"location.href='/off'\">OFF</button>";
 p += "<br>";
 p += "<button class='sp' onclick=\"location.href='/up'\">Speed +</button>";
 p += "<button class='sp' onclick=\"location.href='/down'\">Speed -</button>";
 p += "<div class='bar'><div class='fill' style='width:";
 p += percent;
 p += "%'></div></div>";
 p += "</div>";

 // LIGHT CARD
 p += "<div class='card'>";
 p += "<h3>Lights</h3>";
 p += "Light 1: ";
 p += (light1State ? "ON" : "OFF");
 p += "<br>";
 p += "<button class='relay' onclick=\"location.href='/l1on'\">ON</button>";
 p += "<button class='relay' onclick=\"location.href='/l1off'\">OFF</button>";
 p += "<br><br>";
 p += "Light 2: ";
 p += (light2State ? "ON" : "OFF");
 p += "<br>";
 p += "<button class='relay' onclick=\"location.href='/l2on'\">ON</button>";
 p += "<button class='relay' onclick=\"location.href='/l2off'\">OFF</button>";
 p += "</div>";

 // SAVE STATE CARD
 p += "<div class='card'>";
 p += "<button class='save' onclick=\"location.href='/savestate'\">Save Current State</button>";
 p += "<p style='font-size:12px;margin-top:10px;color:#cbd5e1'>Saves fan speed, status & lights<br>Restores after power loss/load shedding</p>";
 p += "</div>";

 // WIFI SETTINGS BUTTON
 p += "<div class='card'>";
 p += "<button class='wifiButton' onclick=\"location.href='/wifi'\">WiFi Settings</button>";
 p += "</div>";

 p += "</body></html>";
 return p;
}

// ===== WIFI PAGE =====
String wifiPage(){

 String p="<!DOCTYPE html><html><body style='font-family:Arial;text-align:center'>";
 p+="<h2>WiFi Setup</h2>";
 p+="<form action='/save'>";
 p+="SSID:<br>";
 p+="<input name='s'><br>";
 p+="Password:<br>";
 p+="<input name='p'><br><br>";
 p+="<input type='submit' value='Save'>";
 p+="</form>";
 p+="</body></html>";

 return p;
}

// ===== ROUTES =====
void root(){server.send(200,"text/html",webpage());}

void fanON(){fanEnabled=true;server.sendHeader("Location","/");server.send(303);}
void fanOFF(){fanEnabled=false;server.sendHeader("Location","/");server.send(303);}

void speedUp(){
 if(delayMicros>MIN_DELAY)delayMicros-=500;
 server.sendHeader("Location","/");
 server.send(303);
}

void speedDown(){
 if(delayMicros<MAX_DELAY)delayMicros+=500;
 server.sendHeader("Location","/");
 server.send(303);
}

// LIGHT 1
void l1on(){
 light1State=true;
 digitalWrite(LIGHT1_PIN,HIGH);
 server.sendHeader("Location","/");
 server.send(303);
}

void l1off(){
 light1State=false;
 digitalWrite(LIGHT1_PIN,LOW);
 server.sendHeader("Location","/");
 server.send(303);
}

// LIGHT 2
void l2on(){
 light2State=true;
 digitalWrite(LIGHT2_PIN,HIGH);
 server.sendHeader("Location","/");
 server.send(303);
}

void l2off(){
 light2State=false;
 digitalWrite(LIGHT2_PIN,LOW);
 server.sendHeader("Location","/");
 server.send(303);
}

// ===== SAVE STATE HANDLER =====
void handleSaveState(){
 saveState();
 server.send(200,"text/html","<html><body style='font-family:Arial;text-align:center;padding-top:50px'><h2>State Saved Successfully!</h2><p>Fan speed, status and light states saved.</p><p><a href='/'>Back to Home</a></p></body></html>");
}

void wifi(){server.send(200,"text/html",wifiPage());}

void save(){
 String s=server.arg("s");
 String p=server.arg("p");
 saveWiFi(s,p);
 server.send(200,"text/html","Saved. Rebooting...");
 delay(2000);
 ESP.restart();
}

// ===== WIFI START =====
void startWiFi(){
 WiFi.mode(WIFI_AP_STA);
 WiFi.softAP(ap_ssid,ap_pass);
 WiFi.begin(wifi_ssid,wifi_pass);
}

// ===== ZERO CROSS =====
void ICACHE_RAM_ATTR zeroCrossISR(){
 unsigned long now=micros();
 if(now-lastZCTime<ZC_DEBOUNCE)return;
 lastZCTime=now;
 zcDetected=true;
 zcTime=now;
 digitalWrite(TRIAC_PIN,LOW);
}

// ===== TRIAC =====
void handleTriac(){

 if(!zcDetected)return;

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

// ===== IR =====
void handleIR(uint32_t code){

 if(code==0x807F0AF5 && delayMicros>MIN_DELAY)
 delayMicros-=500;

 else if(code==0x807F8A75 && delayMicros<MAX_DELAY)
 delayMicros+=500;

 else if(code==0x807F30CF)
 fanEnabled=!fanEnabled;

 else if(code==0x807F728D){
 light1State=!light1State;
 digitalWrite(LIGHT1_PIN,light1State);
 }

 else if(code==0x807FB04F){
 light2State=!light2State;
 digitalWrite(LIGHT2_PIN,light2State);
 }
}

// ===== SETUP =====
void setup(){

 Serial.begin(115200);

 pinMode(ZC_PIN,INPUT);
 pinMode(TRIAC_PIN,OUTPUT);

 pinMode(LIGHT1_PIN,OUTPUT);
 pinMode(LIGHT2_PIN,OUTPUT);

 digitalWrite(LIGHT1_PIN,LOW);
 digitalWrite(LIGHT2_PIN,LOW);

 attachInterrupt(digitalPinToInterrupt(ZC_PIN),zeroCrossISR,RISING);

 irrecv.enableIRIn();

 loadWiFi();
 
 // Load previous state before starting WiFi
 loadState();
 
 startWiFi();
 
 // Apply saved light states
 applySavedStates();

 MDNS.begin("smarthome");

 server.on("/",root);
 server.on("/on",fanON);
 server.on("/off",fanOFF);
 server.on("/up",speedUp);
 server.on("/down",speedDown);

 server.on("/l1on",l1on);
 server.on("/l1off",l1off);
 server.on("/l2on",l2on);
 server.on("/l2off",l2off);

 server.on("/savestate",handleSaveState);

 server.on("/wifi",wifi);
 server.on("/save",save);

 server.begin();
}

// ===== LOOP =====
void loop(){

 handleTriac();

 if(irrecv.decode(&results)){
  handleIR(results.value);
  irrecv.resume();
 }

 server.handleClient();
 MDNS.update();
}
