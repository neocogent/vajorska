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
#define DEF_SSID "Lucy" //"Vodak32"
#define DEF_PASSWORD "markesmith" //"12345678"
#define WIFI_TIMEOUT 10
#define DEF_TIMEZONE 10*3600

// pin connections
#define ONE_WIRE_BUS 21
#define VOLT_SENSOR  36
#define STEAM_HEAT	 32
#define HEADS_HEAT 	 33
#define STEAM_VALVE  25
#define WASH_VALVE   26
#define FEED_VALVE   27
#define FERM1_VALVE  14
#define FERM2_VALVE  12
#define XTRA_HEAT    13

#define SENSOR_COUNT 8 // number of onewire devices on bus
#define FLOW_COUNT   5 // number of valves
#define SENSOR_UPDATE_SECS  10 // interval for sensor updates (secs)
#define STATE_CYCLE_SECS    30 // interval for state machine cycle (secs)
#define DEF_VOLTS_MAX       61.4 // based on resistor divider values: 3.2/Vmax = 2.2/(40+2.2) 

// the machine states
#define STATE_SLEEP 	0
#define STATE_HEAT_UP 1
#define STATE_COOL_DN 2
#define STATE_RUN 		3

// the temp sensor ids
#define TEMP_HEADS  0
#define TEMP_HEARTS 1
#define TEMP_MID    2
#define TEMP_BASE   3
#define TEMP_TAILS  4
#define TEMP_STEAM  5
#define TEMP_FERM1  6
#define TEMP_FERM2  7

String ssid;
String password;

const char* ntpServer = "pool.ntp.org";
long  gmtOffset_sec;
int   daylightOffset_sec;
hw_timer_t *timer = NULL;
uint8_t outpins[] = {STEAM_HEAT,HEADS_HEAT,STEAM_VALVE,WASH_VALVE,FEED_VALVE,FERM1_VALVE,FERM2_VALVE,XTRA_HEAT};
int numberOfSensors;
float volts_max, volts_now;
uint32_t flow_rates[FLOW_COUNT][3];  // high/low/now triplets in drops per minute where 20 drops = 1ml
DeviceAddress sens_addrs[SENSOR_COUNT];
float tempC[SENSOR_COUNT];
bool sensorUpdate = false;
bool stateUpdate = false;
uint8_t stateNow = STATE_SLEEP;
volatile uint32_t ticks;

DEBUG_INSTANCE(128, Serial);
Preferences nvs;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);

void IRAM_ATTR ticker()
{
  ticks++;
  if((ticks % SENSOR_UPDATE_SECS)==0)
    sensorUpdate = true;
  if((ticks % STATE_CYCLE_SECS)==0)
    stateUpdate = true;
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

void sendJsonDataResponse(AsyncWebServerRequest *request){
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		DynamicJsonBuffer jsonBuffer;
		JsonObject &data = jsonBuffer.createObject();  // holder for returning state data
    JsonArray& temp = data.createNestedArray("temp");
    for(int i=0; i < SENSOR_COUNT; i++)
      temp.add(tempC[i]);
		data["volts"] = volts_now;
		data["state"] = stateNow;
    JsonArray& flows = data.createNestedArray("flows");
    for(int i=0; i < FLOW_COUNT; i++)
      flows.add(flow_rates[i][2]);
		data.printTo(*response);
		request->send(response);
}

void (*(stateFuncs[]))() = {stateUpdateSleep, stateUpdateHeatUp, stateUpdateCoolDn, stateUpdateRun};

void setup() {
  // serial for log / debug output
  Serial.begin(115200); 
  DBG_INFO("vodak32 - version %d.%d.%d", VER_MAJOR, VER_MINOR, VER_PATCH);
  
  // init GPIO pins
  pinMode(ONE_WIRE_BUS, INPUT);
  for(int i = 0; i < sizeof(outpins); i++) {
    pinMode(outpins[i], OUTPUT);
    digitalWrite(outpins[i], LOW);
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
  
  // load temperature sensor ids 
  char key[4] = "Sx\0";
  nvs.begin("onewire", true);
  for(int i = 0; i < SENSOR_COUNT; i++) {
		key[1] = i + 0x41;
    nvs.getBytes(key, sens_addrs[i], 8);
	 }
  nvs.end();
  
  // init temperature bus
  sensors.begin();
  numberOfSensors = sensors.getDeviceCount();
  DBG_INFO("Scanning temperature sensors. Found: %d", numberOfSensors);
  if(numberOfSensors != SENSOR_COUNT)
    DBG_WARNING("Incorrect number of temperature sensors detected.");

  // read config values
	key[0] = 'F';
  DBG_INFO("Loading config.");
  nvs.begin("config", true);
  volts_max = nvs.getFloat("voltsmax", DEF_VOLTS_MAX);
  gmtOffset_sec = nvs.getInt("gmtoffset", DEF_TIMEZONE);
  daylightOffset_sec = nvs.getInt("dstoffset", 0);
  for(int i = 0; i < FLOW_COUNT; i++) {
		key[2] = i + 0x30;
		for(int j = 0; j < 3; j++){
			key[3] = j + 0x30;
			flow_rates[i][j] = nvs.getUInt(key, 0);
		}
	 }
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
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){ sendJsonDataResponse(request); });
  
  AsyncElegantOTA.begin(&server);
  server.begin();
  
  // setup tick timer for sensor reads and state machine
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &ticker, true);
  timerAlarmWrite(timer, 1000000, true);
  timerAlarmEnable(timer);
  DBG_INFO("Ticker started.");
}

void loop() {

  if(sensorUpdate){ 
    DBG_DEBUG("Reading sensors.");
    sensors.requestTemperatures();
    for(int i = 0; i < SENSOR_COUNT; i++)
      if(sens_addrs[i][7]) // family code non-zero if sensor exists
        tempC[i] = sensors.getTempC(sens_addrs[i]);
    volts_now = analogRead(VOLT_SENSOR)*volts_max/4095;
    sensorUpdate = false;
  }
	if(stateUpdate){ 
    stateFuncs[stateNow]();
    stateUpdate = false;
	}
}
