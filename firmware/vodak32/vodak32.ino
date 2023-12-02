#include "WiFi.h"
#include <Preferences.h>
#include "ESPAsyncWebServer.h"
#include <AsyncElegantOTA.h>
#include "SPIFFS.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"

#define DEF_SSID "Lucy" //"Vodak32"
#define DEF_PASSWORD "markesmith" //"12345678"

#define WIFI_TIMEOUT 10
#define ONE_WIRE_BUS 4


String ssid;
String password;
int numberOfSensors;

Preferences nvs;
AsyncWebServer server(80);

void setup() {
  // serial for debugging output
  Serial.begin(115200); 
  
    // initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("Error while mounting SPIFFS");
    return;
  }
  
  // read wifi config if set, or use defaults
  nvs.begin("wifi", false);
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
		Serial.println(ssid);
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
	Serial.print("WiFi up at: ");
  Serial.println(secs < WIFI_TIMEOUT ? WiFi.localIP() : WiFi.softAPIP());
  
  // init temperature bus
  /*OneWire oneWire(ONE_WIRE_BUS);
  DallasTemperature sensors(&oneWire);
  sensors.begin();
  numberOfSensors = sensors.getDeviceCount();
  Serial.print("Scanning temperature sensors. Found: ");
  Serial.println(numberOfSensors, DEC);
  // check here if correct count
  */
  
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
  
  // get temperatures
  /*sensors.requestTemperatures();
   *for(int i = 0; i < numberOfSensors; i++){
   * if(sensors.getAddress(tempDeviceAddress, i)) {
   *   float tempC = sensors.getTempC(tempDeviceAddress);
   *   }
   * }
   */
}
