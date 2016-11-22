#include <Arduino.h>

#define K_AC 15
#define K_BAT 13

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include <TimeLib.h>
#include <NtpClientLib.h>

#include <TickerScheduler.h>

#include <RemoteDebug.h>
RemoteDebug Debug;


WiFiClient espClient;
PubSubClient client(espClient);
TickerScheduler ts(5);

const char* ssid = "mushroom";
const char* password = "gumboots";
const char* mqtt_server = "192.168.0.3";


const char * MQTT_TOPIC_STATE       = "battery/state";
const char * MQTT_TOPIC_VOLTAGE     = "battery/voltage";
const char * MQTT_TOPIC_RELAY_AC    = "battery/state/charger";
const char * MQTT_TOPIC_RELAY_BAT   = "battery/state/inverter";
const char * NTP_SERVER             = "192.168.0.3";


#define MODE_IDLE 0
#define MODE_CHARGING 1
#define MODE_EXPORTING 2
#define MODE_TRIPPED 3

#define AC_CHARGER 0
#define AC_INVERTER 1

#define BAT_DISCONNECTED 0
#define BAT_CONNECTED 1



// Status variables
byte _mode        = MODE_IDLE;
byte _ac          = AC_CHARGER;
byte _bat         = BAT_DISCONNECTED;
// Time keeping
byte _ntp_ready     = false;
byte _dst           = 1;
byte _sunrise       = 6;
byte _sunset        = 19;
short _timezone     = +10;
byte _sun_buffer    = 3;
byte _hr_sleep      = 23;
// Config
uint32_t _analog_interval = 10000;
float _max_voltage = 30;
float _min_voltage = 22;

float _solar_min  = 1500.0;

time_t tmConvert_t(int YYYY, byte MM, byte DD, byte hh, byte mm, byte ss)
{
  tmElements_t tmSet;
  tmSet.Year = YYYY - 1970;
  tmSet.Month = MM;
  tmSet.Day = DD;
  tmSet.Hour = hh;
  tmSet.Minute = mm;
  tmSet.Second = ss;
  return makeTime(tmSet);         //convert to time_t
}

uint16_t adc(unsigned int n){
  unsigned long tmp=0;
  unsigned int i;
  for(i=0;i<n;i++){
    tmp = tmp + analogRead(A0);
  }
  return tmp / n;
}

float voltage(){
  uint16_t tmp = adc(100);
  Serial.println(tmp);
  return tmp * 0.037268293 + 0.496;
}

void _relay_ac(byte state){
  digitalWrite(K_AC, state);
  client.publish(MQTT_TOPIC_RELAY_AC, state ? "1" : "0");
}

void _relay_bat(byte state){
  digitalWrite(K_BAT, state);
  client.publish(MQTT_TOPIC_RELAY_BAT, state ? "1" : "0");
}

void ac(byte ac_mode){
  _ac = ac_mode;
  switch(ac_mode){
    case AC_CHARGER:
      _relay_ac(0);
      break;
    case AC_INVERTER:
      _relay_ac(1);
      break;
    default:
      _relay_ac(0);
      break;
  }
}

void bat(byte bat_mode){
  _bat = bat_mode;
  switch(bat_mode){
    case BAT_DISCONNECTED:
      _relay_bat(0);
      break;
    case BAT_CONNECTED:
      _relay_bat(1);
      break;
    default:
      _relay_bat(0);
      break;
  }
}


// void charger(mode):
// Controls the state of the device
// MODE_IDLE      Measure battery voltage (relay open), inverter connected to AC
// MODE_CHARGING  Connect to battery, charger connected to AC
// MODE_EXPORTING Connect to battery, inverter connected to AC

void charger(byte mode){
  if(_mode != mode){
    _mode = mode;
    switch(mode){

      case MODE_CHARGING:
        ac(AC_CHARGER);
        bat(BAT_CONNECTED);
        client.publish(MQTT_TOPIC_STATE, "charging");
        break;
      case MODE_EXPORTING:
        ac(AC_INVERTER);
        bat(BAT_CONNECTED);
        client.publish(MQTT_TOPIC_STATE, "exporting");
        break;
      case MODE_IDLE:
      case MODE_TRIPPED:
      default:
        ac(AC_CHARGER);
        bat(BAT_DISCONNECTED);
        client.publish(MQTT_TOPIC_STATE, "idle");
        break;
    }
  }
}

// Read an MQTT payload to a String object
String munge_payload(byte * payload, unsigned int length){
  char buf[16] = "";
  int i = 0;
  for (i = 0; i < length; i++) {
    buf[i] = (char)payload[i];
  }
  buf[i] = '\0';
  return String(buf);
}

// Receive an MQTT message on a subscribed topic
void mqtt_rx(char* topic, byte* payload, unsigned int length) {
  String tmp;
  tmp = String(topic);
  String pl =  munge_payload(payload, length);
  Debug.println("Got message " + tmp + " = " + pl);
  // Listen for solar inverter output data
  if(tmp.startsWith("inverter")){
    tmp.replace("inverter/","");
    if(tmp == "Pac1"){
      // Parse as float
      float Pac1 = pl.toFloat();
      // If inverter is exporting < threshold, idle
      if(Pac1 < _solar_min){
        charger(MODE_IDLE);
      // If inverter is exporting >= threashold, charge battery
      }else if(Pac1 >= _solar_min){
        charger(MODE_CHARGING);
      }
    }
  // this is intended for us!
  }else if(tmp.startsWith("battery/settings")){
    tmp.replace("battery/settings/","");
    if(tmp == "mode"){
      if(pl == "charging"){
        charger(MODE_CHARGING);
      }else if(pl == "exporting"){
        charger(MODE_EXPORTING);
      }else{
        charger(MODE_IDLE);
      }
    }
    if(tmp == "solar_min") _solar_min = pl.toFloat();
    if(tmp == "hr_sleep") _hr_sleep = pl.toInt();
    if(tmp == "hr_sunrise") _sunrise = pl.toInt();
    if(tmp == "hr_sunset") _sunset = pl.toInt();
    if(tmp == "sun_buffer") _sun_buffer = pl.toInt();
    if(tmp == "dst") _dst = pl.toInt();
    if(tmp == "max_voltage") _max_voltage = pl.toFloat();
    if(tmp == "min_voltage") _min_voltage = pl.toFloat();
  }
}


void reconnect_mqtt(){
  if (!client.connected()) {
    Debug.println("Reonnecting MQTT...");
    // Attempt to connect
    if (client.connect("ESP8266Client")) {
      Debug.println("MQTT connected");
      // Once connected, publish an announcement...
      client.publish("battery/charger/status", "online");
      // ... and resubscribe
      client.subscribe("battery/settings/#");
      client.subscribe("inverter/Pac1");
    } else {
      Debug.println("failed, rc=" + String(client.state()) + ".");
    }
  }
}

void init_mqtt() {
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqtt_rx);
  // Loop until we're reconnected
  ts.add(2, 5000, reconnect_mqtt, true);
}

void init_ntp(){
  NTP.onNTPSyncEvent([](NTPSyncEvent_t error) {
    if (error) {
      Debug.print("Time Sync error: ");
      if (error == noResponse)
        Debug.println("NTP server not reachable");
      else if (error == invalidAddress)
        Debug.println("Invalid NTP server address");
    }else {
      Debug.println("Got NTP time: " + NTP.getTimeDateString(NTP.getLastNTPSync()));
      _ntp_ready = true;
    }

  });
  NTP.begin(NTP_SERVER, 1, false);
  NTP.setInterval(60);
  NTP.setTimeZone(_timezone + _dst);
}

void init_wifi(){
  Serial.println("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  ArduinoOTA.setHostname("frankencharger");
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  Debug.begin("frankencharger");
  Debug.setResetCmdEnabled(true);
}

void read_analogs(){

  //client.publish("battery/status", "reading analogs");
  float v = voltage();

  // overvoltage protection
  if(v > _max_voltage && _mode == MODE_CHARGING){
    charger(MODE_TRIPPED);
    client.publish(MQTT_TOPIC_STATE, "OV_TRIP");
  }

  if(v < _min_voltage && _mode == MODE_EXPORTING){
    charger(MODE_TRIPPED);
    client.publish(MQTT_TOPIC_STATE, "UV_TRIP");
  }

  char buf[8];
  String tmp = String(voltage(),2);
  tmp.toCharArray(buf, 8);
  client.publish("battery/charger/voltage", buf);
}

void init_analogs(){
  ts.add(0, _analog_interval, read_analogs, true);
}

void read_daytime(){
  time_t hnow = hour(now());
  Debug.println("Evaluating daytime ("+String(hnow)+")");
  if(hnow < _sunrise + _sun_buffer){
    charger(MODE_IDLE);
  }else if (hnow >= _hr_sleep && _mode != MODE_IDLE){
    charger(MODE_IDLE);
  }else if (hnow > _sunset && hnow < _hr_sleep && _mode != MODE_EXPORTING){
    charger(MODE_EXPORTING);
  }
}

void init_daytime(){
  ts.add(1, 10000, read_daytime, true);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");

  init_wifi();

  // put your setup code here, to run once:
  digitalWrite(K_AC, LOW);
  digitalWrite(K_BAT, LOW);
  pinMode(K_AC, OUTPUT);
  pinMode(K_BAT, OUTPUT);

  init_mqtt();
  init_ntp();
  charger(MODE_IDLE);
  init_analogs();
  init_daytime();


  Debug.println("Booted");
}
unsigned long nexthb = 0;

void loop() {
  // task scheduler
  ts.update();
  // OTA updates
  ArduinoOTA.handle();
  // mqtt client
  client.loop();
  // ESP functions
  yield();
  // Debug
  Debug.handle();

  if(millis() > nexthb){
    Debug.println("Heartbeat @ " + NTP.getTimeStr());
    //client.publish("battery/heartbeat", "ping");
    nexthb = millis() + 5000;
  }
}
