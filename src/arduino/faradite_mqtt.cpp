/*
  Read Faradite Motion 360 sensor values using an Arduino with Ethernet Shield and send to MQTT topic.
  
  The main goal of this code is to publish cleansed motion and lux sensor values to MQTT topics at a timely cadence.
  It is also reactive, in that significant sudden changes should break the typical cadence cycle and be sent immediately.
  For this reason the sensor is queried frequently, rather than once a second for example. The use of filters is directly related
  to the polling frequency.
  
  Currently only supports one unit.
*/

#include <Ewma.h>
#include <EwmaT.h>
#include <Ethernet.h>
#include <SPI.h>
#include <PubSubClient.h>


/*
  Build Configuration 
  
  "Sensitive" information such as logins and passwords should be defined at build-time if 
  building with platformio, by setting the build_flags parameter in the target build environment.

  Otherwise, just change the below values to your preferred ones and run.

*/

#ifndef MQTT_SERVER
#define MQTT_SERVER "192.168.0.1"
#endif

// Arduino IP Address
#if !defined(IP1) || !defined(IP2) || !defined(IP3) || !defined(IP4)
#define IP1 192
#define IP2 168
#define IP3 0
#define IP4 20
#endif

/*
  End Build Configuration 
*/


/*
  Parameters

  The following can be changed to alter the behaviour of the code or accomodate your physical setup
*/

// Sensor
unsigned int mainLoopFrequency = 1;                     // How long to wait between main loop iterations in ms. This directly affects how often the pins are read
unsigned int luxPublishFrequency = 60000;               // How often to publish lux in ms
float        luxReactiveThreshold = 1.6;                // Factor that the lux value that needs to change for a reactive publish of lux values
float        filterAlpha = mainLoopFrequency * 0.005;   // Smoothing factor for lux values. Lower is more smoothing but less responsive. Range of 0 - 1.0. Paired with 
                                                        // mainLoopFrequency so that can be changed without affecting the desired filter behaviour

// MQTT
unsigned int mqttReconnectFrequency = 5000;             // How long to wait between reconnection attempts in ms

// Debugging
bool         debug = false;                             // Send various debug output via MQTT
unsigned int debugPublishFrequency = 250;               // How often to publish to debug topic in ms

// Pins (Ethernet blocks the following: 4, 10-13, 50-52)
const int faradite1LuxPin    = A15;                     // Lux sensor pin
const int faradite1MotionPin = 48;                      // Motion sensor pin

/*
  End Parameters
*/


/*
  General vars
*/

// MQTT
EthernetClient ethClient;
IPAddress      ip(IP1, IP2, IP3, IP4);      // IP address of the arduino
byte           mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF };
unsigned int   mqttReconnectMillis = 0;
PubSubClient   mqttClient(ethClient);
const char*    mqttServer = MQTT_SERVER;    // IP address of mqtt server

// General
unsigned long currentMillis = 0;            // Updates baseline for all millisecond comparisons
unsigned long globalLoopPreviousMillis = 0; // Compared between currentMillis for mainLoopFrequency calculation
char          charBuffer[4];                // Stores converted ints - covers ADC range (0 - 1023)

// Debug
unsigned long debugPreviousMillis = 0;
unsigned int  luxReads = 0;                 // How many times have we read

// Lux
unsigned long luxSendPreviousMillis = 0;    // Compared between currentMillis for luxPublishFrequency calculation
float         luxAvgChangeFactor = 0.0;     // The factor of change between avg readings
unsigned int  rawLuxValue = 0;              // Raw lux value from sensor
int           filteredLuxValue = 0;         // Filtered lux value
bool          luxSend = 0;                  // The lux value should be published        
int           luxLastSent = 0;              // Last lux value to be published
Ewma          adcFilter(filterAlpha);       // EWMA filtering object - smooths out noise and jitter from pin readings

// Motion
int           motionValue = 0;              // Motion value from sensor
bool          motionStartSent = 0;          // True if motion has been detected and sent to server
bool          motionStopSent = 1;           // True if motion has stopped and this has been sent to the server

/*
  End General Vars
*/


void readLux();
void readMotion();
void mqttReconnect();
void subscribeReceive(char* topic, byte* payload, unsigned int length);
bool publishToTopic(char* topic, char* payload);
char* ADCIntToChar(int intValue);
float clamp(float d, float min, float max);

void setup() {
  Serial.begin(9600);
  
  // Don't wait to connect on first loop iteration - connect immediately
  mqttReconnectMillis = millis() - mqttReconnectFrequency;
 
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(subscribeReceive);
  
  Ethernet.begin(mac, ip);
  
  // Wait for ethernet to be ready
  delay(2000);
}

void loop() {
  currentMillis = millis();
  
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  
  // Not subscribing to anything yet, but will be useful later for configuring params without redeploying
  // mqttClient.subscribe("TopicToSubscribeTo");
  
  if (currentMillis - globalLoopPreviousMillis >= mainLoopFrequency) {
    globalLoopPreviousMillis = currentMillis;
  
    readLux();
    readMotion();
  }
}

void readLux() {
  // We will send an update every "luxPublishFrequency" ms.
  // However, if a value varies from the last published by a certain degree (i.e. a light turning on or a curtain closing)
  // then we'll send it immediately. Theoretically we could send a message every "luxPublishFrequency", but this is highly
  // unlikely (unless you have a strobe light, in which case you can lower the adcFilter alpha) 
  
  rawLuxValue = analogRead(faradite1LuxPin);
  
  // Filter and round to int - assuming too much noise for float resolution to be useful
  filteredLuxValue = round(adcFilter.filter(rawLuxValue));

  // Calculate change factor (up and down) - Can't divide by 0, so clamp values
  if (filteredLuxValue >= luxLastSent) luxAvgChangeFactor = clamp(filteredLuxValue, 1, 1023) / clamp(luxLastSent, 1, 1023);
  else                            luxAvgChangeFactor = clamp(luxLastSent, 1, 1023) / clamp(filteredLuxValue, 1, 1023);

  // Should we send the latest average?
  if (luxAvgChangeFactor >= luxReactiveThreshold || currentMillis - luxSendPreviousMillis >= luxPublishFrequency) {
    // Lux has either adaptively shifted beyond the last sent value, or the timer has ticked over
    Serial.print("Sending lux value: ");
    Serial.print(filteredLuxValue);
    Serial.print("...");
    
    publishToTopic("arduino/lux", ADCIntToChar(filteredLuxValue));
    
    // Update these redardless of successful publish or not - it won't be long till it loops around again
    luxLastSent = filteredLuxValue;
    luxSendPreviousMillis = currentMillis;
  }

  if (debug) {
    luxReads++;
    
    if (currentMillis - debugPreviousMillis >= debugPublishFrequency) {
      publishToTopic("arduino/debug/lux", ADCIntToChar(filteredLuxValue));
      publishToTopic("arduino/debug/rawlux", ADCIntToChar(rawLuxValue));
      publishToTopic("arduino/debug/luxreads", ADCIntToChar(luxReads));
      
      luxReads = 0;
      
      debugPreviousMillis = currentMillis;
    }
  }
}

void readMotion() {
  // Sensor has a built-in 1 sec leadtime between triggering on and switching off so there's no chance of sending a ton of messages
  motionValue = digitalRead(faradite1MotionPin);
  
  if (motionValue == 1 && motionStartSent == 0) {
    Serial.print("Sending motion start...");
    
    publishToTopic("arduino/motion", "1");
    
    motionStartSent = 1;
    motionStopSent = 0;
  } else if (motionValue == 0 && motionStopSent == 0) {
    Serial.print("Sending motion stop...");
    
    publishToTopic("arduino/motion", "0");
    
    motionStartSent = 0;
    motionStopSent = 1;
  }
}

// Unused at the moment
void subscribeReceive(char* topic, byte* payload, unsigned int length)
{
  // Print the topic
  Serial.print("Topic: ");
  Serial.println(topic);
 
  // Print the message
  Serial.print("Message: ");
  for(int i = 0; i < length; i ++)
  {
    Serial.print(char(payload[i]));
  }
 
  // Print a newline
  Serial.println("");
}

void mqttReconnect() {
  // Try to reconnect if we're not connected, but ensure there's a delay and the sensors continue to poll
  if(currentMillis - mqttReconnectMillis >= mqttReconnectFrequency) {
    mqttReconnectMillis = currentMillis;
    
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect("arduinoClient")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
    }
  }
}

bool publishToTopic(char* topic, char* payload) {
  bool returnVal = true;
  
  if(mqttClient.publish(topic, payload)) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
    returnVal = false;
  }
  
  return returnVal;
}

char* ADCIntToChar(int intValue) {
  String outStr = String(intValue);
  outStr.toCharArray(charBuffer, outStr.length()+1);
  return charBuffer;
}

float clamp(float d, float min, float max) {
  const float t = d < min ? min : d;
  return t > max ? max : t;
}
