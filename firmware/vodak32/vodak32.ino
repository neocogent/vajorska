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
#include <stdarg.h>

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
#define HEADS_HEAT 	 27
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
#define SECS_FULL_DAY				86400 // seconds in a 24 hr day
#define SECS_PART_DAY				64800 // seconds in a 18 hr day (excludes max solar/distill time)
#define SENSOR_UPDATE_SECS  10 // interval for sensor updates (secs)
#define STATE_CYCLE_SECS    30 // interval for state machine cycle (secs)
#define FLOW_UPDATE_SECS		10 // interval for still flow cycle (secs)(wash, steam valves)
#define FERM_UPDATE_SECS		900 // interval for fermentation valve drip cycle (secs)(feed, ferm1, ferm2 valves)
#define DEF_VOLTS_MAX       61.4  // based on resistor divider values: 3.2/Vmax = 2.2/(40+2.2) 
#define DEF_STEAM_OHMS 10   // calc based on maxV and maxP, 36*36/129.6 for 36V system
#define DEF_HEADS_OHMS 31.6	// calc based on maxV and element R, 36*36/41 for 36V system
#define DEF_FERM_FLOW	 1000 // fermentation flow rate, based on 10L kegs and 10 day refills
#define DEF_STEAM_FLOW 2.6  // max steam flow rate, arbitrarily taken from "tight" specs 	
#define DEF_VOLTS_RUN  42   // solar voltage to run distill, wake from sleep
#define ADC_MAX_FACTOR 0.94 // correction for non-linear high end of ADC readings
#define ADC_OFFSET     0.15 // correction for non-linear low end of ADC readings

// pwm channels
#define STEAM_PWM_CHANNEL	0
#define HEADS_PWM_CHANNEL	1
#define PWM_WIDTH					8   // pwm bits
#define PWM_HZ						100 // pwm frequency

// ferm modes
#define FERM_MODE_NONE    		0 // no fermentation, feed, ferm1 and ferm2 valves off
#define FERM_MODE_PREFILL 		1 // operate feed,ferm valves until fills sep tank, no distilling
#define FERM_MODE_SYNC  			2 // sync all valves for distilling
#define FERM_MODE_DELAY_SYNC	3 // as above, but delay sep fill until after distill done

String ferm_modes_msg[] = { "None", "Prefill", "Sync", "Delayed Sync" };

// still modes
#define STILL_MODE_NONE    		0 // no distilling, wash and steam valves off
#define STILL_MODE_SOLAR	 		1 // distill between 8am-4pm, voltage threshold starts
#define STILL_MODE_MANUAL			2 // distill when run is ON, or timer enabled

String still_modes_msg[] = { "None", "Solar", "Manual" };

// the machine states
#define STATE_SLEEP 	0
#define STATE_HEAT_UP 1
#define STATE_COOL_DN 2
#define STATE_RUN 		3

String states_msg[] = { "Sleep", "Heat Up", "Cool Down", "Run" };

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
#define FLOW_RATE_HIGH	0
#define FLOW_RATE_LOW		1
#define FLOW_RATE_NOW 	2
#define TANK_LEVEL_FULL 0
#define TANK_LEVEL_NOW  1

// on_fets indices
#define FETS_FEED		0
#define FETS_FERM1	1
#define FETS_FERM2	2
#define FETS_WASH		3
#define FETS_STEAM	4
#define FETS_XTRA		5

String valves_msg[] = { "Feed", "Ferm1", "Ferm2", "Wash", "Steam", "Xtra" };

typedef struct vtime {
  uint8_t mins;
  uint8_t hrs;
  bool once;
} vtime;

String ssid;
String password;

const char* ntpServer = "pool.ntp.org";
long  gmtOffset_sec;
int   daylightOffset_sec;
hw_timer_t *timer = NULL;
uint8_t out_pins[] = { FEED_VALVE, FERM1_VALVE, FERM2_VALVE, WASH_VALVE, STEAM_VALVE, XTRA_HEAT, STEAM_HEAT, HEADS_HEAT };
int numberOfSensors;
float volts_max, volts_now, volts_run;
float steam_ohms, heads_ohms;
float ferm_flow, max_steam_flow;
uint8_t duty_steam, duty_heads, max_steam_duty, max_heads_duty, still_mode, ferm_mode;
uint16_t flow_rates[FLOW_COUNT][3];  // high/low/now triplets in drops per minute, 20 drops = 1ml
int32_t tank_levels[FLOW_COUNT][2];  // full/now pairs in drops
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
	if(still_mode != STILL_MODE_NONE){
		if(tank_levels[FLOW_WASH][TANK_LEVEL_NOW] > 0){
			float flow_cycle = flow_rates[FLOW_WASH][FLOW_RATE_NOW] * FLOW_UPDATE_SECS / 60; // drops this cycle
			float flow_now = flow_rates[FLOW_WASH][FLOW_RATE_LOW] + 
				(flow_rates[FLOW_WASH][FLOW_RATE_HIGH]-flow_rates[FLOW_WASH][FLOW_RATE_LOW]) 
				* ((float)tank_levels[FLOW_WASH][TANK_LEVEL_NOW]/(float)tank_levels[FLOW_WASH][TANK_LEVEL_FULL]); // drops/min now
			on_fets[FETS_WASH] = flow_cycle * 600 / flow_now + 0.5; // tenths to open valve
			DBG_DEBUG("WASH open %d tenths, flow_now %.0f drops/min", on_fets[FETS_WASH], flow_now);
			tank_levels[FLOW_WASH][TANK_LEVEL_NOW] -= flow_cycle;
			SaveTankLevel(FLOW_WASH, TANK_LEVEL_NOW);
			if(tank_levels[FLOW_WASH][TANK_LEVEL_NOW] <= 0)
				OpLog("Wash tank empty.");
		}
	if(tank_levels[FLOW_STEAM][TANK_LEVEL_NOW] > 0){
			float flow_cycle = flow_rates[FLOW_STEAM][FLOW_RATE_NOW] * FLOW_UPDATE_SECS / 60; // drops this cycle
			float flow_now = flow_rates[FLOW_STEAM][FLOW_RATE_LOW] + 
				(flow_rates[FLOW_STEAM][FLOW_RATE_HIGH]-flow_rates[FLOW_STEAM][FLOW_RATE_LOW]) 
				* ((float)tank_levels[FLOW_STEAM][TANK_LEVEL_NOW]/(float)tank_levels[FLOW_STEAM][TANK_LEVEL_FULL]); // drops/min now
			on_fets[FETS_STEAM] = flow_cycle * 600 / flow_now + 0.5; // tenths to open valve
			DBG_DEBUG("STEAM open %d tenths, flow_now %.0f drops/min", on_fets[FLOW_STEAM], flow_now);
			tank_levels[FLOW_STEAM][TANK_LEVEL_NOW] -= flow_cycle;
			SaveTankLevel(FLOW_STEAM, TANK_LEVEL_NOW);
			if(tank_levels[FLOW_STEAM][TANK_LEVEL_NOW] <= 0)
				OpLog("Water tank empty.");
		}
	}
}

void fermUpdate(void){
	DBG_DEBUG("Ferm update.");
	if((ferm_mode == FERM_MODE_NONE) || ((ferm_mode == FERM_MODE_DELAY_SYNC) && (state_now != STATE_SLEEP)))
		return;
		
	float cycles = ((ferm_mode == FERM_MODE_DELAY_SYNC) ? SECS_PART_DAY : SECS_FULL_DAY) / FERM_UPDATE_SECS; // ferm cycles per day
	float flow_cycle = ferm_flow*20 / cycles; // drops per ferm cycle
	DBG_DEBUG("cycles %.0f, flow_cycle %.0f", cycles, flow_cycle);
	
	if(tank_levels[FLOW_WASH][TANK_LEVEL_NOW] < tank_levels[FLOW_WASH][TANK_LEVEL_FULL]){
		flow_rates[FLOW_FERM2][FLOW_RATE_NOW] = flow_cycle*60/FERM_UPDATE_SECS + 0.5;
		float flow_now = flow_rates[FLOW_FERM2][FLOW_RATE_LOW] + 
			(flow_rates[FLOW_FERM2][FLOW_RATE_HIGH]-flow_rates[FLOW_FERM2][FLOW_RATE_LOW]) 
		  * ((float)tank_levels[FLOW_FERM2][TANK_LEVEL_NOW]/(float)tank_levels[FLOW_FERM2][TANK_LEVEL_FULL]); // drops/min now
		on_fets[FETS_FERM2] = flow_cycle * 600 / flow_now + 0.5; // tenths to open valve
		DBG_DEBUG("FERM2 open %d tenths, flow_now %.0f drops/min", on_fets[FETS_FERM2], flow_now);
		tank_levels[FLOW_WASH][TANK_LEVEL_NOW] += flow_cycle;
		SaveTankLevel(FLOW_WASH, TANK_LEVEL_NOW);
		tank_levels[FLOW_FERM2][TANK_LEVEL_NOW] -= flow_cycle;
		SaveTankLevel(FLOW_FERM2, TANK_LEVEL_NOW);
		if(tank_levels[FLOW_WASH][TANK_LEVEL_NOW] >= tank_levels[FLOW_WASH][TANK_LEVEL_FULL])
			OpLog("Wash tank full.");
	} else flow_rates[FLOW_FERM2][FLOW_RATE_NOW] = 0;
	
	if(tank_levels[FLOW_FERM2][TANK_LEVEL_NOW] < tank_levels[FLOW_FERM2][TANK_LEVEL_FULL]){
		flow_rates[FLOW_FERM1][FLOW_RATE_NOW] = flow_cycle*60/FERM_UPDATE_SECS + 0.5;
		float flow_now = flow_rates[FLOW_FERM1][FLOW_RATE_LOW] + 
			(flow_rates[FLOW_FERM1][FLOW_RATE_HIGH]-flow_rates[FLOW_FERM1][FLOW_RATE_LOW]) 
		  * ((float)tank_levels[FLOW_FERM1][TANK_LEVEL_NOW]/(float)tank_levels[FLOW_FERM1][TANK_LEVEL_FULL]); // drops/min now
		on_fets[FETS_FERM1] = flow_cycle * 600 / flow_now + 0.5; // tenths to open valve
		DBG_DEBUG("FERM1 open %d tenths, flow_now %.0f drops/min", on_fets[FETS_FERM1], flow_now);
		tank_levels[FLOW_FERM2][TANK_LEVEL_NOW] += flow_cycle;
		SaveTankLevel(FLOW_FERM2, TANK_LEVEL_NOW);
		tank_levels[FLOW_FERM1][TANK_LEVEL_NOW] -= flow_cycle;
		SaveTankLevel(FLOW_FERM1, TANK_LEVEL_NOW);
	} else flow_rates[FLOW_FERM1][FLOW_RATE_NOW] = 0;
	
	if(tank_levels[FLOW_FERM1][TANK_LEVEL_NOW] < tank_levels[FLOW_FERM1][TANK_LEVEL_FULL]){
		flow_rates[FLOW_FEED][FLOW_RATE_NOW] = flow_cycle*60/FERM_UPDATE_SECS + 0.5;
		float flow_now = flow_rates[FLOW_FEED][FLOW_RATE_LOW] + 
			(flow_rates[FLOW_FEED][FLOW_RATE_HIGH]-flow_rates[FLOW_FEED][FLOW_RATE_LOW]) 
		  * ((float)tank_levels[FLOW_FEED][TANK_LEVEL_NOW]/(float)tank_levels[FLOW_FEED][TANK_LEVEL_FULL]); // drops/min now
		on_fets[FETS_FEED] = flow_cycle * 600 / flow_now + 0.5; // tenths to open valve
		DBG_DEBUG("FEED open %d tenths, flow_now %.0f drops/min", on_fets[FETS_FEED], flow_now);
		tank_levels[FLOW_FERM1][TANK_LEVEL_NOW] += flow_cycle;
		SaveTankLevel(FLOW_FERM1, TANK_LEVEL_NOW);
		tank_levels[FLOW_FEED][TANK_LEVEL_NOW] -= flow_cycle;
		SaveTankLevel(FLOW_FEED, TANK_LEVEL_NOW);
	} else flow_rates[FLOW_FEED][FLOW_RATE_NOW] = 0;
	
	if(tank_levels[FLOW_FEED][TANK_LEVEL_NOW] < 0){
		tank_levels[FLOW_FEED][TANK_LEVEL_NOW] = 0;
		OpLog("Feed tank empty.");
	}
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
      flows.add(flow_rates[i][2]); // send drops per minute
    data["steam"] = volts_now * volts_now * duty_steam / steam_ohms / ((1<<PWM_WIDTH)-1); // power depends on voltage and duty cycle, R const
    data["heads"] = volts_now * volts_now * duty_heads / heads_ohms / ((1<<PWM_WIDTH)-1);
    if(request->hasParam("cfg")){ // send cfg data only when requested
      JsonObject& cfg = data.createNestedObject("cfg"); 
			cfg["ssid"] = ssid;
			cfg["pwd"] = password;
			cfg["mS"] = still_mode;
			cfg["mF"] = ferm_mode;
			cfg["fR"] = ferm_flow;
			cfg["sF"] = max_steam_flow;
			cfg["sR"] = steam_ohms;
			cfg["sD"] = max_steam_duty*100/((1<<PWM_WIDTH)-1);
			cfg["hR"] = heads_ohms;
			cfg["hD"] = max_heads_duty*100/((1<<PWM_WIDTH)-1);
			cfg["vR"] = volts_run;
			JsonArray& highrates = cfg.createNestedArray("hfr");
			for(int i=0; i < FLOW_COUNT; i++)
				highrates.add(flow_rates[i][0]/20);
			JsonArray& lowrates = cfg.createNestedArray("lfr");
			for(int i=0; i < FLOW_COUNT; i++)
				lowrates.add(flow_rates[i][1]/20);
			JsonArray& tankfull = cfg.createNestedArray("tf");
			for(int i=0; i < FLOW_COUNT; i++)
				tankfull.add(tank_levels[i][0]/20);
			}
		JsonArray& tanknow = data.createNestedArray("tn");
		for(int i=0; i < FLOW_COUNT; i++)
			tanknow.add(tank_levels[i][1]/20);
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
	if(request->hasParam("mS", true)){
		still_mode = atoi(request->getParam("mS", true)->value().c_str());
		if(still_mode == STILL_MODE_NONE){
			flow_rates[FLOW_WASH][2] = 0;
			flow_rates[FLOW_STEAM][2] = 0;
		}
		nvs.putUInt("stillmode", still_mode);
		OpLog("Still mode: %s.", still_modes_msg[still_mode]);
	}
	if(request->hasParam("mF", true)){
		ferm_mode = atoi(request->getParam("mF", true)->value().c_str());
		if(ferm_mode == FERM_MODE_NONE)
			flow_rates[FLOW_FERM1][2] = 0;
		nvs.putUInt("fermmode", ferm_mode);
		OpLog("Ferm mode: %s.", ferm_modes_msg[ferm_mode]);
	}
	if(request->hasParam("sR", true)){
		steam_ohms = atof(request->getParam("sR", true)->value().c_str());
		heads_ohms = atof(request->getParam("hR", true)->value().c_str());
		ferm_flow = atof(request->getParam("fR", true)->value().c_str());
		volts_run = atof(request->getParam("vR", true)->value().c_str());
		max_steam_flow = atof(request->getParam("sF", true)->value().c_str());
		nvs.putFloat("steamohms", steam_ohms);
		nvs.putFloat("headsohms", heads_ohms);
		nvs.putFloat("fermflow", ferm_flow);
		nvs.putFloat("steamflow", max_steam_flow);
		nvs.putFloat("voltsrun", volts_run);
		max_steam_duty = atoi(request->getParam("sD", true)->value().c_str())*((1<<PWM_WIDTH)-1)/100;
		max_heads_duty = atoi(request->getParam("hD", true)->value().c_str())*((1<<PWM_WIDTH)-1)/100;
		nvs.putUInt("maxsteam", max_steam_duty);
		nvs.putUInt("maxheads", max_heads_duty);
		DBG_INFO("Saved power cfg.");
	}
  if(request->hasParam("vN", true) && volts_now != 0){  // calibrate with user vN value
		volts_max = atof(request->getParam("vN", true)->value().c_str()) * 4095 / analogRead(VOLT_SENSOR) * ADC_MAX_FACTOR + ADC_OFFSET; 
		nvs.putFloat("voltsmax", volts_max);
		OpLog("Volts Max: %.1f", volts_max);
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
		DBG_INFO("Saved flow calibration: %s = %d.", key, flow_rates[valve][rate]);
	}
	if(request->hasParam("tank", true)){
		char key[] = "Txx";
		uint8_t tank = atoi(request->getParam("tank", true)->value().c_str());
		uint8_t level = atoi(request->getParam("level", true)->value().c_str());
		tank_levels[tank][level] = atoi(request->getParam("volume", true)->value().c_str())*20;
		key[1] = 0x30 + tank;
		key[2] = 0x30 + level;
		nvs.putUInt(key, tank_levels[tank][level]);
		DBG_INFO("Saved tank levels: %s = %d.", key, tank_levels[tank][level]);
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
    OpLog("Run state: %s.", bRunning ? "ON" : "OFF");
	}
	if(request->hasParam("open", true)){
		uint8_t valve = atoi(request->getParam("open", true)->value().c_str());
		uint16_t tenths = atof(request->getParam("secs", true)->value().c_str())*10;
		if(tenths == 0)
			tenths = 10;
		on_fets[valve-1] = tenths+1; // will open valve on next tick, with count down to close
		DBG_INFO("Open valve %s for %d tenths.", valves_msg[valve-1], tenths);
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
	request->send(200);
}

void SaveTankLevel(uint8_t tank, uint8_t level){
	nvs.begin("config");
	char key[] = "Txx";
	key[1] = 0x30 + tank;
	key[2] = 0x30 + level;
	nvs.putUInt(key, tank_levels[tank][level]);
	nvs.end();
}

void OpLog(const char *fmt, ...){
	struct tm timeinfo;
	char buf[128];
	getLocalTime(&timeinfo);
	size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S ", &timeinfo);
	
	File file = SPIFFS.open("/oplog", "a");
  va_list args;
	va_start(args, fmt);
  vsnprintf(buf+n, sizeof(buf)-n, fmt, args);
  va_end(args);
  file.println(buf);
  file.close();
}

void (*(stateFuncs[]))() = { stateUpdateSleep, stateUpdateHeatUp, stateUpdateCoolDn, stateUpdateRun };

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
  volts_run = nvs.getFloat("voltsrun", DEF_VOLTS_RUN);
  max_steam_flow = nvs.getFloat("steamflow", DEF_STEAM_FLOW);
  ferm_mode = nvs.getUInt("fermmode", FERM_MODE_NONE);
  still_mode = nvs.getUInt("stillmode", STILL_MODE_NONE);
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
		
	// init op log
  SPIFFS.remove("/oplog.old");
	SPIFFS.rename("/oplog", "/oplog.old");
  OpLog("Vodak32 - version %d.%d.%d", VER_MAJOR, VER_MINOR, VER_PATCH);
  OpLog("Still mode: %s, Ferm mode: %s.", still_modes_msg[still_mode], ferm_modes_msg[ferm_mode]);
  
  // routes for web app
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){ sendJsonData(request); });
  server.on("/cfg", HTTP_POST, [](AsyncWebServerRequest *request){ onSaveCfg(request); });
  server.on("/run", HTTP_POST, [](AsyncWebServerRequest *request){ onRunChg(request); });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){ onReset(request); });

	// start web app server
  AsyncElegantOTA.begin(&server);
  server.begin();
  
  // setup heater pwm channels
  DBG_INFO("Setting PWM channels.");
  /*ledcSetup(STEAM_PWM_CHANNEL, PWM_HZ, PWM_WIDTH);
  ledcAttachPin(STEAM_HEAT, STEAM_PWM_CHANNEL);
  ledcWrite(STEAM_PWM_CHANNEL, 0);
  ledcSetup(HEADS_PWM_CHANNEL, PWM_HZ, PWM_WIDTH);
  ledcAttachPin(HEADS_HEAT, HEADS_PWM_CHANNEL);
  ledcWrite(HEADS_PWM_CHANNEL, 0);*/
  digitalWrite(STEAM_HEAT, LOW);
  digitalWrite(HEADS_HEAT, LOW);
  
  // setup tick timer for sensor reads, valve timing and state machine
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &ticker, true);
  timerAlarmWrite(timer, 100000, true);
  timerAlarmEnable(timer);
  DBG_INFO("Ticker started.");
  
  // start ferm without long initial wait
  doFermUpdate = true;
}

void loop() {

  if(doSensorUpdate){ 
    DBG_DEBUG("Reading sensors.");
    sensors.requestTemperatures();
    for(int i = 0; i < SENSOR_COUNT; i++)
      if(sens_addrs[i][7]){ // // device family always non-zero when sensor exists
        tempC[i] = sensors.getTempC(sens_addrs[i]);
        DBG_DEBUG("Temp %d - %.1f", i, tempC[i]);
			}
    volts_now = analogRead(VOLT_SENSOR)*volts_max/4095 + ADC_OFFSET;
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
