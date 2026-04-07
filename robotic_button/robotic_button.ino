#include <LiquidCrystal_I2C.h>  // Библиотека для LCD дисплея
#include <ESP8266WiFi.h>        // Библиотека для работы с WiFi на ESP8266
#include <WiFiClient.h>         // Библиотека для TCP клиента
#include <WiFiUdp.h>            // Библиотека для UDP (нужна для NTP)
#include <NTPClient.h>          // Библиотека для получения времени через NTP

// ==================== НАСТРОЙКИ ====================
String host = "ЗАПОЛНИТЕ СВОИМ";  // Hostname Raspberry Pi (будет разрешаться в IP через DNS)
int port = ЗАПОЛНИТЕ СВОИМ;                 // Порт HTTP сервера на Raspberry Pi

#define WIFI_SSID "ЗАПОЛНИТЕ СВОИМ"  // Имя WiFi сети
#define WIFI_PASS "ЗАПОЛНИТЕ СВОИМ"      // Пароль WiFi сети
#define BUTTON 13                 // Пин кнопки (с внутренним подтягом)
#define LED_OPEN 12               // Пин светодиода "ОТКРЫТО"
#define LED_CLOSE 14              // Пин светодиода "ЗАКРЫТО"

#define STATE_DELAY 60 * 1000  // Интервал проверки состояния (1 минута)
#define CLOCK_DELAY 60 * 1000  // Интервал проверки времени (1 минута)
#define CLOCK_START 22         // Час начала "ночного режима" (22:00)
#define CLOCK_END 7            // Час окончания "ночного режима" (07:00)

// ==================== ТЕКСТОВЫЕ КОНСТАНТЫ ДЛЯ LCD И SERIAL ====================
#define INIT_1 " Initialization "
#define INIT_2 "    program     "

#define OPENED_1 "     Robotic    "
#define OPENED_2 "     OPENED     "

#define CLOSED_1 "     Robotic    "
#define CLOSED_2 "     CLOSED     "

#define START_SERVER_1 "   Web-server   "
#define START_SERVER_2 "     started    "

#define CONNECT_SERVER_1 "     Connect    "
#define CONNECT_SERVER_2 "    to server   "

#define CONNECT_SERVER_FAIL_1 " Connect server "
#define CONNECT_SERVER_FAIL_2 "     FAILED     "

#define CONNECT_SERVER_SUCCESS_1 " Connect server "
#define CONNECT_SERVER_SUCCESS_2 "     SUCCESS    "

#define CONNECT_WIFI_1 "     Connect    "
#define CONNECT_WIFI_2 "     to WiFi    "
#define CONNECTED_WIFI_1 " Connected WiFi "
#define CONNECTED_WIFI_2 " "  // Для IP адреса (будет добавляться динамически)

#define WIFI_FAIL_1 " WiFi connect  "
#define WIFI_FAIL_2 "    FAILED!    "

#define SEND_FAIL_1 "  Send failed   "
#define SEND_FAIL_2 "   Check conn   "

#define RESPONSE_TIMEOUT_1 "  Response      "
#define RESPONSE_TIMEOUT_2 "    timeout     "

#define INVALID_RESPONSE_1 " Invalid server "
#define INVALID_RESPONSE_2 "    response    "

#define SERVER_ERROR_1 "  Server error  "
#define SERVER_ERROR_2 "  State not set "

#define HTTP_ERROR_1 " HTTP Error:    "

#define TIME_UPDATE_FAILED "Failed to update time"

#define BUTTON_PRESSED "Button pressed - changing state"
#define OPEN_STATE "OPEN"
#define CLOSE_STATE "CLOSE"
#define LATE_NIGHT_CLOSING "Late night - closing"
#define SENDING_REQUEST "Sending request:"
#define RESPONSE_RECEIVED "Response received:"
#define STATE_OPEN "State: OPEN"
#define STATE_CLOSED "State: CLOSED"
#define NO_STATE_IN_RESPONSE "Response doesn't contain state"
#define HTTP_ERROR "HTTP Error: "

#define HTTP_TIMEOUT 5000  // Таймаут HTTP запроса (5 секунд)

// ==================== HTTP ENDPOINTS ====================
String endpointChange = "/change_state";  // Эндпоинт для изменения состояния
String endpointState = "/state";          // Эндпоинт для получения состояния

// ==================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ====================
volatile bool opened = false;                  // Состояние открытости/закрытости (volatile т.к. меняется в прерывании)
volatile bool stateChanged = false;            // Флаг изменения состояния от кнопки
bool lastOpened = false;                       // Для отслеживания изменений (не volatile)
LiquidCrystal_I2C LCD(0x27, 16, 2);            // LCD дисплей (адрес 0x27, 16 символов, 2 строки)
WiFiUDP ntpUDP;                                // UDP клиент для NTP
NTPClient timeClient(ntpUDP, "pool.ntp.org");  // NTP клиент для получения времени
WiFiClient client;                             // TCP клиент для HTTP запросов

// ==================== ОБРАБОТЧИК ПРЕРЫВАНИЯ КНОПКИ ====================
ICACHE_RAM_ATTR void handleButtonInterrupt() {
  static unsigned long lastInterruptTime = 0;  // Время последнего прерывания (антидребезг)
  unsigned long interruptTime = millis();

  // Простой антидребезг - игнорируем прерывания, которые пришли слишком быстро
  if (interruptTime - lastInterruptTime > 200) {
    opened = !opened;     // Инвертируем состояние
    stateChanged = true;  // Устанавливаем флаг для обработки в loop
  }
  lastInterruptTime = interruptTime;
}

// ==================== НАЧАЛЬНАЯ НАСТРОЙКА ====================
void setup() {
  Serial.begin(9600);  // Инициализация Serial порта для отладки

  // Настройка пинов
  pinMode(BUTTON, INPUT_PULLUP);  // Кнопка с внутренним подтягивающим резистором
  pinMode(LED_OPEN, OUTPUT);      // Светодиод "ОТКРЫТО"
  pinMode(LED_CLOSE, OUTPUT);     // Светодиод "ЗАКРЫТО"

  // Настройка прерывания для кнопки (по спадающему фронту)
  attachInterrupt(digitalPinToInterrupt(BUTTON), handleButtonInterrupt, FALLING);

  // Инициализация LCD
  LCD.init();
  LCD.backlight();
  printLCD(INIT_1, INIT_2);  // Вывод сообщения инициализации

  // Настройка NTP клиента
  timeClient.begin();
  timeClient.setTimeOffset(+4 * 3600);  // Часовой пояс Москва (UTC+4)

  delay(1000);
  connectWiFi();      // Подключение к WiFi
  updateState();      // Получение начального состояния с сервера
  setButton(opened);  // Установка начального состояния светодиодов
}

// ==================== ОСНОВНОЙ ЦИКЛ ====================
void loop() {
  // Проверяем флаг от прерывания (кнопка была нажата)
  if (stateChanged) {
    stateChanged = false;

    // Безопасное чтение volatile переменной
    noInterrupts();
    bool currentState = opened;
    interrupts();

    Serial.println(BUTTON_PRESSED);
    sendState(currentState);  // Отправляем новое состояние на сервер
    setButton(currentState);  // Обновляем светодиоды
  }

  checkState();  // Периодическая проверка состояния с сервера
  checkClock();  // Периодическая проверка времени (ночной режим)
}

// ==================== ПОДКЛЮЧЕНИЕ К WiFi ====================
void connectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    printLCD(CONNECT_WIFI_1, CONNECT_WIFI_2);

    WiFi.begin(WIFI_SSID, WIFI_PASS);  // Подключение к WiFi сети

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // Максимум 10 секунд (20 * 500ms)
      delay(500);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      String ipStr = " " + WiFi.localIP().toString();  // Получаем IP адрес ESP8266
      Serial.println(CONNECTED_WIFI_1 + ipStr);
      printLCD(CONNECTED_WIFI_1, ipStr);
      delay(5000);
    } else {
      printLCD(WIFI_FAIL_1, WIFI_FAIL_2);  // Ошибка подключения
      delay(5000);
    }
  }
}

// ==================== ПОДКЛЮЧЕНИЕ К СЕРВЕРУ ====================
bool connectServer() {
  if (!client.connected()) {
    printLCD(CONNECT_SERVER_1, CONNECT_SERVER_2);
    delay(300);

    // Разрешаем hostname в IP (динамическое получение IP Raspberry Pi)
    IPAddress ip;
    if (!WiFi.hostByName(host.c_str(), ip)) {
      // Если не удалось разрешить hostname
      printLCD(CONNECT_SERVER_FAIL_1, CONNECT_SERVER_FAIL_2);
      delay(5000);
      return false;
    }

    // Вывод разрешенного IP в Serial для отладки
    Serial.print("Resolved " + host + " to: ");
    Serial.println(ip);

    // Подключение по полученному IP адресу
    if (!client.connect(ip, port)) {
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

// ==================== УПРАВЛЕНИЕ СВЕТОДИОДАМИ И LCD ====================
void setButton(bool state) {
  if (state) {
    digitalWrite(LED_OPEN, HIGH);  // Включаем зеленый светодиод (ОТКРЫТО)
    digitalWrite(LED_CLOSE, LOW);  // Выключаем красный светодиод
    Serial.println(OPEN_STATE);
    printLCD(OPENED_1, OPENED_2);  // Выводим "OPENED" на LCD
  } else {
    digitalWrite(LED_OPEN, LOW);    // Выключаем зеленый светодиод
    digitalWrite(LED_CLOSE, HIGH);  // Включаем красный светодиод (ЗАКРЫТО)
    Serial.println(CLOSE_STATE);
    printLCD(CLOSED_1, CLOSED_2);  // Выводим "CLOSED" на LCD
  }
  lastOpened = state;  // Сохраняем состояние для отслеживания изменений
}

// ==================== ВЫВОД НА LCD ДИСПЛЕЙ ====================
void printLCD(String line1, String line2) {
  // Выводим в Serial для отладки
  Serial.println(line1);
  Serial.println(line2);
  Serial.println();

  // Выводим на LCD
  LCD.setCursor(0, 0);  // Первая строка
  LCD.print(line1);
  LCD.setCursor(0, 1);  // Вторая строка
  LCD.print(line2);
}

// ==================== ПРОВЕРКА ВРЕМЕНИ (НОЧНОЙ РЕЖИМ) ====================
void checkClock() {
  static unsigned long lastClockCheck = 0;  // Время последней проверки
  unsigned long curTime = millis();

  // Проверяем каждые CLOCK_DELAY миллисекунд
  if ((curTime - lastClockCheck) > CLOCK_DELAY || curTime < lastClockCheck) {
    lastClockCheck = curTime;

    // Обновляем время с NTP сервера
    if (!timeClient.update()) {
      Serial.println(TIME_UPDATE_FAILED);
      return;
    }

    int currentHour = timeClient.getHours();  // Получаем текущий час
    Serial.print("Current hour: ");
    Serial.println(currentHour);

    // Безопасное чтение состояния
    noInterrupts();
    bool currentOpened = opened;
    interrupts();

    // Если ночное время (22:00 - 07:00) И состояние ОТКРЫТО
    if ((CLOCK_START <= currentHour || currentHour < CLOCK_END) && currentOpened) {
      Serial.println(LATE_NIGHT_CLOSING);
      // Автоматически закрываем
      noInterrupts();
      opened = false;
      interrupts();
      sendState(false);  // Отправляем команду закрытия на сервер
      setButton(false);  // Обновляем светодиоды
    }
  }
}

// ==================== ПЕРИОДИЧЕСКАЯ ПРОВЕРКА СОСТОЯНИЯ ====================
void checkState() {
  static unsigned long lastStateCheck = 0;
  unsigned long curTime = millis();

  // Проверяем каждые STATE_DELAY миллисекунд
  if ((curTime - lastStateCheck) > STATE_DELAY || curTime < lastStateCheck) {
    lastStateCheck = curTime;
    updateState();  // Обновляем состояние с сервера
  }
}

// ==================== ОБНОВЛЕНИЕ СОСТОЯНИЯ С СЕРВЕРА ====================
void updateState() {
  if (!checkWiFiAndConnect()) return;  // Проверяем WiFi подключение
  if (!connectServer()) return;        // Подключаемся к серверу

  // Формируем GET запрос
  String request = "GET " + endpointState + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";  // Hostname (используется для HTTP/1.1)
  request += "Connection: close\r\n";
  request += "Accept: application/json\r\n\r\n";

  Serial.println(SENDING_REQUEST);
  Serial.println(request);

  // Отправляем запрос
  if (!sendHttpRequest(request)) {
    client.stop();
    return;
  }

  // Читаем ответ
  String response = readHttpResponse();
  client.stop();

  // Парсим ответ и обновляем состояние при необходимости
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

// ==================== ОТПРАВКА СОСТОЯНИЯ НА СЕРВЕР ====================
void sendState(bool state) {
  if (!checkWiFiAndConnect()) return;  // Проверяем WiFi
  if (!connectServer()) return;        // Подключаемся к серверу

  // Формируем JSON тело запроса
  String postData = state ? "{\"state\": true}" : "{\"state\": false}";

  // Формируем POST запрос
  String request = "POST " + endpointChange + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Connection: close\r\n";
  request += "Accept: application/json\r\n";
  request += "Content-Length: " + String(postData.length()) + "\r\n\r\n";
  request += postData;

  Serial.println(SENDING_REQUEST);
  Serial.println(request);

  // Отправляем запрос
  if (!sendHttpRequest(request)) {
    client.stop();
    return;
  }

  // Читаем ответ
  String response = readHttpResponse();
  client.stop();

  if (response.length() > 0) {
    Serial.println(RESPONSE_RECEIVED);
    Serial.println(response);

    // Проверяем статус ответа
    if (response.indexOf("200 OK") == -1) {
      Serial.println("Server returned error status");
      printLCD(SERVER_ERROR_1, SERVER_ERROR_2);
      delay(2000);
    }
  }
}

// ==================== ПРОВЕРКА WIFI ПОДКЛЮЧЕНИЯ ====================
bool checkWiFiAndConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();  // Пытаемся переподключиться
    if (WiFi.status() != WL_CONNECTED) {
      return false;  // Не удалось подключиться
    }
  }
  return true;  // WiFi подключен
}

// ==================== ОТПРАВКА HTTP ЗАПРОСА ====================
bool sendHttpRequest(const String& request) {
  client.setTimeout(HTTP_TIMEOUT);  // Устанавливаем таймаут

  int bytesSent = client.println(request);  // Отправляем запрос
  if (bytesSent == 0) {
    Serial.println("Failed to send request");
    printLCD(SEND_FAIL_1, SEND_FAIL_2);
    delay(2000);
    return false;
  }

  return true;
}

// ==================== ЧТЕНИЕ HTTP ОТВЕТА ====================
String readHttpResponse() {
  unsigned long timeout = millis() + HTTP_TIMEOUT;
  String response = "";

  // Читаем ответ пока не истек таймаут
  while (millis() < timeout) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      response += line + "\n";

      // Если достигнут конец заголовков (пустая строка)
      if (line == "\r") {
        // Читаем тело ответа
        while (client.available() && millis() < timeout) {
          response += client.readString();
        }
        break;
      }
    }
    delay(10);  // Небольшая задержка для предотвращения зависания
  }

  if (response.length() == 0) {
    Serial.println("Response timeout");
    printLCD(RESPONSE_TIMEOUT_1, RESPONSE_TIMEOUT_2);
    delay(2000);
  }

  return response;
}

// ==================== ПАРСИНГ ОТВЕТА ОТ СЕРВЕРА ====================
bool parseStateResponse(const String& response) {
  // Ищем начало тела ответа (после двойного переноса строки)
  int bodyStart = response.indexOf("\r\n\r\n");
  if (bodyStart != -1) {
    String body = response.substring(bodyStart + 4);  // Извлекаем тело
    body.trim();

    Serial.println("Response body:");
    Serial.println(body);

    // Проверяем JSON ответ
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
    // Если не нашли тело ответа, проверяем статус ошибки
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
