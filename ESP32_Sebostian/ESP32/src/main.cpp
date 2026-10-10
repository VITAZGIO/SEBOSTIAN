// =====================================================================
//  СЕБАСТЬЯН — стендовый тест, СО-ПРОЦЕССОР (обычная ESP32)
//  v7.2: 74HC4053 (выбор источника звука) — GPIO21: LOW = S3, HIGH = Bluetooth/мелодия со-процессора.
//        Голова шлёт "S3B 1/0" (звучит/молчит). S3 звучит -> всегда S3 (голос важнее музыки).
//        Иначе, если играет Bluetooth или тестовая мелодия -> BT. В "S,..." 9-е поле = mux.
//  v7.1: флаг «аккум сел» хранится во флеше (NVS), а не в RTC — переживает выключение тумблером.
//        Включили тумблером на севшем аккуме -> голове "G,mV", 12 с ждём "FORCE" (кнопка на экране)
//        или касание сенсора 2 -> работаем (для зарядки). Нет -> спать дальше.
//  v7: защита аккума: среднее за 10 с < 3.0 В -> голове "B,2" (до ответа "SLEEP", макс 20 с), глубокий сон.
//      Во сне раз в минуту (или по сенсору 2) меряем аккум: >= 3.55 В (зарядка) -> обычный старт.
//  v6.1: при старте на ленте крутится радуга (проверка прошивки по Wi-Fi).
//  v6: прошивка по Wi-Fi (OTA): команда "OTA" с экрана головы -> BT выкл, Wi-Fi, 5 мин ждёт.
//  v5.3: HOLD = код #1 три раза подряд (как 3 нажатия за секунду).
//  v5: тест радио 433 (RCSwitch): приём 8 триггеров + коды подсветки, передача 21 код + HOLD.
//  v4: радуга крутится постоянно, лента 4 диода.
//  v3: Bluetooth-колонка «Sebastian» (A2DP) -> PCM5102A -> AUX,
//  тестовая мелодия в AUX по кнопке с экрана S3, ARGB, 3 сенсора TTP223,
//  кнопка мика, батарея, UART-связь с S3.  
// =====================================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include "BluetoothA2DPSink.h"
#include <RCSwitch.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "secrets.h"      // WIFI_SSID, WIFI_PASS, OTA_PASS — тот же файл, что у головы
#include <esp_sleep.h>
#include <Preferences.h>
#include <driver/gpio.h>

// ------------------------- ПИНЫ ESP32 --------------------------------
#define PCM_BCK   26     // ВРЕМЕННО: PCM5102A напрямую (потом эти же пины уйдут в 74HC4053)
#define PCM_LCK   25
#define PCM_DIN   22
#define LED_PIN   32     // ARGB -> 470 Ом -> Din ленты
#define LED_COUNT 4      // сколько диодов в ленте
#define TOUCH1    36     // VP  — сенсор 1 (тише)
#define TOUCH2    35     // D35 — сенсор 2 (play/stop)
#define TOUCH3    34     // D34 — сенсор 3 (громче)
#define MIC_BTN   4      // кнопка мика на GND (пока не подключена)
#define BAT_ADC   33     // делитель 1:2 от аккума
#define MUX_SEL   21     // выбор 74HC4053: LOW = играет S3, HIGH = играет Bluetooth (+10 кОм на GND)
#define LINK_RX   16     // RX2 <- TX S3 (GPIO8)
#define LINK_TX   17     // TX2 -> RX S3 (GPIO18)
#define RF_RX     27     // приёмник 433 DATA (если RX питаешь 5V — через делитель 10к/20к!)
#define RF_TX     13     // передатчик 433 DATA

// ------------------------- БАТАРЕЯ: пороги (мВ, на одну банку) --------
#define BAT_CAL_MV    0      // поправка АЦП: (мультиметр на B+) минус (что пишет экран). Пример: 3.92 − 3.87 -> 50
#define BAT_NONE      2500   // ниже = аккума нет (стенд от USB) — защиту НЕ включаем
#define BAT_LOW       3300   // предупреждение: красные вспышки, на экране LOW BAT
#define BAT_OFF       3000   // среднее за 10 с ниже -> всё выключаем и спим (железная защита модуля — 2.5 В)
#define BAT_WAKE      3550   // проснуться только выше этого (после нагрузки аккум «отпрыгивает» до ~3.3 В)
#define SLEEP_CHECK_S 60     // во сне просыпаемся раз в минуту проверить аккум
Preferences prefs;                     // NVS — маленькое хранилище во флеше, переживает выключение питания
bool lowFlagGet() { prefs.begin("seb", true); bool v = prefs.getBool("lowbat", false); prefs.end(); return v; }
void lowFlagSet(bool v) {                // пишем только если изменилось (флеш не любит лишних записей)
  prefs.begin("seb", false);
  if (prefs.getBool("lowbat", !v) != v) prefs.putBool("lowbat", v);
  prefs.end();
}

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
BluetoothA2DPSink a2dp;
RCSwitch rf;

// ------------------------- РАДИО 433: коды -----------------------------
// параметры твоих устройств (из esp32monitoring): протокол 1, импульс 389 мкс, 24 бита, повтор 8
const unsigned long RF_TRIG[8] = {1382424, 1382420, 1382428, 1382418, 1382426, 1382422, 1382423, 1382431};
const unsigned long RF_LIGHT_BASE = 9348096;     // код подсветки #n = 9348096 + n  (n = 1..21)
unsigned long trigLast[8] = {0};
unsigned long lightLastMs = 0, unkLastMs = 0, rxMuteUntil = 0;
unsigned long holdNext = 0;

// ------------------------- СОСТОЯНИЕ ---------------------------------
int t[3] = {0, 0, 0}, tStable[3] = {0, 0, 0}, tCnt[3] = {0, 0, 0};
const int tPins[3] = {TOUCH1, TOUCH2, TOUCH3};
int micBtn = 0;
int batmV = 0;
int ledMode = 0;
unsigned long lastPing = 0, pings = 0;

// =====================================================================
//  Bluetooth A2DP: телефон/комп видит колонку «Sebastian», звук -> I2S0 -> PCM5102A
//  (I2S-драйвер ставит сама библиотека; наши тоны пишем в тот же I2S0)
// =====================================================================
void startBT() {
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PCM_BCK;
  pins.ws_io_num = PCM_LCK;
  pins.data_out_num = PCM_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  a2dp.set_pin_config(pins);
  a2dp.start("Sebastian");
}

bool btConnected() { return a2dp.is_connected(); }
bool btPlaying()   { return a2dp.is_connected() && a2dp.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED; }

void toneI2S(float freq, int ms, int amp = 7000) {
  const int SR = 44100;
  const int total = SR * ms / 1000;
  const int fade = SR / 200;              // 5 мс плавного входа/выхода — без щелчков
  static int16_t buf[256 * 2];
  float ph = 0, inc = 2.0f * PI * freq / SR;
  int done = 0;
  while (done < total) {
    int n = min(256, total - done);
    for (int i = 0; i < n; i++) {
      int k = done + i;
      float env = 1.0f;
      if (k < fade) env = (float)k / fade;
      else if (k > total - fade) env = (float)(total - k) / fade;
      int16_t v = freq > 0 ? (int16_t)(amp * env * sinf(ph)) : 0;
      ph += inc; if (ph > 2 * PI) ph -= 2 * PI;
      buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    size_t bw;
    i2s_write(I2S_NUM_0, buf, n * 4, &bw, portMAX_DELAY);
    done += n;
  }
}

int muxState = -1;                        // текущее положение 74HC4053: 0 = S3, 1 = BT
void beepMelody() {
  Serial.println("[AUX] мелодия со-процессора");
  digitalWrite(MUX_SEL, HIGH); muxState = -1;      // на время мелодии 4053 -> со-процессор, потом решит muxTick
  const float notes[] = {523.3f, 659.3f, 784.0f, 1046.5f};
  for (float f : notes) toneI2S(f, 140);
  toneI2S(0, 30);
}

// ---- тестовая музыка в AUX (не блокирует: играет кусочками в loop) ----
// «Ода к радости» (Бетховен), по кругу
struct Note { float f; int ms; };
#define E4 329.6f
#define F4 349.2f
#define G4 392.0f
#define C4 261.6f
#define D4 293.7f
#define G3 196.0f
const Note SONG[] = {
  {E4,400},{E4,400},{F4,400},{G4,400},{G4,400},{F4,400},{E4,400},{D4,400},
  {C4,400},{C4,400},{D4,400},{E4,400},{E4,600},{D4,200},{D4,800},
  {E4,400},{E4,400},{F4,400},{G4,400},{G4,400},{F4,400},{E4,400},{D4,400},
  {C4,400},{C4,400},{D4,400},{E4,400},{D4,600},{C4,200},{C4,800},
  {D4,400},{D4,400},{E4,400},{C4,400},{D4,400},{E4,200},{F4,200},{E4,400},{C4,400},
  {D4,400},{E4,200},{F4,200},{E4,400},{D4,400},{C4,400},{D4,400},{G3,800},{0,400}};
const int SONG_LEN = sizeof(SONG) / sizeof(SONG[0]);

bool musicOn = false;
int noteIdx = 0, noteDone = 0;
float mph = 0;

void musicToggle() {
  if (btPlaying()) { Serial.println("[AUX] сейчас играет Bluetooth — мелодию не включаю"); return; }
  musicOn = !musicOn;
  noteIdx = 0; noteDone = 0; mph = 0;
  if (!musicOn) i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.printf("[AUX] тестовая музыка: %s\n", musicOn ? "ВКЛ" : "выкл");
}

void musicTick() {
  if (!musicOn) return;
  if (btPlaying()) { musicOn = false; Serial.println("[AUX] пошёл Bluetooth — мелодия выкл"); return; }
  const int SR = 44100;
  static int16_t buf[256 * 2];
  for (int i = 0; i < 256; i++) {
    const Note &n = SONG[noteIdx];
    int total = SR * n.ms / 1000;
    int k = noteDone;
    float env = 1.0f;
    int att = SR / 100, rel = SR / 25;                 // 10 мс вход, 40 мс затухание
    if (k < att) env = (float)k / att;
    else if (k > total - rel) env = (float)(total - k) / rel;
    if (env < 0) env = 0;
    float v = 0;
    if (n.f > 0) {
      v = sinf(mph) + 0.35f * sinf(2 * mph) + 0.15f * sinf(3 * mph);   // чуть «органа» вместо голого писка
      mph += 2.0f * PI * n.f / SR;
      if (mph > 2 * PI) mph -= 2 * PI;
    }
    int16_t sm = (int16_t)(6000 * env * v);
    buf[2 * i] = sm; buf[2 * i + 1] = sm;
    if (++noteDone >= total) { noteDone = 0; mph = 0; noteIdx = (noteIdx + 1) % SONG_LEN; }
  }
  size_t bw;
  i2s_write(I2S_NUM_0, buf, sizeof(buf), &bw, portMAX_DELAY);
}

// =====================================================================
//  74HC4053: кто играет в динамики и AUX
// =====================================================================
bool s3Busy = false;
unsigned long s3BusyMs = 0;                       // когда голова последний раз прислала S3B

void muxTick() {
  bool s3 = s3Busy && millis() - s3BusyMs < 3000; // голова молчит > 3 с (перезагрузка?) — не верим старому «звучит»
  bool cp = btPlaying() || musicOn;               // со-процессору есть что играть
  int want = (cp && !s3) ? 1 : 0;                 // голос/звук головы важнее музыки
  if (want == muxState) return;
  muxState = want;
  digitalWrite(MUX_SEL, want ? HIGH : LOW);
  Serial.printf("[MUX] 74HC4053 -> %s\n", want ? "BLUETOOTH" : "S3");
}

// =====================================================================
//  ARGB
// =====================================================================
void ledFill(uint32_t c) { strip.fill(c); strip.show(); }

void ledRainbowSweep() {
  for (int j = 0; j < 256; j += 4) {
    for (int i = 0; i < LED_COUNT; i++)
      strip.setPixelColor(i, strip.gamma32(strip.ColorHSV((i * 65536L / LED_COUNT) + j * 256)));
    strip.show();
    delay(8);
  }
}

void ledNext() {
  ledMode = (ledMode + 1) % 6;
  const char *names[] = {"OFF", "RED", "GREEN", "BLUE", "WHITE", "RAINBOW"};
  Serial.printf("[LED] режим %s\n", names[ledMode]);
  switch (ledMode) {
    case 0: ledFill(0); break;
    case 1: ledFill(strip.Color(255, 0, 0)); break;
    case 2: ledFill(strip.Color(0, 255, 0)); break;
    case 3: ledFill(strip.Color(0, 0, 255)); break;
    case 4: ledFill(strip.Color(255, 255, 255)); break;
    case 5: break;                                        // радуга крутится в ledTick()
  }
}

// Радуга без блокировки: каждые 20 мс сдвигаем оттенок, крутится бесконечно
uint16_t rainbowHue = 0;
unsigned long tRainbow = 0;
void ledTick() {
  if (ledMode != 5 || millis() - tRainbow < 20) return;
  tRainbow = millis();
  rainbowHue += 512;                                      // 65536/512 = 128 шагов ≈ 2.5 с на круг
  for (int i = 0; i < LED_COUNT; i++)
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(rainbowHue + i * 65536L / LED_COUNT)));
  strip.show();
}

// =====================================================================
//  СЕНСОРЫ, КНОПКА, БАТАРЕЯ
// =====================================================================
void pollInputs() {
  static unsigned long tIn = 0;
  if (millis() - tIn < 20) return;
  tIn = millis();
  for (int i = 0; i < 3; i++) {
    int r = digitalRead(tPins[i]);                 // TTP223: HIGH = касание
    if (r != tStable[i]) {
      if (++tCnt[i] >= 2) {                        // 2 одинаковых чтения подряд = стабильно
        tStable[i] = r; tCnt[i] = 0;
        t[i] = r;
        Serial.printf("[TOUCH] сенсор %d: %s\n", i + 1, r ? "НАЖАТ" : "отпущен");
        if (r) {
          Serial2.printf("T,%d\n", i + 1);         // событие для S3
          const uint32_t cols[3] = {strip.Color(255, 0, 0), strip.Color(0, 255, 0), strip.Color(0, 0, 255)};
          ledFill(cols[i]);                         // сразу видно на ленте
        }
      }
    } else tCnt[i] = 0;
  }
  int mb = digitalRead(MIC_BTN) == LOW;
  if (mb != micBtn) { micBtn = mb; Serial.printf("[BTN] кнопка мика: %s\n", mb ? "НАЖАТА" : "отпущена"); }
}

int readBatNow() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_ADC);
  int v = (int)(sum / 16) * 2;                     // делитель 1:2
  return v < BAT_NONE ? v : v + BAT_CAL_MV;
}

// Среднее за 10 последних замеров (раз в секунду) — короткие просадки на басах не считаются
int batRing[10], batN = 0, batI = 0;
void readBattery() {
  batRing[batI] = readBatNow();
  batI = (batI + 1) % 10;
  if (batN < 10) batN++;
  long s = 0;
  for (int i = 0; i < batN; i++) s += batRing[i];
  batmV = s / batN;
}

// =====================================================================
//  UART К S3
// =====================================================================
char lb[64];
int ll = 0;

// =====================================================================
//  РАДИО 433
// =====================================================================
void rfSend(unsigned long code) {
  rf.disableReceive();                            // чтобы не поймать самих себя
  rf.send(code, 24);
  rf.enableReceive(RF_RX);
  rxMuteUntil = millis() + 300;
}

void rfSendN(int n) {                             // код подсветки #n (1..21)
  if (n < 1 || n > 21) return;
  unsigned long code = RF_LIGHT_BASE + n;
  strip.fill(strip.Color(255, 120, 0)); strip.show();   // оранжевая вспышка = передаю
  rfSend(code);
  strip.fill(0); strip.show();
  Serial.printf("[RF] TX #%d код %lu\n", n, code);
  Serial2.printf("X,%d,%lu\n", n, code);
}

// «HOLD» = код #1 три раза подряд (так устройство его и понимает — проверено тыком:
// 3 нажатия за секунду срабатывают, а длинная непрерывная пачка — нет)
int holdLeft = 0;
void rfHoldStart() {
  holdLeft = 3;
  holdNext = 0;
  Serial.println("[RF] HOLD: код #1 x3");
}

void rfHoldTick() {
  if (!holdLeft) return;
  if (millis() < holdNext) return;
  rfSend(RF_LIGHT_BASE + 1);                      // сама отправка ~0.4 с (8 повторов)
  holdLeft--;
  holdNext = millis() + 100;                      // короткая пауза между «нажатиями»
  if (!holdLeft) {
    Serial2.printf("X,22,%lu\n", RF_LIGHT_BASE + 1);
    Serial.println("[RF] HOLD готово");
  }
}

void rfPoll() {
  if (!rf.available()) return;
  unsigned long v = rf.getReceivedValue();
  int bits = rf.getReceivedBitlength();
  int proto = rf.getReceivedProtocol();
  int pulse = rf.getReceivedDelay();
  rf.resetAvailable();
  if (millis() < rxMuteUntil) return;
  // монитор видит ВСЁ, что ловит приёмник — удобно понять, работает ли он вообще
  Serial.printf("[RF] RX %lu  бит:%d прот:%d импульс:%dмкс\n", v, bits, proto, pulse);
  if (bits != 24 || proto != 1) return;           // фильтр: 433-приёмник ловит много мусора
  for (int i = 0; i < 8; i++) {
    if (v == RF_TRIG[i]) {
      if (millis() - trigLast[i] < 1000) return;  // антидребезг: пульт шлёт код пачкой
      trigLast[i] = millis();
      Serial.printf("[RF] >>> ТРИГГЕР %d <<<\n", i + 1);
      Serial2.printf("R,%d,%lu\n", i + 1, v);
      strip.fill(strip.Color(0, 255, 0)); strip.show(); delay(80); strip.fill(0); strip.show();
      return;
    }
  }
  if (v > RF_LIGHT_BASE && v <= RF_LIGHT_BASE + 21) {
    if (millis() - lightLastMs < 300) return;
    lightLastMs = millis();
    Serial2.printf("L,%d,%lu\n", (int)(v - RF_LIGHT_BASE), v);
    return;
  }
  if (millis() - unkLastMs < 500) return;
  unkLastMs = millis();
  Serial2.printf("U,%lu\n", v);
}

// =====================================================================
//  ЗАЩИТА АККУМА: программное отключение на 3.0 В
// =====================================================================
bool linkStarted = false, rfStarted = false;

void holdLow(int pin) {                           // пин держит LOW и во сне (иначе висит в воздухе)
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  gpio_hold_en((gpio_num_t)pin);
}

void goSleep(bool announce) {
  if (announce) {
    Serial.printf("[BAT] аккум сел (%.2f В в среднем) — голове команда спать, потом сплю сам\n", batmV / 1000.0f);
    musicOn = false;
    holdLeft = 0;
    strip.fill(strip.Color(255, 0, 0)); strip.show();
    // шлём голове "B,2" каждые 0.3 с, пока не ответит "SLEEP" (голова могла быть занята разговором), макс 20 с
    char rb[16]; int rn = 0; bool ack = false;
    unsigned long t0 = millis();
    while (!ack && millis() - t0 < 20000) {
      Serial2.printf("B,2,%d\n", batmV);
      unsigned long t1 = millis();
      while (!ack && millis() - t1 < 300) {
        while (Serial2.available()) {
          char c = Serial2.read();
          if (c == '\n') { rb[rn] = 0; if (!strncmp(rb, "SLEEP", 5)) ack = true; rn = 0; }
          else if (c != '\r' && rn < 15) rb[rn++] = c;
        }
        delay(5);
      }
    }
    Serial.println(ack ? "[BAT] голова ответила SLEEP" : "[BAT] голова молчит — сплю сам");
    delay(300);
  }
  if (rfStarted) rf.disableReceive();             // приёмник включён только в обычном режиме
  strip.begin();
  strip.fill(0); strip.show();
  if (linkStarted) { Serial2.flush(); Serial2.end(); }
  pinMode(LINK_TX, INPUT_PULLUP);                 // TX отпускаем: у головы там подтяжка, лишний раз не будим
  holdLow(RF_TX);                                  // передатчик 433 молчит
  holdLow(LED_PIN);                                // лента не ловит мусор
  gpio_deep_sleep_hold_en();
  lowFlagSet(true);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_CHECK_S * 1000000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH2, 1);   // сенсор 2 (play) — проверить сразу
  Serial.println("[BAT] сплю. Просыпаюсь раз в минуту или по сенсору 2.");
  Serial.flush();
  esp_deep_sleep_start();
}

// Включили тумблером на севшем аккуме: 12 с шлём голове "G,mV" и ждём FORCE / SLEEP / сенсор 2
void graceWindow(int v) {
  Serial2.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  linkStarted = true;
  strip.begin(); strip.setBrightness(40);
  Serial.println("[BAT] включили на севшем аккуме: жду кнопку на экране или сенсор 2 (до 12 с)");
  char rb[16]; int rn = 0, res = 0;                 // res: 1 = работаем, 2 = спать
  bool blink = false;
  unsigned long t0 = millis(), tG = 0;
  while (!res && millis() - t0 < 12000) {
    if (millis() - tG > 300) {
      tG = millis();
      Serial2.printf("G,%d\n", v);
      blink = !blink;
      strip.fill(blink ? strip.Color(255, 90, 0) : 0); strip.show();   // оранжевое мигание = ждём решения
    }
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '\n') {
        rb[rn] = 0;
        if (!strncmp(rb, "FORCE", 5)) res = 1;
        else if (!strncmp(rb, "SLEEP", 5)) res = 2;
        rn = 0;
      } else if (c != '\r' && rn < 15) rb[rn++] = c;
    }
    if (digitalRead(TOUCH2) == HIGH) { res = 1; Serial2.print("F\n"); }
    delay(5);
  }
  strip.fill(0); strip.show();
  if (res == 1) {
    lowFlagSet(false);
    Serial.println("[BAT] FORCE — работаем (выключусь, если среднее < 3.0 В)");
    return;
  }
  Serial.println(res == 2 ? "[BAT] голова: спать" : "[BAT] никто не нажал — сплю");
  goSleep(false);
}

// Самое начало setup(): если спали из-за аккума — решаем, просыпаться или спать дальше
void bootGate() {
  gpio_deep_sleep_hold_dis();                      // отпускаем пины, которые держали во сне
  gpio_hold_dis((gpio_num_t)RF_TX);
  gpio_hold_dis((gpio_num_t)LED_PIN);
  esp_sleep_wakeup_cause_t w = esp_sleep_get_wakeup_cause();
  if (!lowFlagGet()) return;                        // аккум в порядке — обычный старт
  analogSetPinAttenuation(BAT_ADC, ADC_11db);
  pinMode(TOUCH2, INPUT);
  int v = readBatNow();
  bool fromSleep = (w == ESP_SLEEP_WAKEUP_TIMER || w == ESP_SLEEP_WAKEUP_EXT0);
  Serial.printf("[BAT] старт с флагом «сел» (%s): %.2f В\n",
                w == ESP_SLEEP_WAKEUP_EXT0 ? "сенсор 2" : w == ESP_SLEEP_WAKEUP_TIMER ? "таймер" : "включили питание", v / 1000.0f);
  if (v >= BAT_WAKE || v < BAT_NONE) {             // зарядился (или аккум отключён, питание от USB)
    lowFlagSet(false);
    Serial.println("[BAT] заряжается — обычный старт, будим голову");
    return;
  }
  if (!fromSleep) { graceWindow(v); return; }       // включили тумблером — окно на кнопку
  if (w == ESP_SLEEP_WAKEUP_EXT0) {                // тронули сенсор — показать, что сел
    strip.begin(); strip.setBrightness(40);
    for (int i = 0; i < 3; i++) { strip.fill(strip.Color(255, 0, 0)); strip.show(); delay(150); strip.fill(0); strip.show(); delay(150); }
    unsigned long t0 = millis();
    while (digitalRead(TOUCH2) == HIGH && millis() - t0 < 3000) delay(20);   // ждём, пока отпустят
  }
  goSleep(false);
}

void batTick() {
  if (batN < 10 || millis() < 15000) return;       // нужно 10 с замеров после старта
  if (batmV < BAT_NONE) return;                     // аккума нет (стенд от USB)
  if (batmV < BAT_OFF) goSleep(true);
  static unsigned long tWarn = 0;
  if (batmV < BAT_LOW && millis() - tWarn > 30000) {
    tWarn = millis();
    Serial.printf("[BAT] мало заряда: %.2f В — поставь на зарядку\n", batmV / 1000.0f);
    for (int i = 0; i < 2; i++) { strip.fill(strip.Color(255, 0, 0)); strip.show(); delay(120); strip.fill(0); strip.show(); delay(120); }
  }
}

// =====================================================================
//  OTA: прошивка по Wi-Fi. Bluetooth и Wi-Fi на одном радио мешают друг другу,
//  поэтому на время прошивки BT выключаем, а потом плата перезагружается.
// =====================================================================
bool otaMode = false;
int otaState = 0;                 // 1 подключаюсь, 2 жду, 3 прошиваюсь, 4 готово, 9 ошибка
int otaPct = 0;
unsigned long otaStartMs = 0, otaStatMs = 0, otaErrMs = 0;
String otaIp = "-";
const unsigned long OTA_TIMEOUT = 5UL * 60UL * 1000UL;   // 5 минут ждём, потом назад в обычный режим

void otaReport() { Serial2.printf("W,%d,%d,%s\n", otaState, otaPct, otaIp.c_str()); }

void otaStart() {
  if (otaMode) return;
  otaMode = true;
  otaState = 1; otaPct = 0; otaStartMs = millis();
  Serial.println("[OTA] режим прошивки: выключаю Bluetooth, подключаюсь к Wi-Fi...");
  otaReport();
  musicOn = false;
  holdLeft = 0;
  rf.disableReceive();
  strip.fill(strip.Color(120, 0, 160)); strip.show();   // фиолетовая = режим прошивки
  a2dp.end(true);                                       // BT выкл + освободить память под Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("sebastian-coproc");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void otaTick() {
  if (otaState == 1) {
    if (WiFi.status() == WL_CONNECTED) {
      otaIp = WiFi.localIP().toString();
      ArduinoOTA.setHostname("sebastian-coproc");
      ArduinoOTA.setPassword(OTA_PASS);
      ArduinoOTA.onStart([]() { otaState = 3; otaPct = 0; otaReport(); Serial.println("[OTA] приём прошивки..."); });
      ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        int p = total ? done * 100 / total : 0;
        if (p != otaPct) { otaPct = p; if (p % 5 == 0) otaReport(); }
      });
      ArduinoOTA.onEnd([]() { otaState = 4; otaReport(); Serial2.flush(); Serial.println("[OTA] готово, перезагрузка"); });
      ArduinoOTA.onError([](ota_error_t e) { otaState = 9; otaErrMs = millis(); otaReport(); Serial.printf("[OTA] ошибка %u\n", e); });
      ArduinoOTA.begin();
      otaState = 2;
      Serial.printf("[OTA] жду прошивку: IP %s (sebastian-coproc.local), 5 минут\n", otaIp.c_str());
    } else if (millis() - otaStartMs > 20000) {
      otaState = 9; otaErrMs = millis();
      Serial.println("[OTA] Wi-Fi не подключился (SSID/пароль в secrets.h?)");
    }
  }
  if (otaState == 2 || otaState == 3) ArduinoOTA.handle();
  if (millis() - otaStatMs > 500) { otaStatMs = millis(); otaReport(); }
  // выход: ошибка -> через 3 с, никто не прошил за 5 мин -> перезагрузка в обычный режим с BT
  if ((otaState == 9 && millis() - otaErrMs > 3000) || (otaState == 2 && millis() - otaStartMs > OTA_TIMEOUT)) {
    Serial.println("[OTA] выхожу из режима прошивки — перезагрузка");
    otaState = 0; otaReport(); Serial2.flush();
    delay(200);
    ESP.restart();
  }
}

void handleCmd(char *s) {
  if (!strncmp(s, "PING", 4)) {                  // PING n -> отвечаем P,n
    lastPing = millis(); pings++;
    Serial2.printf("P,%s\n", s[4] ? s + 5 : "0");
    return;
  }
  if (!strncmp(s, "S3B ", 4)) {                   // S3B 1/0 — голова звучит/молчит (шлёт часто, без лога)
    s3Busy = atoi(s + 4) != 0; s3BusyMs = millis();
    return;
  }
  if (otaMode) return;                            // в режиме прошивки остальные команды не выполняем
  Serial.printf("[LINK] команда от S3: %s\n", s);
  if (!strcmp(s, "BEEP")) beepMelody();
  else if (!strcmp(s, "MUSIC")) musicToggle();
  else if (!strcmp(s, "LED")) ledNext();
  else if (!strncmp(s, "RF ", 3)) rfSendN(atoi(s + 3));
  else if (!strcmp(s, "RFHOLD")) rfHoldStart();
  else if (!strcmp(s, "OTA")) otaStart();
}

void pollLink() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') { lb[ll] = 0; if (ll) handleCmd(lb); ll = 0; }
    else if (c != '\r' && ll < (int)sizeof(lb) - 1) lb[ll++] = c;
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n===== SEBASTIAN BENCH v7.2: CO-PROCESSOR (ESP32) =====");
  bootGate();                                             // спали из-за аккума? может, спим дальше

  if (!linkStarted) Serial2.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  linkStarted = true;

  for (int i = 0; i < 3; i++) pinMode(tPins[i], INPUT);   // 34/35/36 — только вход, без подтяжек
  pinMode(MIC_BTN, INPUT_PULLUP);
  pinMode(MUX_SEL, OUTPUT);
  digitalWrite(MUX_SEL, LOW);                             // 4053: по умолчанию играет S3
  muxState = 0;
  analogSetPinAttenuation(BAT_ADC, ADC_11db);

  strip.begin();
  strip.setBrightness(40);                                // ~15% — не жжём глаза и БП
  ledMode = 5;                                            // при старте — радуга (крутится в ledTick)

  rf.enableReceive(RF_RX);                        // на ESP32 номер прерывания = номер пина
  rfStarted = true;
  rf.enableTransmit(RF_TX);
  rf.setProtocol(1);
  rf.setPulseLength(389);                          // ПОСЛЕ setProtocol (он сбрасывает длину)
  rf.setRepeatTransmit(8);
  Serial.printf("[RF] радио 433: RX=%d TX=%d, прот 1, 389 мкс, 24 бита\n", RF_RX, RF_TX);

  startBT();
  Serial.printf("[BT] Bluetooth-колонка \"Sebastian\" запущена, I2S -> PCM5102A (BCK %d, LCK %d, DIN %d)\n",
                PCM_BCK, PCM_LCK, PCM_DIN);
  delay(300);
  beepMelody();                                           // при старте AUX должен пискнуть

  readBattery();
  Serial.printf("[BAT] %.2f V\n", batmV / 1000.0f);
  Serial2.println("HELLO coproc");
  Serial.println("[SYS] готово. Трогай сенсоры, жми кнопки на экране S3.");
}

unsigned long tStat = 0, tBat = 0, tLog = 0;

void loop() {
  if (otaMode) { pollLink(); otaTick(); return; }   // в режиме прошивки — только связь и OTA
  pollLink();
  pollInputs();
  musicTick();
  ledTick();
  rfPoll();
  rfHoldTick();
  muxTick();

  static int lastBt = -1;
  int btNow = btPlaying() ? 2 : (btConnected() ? 1 : 0);
  if (btNow != lastBt) {
    lastBt = btNow;
    const char *names[] = {"ждёт подключения", "ПОДКЛЮЧЁН", "ИГРАЕТ"};
    Serial.printf("[BT] %s%s%s\n", names[btNow], btNow ? " — " : "", btNow ? a2dp.get_connected_source_name() : "");
    if (btNow == 1) ledFill(strip.Color(0, 0, 255));      // подключился — лента синяя
  }

  if (millis() - tBat > 1000) { tBat = millis(); readBattery(); batTick(); }
  if (millis() - tStat > 200) {
    tStat = millis();
    int bt = btPlaying() ? 2 : (btConnected() ? 1 : 0);
    Serial2.printf("S,%d,%d,%d,%d,%d,%lu,%d,%d,%d\n", t[0], t[1], t[2], micBtn, batmV, millis() / 1000, bt, musicOn ? 1 : 0,
                   muxState == 1 ? 1 : 0);
  }
  if (millis() - tLog > 2000) {
    tLog = millis();
    Serial.printf("[STAT] T:%d%d%d micbtn:%d bat:%.2fV BT:%s music:%d | от S3: %s (PING %lu)\n",
                  t[0], t[1], t[2], micBtn, batmV / 1000.0f,
                  btPlaying() ? "play" : btConnected() ? "conn" : "wait", musicOn,
                  (lastPing && millis() - lastPing < 2500) ? "OK" : "НЕТ (провод S3 8 -> ESP 16?)", pings);
  }
}