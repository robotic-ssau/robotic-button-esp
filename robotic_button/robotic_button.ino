/*
 * ============================================================================
 *  Robotic Button — прошивка v2.0 (ESP8266, Arduino IDE)
 * ============================================================================
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <LiquidCrystal_I2C.h>

#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR  // для старых версий ядра ESP8266
#endif

#define FW_VERSION "2.0"

// ============================================================================
//  НАСТРОЙКИ
// ============================================================================
const char*    WIFI_SSID   = "Robotic_Wifi";
const char*    WIFI_PASS   = "robowifi";

const char*    SERVER_HOST = "192.168.68.120";  // IP или доменное имя сервера
const uint16_t SERVER_PORT = 8005;
const char*    ENDPOINT_STATE  = "/state";         // GET  -> {"state": true|false}
const char*    ENDPOINT_CHANGE = "/change_state";  // POST <- {"state": true|false}

const uint8_t  PIN_BUTTON    = 13;  // кнопка на GND, внутренняя подтяжка
const uint8_t  PIN_LED_OPEN  = 12;  // светодиод «ОТКРЫТО»
const uint8_t  PIN_LED_CLOSE = 14;  // светодиод «ЗАКРЫТО»

const uint8_t  LCD_ADDR = 0x27;
const uint8_t  LCD_COLS = 16;

// Интервалы, мс
const uint32_t POLL_INTERVAL_MS      = 60UL * 1000;  // опрос сервера в норме
const uint32_t POLL_RETRY_MS         = 15UL * 1000;  // опрос сервера после ошибки
const uint32_t SEND_RETRY_MS         = 10UL * 1000;  // повтор неотправленного нажатия
const uint32_t CLOCK_INTERVAL_MS     = 60UL * 1000;  // проверка ночного режима
const uint32_t WIFI_BOOT_TIMEOUT_MS  = 15UL * 1000;  // ожидание WiFi при старте
const uint32_t WIFI_RETRY_MS         = 30UL * 1000;  // перезапуск подключения WiFi
const uint32_t DNS_TIMEOUT_MS        = 3000;
const uint32_t HTTP_CONNECT_TIMEOUT_MS  = 3000;
const uint32_t HTTP_RESPONSE_TIMEOUT_MS = 5000;
const uint32_t MESSAGE_MS            = 3000;  // сколько показывается сообщение
const uint32_t BTN_STABLE_MS         = 50;    // кнопка должна быть отпущена >= N мс
const uint32_t BTN_MIN_INTERVAL_MS   = 300;   // минимум между двумя нажатиями

// Ночной режим
const int      NIGHT_START_HOUR = 22;  // с 22:00
const int      NIGHT_END_HOUR   = 7;   // до 07:00
const long     TZ_OFFSET_SEC    = 4L * 3600;  // UTC+4 (Самара)
const char*    NTP_SERVER       = "pool.ntp.org";
const uint32_t NTP_UPDATE_MS    = 10UL * 60 * 1000;

const size_t   MAX_RESPONSE_LEN = 1024;
const uint32_t SERIAL_BAUD      = 9600;

// ============================================================================
//  ТИПЫ
// ============================================================================
enum NetError {
  NET_NONE = 0,
  NET_WIFI_NO_SSID,    // сеть с таким именем не найдена
  NET_WIFI_BAD_PASS,   // неверный пароль
  NET_WIFI,            // нет подключения к WiFi (прочие причины)
  NET_DNS,             // не удалось получить IP сервера
  NET_CONNECT,         // сервер не принимает TCP-соединение
  NET_SEND,            // запрос не отправился
  NET_TIMEOUT,         // сервер не ответил
  NET_HTTP,            // сервер ответил кодом != 2xx
  NET_BAD_RESPONSE     // ответ не удалось разобрать
};

struct HttpResult {
  NetError    err;
  int         code;   // HTTP-код (если получен)
  String      body;   // тело ответа
  const char* path;   // к какому адресу был запрос
};

// ============================================================================
//  ГЛОБАЛЬНОЕ СОСТОЯНИЕ
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, 2);
WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, TZ_OFFSET_SEC, NTP_UPDATE_MS);

bool opened      = false;  // текущее состояние (то, что показывают светодиоды)
bool stateKnown  = false;  // состояние хотя бы раз подтверждено сервером
bool pendingSend = false;  // локальное изменение ещё не принято сервером
bool firstSendTry = false; // первая попытка отправки после нажатия
bool timeSynced  = false;  // время получено по NTP хотя бы раз

NetError lastError    = NET_NONE;
int      lastHttpCode = 0;

uint32_t nextPollMs  = 0;
uint32_t nextSendMs  = 0;
uint32_t nextClockMs = 0;

bool     wifiWasConnected = false;
uint32_t wifiBeginMs      = 0;

// Сообщение поверх экрана состояния
bool     messageActive  = false;
uint32_t messageStartMs = 0;
uint32_t messageDurMs   = 0;

// Кэш строк LCD — перерисовываем только изменившееся (без мерцания)
String lcdCache[2];

// Кнопка (меняется в прерывании)
volatile bool     buttonFlag      = false;
volatile uint32_t btnLastEdgeMs   = 0;
volatile uint32_t btnLastAcceptMs = 0;

// ============================================================================
//  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================================

// Наступил ли момент t (корректно при переполнении millis()).
bool isDue(uint32_t t) {
  return (int32_t)(millis() - t) >= 0;
}

void logLine(const String& s) {
  Serial.print('[');
  Serial.print(millis() / 1000.0, 1);
  Serial.print("] ");
  Serial.println(s);
}

// Обрезает/дополняет строку пробелами до ширины дисплея.
String fit(String s) {
  if (s.length() > LCD_COLS) s = s.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  return s;
}

// Центрирует строку на дисплее.
String center(const String& s) {
  if (s.length() >= LCD_COLS) return fit(s);
  String out;
  for (unsigned i = 0; i < (LCD_COLS - s.length()) / 2; i++) out += ' ';
  out += s;
  return fit(out);
}

// Строка вида "Robotic   noWiFi": слева текст, справа метка.
String leftRight(const String& left, const String& right) {
  String s = left;
  int spaces = (int)LCD_COLS - (int)left.length() - (int)right.length();
  if (spaces < 1) spaces = 1;
  for (int i = 0; i < spaces; i++) s += ' ';
  s += right;
  return fit(s);
}

// ============================================================================
//  ДИСПЛЕЙ
// ============================================================================
void lcdLine(uint8_t row, const String& text) {
  String t = fit(text);
  if (t == lcdCache[row]) return;
  lcd.setCursor(0, row);
  lcd.print(t);
  lcdCache[row] = t;
}

void lcdShow(const String& l1, const String& l2) {
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// Временное сообщение; по истечении времени экран сам вернётся к состоянию.
void showMessage(const String& l1, const String& l2, uint32_t durationMs = MESSAGE_MS) {
  logLine("LCD: " + l1 + " | " + l2);
  lcdShow(l1, l2);
  messageActive  = true;
  messageStartMs = millis();
  messageDurMs   = durationMs;
}

// Короткая метка проблемы для экрана состояния.
String statusTag() {
  if (pendingSend) return "sync..";
  switch (lastError) {
    case NET_NONE:          return "";
    case NET_WIFI_NO_SSID:
    case NET_WIFI_BAD_PASS:
    case NET_WIFI:          return "noWiFi";
    case NET_DNS:           return "noDNS";
    case NET_CONNECT:
    case NET_SEND:
    case NET_TIMEOUT:       return "noSrv";
    case NET_HTTP:          return "E" + String(lastHttpCode);
    case NET_BAD_RESPONSE:  return "badRsp";
  }
  return "err";
}

void drawStateScreen() {
  String tag = statusTag();
  String l1 = tag.length() ? leftRight("Robotic", tag) : center("Robotic");
  String l2;
  if (!stateKnown && !pendingSend) l2 = center("STATE UNKNOWN");
  else                             l2 = center(opened ? "OPENED" : "CLOSED");
  lcdShow(l1, l2);
}

// Вызывается в каждом проходе loop().
void updateDisplay() {
  if (messageActive) {
    if (millis() - messageStartMs < messageDurMs) return;
    messageActive = false;
  }
  drawStateScreen();
}

// ============================================================================
//  ОШИБКИ
// ============================================================================
void errorText(NetError e, int code, const char* path, String& l1, String& l2) {
  switch (e) {
    case NET_WIFI_NO_SSID:  l1 = "WiFi not found";   l2 = WIFI_SSID;   break;
    case NET_WIFI_BAD_PASS: l1 = "WiFi bad passwd";  l2 = WIFI_SSID;   break;
    case NET_WIFI:          l1 = "No WiFi connect";  l2 = WIFI_SSID;   break;
    case NET_DNS:           l1 = "DNS error";        l2 = SERVER_HOST; break;
    case NET_CONNECT:       l1 = "Server offline";   l2 = SERVER_HOST; break;
    case NET_SEND:          l1 = "Send failed";      l2 = "Check network"; break;
    case NET_TIMEOUT:       l1 = "Server timeout";   l2 = "No response";   break;
    case NET_HTTP:          l1 = "Server error";
                            l2 = "HTTP " + String(code) + " " + (path ? path : ""); break;
    case NET_BAD_RESPONSE:  l1 = "Bad response";     l2 = path ? path : ""; break;
    default:                l1 = "Unknown error";    l2 = "";          break;
  }
}

// Полноэкранное сообщение показывается, если ошибка новая или force=true
// (например, после нажатия кнопки). Повторяющаяся фоновая ошибка видна
// только меткой в первой строке экрана состояния — без мельтешения.
void reportError(NetError e, int code = 0, const char* path = nullptr, bool force = false) {
  bool isNew   = (e != lastError) || (code != lastHttpCode);
  lastError    = e;
  lastHttpCode = code;
  String l1, l2;
  errorText(e, code, path, l1, l2);
  String extra = (e == NET_CONNECT) ? String(" port ") + String(SERVER_PORT) : String();
  logLine("ERROR: " + l1 + " / " + l2 + extra);
  if (isNew || force) showMessage(l1, l2);
}

void clearError() {
  if (lastError != NET_NONE) logLine("Connection restored");
  lastError    = NET_NONE;
  lastHttpCode = 0;
}

// ============================================================================
//  СВЕТОДИОДЫ / СОСТОЯНИЕ
// ============================================================================
void setLeds() {
  if (!stateKnown && !pendingSend) {  // состояние неизвестно — оба выключены
    digitalWrite(PIN_LED_OPEN, LOW);
    digitalWrite(PIN_LED_CLOSE, LOW);
    return;
  }
  digitalWrite(PIN_LED_OPEN,  opened ? HIGH : LOW);
  digitalWrite(PIN_LED_CLOSE, opened ? LOW  : HIGH);
}

// Применить новое состояние. local=true — изменение сделано здесь
// (кнопка / ночной режим) и его нужно отправить на сервер.
void applyState(bool newState, bool local) {
  opened = newState;
  if (local) {
    pendingSend  = true;
    firstSendTry = true;
    nextSendMs   = millis();  // отправить как можно скорее
  } else {
    stateKnown = true;
  }
  logLine(String("State: ") + (opened ? "OPEN" : "CLOSED") +
          (local ? " (local, will send)" : " (from server)"));
  setLeds();
  messageActive = false;  // состояние важнее старого сообщения
  drawStateScreen();
}

// ============================================================================
//  КНОПКА
// ============================================================================
// Прерывание по любому фронту. Нажатие засчитывается, если пин стал LOW,
// а до этого кнопка была стабильно отпущена не меньше BTN_STABLE_MS.
// Это отсекает дребезг и при нажатии, и при отпускании.
IRAM_ATTR void onButtonEdge() {
  uint32_t now = millis();
  bool isLow = digitalRead(PIN_BUTTON) == LOW;
  if (isLow &&
      (now - btnLastEdgeMs) >= BTN_STABLE_MS &&
      (now - btnLastAcceptMs) >= BTN_MIN_INTERVAL_MS) {
    buttonFlag      = true;
    btnLastAcceptMs = now;
  }
  btnLastEdgeMs = now;
}

void handleButton() {
  if (!buttonFlag) return;
  buttonFlag = false;  // bool на ESP8266 пишется атомарно
  logLine("Button pressed");
  applyState(!opened, true);
}

// ============================================================================
//  WiFi
// ============================================================================
NetError wifiError() {
  int st = (int)WiFi.status();
  if (st == WL_NO_SSID_AVAIL) return NET_WIFI_NO_SSID;
  if (st == 6) return NET_WIFI_BAD_PASS;  // WL_WRONG_PASSWORD (ядро ESP8266 >= 3.0)
  return NET_WIFI;
}

void wifiBegin() {
  logLine(String("WiFi: connecting to ") + WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiBeginMs = millis();
}

void onWifiConnected() {
  wifiWasConnected = true;
  String ip = WiFi.localIP().toString();
  logLine("WiFi connected, IP " + ip + ", RSSI " + String(WiFi.RSSI()) + " dBm");
  if (lastError == NET_WIFI || lastError == NET_WIFI_NO_SSID || lastError == NET_WIFI_BAD_PASS) {
    clearError();
  }
  showMessage(center("WiFi connected"), center(ip), 2000);
  nextPollMs = millis();  // сразу синхронизироваться с сервером
}

// Неблокирующее обслуживание WiFi, вызывается в loop().
void wifiMaintain() {
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wifiWasConnected) {
    onWifiConnected();
  } else if (!connected && wifiWasConnected) {
    wifiWasConnected = false;
    wifiBeginMs = millis();  // даём автопереподключению время
    reportError(wifiError());
  } else if (!connected && millis() - wifiBeginMs > WIFI_RETRY_MS) {
    NetError e = wifiError();
    if (e != lastError) reportError(e);
    wifiBegin();
  }
}

// ============================================================================
//  HTTP
// ============================================================================
HttpResult httpRequest(const char* method, const char* path, const String& body) {
  HttpResult r;
  r.err  = NET_NONE;
  r.code = 0;
  r.path = path;

  if (WiFi.status() != WL_CONNECTED) { r.err = wifiError(); return r; }

  // IP сервера: если задан IP — используем его, иначе DNS
  IPAddress ip;
  if (!ip.fromString(SERVER_HOST)) {
    if (!WiFi.hostByName(SERVER_HOST, ip, DNS_TIMEOUT_MS)) { r.err = NET_DNS; return r; }
  }

  WiFiClient client;
  client.setTimeout(HTTP_CONNECT_TIMEOUT_MS);
  if (!client.connect(ip, SERVER_PORT)) { r.err = NET_CONNECT; return r; }

  String req;
  req.reserve(256 + body.length());
  req += method; req += ' '; req += path; req += " HTTP/1.1\r\n";
  req += "Host: "; req += SERVER_HOST;
  if (SERVER_PORT != 80) { req += ':'; req += String(SERVER_PORT); }
  req += "\r\n";
  req += "User-Agent: RoboticButton/" FW_VERSION "\r\n";
  req += "Accept: application/json\r\n";
  req += "Connection: close\r\n";
  if (body.length()) {
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: "; req += String(body.length()); req += "\r\n";
  }
  req += "\r\n";
  req += body;

  logLine(String(method) + " " + path + (body.length() ? " " + body : ""));

  if (client.print(req) != req.length()) {
    client.stop();
    r.err = NET_SEND;
    return r;
  }

  // Читаем ответ до закрытия соединения сервером или до таймаута
  String raw;
  raw.reserve(MAX_RESPONSE_LEN);
  uint32_t start = millis();
  while (millis() - start < HTTP_RESPONSE_TIMEOUT_MS) {
    while (client.available()) {
      char c = client.read();
      if (raw.length() < MAX_RESPONSE_LEN) raw += c;
    }
    if (!client.connected() && !client.available()) break;
    delay(5);  // отдаём время WiFi-стеку
  }
  client.stop();

  int headerEnd = raw.indexOf("\r\n\r\n");
  if (raw.length() == 0 || headerEnd < 0) {
    r.err = raw.length() ? NET_BAD_RESPONSE : NET_TIMEOUT;
    if (raw.length()) logLine("Raw response: " + raw);
    return r;
  }

  // Статус: "HTTP/1.1 200 OK"
  if (!raw.startsWith("HTTP/")) { r.err = NET_BAD_RESPONSE; return r; }
  int sp = raw.indexOf(' ');
  r.code = (sp > 0) ? raw.substring(sp + 1, sp + 4).toInt() : 0;
  r.body = raw.substring(headerEnd + 4);
  r.body.trim();

  logLine("HTTP " + String(r.code) + " body: " + r.body);

  if (r.code == 0)                     r.err = NET_BAD_RESPONSE;
  else if (r.code < 200 || r.code > 299) r.err = NET_HTTP;
  return r;
}

// Ищет "state": true/false. Возвращает 1, 0 или -1 (не найдено).
int parseState(const String& body) {
  int k = body.indexOf("\"state\"");
  if (k < 0) return -1;
  unsigned i = k + 7;
  while (i < body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
  if (i >= body.length() || body[i] != ':') return -1;
  i++;
  while (i < body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
  if (body.startsWith("true", i))  return 1;
  if (body.startsWith("false", i)) return 0;
  return -1;
}

// ============================================================================
//  СИНХРОНИЗАЦИЯ С СЕРВЕРОМ
// ============================================================================
// Получить состояние с сервера (фоново, без надписей на экране при успехе).
void pollState() {
  HttpResult r = httpRequest("GET", ENDPOINT_STATE, "");
  if (r.err != NET_NONE) { reportError(r.err, r.code, r.path); return; }

  int s = parseState(r.body);
  if (s < 0) { reportError(NET_BAD_RESPONSE, r.code, r.path); return; }

  clearError();
  // Пока шёл запрос, могли нажать кнопку — локальное изменение важнее
  if (buttonFlag || pendingSend) return;

  if (!stateKnown || (s == 1) != opened) applyState(s == 1, false);
}

// Отправить локальное изменение на сервер.
void sendState() {
  bool sent = opened;
  String body = sent ? "{\"state\": true}" : "{\"state\": false}";
  HttpResult r = httpRequest("POST", ENDPOINT_CHANGE, body);

  if (r.err == NET_NONE) {
    if (opened == sent) pendingSend = false;  // за время запроса не нажимали снова
    stateKnown = true;
    clearError();
    logLine("State delivered to server");
    nextPollMs = millis() + POLL_INTERVAL_MS;
    return;
  }

  reportError(r.err, r.code, r.path, firstSendTry);
  firstSendTry = false;

  if (r.err == NET_HTTP) {
    // Сервер отказал явно — не повторяем, берём состояние с сервера
    pendingSend = false;
    nextPollMs  = millis();
  } else {
    // Сетевая ошибка — повторим позже, состояние на кнопке сохраняется
    nextSendMs = millis() + SEND_RETRY_MS;
  }
}

// ============================================================================
//  НОЧНОЙ РЕЖИМ
// ============================================================================
void checkNight() {
  if (!isDue(nextClockMs)) return;
  nextClockMs = millis() + CLOCK_INTERVAL_MS;

  if (WiFi.status() == WL_CONNECTED) {
    // update() возвращает false, если обновлять ещё рано, — это не ошибка
    if (!timeSynced) timeSynced = timeClient.forceUpdate();
    else             timeClient.update();
  }
  if (!timeSynced) { logLine("NTP: time not synced yet"); return; }

  int hour = timeClient.getHours();
  bool night = (hour >= NIGHT_START_HOUR) || (hour < NIGHT_END_HOUR);
  logLine("Time " + timeClient.getFormattedTime() + (night ? " (night)" : ""));

  if (night && opened) {
    logLine("Night mode: auto close");
    applyState(false, true);
    showMessage(center("Night mode"), center("auto CLOSED"));
  }
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  logLine("Robotic Button v" FW_VERSION);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_OPEN, OUTPUT);
  pinMode(PIN_LED_CLOSE, OUTPUT);
  setLeds();  // пока состояние неизвестно — оба выключены

  lcd.init();
  lcd.backlight();
  lcdShow(center("Robotic Button"), center("firmware v" FW_VERSION));
  delay(1500);

  // WiFi: станция, без записи во flash, с автопереподключением
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  lcdShow(center("Connect to WiFi"), center(WIFI_SSID));
  wifiBegin();

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_BOOT_TIMEOUT_MS) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected();
  } else {
    reportError(wifiError());
  }

  timeClient.begin();

  btnLastEdgeMs = millis();
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonEdge, CHANGE);

  nextPollMs  = millis();  // сразу запросить состояние
  nextClockMs = millis() + 5000;
}

void loop() {
  handleButton();
  wifiMaintain();

  if (pendingSend) {
    if (isDue(nextSendMs)) sendState();
  } else if (isDue(nextPollMs)) {
    pollState();
    nextPollMs = millis() + (lastError == NET_NONE ? POLL_INTERVAL_MS : POLL_RETRY_MS);
  }

  handleButton();  // нажатие во время сетевого запроса обрабатываем сразу
  checkNight();
  updateDisplay();
  delay(10);
}
