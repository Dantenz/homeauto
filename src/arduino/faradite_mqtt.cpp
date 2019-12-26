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
unsigned int  mainLoopFrequency = 1;                    // How long to wait between main loop iterations in ms
unsigned int  luxPollFrequency = 250;                   // How often to read lux in ms
unsigned long luxPublishFrequency = 60000;              // How often to publish lux in ms
float         luxReactiveThreshold = 0.1;               // Controls amount of change required between last sent and current lux value, before resending reactively. Range of 0 - 0.5.
float         filterAlpha = 0.07;                       // Smoothing factor for lux values. Lower is more smoothing but less responsive. Range of 0 - 1.0.

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
char          charBuffer[10];               // Stores converted vals

// Debug
unsigned long debugPreviousMillis = 0;
unsigned int  luxReads = 0;                 // How many times have we read

// Lux
unsigned long luxPublishPreviousMillis = 0; // Compared between currentMillis for luxPublishFrequency calculation
unsigned long luxPollPreviousMillis = 0;    // Compared between currentMillis for luxPollFrequency calculation
float         luxAvgChangeFactor = 0.0;     // The factor of change between avg readings
unsigned int  rawLuxValue = 0;              // Raw lux value from sensor
unsigned int  filteredLuxValue = 0;         // Filtered lux value       
unsigned int  luxLastSent = 0;              // Last lux value to be published
unsigned int  luxReactiveChangeRequired = 0;// Difference required between current and previously sent values to trigger a change
unsigned int  luxReactiveDifference = 0;    // Difference between current and previously sent values
bool          reactiveLuxSend = false;      // Whether or not a reactive send as been triggered
Ewma          adcFilter(filterAlpha);       // EWMA filtering object - smooths out noise and jitter from pin readings.

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
void subscribeToTopics();
void subscribeReceive(char* topic, byte* payload, unsigned int length);
bool publishToTopic(char* topic, char* payload);
bool isTopic(char* topicReceived, char* topicToMatch);
void setDebug(bool newValue);
void setDebugPublishFrequency(unsigned int newValue);
void setLuxPollFrequency(unsigned int newValue);
void setLuxPublishFrequency(unsigned long newValue);
void setLuxReactiveThreshold(float newValue);
void setFilterAlpha(float newValue);
char* intToChar(int intValue);
char* floatToChar(float floatValue);
char* longToChar(long longValue);
String byteToString(byte* payload, unsigned int length);
float clamp(float d, float min, float max);

void setup() {
  Serial.begin(9600);
 
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(subscribeReceive);
  
  Ethernet.begin(mac, ip);

  char ipMsgBuff[40];
  sprintf(ipMsgBuff, "Starting ethernet with IP %d.%d.%d.%d", IP1, IP2, IP3, IP4);
  Serial.println(ipMsgBuff);
  
  // Wait for ethernet to be ready
  delay(2000);

  // Don't wait for reconnection timmer - connect immediately
  mqttReconnectMillis = millis() - mqttReconnectFrequency;
  mqttReconnect();

  // Publish a lux value immediately
  luxPublishPreviousMillis = millis() - luxPublishFrequency;

  // Sanitise parameters and send initial values
  setDebug(debug);
  setDebugPublishFrequency(debugPublishFrequency);
  setLuxPollFrequency(luxPollFrequency);
  setLuxPublishFrequency(luxPublishFrequency);
  setLuxReactiveThreshold(luxReactiveThreshold);
  setFilterAlpha(filterAlpha);
}

void loop() {
  currentMillis = millis();
  
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  
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

  if (currentMillis - luxPollPreviousMillis >= luxPollFrequency) {
    luxPollPreviousMillis = currentMillis;

    rawLuxValue = analogRead(faradite1LuxPin);
    
    filteredLuxValue = round(adcFilter.filter(rawLuxValue));

    reactiveLuxSend = false;

    if (filteredLuxValue >= luxLastSent) luxReactiveDifference = filteredLuxValue - luxLastSent;
    else                                 luxReactiveDifference = luxLastSent - filteredLuxValue;
    
    // Only reactive send if the value has actually changed
    if (luxReactiveDifference >= 1) {
      // Check if the difference is enough to trigger a reactive change
      luxReactiveChangeRequired = floor(filteredLuxValue * luxReactiveThreshold) + 1;

      if (luxReactiveDifference >= luxReactiveChangeRequired) reactiveLuxSend = true;
    }

    // Should we send the latest average?
    if (reactiveLuxSend || currentMillis - luxPublishPreviousMillis >= luxPublishFrequency) {
      // Lux has either adaptively shifted beyond the last sent value, or the timer has ticked over
      publishToTopic("arduino/lux", intToChar(filteredLuxValue));
      
      // Update these redardless of successful publish or not - it won't be long till it loops around again
      luxLastSent = filteredLuxValue;
      luxPublishPreviousMillis = currentMillis;
    }

    if (debug) {
      luxReads++;
      
      if (currentMillis - debugPreviousMillis >= debugPublishFrequency) {
        publishToTopic("arduino/debug/lux", intToChar(filteredLuxValue));
        publishToTopic("arduino/debug/rawlux", intToChar(rawLuxValue));
        publishToTopic("arduino/debug/luxreads", intToChar(luxReads));

        // We're not looping as fast as we should be
        if (luxReads < (debugPublishFrequency / luxPollFrequency) * 0.75 ) Serial.println("WARNING: CPU Choke");

        luxReads = 0;
        
        debugPreviousMillis = currentMillis;
      }
    }
  }
}

void readMotion() {
  // Sensor has a built-in 1 sec leadtime between triggering on and switching off so there's no chance of sending a ton of messages
  motionValue = digitalRead(faradite1MotionPin);
  
  if (motionValue == 1 && motionStartSent == 0) {
    publishToTopic("arduino/motion", "1");
    
    motionStartSent = 1;
    motionStopSent = 0;
  } else if (motionValue == 0 && motionStopSent == 0) {
    publishToTopic("arduino/motion", "0");
    
    motionStartSent = 0;
    motionStopSent = 1;
  }
}

void subscribeToTopics() {
  mqttClient.subscribe("arduino/set/debug");
  mqttClient.subscribe("arduino/set/debugpublishfrequency");
  mqttClient.subscribe("arduino/set/luxpollfrequency");
  mqttClient.subscribe("arduino/set/luxpublishfrequency");
  mqttClient.subscribe("arduino/set/luxreactivethreshold");
  mqttClient.subscribe("arduino/set/filteralpha");

  mqttClient.subscribe("arduino/get/debug");
  mqttClient.subscribe("arduino/get/debugpublishfrequency");
  mqttClient.subscribe("arduino/get/luxpollfrequency");
  mqttClient.subscribe("arduino/get/luxpublishfrequency");
  mqttClient.subscribe("arduino/get/luxreactivethreshold");
  mqttClient.subscribe("arduino/get/filteralpha");
}

void subscribeReceive(char* topic, byte* payload, unsigned int length) {

  String strPayload = byteToString(payload, length);
  strPayload.toCharArray(charBuffer, strPayload.length()+1);

  // Debug get/set
  if (isTopic(topic, "arduino/set/debug")) {
    setDebug(strPayload.toInt());
  } else if(isTopic(topic, "arduino/get/debug")) {
    publishToTopic("arduino/debug", intToChar(debug));

  // Debug Publish Frequency get/set
  } else if(isTopic(topic, "arduino/set/debugpublishfrequency")) {
    setDebugPublishFrequency(strPayload.toInt());
  } else if(isTopic(topic, "arduino/get/debugpublishfrequency")) {
    publishToTopic("arduino/debugpublishfrequency", intToChar(debugPublishFrequency));

  // Lux Poll Frequency get/set
  } else if(isTopic(topic, "arduino/set/luxpollfrequency")) {
    setLuxPollFrequency(strPayload.toInt());
  } else if(isTopic(topic, "arduino/get/luxpollfrequency")) {
    publishToTopic("arduino/luxpollfrequency", intToChar(luxPollFrequency));

  // Lux Publish Frequency get/set
  } else if(isTopic(topic, "arduino/set/luxpublishfrequency")) {
    setLuxPublishFrequency(strPayload.toInt());
  } else if(isTopic(topic, "arduino/get/luxpublishfrequency")) {
    publishToTopic("arduino/luxpublishfrequency", longToChar(luxPublishFrequency));

  // Lux Reactive Threshold get/set
  } else if(isTopic(topic, "arduino/set/luxreactivethreshold")) {
    setLuxReactiveThreshold(strPayload.toFloat());
  } else if(isTopic(topic, "arduino/get/luxreactivethreshold")) {
    publishToTopic("arduino/luxreactivethreshold", floatToChar(luxReactiveThreshold));

  // Filter Alpha get/set
  } else if(isTopic(topic, "arduino/set/filteralpha")) {
    setFilterAlpha(strPayload.toFloat());
  } else if(isTopic(topic, "arduino/get/filteralpha")) {
    publishToTopic("arduino/filteralpha", floatToChar(filterAlpha));
  }
}

void mqttReconnect() {
  // Try to reconnect if we're not connected, but ensure there's a delay and the sensors continue to poll
  if(currentMillis - mqttReconnectMillis >= mqttReconnectFrequency) {
    mqttReconnectMillis = currentMillis;
    
    Serial.print("Attempting MQTT connection to "); Serial.print(mqttServer); Serial.print("...");
    // Attempt to connect
    if (mqttClient.connect("arduinoClient")) {
      Serial.println("connected");

      subscribeToTopics();
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

bool publishToTopic(char* topic, char* payload) {
  bool returnVal = true;

  if(!mqttClient.publish(topic, payload)) {
    returnVal = false;
    Serial.println("Failed to publish payload");
  }

  return returnVal;
}

bool isTopic(char* topicReceived, char* topicToMatch) {
  // Abstract the backwards strcmp and make life a bit easier
  return !strcmp(topicReceived, topicToMatch);
}

void setDebug(bool newValue) {
  debug = newValue;

  publishToTopic("arduino/debug", intToChar(debug));
}

void setDebugPublishFrequency(unsigned int newValue) {
  debugPublishFrequency = newValue;

  publishToTopic("arduino/debugpublishfrequency", intToChar(debugPublishFrequency));
}

void setLuxPollFrequency(unsigned int newValue) {
  luxPollFrequency = clamp(newValue, mainLoopFrequency, 60000);

  publishToTopic("arduino/luxpollfrequency", intToChar(luxPollFrequency));
}

void setLuxPublishFrequency(unsigned long newValue) {
  // Clamp floor to mainLoopFrequency - there is no point in sending more frequently than the pins are polled
  luxPublishFrequency = clamp(newValue, luxPollFrequency, 604800000);  // 1 week max should be more than enough

  publishToTopic("arduino/luxpublishfrequency", longToChar(luxPublishFrequency));
}

void setLuxReactiveThreshold(float newValue) {
  // A floor of 1 will always fire when the value is the same as the last, so clamp it out
  luxReactiveThreshold = clamp(newValue, 0, 0.5);

  publishToTopic("arduino/luxreactivethreshold", floatToChar(luxReactiveThreshold));
}

void setFilterAlpha(float newValue) {
  filterAlpha = clamp(newValue, 0, 1);
  adcFilter.alpha = filterAlpha;

  publishToTopic("arduino/filteralpha", floatToChar(filterAlpha));
}

char* intToChar(int intValue) {
  String outStr = String(intValue);
  outStr.toCharArray(charBuffer, outStr.length()+1);
  return charBuffer;
}

char* floatToChar(float floatValue) {
  String outStr = String(floatValue);
  outStr.toCharArray(charBuffer, outStr.length()+1);
  return charBuffer;
}

char* longToChar(long longValue) {
  String outStr = String(longValue);
  outStr.toCharArray(charBuffer, outStr.length()+1);
  return charBuffer;
}

String byteToString(byte* payload, unsigned int length) {
  int i; 
  String outStr = ""; 
  for (i = 0; i < length; i++) { 
      outStr = outStr + char(payload[i]); 
  } 
  return outStr; 
}

float clamp(float d, float min, float max) {
  const float t = d < min ? min : d;
  return t > max ? max : t;
}
