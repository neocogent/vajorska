#include "WiFi.h"
#include <Preferences.h>
#include "ESPAsyncWebServer.h"
#include <AsyncElegantOTA.h>
#include "SPIFFS.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "ESP32TimerInterrupt.h"
#include "time.h"

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
#define FLOW_COUNT  5 // number of valves
#define SENSOR_UPDATE_MS  5000 // mS interval for sensor updates
#define DEF_VOLTS_MAX  61.4 // based on resistor divider values: 3.2/Vmax = 2.2/(40+2.2) 

String ssid;
String password;

const char* ntpServer = "pool.ntp.org";
long  gmtOffset_sec;
int   daylightOffset_sec;

uint8_t outpins[] = {STEAM_HEAT,HEADS_HEAT,STEAM_VALVE,WASH_VALVE,FEED_VALVE,FERM1_VALVE,FERM2_VALVE,XTRA_HEAT};
int numberOfSensors;
float volts_max, volts_now;
uint32_t flow_rates[FLOW_COUNT][3];  // high/low/now triplets in drops per minute where 20 drops = 1ml
DeviceAddress sens_addrs[SENSOR_COUNT];
float tempC[SENSOR_COUNT];
bool sensorUpdate = false;

Preferences nvs;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);
ESP32Timer ITimer0(0);

bool IRAM_ATTR SensorRead(void * timerNo)
{
  // set flag to read sensors
  sensorUpdate = true;
  return true;
}

void setup() {
  // serial for log / debug output
  Serial.begin(115200); 
  
  // init GPIO pins
  pinMode(ONE_WIRE_BUS, INPUT);
  for(int i = 0; i < sizeof(outpins); i++) {
    pinMode(outpins[i], OUTPUT);
    digitalWrite(outpins[i], LOW);
  }
  
    // initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("Error while mounting SPIFFS");
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
    Serial.print("Connecting to WiFi: ");
		Serial.print(ssid);
		while (secs++ < WIFI_TIMEOUT && WiFi.status() != WL_CONNECTED) {
			delay(1000);
			Serial.print(".");
		}
		if(secs < WIFI_TIMEOUT) 
			break;
		Serial.print("\nCannot connect. Creating AP: ");
		Serial.println(ssid);
		WiFi.disconnect();
		WiFi.mode(WIFI_AP);
		if(WiFi.softAP(ssid.c_str(), password.c_str())){
			delay(100);
      IPAddress Ip(192, 168, 1, 1);
      IPAddress NMask(255, 255, 255, 0);
      WiFi.softAPConfig(Ip, Ip, NMask);
			break;
    }
		Serial.println("AP failed. Starting over.");
		secs = 0;
	}
	Serial.print("\nWiFi up at: ");
  Serial.println(secs < WIFI_TIMEOUT ? WiFi.localIP() : WiFi.softAPIP());
  
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
  Serial.print("Scanning temperature sensors. Found: ");
  Serial.println(numberOfSensors, DEC);
  if(numberOfSensors != SENSOR_COUNT)
    Serial.println("Warning: Incorrect number of temperature sensors detected.");
  
  // setup interval for sensor reads
	if (ITimer0.attachInterruptInterval(SENSOR_UPDATE_MS * 1000, SensorRead))
		Serial.println("Sensor reading started.");
	else
		Serial.println("Error: cannot start timer for sensor reading.");

  // read config values
	key[0] = 'F';
  Serial.println("Loading config.");
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
  if(!getLocalTime(&timeinfo))
    Serial.println("Failed to obtain time.");
  else
		Serial.println(&timeinfo, "Time: %H:%M:%S");
  
  // routes for web app
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		DynamicJsonBuffer jsonBuffer;
		JsonObject &data = jsonBuffer.createObject();
		data["heap"] = ESP.getFreeHeap(); // chg to get real state data
		data["ssid"] = WiFi.softAPSSID();
		data["ip"] = WiFi.localIP().toString();
		data.printTo(*response);
		request->send(response);
  });
  
  AsyncElegantOTA.begin(&server);
  server.begin();
}

void loop() {

  if(sensorUpdate){ // set by ITimer0 ISR
    //Serial.println("Reading sensors.");
    sensors.requestTemperatures();
    for(int i = 0; i < SENSOR_COUNT; i++)
      if(sens_addrs[i][7]) // family code non-zero if sensor exists
        tempC[i] = sensors.getTempC(sens_addrs[i]);
    volts_now = analogRead(VOLT_SENSOR)*volts_max/4095;
    sensorUpdate = false; // ensure only once
  }
   
}
