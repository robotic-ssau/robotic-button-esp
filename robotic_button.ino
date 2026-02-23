#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

#define WIFI_SSID "ЗАПОЛНИТЕ"
#define WIFI_PASS "ЗАПОЛНИТЕ"
#define BUTTON 13
#define LED_OPEN 12
#define LED_CLOSE 14

#define STATE_DELAY 60*1000
#define CLOCK_DELAY 60*1000
#define CLOCK_START 22
#define CLOCK_END 7

// Текстовые константы для LCD и Serial
#define INIT_1                   " Initialization "
#define INIT_2                   "    program     "

#define OPENED_1                 "     Robotic    "
#define OPENED_2                 "     OPENED     "

#define CLOSED_1                 "     Robotic    "
#define CLOSED_2                 "     CLOSED     "

#define START_SERVER_1           "   Web-server   "
#define START_SERVER_2           "     started    "

#define CONNECT_SERVER_1         "     Connect    "
#define CONNECT_SERVER_2         "    to server   "

#define CONNECT_SERVER_FAIL_1    " Connect server "
#define CONNECT_SERVER_FAIL_2    "     FAILED     "

#define CONNECT_SERVER_SUCCESS_1 " Connect server "
#define CONNECT_SERVER_SUCCESS_2 "     SUCCESS    "

#define CONNECT_WIFI_1           "     Connect    "
#define CONNECT_WIFI_2           "     to WiFi    "
#define CONNECTED_WIFI_1         " Connected WiFi "
#define CONNECTED_WIFI_2         " "  // Для IP адреса (будет добавляться динамически)

#define WIFI_FAIL_1              " WiFi connect  "
#define WIFI_FAIL_2              "    FAILED!    "

#define SEND_FAIL_1              "  Send failed   "
#define SEND_FAIL_2              "   Check conn   "

#define RESPONSE_TIMEOUT_1       "  Response      "
#define RESPONSE_TIMEOUT_2       "    timeout     "

#define INVALID_RESPONSE_1       " Invalid server "
#define INVALID_RESPONSE_2       "    response    "

#define SERVER_ERROR_1           "  Server error  "
#define SERVER_ERROR_2           "  State not set "

#define HTTP_ERROR_1             " HTTP Error:    "
// HTTP_ERROR_2 будет динамическим

#define TIME_UPDATE_FAILED       "Failed to update time"

#define BUTTON_PRESSED           "Button pressed - changing state"
#define OPEN_STATE               "OPEN"
#define CLOSE_STATE              "CLOSE"
#define LATE_NIGHT_CLOSING       "Late night - closing"
#define SENDING_REQUEST          "Sending request:"
#define RESPONSE_RECEIVED        "Response received:"
#define STATE_OPEN               "State: OPEN"
#define STATE_CLOSED             "State: CLOSED"
#define NO_STATE_IN_RESPONSE     "Response doesn't contain state"
#define HTTP_ERROR               "HTTP Error: "

#define HTTP_TIMEOUT 5000

String host = "ЗАПОЛНИТЕ";
int port = 8000;
String endpointChange = "/change_state";
String endpointState = "/state";

volatile bool opened = false;           // Состояние открытости/закрытости лаборатории
volatile bool stateChanged = false;     // Флаг изменения состояния от кнопки
bool lastOpened = false;                // Для отслеживания изменений (не volatile)

ICACHE_RAM_ATTR void handleButtonInterrupt() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();

  // Простой антидребезг - игнорируем прерывания, которые пришли слишком быстро
  if (interruptTime - lastInterruptTime > 200) {
    opened = !opened;           // Инвертируем состояние
    stateChanged = true;        // Устанавливаем флаг для обработки в loop
  }
  lastInterruptTime = interruptTime;
}

LiquidCrystal_I2C LCD(0x27, 16, 2);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
WiFiClient client;

void setup() {
  Serial.begin(9600);

  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(LED_OPEN, OUTPUT);
  pinMode(LED_CLOSE, OUTPUT);

  // Настройка прерывания для кнопки
  attachInterrupt(digitalPinToInterrupt(BUTTON), handleButtonInterrupt, FALLING);

  LCD.init();
  LCD.backlight();
  printLCD(INIT_1, INIT_2);

  timeClient.begin();
  timeClient.setTimeOffset(+4 * 3600);

  delay(1000);
  connectWiFi();
  updateState();
  setButton(opened);
}

void loop() {
  // Проверяем флаг от прерывания
  if (stateChanged) {
    stateChanged = false;
    
    noInterrupts();
    bool currentState = opened;
    interrupts();
    
    Serial.println(BUTTON_PRESSED);
    sendState(currentState);
    setButton(currentState);
  }
  
  checkState();
  checkClock();
}

void connectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    printLCD(CONNECT_WIFI_1, CONNECT_WIFI_2);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      String ipStr = " " + WiFi.localIP().toString();
      Serial.println(CONNECTED_WIFI_1 + ipStr);
      printLCD(CONNECTED_WIFI_1, ipStr);
      delay(5000);
    } else {
      printLCD(WIFI_FAIL_1, WIFI_FAIL_2);
      delay(5000);
    }
  }
}

bool connectServer() {
  if (!client.connected()) {
    printLCD(CONNECT_SERVER_1, CONNECT_SERVER_2);
    delay(300);

    if (!client.connect(host, port)) {
      printLCD(CONNECT_SERVER_FAIL_1, CONNECT_SERVER_FAIL_2);
      delay(5000);
      return false;
    }

    printLCD(CONNECT_SERVER_SUCCESS_1, CONNECT_SERVER_SUCCESS_2);
    delay(300);
    return true;
  }
  return true;
}

void setButton(bool state) {
  if (state) {
    digitalWrite(LED_OPEN, HIGH);
    digitalWrite(LED_CLOSE, LOW);
    Serial.println(OPEN_STATE);
    printLCD(OPENED_1, OPENED_2);
  } else {
    digitalWrite(LED_OPEN, LOW);
    digitalWrite(LED_CLOSE, HIGH);
    Serial.println(CLOSE_STATE);
    printLCD(CLOSED_1, CLOSED_2);
  }
  lastOpened = state;
}

void printLCD(String line1, String line2) {
  // Выводим в Serial
  Serial.println(line1);
  Serial.println(line2);
  Serial.println();
  
  // Выводим на LCD
  LCD.setCursor(0, 0);
  LCD.print(line1);
  LCD.setCursor(0, 1);
  LCD.print(line2);
}

void checkClock() {
  static unsigned long lastClockCheck = 0;
  unsigned long curTime = millis();

  if ((curTime - lastClockCheck) > CLOCK_DELAY || curTime < lastClockCheck) {
    lastClockCheck = curTime;

    if (!timeClient.update()) {
      Serial.println(TIME_UPDATE_FAILED);
      return;
    }

    int currentHour = timeClient.getHours();
    Serial.print("Current hour: ");
    Serial.println(currentHour);

    noInterrupts();
    bool currentOpened = opened;
    interrupts();

    if ((CLOCK_START <= currentHour || currentHour < CLOCK_END) && currentOpened) {
      Serial.println(LATE_NIGHT_CLOSING);
      noInterrupts();
      opened = false;
      interrupts();
      sendState(false);
      setButton(false);
    }
  }
}

void checkState() {
  static unsigned long lastStateCheck = 0;
  unsigned long curTime = millis();

  if ((curTime - lastStateCheck) > STATE_DELAY || curTime < lastStateCheck) {
    lastStateCheck = curTime;
    updateState();
  }
}

void updateState() {
  if (!checkWiFiAndConnect()) return;
  if (!connectServer()) return;

  String request = "GET " + endpointState + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Connection: close\r\n";
  request += "Accept: application/json\r\n\r\n";

  Serial.println(SENDING_REQUEST);
  Serial.println(request);

  if (!sendHttpRequest(request)) {
    client.stop();
    return;
  }

  String response = readHttpResponse();
  client.stop();

  if (response.length() > 0) {
    bool newState = parseStateResponse(response);

    noInterrupts();
    if (newState != opened) {
      opened = newState;
      setButton(opened);
    }
    interrupts();
  }
}

void sendState(bool state) {
  if (!checkWiFiAndConnect()) return;
  if (!connectServer()) return;

  String postData = state ? "{\"state\": true}" : "{\"state\": false}";
  String request = "POST " + endpointChange + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Connection: close\r\n";
  request += "Accept: application/json\r\n";
  request += "Content-Length: " + String(postData.length()) + "\r\n\r\n";
  request += postData;

  Serial.println(SENDING_REQUEST);
  Serial.println(request);

  if (!sendHttpRequest(request)) {
    client.stop();
    return;
  }

  String response = readHttpResponse();
  client.stop();

  if (response.length() > 0) {
    Serial.println(RESPONSE_RECEIVED);
    Serial.println(response);

    if (response.indexOf("200 OK") == -1) {
      Serial.println("Server returned error status");
      printLCD(SERVER_ERROR_1, SERVER_ERROR_2);
      delay(2000);
    }
  }
}

bool checkWiFiAndConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }
  }
  return true;
}

bool sendHttpRequest(const String& request) {
  client.setTimeout(HTTP_TIMEOUT);

  int bytesSent = client.println(request);
  if (bytesSent == 0) {
    Serial.println("Failed to send request");
    printLCD(SEND_FAIL_1, SEND_FAIL_2);
    delay(2000);
    return false;
  }

  return true;
}

String readHttpResponse() {
  unsigned long timeout = millis() + HTTP_TIMEOUT;
  String response = "";

  while (millis() < timeout) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      response += line + "\n";

      if (line == "\r") {
        while (client.available() && millis() < timeout) {
          response += client.readString();
        }
        break;
      }
    }
    delay(10);
  }

  if (response.length() == 0) {
    Serial.println("Response timeout");
    printLCD(RESPONSE_TIMEOUT_1, RESPONSE_TIMEOUT_2);
    delay(2000);
  }

  return response;
}

bool parseStateResponse(const String& response) {
  int bodyStart = response.indexOf("\r\n\r\n");
  if (bodyStart != -1) {
    String body = response.substring(bodyStart + 4);
    body.trim();

    Serial.println("Response body:");
    Serial.println(body);

    if (body.indexOf("{\"state\":true}") != -1) {
      Serial.println(STATE_OPEN);
      return true;
    } else if (body.indexOf("{\"state\":false}") != -1) {
      Serial.println(STATE_CLOSED);
      return false;
    } else {
      Serial.println(NO_STATE_IN_RESPONSE);
      printLCD(INVALID_RESPONSE_1, INVALID_RESPONSE_2);
      delay(2000);
    }
  } else {
    if (response.indexOf("200 OK") == -1) {
      int statusStart = response.indexOf(' ');
      if (statusStart != -1) {
        String status = response.substring(statusStart + 1, response.indexOf('\n'));
        Serial.println(HTTP_ERROR + status);
        printLCD(HTTP_ERROR_1, status.substring(0, 15));
        delay(2000);
      }
    }
  }

  // Возвращаем текущее состояние, если не удалось распарсить
  noInterrupts();
  bool currentState = opened;
  interrupts();
  return currentState;
}
