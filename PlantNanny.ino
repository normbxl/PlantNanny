#define DEBUG

#ifdef DEBUG
	#define TRACE(d) dbgSerial.println(d)
  #define LOGGING
#else
	#define TRACE(d)
#endif

#include <avr/wdt.h>
#include <ESP8266.h>
#include <ESP8266Client.h>
#include <HttpClient.h>

#include <Wire.h>
#include "OLED.h"
#ifdef DEBUG
	#include <SoftwareSerial.h>
#endif
#include <EEPROM.h>


#define NTC_PIN A0
#define MOISTURE_SWITCH A2
#define MOISTURE_PIN A1
#define VCC_PIN A3
#define PUMP_PIN 12

#define ROM_ADDRESS 23

#define WIFI_SSID "<SSID>"
#define WIFI_PWD "<PWD>"

#define ADC_REF 3.3
#define WWW_UPDATE_INTERVAL 300


/*
Watchdog requires Optiboot firmware, otherwise a WDT-reset will result in an endless reboot
*/

uint8_t mcusr_mirror __attribute__((section(".noinit")));

void get_mcusr(void) \
  __attribute__((naked)) \
  __attribute__((section(".init3")));
void get_mcusr(void) {
  mcusr_mirror = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

const char* HOST_NAME = "d-parc.be";
unsigned int MOISTURE_PUMP_THRESHOLD = 720;

OLED oled;

volatile float Tavg;
volatile float Tnow;
volatile float vcc;
volatile byte sendPumpPingFlag;

long Tdiff;

const float Talpha = 0.005;
const float moistureAlpha = 0.1;

unsigned int tAvgCounter=0;

int moisture =  0;

bool wasWDTReset = false;
bool forcePumping = false;

#ifdef DEBUG
	SoftwareSerial dbgSerial(2, 3);
	ESP8266 wifi = ESP8266(Serial, dbgSerial);
#else
	ESP8266 wifi = ESP8266(Serial);
#endif


IPAddress ipAddress;

ESP8266Client espClient(wifi);
#ifdef DEBUG
	HttpClient http = HttpClient(espClient, dbgSerial);
#else
	HttpClient http = HttpClient(espClient);
#endif

IPAddress serverIP(46, 30, 213, 204);

enum States {
	IDLE,
	SEND,
	PUMP
};
States state = IDLE;

volatile unsigned long now = 0L;
unsigned long lastPumpTS = 0L;
const unsigned long oneDay = 3600L * 24L;//1440; // ein Tag in Minuten
unsigned long nextPumpTS = oneDay; // 24 Stunden

long dayAberration = 0L;

byte tickCounter = 0;
volatile boolean tick = false;

unsigned long sendTS = 0L;
bool waitingForResponse = false;
int DAY_ABR_ZERO = 20;


boolean setupWiFi() {
	boolean result = false;
	oled.setCursor(0, 0);
	oled.print("WiFi: ");
	if (wifi.begin() == true) {
		wifi.setTimeout(5000);
		oled.print("OK");
		delay(500);
		oled.setCursor(0, 1);
		oled.print("AP: ");
		ESP8266CommandStatus status = wifi.setMode(ESP8266_WIFI_STATION);
		if (status == ESP8266_COMMAND_OK || status == ESP8266_COMMAND_NO_CHANGE) {
			oled.print("OK ");
			wifi.setMultipleConnections(true);
			status = wifi.joinAP(WIFI_SSID, WIFI_PWD);
			if (status == ESP8266_COMMAND_OK || status == ESP8266_COMMAND_NO_CHANGE) {
				oled.setCursor(3, 1);
				oled.print("joined      ");
				oled.setCursor(0, 0);
				status = wifi.getIP(ESP8266_WIFI_STATION, ipAddress);
				oled.print(wifiStatusString(status));
				TRACE("IP-Address:");
				TRACE(ipAddress);
				oled.setCursor(0, 1);
				//if (status == ESP8266_COMMAND_OK || status == ESP8266_COMMAND_NO_CHANGE) {
				oled.print(ipAddress[0]);
				oled.print(".");
				oled.print(ipAddress[1]);
				oled.print(".");
				oled.print(ipAddress[2]);
				oled.print(".");
				oled.print(ipAddress[3]);
				//}
				/*else {
				oled.setCursor(0, 0);
				oled.print("IP Address  ");
				oled.setCursor(0, 1);
				oled.print(wifiStatusString(status));
				delay(5000);
				}*/
				delay(1000);

			}
			else {
				oled.setCursor(0, 1);
				oled.print(wifiStatusString(status));
			}
			espClient.begin();
			result = true;
		}
		else {
			oled.setCursor(0, 1);
			oled.print(wifiStatusString(status));
		}
	}
	else {
		oled.print("FAIL");
	}
	return result;
}


int readMoisture() {
  digitalWrite(MOISTURE_SWITCH, LOW);
  delay(1);
  int val=0;
  for(byte i=0; i<4; i++) {
    val += analogRead(MOISTURE_PIN);
    delay(1);
  }
  digitalWrite(MOISTURE_SWITCH, HIGH);
  // averaging
  // reading inverted by circuit
  return 1023 - (val >> 2); // div 4
}

float readVcc() {
  // is connected via a voltage divider which halfs the real volate
  float val=(float)analogRead(VCC_PIN);
  return (val/511.5) * ADC_REF;
}

void setupTimer() {
  // set timer to 1 sec
  noInterrupts();
	// cli();          // disable global interrupts
 	TCCR1A = 0;     // set entire TCCR1A register to 0
 	TCCR1B = 0;     // same for TCCR1B
 	// set compare match register to desired timer count:
	OCR1A = 15624;
 	// turn on CTC mode:
	TCCR1B |= (1 << WGM12);
 	// Set CS10 and CS12 bits for 1024 prescaler:
	TCCR1B |= (1 << CS10);
	TCCR1B |= (1 << CS12);
 	
 	//sei();          // enable global interrupts  
  interrupts();
 }

void enableTimer() {
  noInterrupts();
  // enable timer compare interrupt:
  TIMSK1 |= (1 << OCIE1A);
  interrupts();
}

float readTemperature()
{
  int B = 3975;
  int val = analogRead(A0);
  float resistance = (float)(1023.0 - val) * 10000.0 / (float)val;
  float temp = 1 / (log(resistance / 10000) / B + 1 / 298.15) - 273.15;

  return temp;
}

void readSensors() {
  Tnow = readTemperature();

  // once in a minute update the average temperature
  tickCounter = (tickCounter+1) % 60;
  if (tickCounter == 0) {
    // Tavg = (Tnow * Talpha) + Tavg*(1.0 - Talpha);
    float t = (float)tAvgCounter * Tavg;
    tAvgCounter++;
    t += Tnow;
    Tavg = t / (float)tAvgCounter;
    TRACE("Tavg=");
    TRACE(Tavg);
  }
  
  moisture = (int)((float)readMoisture() * moistureAlpha + (float)moisture * (1.0 - moistureAlpha));
  
}

void handleFSM() {
  switch (state) {
    case IDLE:
      if (now > nextPumpTS - dayAberration || forcePumping) {
        state = PUMP;
        tAvgCounter=10;
        
        //oled.setCursor(0, 1);
        if (moisture < MOISTURE_PUMP_THRESHOLD || forcePumping) {
          digitalWrite(PUMP_PIN, HIGH);
          digitalWrite(LED_BUILTIN, HIGH);
          //oled.print("pumping     ");
          sendPumpPingFlag=2;
        }
        else {
          //oled.print("pump suspend");
          sendPumpPingFlag=1;
        }
        forcePumping = false;
        lastPumpTS = now;
      }
      else {
        float abFac = (exp((17.62 * Tavg) / (273.12 + Tavg))) / (float)DAY_ABR_ZERO; // Null-wert bei avg $DAY_ABR_ZERO °C
        dayAberration = (long)(abFac*(float)oneDay);

        Tdiff = (long)(nextPumpTS - dayAberration) - now;
        
      }
      break;
    case PUMP:
      if (now > lastPumpTS + 65L) {
        state = IDLE;
        digitalWrite(PUMP_PIN, LOW);
        digitalWrite(LED_BUILTIN, LOW);
        //oled.clear();
        nextPumpTS = now + oneDay;
      }
      break;
    }
}

ISR(TIMER1_COMPA_vect) {
	// noInterrupts();
	now++;
	digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
	tick = true;

  readSensors();
  handleFSM();
  // interrupts();
}

unsigned int readPumpThreshold() {
	return (unsigned int)((EEPROM.read(ROM_ADDRESS) << 8) + EEPROM.read(ROM_ADDRESS + 1));
}
void savePumpThreshold() {
	EEPROM.write(ROM_ADDRESS, highByte(MOISTURE_PUMP_THRESHOLD));
	EEPROM.write(ROM_ADDRESS + 1, lowByte(MOISTURE_PUMP_THRESHOLD));
}

void saveDayAbrZero() {
	EEPROM.write(ROM_ADDRESS + 2, highByte(DAY_ABR_ZERO));
	EEPROM.write(ROM_ADDRESS + 3, lowByte(DAY_ABR_ZERO));
}

int readDayAbrZero() {
	return (int)((EEPROM.read(ROM_ADDRESS+2) << 8) + EEPROM.read(ROM_ADDRESS + 3));
}

void sendSensorData() {
  TRACE(F("Sending sensor data.."));
	if (wifi.test() != ESP8266_COMMAND_OK) {
    wdt_reset();
    TRACE(F("restarting WiFi"));
		wifi.restart();
    wdt_reset();
		delay(1000);
    TRACE(F("re-setup WiFi"));
		setupWiFi();
	}
	//else {
		// reset
	//	http.stop();
	//}
	wdt_reset();
  // format of s[]: sensor-ID, Type (M-oisture, T-emperature, P-ump, V-oltage,D-elta time), value
	String url = F("/plant_nanny/update.php?cmd=save&s[]=1,M,");
	url.concat(moisture);
	url.concat(F("&s[]=3,T,"));
	url.concat(Tnow);
  url.concat(F("&s[]=4,D,"));
  url.concat(Tdiff);
  url.concat(F("&s[]=5,T,"));
  url.concat(Tavg);
	oled.setCursor(0, 0);
	oled.print("TX ");

	TRACE(F("Sending request:"));
  TRACE(url);
	int err = http.get(serverIP, HOST_NAME, url.c_str());
  TRACE(F("result:"));
  TRACE(err);
	wdt_reset();
	if (err > 0) {
		oled.print("c:");
		oled.print(err);
		oled.print(" ");
	}
	else {
		oled.print("#");
		oled.print(err);
	}
	sendTS = now;
	waitingForResponse = true;
}

void sendPumpPing(boolean pump) {
	if (wifi.test() != ESP8266_COMMAND_OK) {
		wifi.restart();
		delay(1000);
		wdt_disable();
		setupWiFi();
		wdt_enable(WDTO_4S);
	}
	//else {
		// reset
	//	http.stop();
	//}

	String url = F("/plant_nanny/update.php?cmd=save&");
	if (pump) {
		url.concat(F("s[]=1,P,1&s[]=2,P,1"));
	}
	else {
		url.concat(F("s[]=1,P,0&s[]=2,P,0"));
	}
	sendTS = now;
  wdt_reset();
	http.get(serverIP, HOST_NAME, url.c_str());
	wdt_reset();
	waitingForResponse = true;
}

void receiveHttp() {
	waitingForResponse = false;
  TRACE(F("Receiving data.."));
	oled.setCursor(0, 0);
	oled.print("RX ");
	int err = http.responseStatusCode();
	oled.print("#");
	
	oled.print(err);
  TRACE(err);
	// err = 200;
	if (err == 200) {
		int res = http.skipResponseHeaders();
		TRACE(F("Transfer Encoding:"));
		TRACE(http.getTransferEncoding());
		TRACE(F("Content Length:"));
		TRACE(http.contentLength());

		int length = http.contentLength();
		uint8_t* buffer = (uint8_t*)malloc(length+1);
		if (buffer != NULL) {
			http.readBytes(buffer, length);

			// Terminate String
			buffer[length] = '\0';

			oled.clear();
			oled.setCursor(0, 0);
			oled.print(">");
			// oled.print((char*)buffer);

			TRACE(F("HTTP-Content: "));
			TRACE((char*)buffer);

			parseReceivedData((char*)buffer);

			free(buffer);
      // empty RX buffer;
      while (http.available() > 0) {
        http.read();
      }
		}
		else {
			oled.print("MALLOC ");
		}
	}
	
	http.stop();
	
}

void parseReceivedData(const char* data) {
	String str(data);
  TRACE("rec data:\r\n");
  TRACE(data);
	int iEqual = -1;
	do {
		int newEqual = str.indexOf('=', iEqual+1);
		if (newEqual > -1) {
			String varname = str.substring(iEqual + 1, newEqual);
			int valueIndex = str.indexOf('&', newEqual);
			String value = str.substring(newEqual + 1, valueIndex == -1 ? str.length() : valueIndex);
			newEqual += value.length() + 1;
			TRACE(varname + " = " + value);
			if (varname == F("result")) {
				oled.print(value.c_str());
			}
			else if (varname == F("pump_threshold")) {
				int newThreshold = value.toInt();
				if (newThreshold > 0) {
					MOISTURE_PUMP_THRESHOLD = (uint16_t)newThreshold;
					savePumpThreshold();
				}
			}
			else if (varname == F("cmd")) {
				if (value == F("pump_now")) {
					// nextPumpTS = now + dayAberration - 1;
          forcePumping = true;
				}
			}
			else if (varname == F("abr_temp")) {
				DAY_ABR_ZERO = (int)value.toInt();
				saveDayAbrZero();
			}
		}
		iEqual = newEqual;
	} 
	while (iEqual != -1);
}

void setup() {
  wasWDTReset = bitRead( mcusr_mirror,WDRF);
  wdt_disable();
  
  digitalWrite(MOISTURE_SWITCH, HIGH);
  
  Serial.begin(9600);
#ifdef DEBUG
  dbgSerial.begin(9600);
#endif
  TRACE("> Starting");
  
  if (wasWDTReset) {
    TRACE(" WDT Reset");
  }

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  MOISTURE_PUMP_THRESHOLD = readPumpThreshold();
  DAY_ABR_ZERO = readDayAbrZero() != 0 ? readDayAbrZero() : DAY_ABR_ZERO;

  oled.begin(12, 2);
  delay(100);
  oled.clear();

  oled.setCursor(0, 1);
  oled.print((int)MOISTURE_PUMP_THRESHOLD);
  oled.print(" ");
  oled.print(DAY_ABR_ZERO);
  
  oled.setCursor(0, 0);
  oled.print("pump-test");
  digitalWrite(PUMP_PIN, HIGH);
  delay(1000);
  digitalWrite(PUMP_PIN, LOW);
  oled.print("ok");
  delay(1000);
  
  TRACE("timer setup..");
  setupTimer();
  TRACE("done."); 
  
  bool res=false;
  byte si=0;
  if (wifi.restart()==false) {
    oled.setCursor(0,0);
    oled.print("wifi RST fail");
  }
  do {
    res=setupWiFi();
    si++;
  }
  while (res==false && si < 10);
  #ifdef DEBUG
  TRACE("Wifi-setup:");
  TRACE(res ? "OK " : "FAILED ");
  TRACE(si);
  #endif
  delay(1000);

  Tnow = Tavg = readTemperature();

  moisture = readMoisture();

  vcc = readVcc();
  
// enable watchdog-timer at 4 seconds
  TRACE("enable WDT"); 
  wdt_enable(WDTO_4S); 
  
  TRACE("enable Timer IRQ");
  enableTimer();
  TRACE("setup done"); 
}


void loop() {
	
	wdt_reset();
	if (sendTS == 0L || now - sendTS > WWW_UPDATE_INTERVAL) {
		sendSensorData();
	}

  if (sendPumpPingFlag > 0) {
    sendPumpPing( sendPumpPingFlag==2 );
    oled.setCursor(0,0);
    if (sendPumpPingFlag==2) {
      oled.print("pumping     ");
    }
    else {
      oled.print("pump suspend");
    }
    sendPumpPingFlag=0;
  }

	if (waitingForResponse && http.available()) {
		receiveHttp();
	}

	if (tick) {
		tick = false;
   
		oled.setCursor(0, 1);
    if (now % 2 == 0) {
		  oled.print((int)Tavg);
		  oled.print("C ");
    }
    else {
      oled.print(max(0, (int)(((moisture-MOISTURE_PUMP_THRESHOLD)*100) / (1023-MOISTURE_PUMP_THRESHOLD))));
      oled.print("% ");
    }

    if (state==IDLE) {
      
        float abFac = (exp((17.62 * Tavg) / (273.12 + Tavg))) / (float)DAY_ABR_ZERO; // Null-wert bei avg $DAY_ABR_ZERO °C
        dayAberration = (long)(abFac*(float)oneDay);

        Tdiff = (long)(nextPumpTS - dayAberration) - now;
        // oled.setCursor(0, 1);
        
        oled.print(Tdiff / 3600L);
        oled.print(":");
        oled.print((Tdiff % 3600L) / 60L);
        oled.print(":");
        oled.print((Tdiff % 3600L) % 60L);
   
    } 
	}
	
}
char* wifiStatusString(ESP8266CommandStatus status) {
	switch (status) {
	case ESP8266_COMMAND_INVALID:
		return "INVALID";
		break;

	case ESP8266_COMMAND_TIMEOUT:
		return "TIMEOUT";
		break;

	case ESP8266_COMMAND_OK:
		return "OK";
		break;

	case ESP8266_COMMAND_NO_CHANGE:
		return "NO CHANGE";
		break;

	case ESP8266_COMMAND_ERROR:
		return "ERROR";
		break;

	case ESP8266_COMMAND_NO_LINK:
		return "NO LINK";
		break;

	case ESP8266_COMMAND_TOO_LONG:
		return "TOO LONG";
		break;

	case ESP8266_COMMAND_FAIL:
		return "FAIL";
		break;

	default:
		return "UNKNOWN COMMAND STATUS";
		break;
	}
}
