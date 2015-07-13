#define TRACE(d) dbgSerial.println(d)

#include <ESP8266Client.h>
#include <ESP8266.h>
#include <Wire.h>
#include "OLED.h"
#include <SoftwareSerial\SoftwareSerial.h>
#include "HttpClient.h"

#define NTC_PIN A0
#define MOISTURE_PIN_1 A1
#define MOISTURE_PIN_2 A2
#define PUMP_PIN A3


#define WIFI_SSID "lovely-N"
#define WIFI_PWD "d0tlab1$$up1"

unsigned int MOISTURE_PUMP_THRESHOLD = 300;

OLED oled;

volatile float Tavg;
volatile float Tnow;

const float Talpha = 0.005;
const float moistureAlpha = 0.1;

int moisture[] = { 0, 0 };

SoftwareSerial dbgSerial(2, 3);

ESP8266 wifi = ESP8266(Serial, dbgSerial);
IPAddress ipAddress;

ESP8266Client espClient(wifi);
HttpClient http(espClient);

IPAddress serverIP(192, 168, 1, 43);

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

// Hardware-Serial > SoftwareSerial pass-through
//void serialEvent() {
//	char *buffer = (char *)malloc(Serial.available());
//	Serial.readBytes(buffer, Serial.available());
//	espSerial.write(buffer);
//}

void setup()
{
	Serial.begin(9600);
	dbgSerial.begin(9600);
	TRACE("> Starting");

	pinMode(LED_BUILTIN, OUTPUT);
	pinMode(PUMP_PIN, OUTPUT);
	oled.begin(12, 2);
	delay(100);
	oled.clear();

	oled.setCursor(0, 0);
	oled.print("pump-test");
	digitalWrite(PUMP_PIN, HIGH);
	delay(1000);
	digitalWrite(PUMP_PIN, LOW);
	oled.print("ok");
	delay(1000);

	oled.setCursor(0, 0);
	oled.print("WiFi: ");
	if (wifi.begin() == true) {
        wifi.setTimeout(5000);
		wifi.restart();
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
					oled.print('.');
					oled.print(ipAddress[1]);
					oled.print('.');
					oled.print(ipAddress[2]);
					oled.print('.');
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
			
		}
		else {
			oled.setCursor(0, 1);
			oled.print(wifiStatusString(status));
		}
		
	}
	else {
		oled.print("FAIL");
	}
	delay(5000);

	Tnow = Tavg = readTemperature();

	moisture[0] = analogRead(MOISTURE_PIN_1);
	moisture[1] = analogRead(MOISTURE_PIN_2);

	setupTimer();
}

void setupTimer() {
	cli();          // disable global interrupts
	TCCR1A = 0;     // set entire TCCR1A register to 0
	TCCR1B = 0;     // same for TCCR1B

	// set compare match register to desired timer count:
	OCR1A = 15624;
	// turn on CTC mode:
	TCCR1B |= (1 << WGM12);
	// Set CS10 and CS12 bits for 1024 prescaler:
	TCCR1B |= (1 << CS10);
	TCCR1B |= (1 << CS12);
	// enable timer compare interrupt:
	TIMSK1 |= (1 << OCIE1A);
	sei();          // enable global interrupts  
}

ISR(TIMER1_COMPA_vect) {
	Tnow = readTemperature();

	// once in a minute update the average temperature
	tickCounter = (tickCounter+1) % 60;
	if (tickCounter == 0) {
		Tavg = (Tnow * Talpha) + Tavg*(1.0 - Talpha);
	}

	moisture[0] = (int)((float)analogRead(MOISTURE_PIN_1) * moistureAlpha + (float)moisture[0] * (1.0 - moistureAlpha));
	moisture[1] = (int)((float)analogRead(MOISTURE_PIN_2) * moistureAlpha + (float)moisture[1] * (1.0 - moistureAlpha));
	now++;
	digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
	tick = true;
}

float readTemperature()
{
	int B = 3975;
	int val = analogRead(A0);
	float resistance = (float)(1023.0 - val) * 10000.0 / (float)val;
	float temp = 1 / (log(resistance / 10000) / B + 1 / 298.15) - 273.15;

	return temp;
}

unsigned long sendTS = 0L;
bool waitingForResponse = false;

void sendSensorData() {
	String url = F("/d-parc/fb_flora_feed/update.php?cmd=save&s[0][id]=1&s[0][t]=0&s[1][id]=2&s[1][t]=0&s[2][id]=3&s[2][t]=1&s[0][v]=");
	url.concat(moisture[0]);
	url.concat("&s[1][v]=");
	url.concat(moisture[1]);
	url.concat("&s[2][v]=");
	url.concat(Tavg);
	oled.setCursor(0, 0);
	oled.print("TX ");
	int err = http.get(serverIP, "192.168.1.43", url.c_str());
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

void receiveHttp() {
	waitingForResponse = false;

	oled.setCursor(0, 0);
	oled.print("RX ");
	int err = http.responseStatusCode();
	oled.print("#");
	oled.print(err);
	if (err == 200) {
		int res = http.skipResponseHeaders();
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

		}
		else {
			oled.print("MALLOC ");
		}
	}
	else {
		oled.print("r:");
		oled.print(err);
	}
	http.stop();
	
}

void parseReceivedData(const char* data) {
	String str(data);
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
				}
			}
			else if (varname == F("cmd")) {
				if (value == F("pump_now")) {
					nextPumpTS = now + dayAberration;
				}
			}
		}
		iEqual = newEqual;
	} 
	while (iEqual != -1);
}

void loop()
{
	if (sendTS == 0L || now - sendTS > 300) {
		sendSensorData();
	}

	if (waitingForResponse && http.available()) {
		receiveHttp();
	}

	if (tick) {
		tick = false;

		oled.setCursor(0, 1);
		oled.print(Tavg, 0);
		oled.print(' ');
		switch (state) {
		case IDLE:
			if (now > nextPumpTS - dayAberration) {
				state = PUMP;
				oled.setCursor(0, 1);
				if (moisture[0] < MOISTURE_PUMP_THRESHOLD && moisture[1] < MOISTURE_PUMP_THRESHOLD) {
					digitalWrite(PUMP_PIN, HIGH);
					digitalWrite(13, HIGH);
					oled.print("pumping     ");
				}
				else {
					oled.print("pump suspend");
				}
				lastPumpTS = now;
			}
			else {
				float abFac = (exp((17.62 * Tavg) / (273.12 + Tavg))) / 20.0; // Null-wert bei avg 15 °C
				dayAberration = (long)(abFac*(float)oneDay);

				long Tdiff = (long)(nextPumpTS - dayAberration) - now;
				// oled.setCursor(0, 1);
				oled.print(Tdiff / 3600L);
				oled.print(":");
				oled.print((Tdiff % 3600L) / 60L);
				oled.print(":");
				oled.print((Tdiff % 3600L) % 60L);
			}
			break;
		case PUMP:
			if (now > lastPumpTS + 65L) {
				state = IDLE;
				digitalWrite(PUMP_PIN, LOW);
				digitalWrite(13, LOW);
				oled.clear();
				nextPumpTS += oneDay;
			}
			break;
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
