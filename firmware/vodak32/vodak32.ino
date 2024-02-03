#include "WiFi.h"
#include <Preferences.h>
#include "ESPAsyncWebServer.h"
#include <AsyncElegantOTA.h>
#include "SPIFFS.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "time.h"
#include <string.h>

#define DBG_ENABLE_ERROR
#define DBG_ENABLE_WARNING
#define DBG_ENABLE_INFO
#define DBG_ENABLE_DEBUG
#define DBG_ENABLE_VERBOSE
#include <107-Arduino-Debug.hpp>

// version
#define VER_MAJOR	0
#define VER_MINOR 1
#define VER_PATCH	0

// wifi defaults
#define DEF_SSID "Vodak32"
#define DEF_PASSWORD "12345678"
#define WIFI_TIMEOUT 10
#define DEF_TIMEZONE 10*3600

// pin connections
#define ONE_WIRE_BUS 21
#define VOLT_SENSOR  36
#define STEAM_HEAT	 14
#define HEADS_HEAT 	 22
#define STEAM_VALVE  13
#define WASH_VALVE   12
#define FEED_VALVE   26
#define FERM1_VALVE  25
#define FERM2_VALVE  33
#define XTRA_HEAT    32

// system parameters
#define SENSOR_COUNT 8 // number of onewire devices on bus
#define FLOW_COUNT   5 // number of valves
#define FETS_COUNT	 6 // number of FET switches - 5 are valves, 1 extra (indexing first 6 outpins)
#define SENSOR_UPDATE_SECS  10 // interval for sensor updates (secs)
#define STATE_CYCLE_SECS    30 // interval for state machine cycle (secs)
#define FLOW_UPDATE_SECS		5  // interval for still flow cycle (secs)(wash, steam valves)
#define FERM_UPDATE_SECS		300 // interval for fermentation valve drip cycle (secs)(feed, ferm1, ferm2 valves)
#define DEF_VOLTS_MAX       61.4  // based on resistor divider values: 3.2/Vmax = 2.2/(40+2.2) 
#define DEF_STEAM_OHMS 10   // calc based on maxV and maxP, 36*36/129.6 for 36V system
#define DEF_HEADS_OHMS 31.6	// calc based on maxV and element R, 36*36/41 for 36V system
#define DEF_FERM_FLOW	 1000 // fermentation flow rate, based on 10L kegs and 10 day refills
#define DEF_STEAM_FLOW 2.6  // max steam flow rate, arbitrarily taken from "tight" specs 	

// pwm channels
#define STEAM_PWM_CHANNEL	0
#define HEADS_PWM_CHANNEL	1
#define PWM_WIDTH					8   // pwm bits
#define PWM_HZ						100 // pwm frequency

// op modes
#define OP_MODE_NONE    		0 // no fermentation, wash and steam valves only
#define OP_MODE_PREFILL 		1 // operate feed,ferm valves until fills sep tank, no distilling
#define OP_MODE_SYNC  			2 // sync all valves for distilling
#define OP_MODE_DELAY_SYNC	3 // as above, but delay sep fill until after distill done

// the machine states
#define STATE_SLEEP 	0
#define STATE_HEAT_UP 1
#define STATE_COOL_DN 2
#define STATE_RUN 		3

// the temp sensor row ids
#define TEMP_HEADS  0
#define TEMP_HEARTS 1
#define TEMP_MID    2
#define TEMP_BASE   3
#define TEMP_TAILS  4
#define TEMP_STEAM  5
#define TEMP_FERM1  6
#define TEMP_FERM2  7

// the flow valve row ids, col 0=high, col 1=low, col 2=now
#define FLOW_STEAM	0
#define FLOW_WASH		1
#define FLOW_FEED		2
#define FLOW_FERM1	3
#define FLOW_FERM2	4

typedef struct vtime {
  uint8_t mins;
  uint8_t hrs;
  bool once;
} vtime;

String ssid;
String password;
File op_log;

const char* ntpServer = "pool.ntp.org";
long  gmtOffset_sec;
int   daylightOffset_sec;
hw_timer_t *timer = NULL;
uint8_t out_pins[] = { FEED_VALVE, FERM1_VALVE, FERM2_VALVE, WASH_VALVE, STEAM_VALVE, XTRA_HEAT, STEAM_HEAT, HEADS_HEAT };
int numberOfSensors;
float volts_max, volts_now;
float steam_ohms, heads_ohms;
float ferm_flow, max_steam_flow;
uint8_t duty_steam, duty_heads, max_steam_duty, max_heads_duty, op_mode;
uint16_t flow_rates[FLOW_COUNT][3];  // high/low/now triplets in drops per minute where 20 drops = 1ml
uint16_t tank_levels[FLOW_COUNT][2];  // full/now pairs in millilitres
uint16_t on_fets[FETS_COUNT]; // tick counts for FETS enabled (on)
vtime timerOn = {0,0,false}, timerOff = {0,0,false};
DeviceAddress sens_addrs[SENSOR_COUNT];
float tempC[SENSOR_COUNT];
uint8_t state_now = STATE_SLEEP;
bool doSensorUpdate = false;
bool doStateUpdate = false;
bool doFermUpdate = false;
bool doFlowUpdate = false;
bool bRunning = false;
volatile uint32_t ticks;

DEBUG_INSTANCE(128, Serial);
Preferences nvs;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);

void IRAM_ATTR ticker()
{
  ticks++; // ticks are tenths of a second
  if((ticks % 10) == 0){
		if((ticks % (SENSOR_UPDATE_SECS*10)) == 0)
			doSensorUpdate = true;
		if((ticks % (STATE_CYCLE_SECS*10)) == 0)
			doStateUpdate = true;
		if((ticks % (FLOW_UPDATE_SECS*10)) == 0)
			doFlowUpdate = true;
		if((ticks % (FERM_UPDATE_SECS*10)) == 0)
			doFermUpdate = true;
	}
	for(int i=0; i < FETS_COUNT; i++){
		if(on_fets[i] != 0){
			if(--on_fets[i] == 0)
				digitalWrite(out_pins[i], LOW); // close FET
			else 
				digitalWrite(out_pins[i], HIGH); // open FET
		}
	}
}

void stateUpdateSleep(void){
  DBG_DEBUG("State [SLEEP] update.");
}

void stateUpdateHeatUp(void){
  DBG_DEBUG("State [HEAT_UP] update.");
}

void stateUpdateCoolDn(void){
  DBG_DEBUG("State [COOL_DN] update.");
}

void stateUpdateRun(void){
  DBG_DEBUG("State [RUN] update.");
}

void flowUpdate(void){
  DBG_DEBUG("Flow update.");
}

void fermUpdate(void){
  DBG_DEBUG("Ferm update.");
}

void sendJsonData(AsyncWebServerRequest *request){
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		DynamicJsonBuffer jsonBuffer;
		JsonObject &data = jsonBuffer.createObject();  // holder for returning state data
    JsonArray& temp = data.createNestedArray("temp");
    for(int i=0; i < SENSOR_COUNT; i++)
      temp.add(tempC[i]);
		data["volts"] = volts_now;
		data["state"] = state_now;
    data["run"] = bRunning;
    JsonArray& flows = data.createNestedArray("flows");
    for(int i=0; i < FLOW_COUNT; i++)
      flows.add(flow_rates[i][2]/20);
    data["steam"] = volts_now * volts_now * duty_steam / steam_ohms / ((1<<PWM_WIDTH)-1); // power depends on voltage and duty cycle, R const
    data["heads"] = volts_now * volts_now * duty_heads / heads_ohms / ((1<<PWM_WIDTH)-1);
    if(request->hasParam("cfg")){ // send cfg data only when requested
      JsonObject& cfg = data.createNestedObject("cfg"); 
			cfg["ssid"] = ssid;
			cfg["pwd"] = password;
			cfg["sR"] = steam_ohms;
			cfg["sD"] = max_steam_duty*100/((1<<PWM_WIDTH)-1);
			cfg["hR"] = heads_ohms;
			cfg["hD"] = max_heads_duty*100/((1<<PWM_WIDTH)-1);
			JsonArray& highrates = cfg.createNestedArray("hfr");
			for(int i=0; i < FLOW_COUNT; i++)
				highrates.add(flow_rates[i][0]/20);
			JsonArray& lowrates = cfg.createNestedArray("lfr");
			for(int i=0; i < FLOW_COUNT; i++)
				lowrates.add(flow_rates[i][1]/20);
			}
		data.printTo(*response);
		request->send(response);
}

void onSaveCfg(AsyncWebServerRequest *request){
  // save cfg data using preferences
  if(request->hasParam("ssid", true)){
		ssid = request->getParam("ssid", true)->value();
		password = request->getParam("pwd", true)->value();
		nvs.begin("wifi");
		nvs.putString("ssid", ssid);
		nvs.putString("password", password);
		nvs.end();
		DBG_INFO("Saved wifi cfg.");
		return;
	}
	nvs.begin("config");
	if(request->hasParam("opmode", true)){
		op_mode = atoi(request->getParam("opmode", true)->value().c_str());
		nvs.putUInt("opmode", op_mode);
		DBG_INFO("Op mode set to %1d", op_mode);
	}
	if(request->hasParam("sR", true)){
		steam_ohms = atof(request->getParam("sR", true)->value().c_str());
		heads_ohms = atof(request->getParam("hR", true)->value().c_str());
		ferm_flow = atof(request->getParam("fR", true)->value().c_str());
		max_steam_flow = atof(request->getParam("sF", true)->value().c_str());
		nvs.putFloat("steamohms", steam_ohms);
		nvs.putFloat("headsohms", heads_ohms);
		nvs.putFloat("fermflow", ferm_flow);
		nvs.putFloat("steamflow", max_steam_flow);
		max_steam_duty = atoi(request->getParam("sD", true)->value().c_str())*((1<<PWM_WIDTH)-1)/100;
		max_heads_duty = atoi(request->getParam("hD", true)->value().c_str())*((1<<PWM_WIDTH)-1)/100;
		nvs.putUInt("maxsteam", max_steam_duty);
		nvs.putUInt("maxheads", max_heads_duty);
		DBG_INFO("Saved power cfg.");
	}
  if(request->hasParam("vN", true) && volts_now != 0){
		volts_max = atof(request->getParam("vN", true)->value().c_str()) * 4095 / volts_now; // calibrate with user vN value
		nvs.putFloat("voltsmax", volts_max);
		DBG_INFO("Saved volts calibration.");
	}
	if(request->hasParam("valve", true)){
		char key[] = "Fxx";
		uint8_t valve = atoi(request->getParam("valve", true)->value().c_str());
		uint8_t rate = atoi(request->getParam("rate", true)->value().c_str());
		flow_rates[valve][rate] = atoi(request->getParam("flow", true)->value().c_str())*20;
		key[1] = 0x30 + valve;
		key[2] = 0x30 + rate;
		nvs.putUInt(key, flow_rates[valve][rate]);
		DBG_INFO("Saved flow calibration: %s.", key);
	}
	if(request->hasParam("tank", true)){
		char key[] = "Txx";
		uint8_t tank = atoi(request->getParam("tank", true)->value().c_str());
		uint8_t level = atoi(request->getParam("level", true)->value().c_str());
		tank_levels[tank][level] = atoi(request->getParam("volume", true)->value().c_str());
		key[1] = 0x30 + tank;
		key[2] = 0x30 + level;
		nvs.putUInt(key, tank_levels[tank][level]);
		DBG_INFO("Saved tank levels: %s.", key);
	}
	if(request->hasParam("sid", true)){
		char key[] = "Sx";
    float swapC;
		DeviceAddress swapAddr;
		uint8_t sid = atoi(request->getParam("sid", true)->value().c_str());
		uint8_t tid = atoi(request->getParam("tid", true)->value().c_str());
		memcpy(swapAddr, sens_addrs[sid], 8);
    swapC = tempC[sid];
    memcpy(sens_addrs[sid], sens_addrs[tid], 8);
    tempC[sid] = tempC[tid];
		memcpy(sens_addrs[tid], swapAddr, 8);
    tempC[tid] = swapC;
		key[1] = 0x30+sid;
		nvs.putBytes(key, sens_addrs[sid], 8);
		key[1] = 0x30+tid;
		nvs.putBytes(key, sens_addrs[tid], 8);
		DBG_INFO("Saved sensor assignment: %s.", key);
	}
	if(request->hasParam("onHr", true)){
		int h = atoi(request->getParam("onHr", true)->value().c_str());
		int m = atoi(request->getParam("onMin", true)->value().c_str());
		int once = atoi(request->getParam("onOnce", true)->value().c_str());
		if(h < 24 && h >=0 && m < 60 && m >= 0){
			timerOn.hrs = h;
			timerOn.mins = m;
			timerOn.once = once;
		}
		h = atoi(request->getParam("offHr", true)->value().c_str());
		m = atoi(request->getParam("offMin", true)->value().c_str());
		once = atoi(request->getParam("offOnce", true)->value().c_str());
		if(h < 24 && h >=0 && m < 60 && m >= 0){
			timerOff.hrs = h;
			timerOff.mins = m;
			timerOff.once = once;
		}		
		nvs.putBytes("timerOn", &timerOn, 3);
		nvs.putBytes("timerOff", &timerOff, 3);
	}
	nvs.end();
  request->redirect("/data?cfg=true");
}
void onRunChg(AsyncWebServerRequest *request){
  if(request->hasParam("on", true)){
    bRunning = request->getParam("on", true)->value() == "true";
    DBG_INFO("Run state: %s.", bRunning ? "ON" : "OFF");
	}
	if(request->hasParam("open", true)){
		uint8_t valve = atoi(request->getParam("open", true)->value().c_str());
		uint16_t tenths = atof(request->getParam("secs", true)->value().c_str())*10;
		if(tenths == 0)
			tenths = 10;
		on_fets[valve-1] = tenths+1; // will open valve on next tick, with count down to close
		DBG_INFO("Open valve %d for %d tenths.", valve, tenths);
	}
	request->send(200);
}
void onReset(AsyncWebServerRequest *request){
	if(request->hasParam("sensors")){
		char key[] = "Sx";
		nvs.begin("config");
		for(int i = 0; i < SENSOR_COUNT; i++) {
			key[1] = i + 0x30;
			nvs.remove(key);
		}
		nvs.end();
		DBG_INFO("Sensors reset.");
	}
}

void (*(stateFuncs[]))() = {stateUpdateSleep, stateUpdateHeatUp, stateUpdateCoolDn, stateUpdateRun};

void setup() {
  // serial for log / debug output
  Serial.begin(115200); 
  DBG_INFO("vodak32 - version %d.%d.%d", VER_MAJOR, VER_MINOR, VER_PATCH);
  
  // init GPIO pins
  pinMode(ONE_WIRE_BUS, INPUT);
  for(int i = 0; i < sizeof(out_pins); i++) {
    pinMode(out_pins[i], OUTPUT);
    digitalWrite(out_pins[i], LOW);
  }
  
  // initialize SPIFFS
  if(!SPIFFS.begin(true)){
    DBG_ERROR("Error while mounting SPIFFS");
    return;
  }
  
  // init op log
  DBG_INFO("Opening Op Log.");
  op_log = SPIFFS.open("/oplog", "a");
  op_log.printf("vodak32 - version %d.%d.%d\n", VER_MAJOR, VER_MINOR, VER_PATCH);
  op_log.close();
  
  // read wifi config if set, or use defaults
  nvs.begin("wifi", true);
  ssid = nvs.getString("ssid", DEF_SSID);
  password = nvs.getString("password", DEF_PASSWORD);
  nvs.end();
  
  // connect to WiFi hotspot or fallbck as AP mode
  int secs = 0;
  while(1) {
    WiFi.disconnect();
	  WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    DBG_INFO("Connecting to WiFi: %s", ssid.c_str());
		while (secs++ < WIFI_TIMEOUT && WiFi.status() != WL_CONNECTED)
			delay(1000);
		if(secs < WIFI_TIMEOUT) 
			break;
		DBG_WARNING("\nCannot connect. Creating AP: %s", ssid.c_str());
		WiFi.disconnect();
		WiFi.mode(WIFI_AP);
		if(WiFi.softAP(ssid.c_str(), password.c_str())){
			delay(100);
      IPAddress Ip(192, 168, 1, 1);
      IPAddress NMask(255, 255, 255, 0);
      WiFi.softAPConfig(Ip, Ip, NMask);
			break;
    }
		DBG_WARNING("AP failed. Starting over.");
		secs = 0;
	}
	DBG_INFO("WiFi up at: %s", secs < WIFI_TIMEOUT ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  
  
  nvs.begin("config");
  
  // load temperature sensor ids 
  char key[] = "Sx\0";
  for(int i = 0; i < SENSOR_COUNT; i++) {
		key[1] = i + 0x30;
    nvs.getBytes(key, sens_addrs[i], 8);
	 }
	 
  // init temperature bus, save any found devices
  sensors.begin();
  numberOfSensors = sensors.getDeviceCount();
  DBG_INFO("Scanning temperature sensors. Found: %d", numberOfSensors);
  if(numberOfSensors != SENSOR_COUNT)
    DBG_WARNING("Incorrect number of temperature sensors detected.");
  int j;
  DeviceAddress tmpAddr;
  for(int i = 0; i < numberOfSensors; i++){
		uint8_t empty_slot = 0xFF;
		sensors.getAddress(tmpAddr, i);
		for(j = 0; j < SENSOR_COUNT; j++){
			if(sens_addrs[j][7] == 0) // device family always non-zero
				empty_slot = j;
			if(!memcmp(sens_addrs[j], tmpAddr, 8))
				break;
		}
		if(j == SENSOR_COUNT){
			if(empty_slot != 0xFF){
				key[1] = empty_slot + 0x30;
				memcpy(sens_addrs[empty_slot], tmpAddr, 8);
				nvs.putBytes(key, sens_addrs[empty_slot], 8);
			}
			else
				DBG_WARNING("Sensor found but no empty slots.");
		}
	}

  // read config values
  DBG_INFO("Loading config.");
  volts_max = nvs.getFloat("voltsmax", DEF_VOLTS_MAX);
  steam_ohms = nvs.getFloat("steamohms", DEF_STEAM_OHMS);
  heads_ohms= nvs.getFloat("headsohms", DEF_HEADS_OHMS);
  ferm_flow = nvs.getFloat("fermflow", DEF_FERM_FLOW);
  max_steam_flow = nvs.getFloat("steamflow", DEF_STEAM_FLOW);
  op_mode = nvs.getUInt("opmode", OP_MODE_NONE);
  max_steam_duty = nvs.getUInt("maxsteam", (1<<PWM_WIDTH)-1);
  max_heads_duty= nvs.getUInt("maxheads", (1<<PWM_WIDTH)-1);
  gmtOffset_sec = nvs.getInt("gmtoffset", DEF_TIMEZONE);
  daylightOffset_sec = nvs.getInt("dstoffset", 0);
  key[0] = 'F'; // flow_rates key
  for(int i = 0; i < FLOW_COUNT; i++) {
		key[1] = i + 0x30;
		for(int j = 0; j < 3; j++){
			key[2] = j + 0x30;
			flow_rates[i][j] = nvs.getUInt(key, 0);
		}
	 }
	key[0] = 'T'; // tank_levels key
  for(int i = 0; i < FLOW_COUNT; i++) {
		key[1] = i + 0x30;
		for(int j = 0; j < 2; j++){
			key[2] = j + 0x30;
			tank_levels[i][j] = nvs.getUInt(key, 0);
		}
	 }
	nvs.getBytes("timerOn", &timerOn, 3);
	nvs.getBytes("timerOff", &timerOff, 3);
	
  nvs.end();

  // enable real time clock and ntp syncs
  struct tm timeinfo;
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(2000);
  if(!getLocalTime(&timeinfo))
    DBG_WARNING("Failed to obtain time.");
  else
		DBG_INFO("Time: %d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  
  // routes for web app
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){ sendJsonData(request); });
  server.on("/cfg", HTTP_POST, [](AsyncWebServerRequest *request){ onSaveCfg(request); });
  server.on("/run", HTTP_POST, [](AsyncWebServerRequest *request){ onRunChg(request); });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){ onReset(request); });

  AsyncElegantOTA.begin(&server);
  server.begin();
  
  // setup heater pwm channels
  DBG_INFO("Setting PWM channels.");
  ledcSetup(STEAM_PWM_CHANNEL, PWM_HZ, PWM_WIDTH);
  ledcAttachPin(STEAM_HEAT, STEAM_PWM_CHANNEL);  // use ledcWrite(STEAM_PWM_CHANNEL, duty_steam) to update
  ledcSetup(HEADS_PWM_CHANNEL, PWM_HZ, PWM_WIDTH);
  ledcAttachPin(HEADS_HEAT, HEADS_PWM_CHANNEL);
  
  // setup tick timer for sensor reads and state machine
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &ticker, true);
  timerAlarmWrite(timer, 100000, true);
  timerAlarmEnable(timer);
  DBG_INFO("Ticker started.");
}

void loop() {

  if(doSensorUpdate){ 
    DBG_DEBUG("Reading sensors.");
    sensors.requestTemperatures();
    for(int i = 0; i < SENSOR_COUNT; i++)
      if(sens_addrs[i][7]) // // device family always non-zero when sensor exists
        tempC[i] = sensors.getTempC(sens_addrs[i]);
    volts_now = analogRead(VOLT_SENSOR)*volts_max/4095;
    doSensorUpdate = false;
  }
	if(doStateUpdate){ 
    stateFuncs[state_now]();
    doStateUpdate = false;
	}
	if(doFlowUpdate){ 
    flowUpdate();
    doFlowUpdate = false;
	}
	if(doFermUpdate){ 
    fermUpdate();
    doFermUpdate = false;
	}
}
