#define K_AC 15
#define K_BAT 13

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <ESP8266WiFi.h>
#include <PubSubClient.h>


WiFiClient espClient;
PubSubClient client(espClient);


const char* ssid = "mushroom";
const char* password = "gumboots";
const char* mqtt_server = "192.168.0.3";

byte charger_state = 0;

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

void charger(byte state){
  charger_state = state ? 1 : 0;
  digitalWrite(K_AC, state ? 0 : 1);
  digitalWrite(K_BAT, state);
  
  client.publish("battery/charger/status/run", state ? "1" : "0");
}

String munge_payload(byte * payload, unsigned int length){
  char buf[16] = "";
  int i = 0;
  for (i = 0; i < length; i++) {
    buf[i] = (char)payload[i];
  }
  buf[i] = '\0';
  return String(buf);
}

void mqtt_rx(char* topic, byte* payload, unsigned int length) {
  String tmp;
  tmp = String(topic);

  if(tmp.startsWith("inverter")){
    tmp.replace("inverter/",""); 
    if(tmp == "Pac1"){
      // do something
      float Pac1 = munge_payload(payload, length).toFloat();
      if(Pac1 < 2000 && charger_state){
        charger(0);
      }else if(Pac1 >= 2000 && !charger_state){
        charger(1);
      }
    }
  }else{
    tmp.replace("battery/charger/settings/","");
    if(tmp == "run"){      
      charger(munge_payload(payload, length).toInt());
    }
  }

    
    
  
  
  
}


void init_mqtt() {
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqtt_rx);
  // Loop until we're reconnected
  if (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266Client")) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish("battery/charger/status", "online");
      // ... and resubscribe
      client.subscribe("battery/charger/settings/#");
      client.subscribe("inverter/Pac1");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");
  
  Serial.println("Wifi connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("wifi connected, doing other shit");
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
  
  // put your setup code here, to run once:
  digitalWrite(K_AC, LOW);
  digitalWrite(K_BAT, LOW);
  pinMode(K_AC, OUTPUT);
  pinMode(K_BAT, OUTPUT);
  unsigned long ts = millis();

  charger(1);
  init_mqtt();
  
  Serial.println("Booted.");
}
unsigned long ts = millis();

void runStack(){
  ArduinoOTA.handle();
  client.loop();
  yield();
}


unsigned long nextAnalog = 0;

void loop() {
  /*
  // put your main code here, to run repeatedly:
  digitalWrite(K_BAT, 1);
  digitalWrite(K_AC, 0);
  
  while(millis() < ts + 5000){
    runStack();
  }
  Serial.println("On " + String(voltage(), 2)+"V");
  
  digitalWrite(K_BAT, 0);
  digitalWrite(K_AC, 1);
  
  ts = millis();
  while(millis() < ts + 5000){
    runStack();
  }
  Serial.println("Off " + String(voltage(), 2)+"V");
  */

  if(millis() > nextAnalog){
    nextAnalog = millis() + 5000;
    char buf[8];
    String tmp = String(voltage(),2);
    tmp.toCharArray(buf, 8);
    client.publish("battery/charger/voltage", buf);
  }
  
  runStack();
}
