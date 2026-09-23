// =====================================================================
//  СЕБАСТЬЯН — стендовый тест, СО-ПРОЦЕССОР (обычная ESP32)
//  v2: PCM5102A -> AUX (мелодия), ARGB через 74AHCT125, 3 сенсора TTP223,
//  кнопку мика, батарею, UART-связь с S3 (отвечает на PING).
//  Радио 433 пока убрано — оттестим отдельно.
// =====================================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>

// ------------------------- ПИНЫ ESP32 --------------------------------
#define PCM_BCK   26     // ВРЕМЕННО: PCM5102A напрямую (потом эти же пины уйдут в 74HC4053)
#define PCM_LCK   25
#define PCM_DIN   22
#define LED_PIN   32     // ARGB -> 470 Ом -> 74AHCT125 -> лента
#define LED_COUNT 30     // сколько диодов в ленте (больше реального — не страшно)
#define TOUCH1    36     // VP  — сенсор 1 (тише)
#define TOUCH2    35     // D35 — сенсор 2 (play/stop)
#define TOUCH3    34     // D34 — сенсор 3 (громче)
#define MIC_BTN   4      // кнопка мика на GND (пока не подключена)
#define BAT_ADC   33     // делитель 1:2 от аккума
#define MUX_SEL   21     // выбор 74HC4053 (пока чипа нет) — LOW = играет S3
#define LINK_RX   16     // RX2 <- TX S3 (GPIO8)
#define LINK_TX   17     // TX2 -> RX S3 (GPIO18)

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ------------------------- СОСТОЯНИЕ ---------------------------------
int t[3] = {0, 0, 0}, tStable[3] = {0, 0, 0}, tCnt[3] = {0, 0, 0};
const int tPins[3] = {TOUCH1, TOUCH2, TOUCH3};
int micBtn = 0;
int batmV = 0;
int ledMode = 0;
unsigned long lastPing = 0, pings = 0;

// =====================================================================
//  PCM5102A (I2S0) — генератор тона
// =====================================================================
bool initPCM() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 44100;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;                    // у обычной ESP32 APLL есть — чище клок для ЦАП
  cfg.tx_desc_auto_clear = true;          // когда молчим — в ЦАП идут нули
  cfg.fixed_mclk = 0;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PCM_BCK;
  pins.ws_io_num = PCM_LCK;
  pins.data_out_num = PCM_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  return i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
}

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

void beepMelody() {
  Serial.println("[AUX] мелодия в PCM5102A -> AUX");
  const float notes[] = {523.3f, 659.3f, 784.0f, 1046.5f};
  for (float f : notes) toneI2S(f, 140);
  toneI2S(0, 30);
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
    case 5: ledRainbowSweep(); break;
  }
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

void readBattery() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_ADC);
  batmV = (int)(sum / 16) * 2;                     // делитель 1:2
}

// =====================================================================
//  UART К S3
// =====================================================================
char lb[64];
int ll = 0;

void handleCmd(char *s) {
  if (!strncmp(s, "PING", 4)) {                  // PING n -> отвечаем P,n
    lastPing = millis(); pings++;
    Serial2.printf("P,%s\n", s[4] ? s + 5 : "0");
    return;
  }
  Serial.printf("[LINK] команда от S3: %s\n", s);
  if (!strcmp(s, "BEEP")) beepMelody();
  else if (!strcmp(s, "LED")) ledNext();
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
  Serial.println("\n\n===== SEBASTIAN BENCH v2: CO-PROCESSOR (ESP32) =====");

  Serial2.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);

  for (int i = 0; i < 3; i++) pinMode(tPins[i], INPUT);   // 34/35/36 — только вход, без подтяжек
  pinMode(MIC_BTN, INPUT_PULLUP);
  pinMode(MUX_SEL, OUTPUT);
  digitalWrite(MUX_SEL, LOW);                             // 4053 (когда будет): играет S3
  analogSetPinAttenuation(BAT_ADC, ADC_11db);

  strip.begin();
  strip.setBrightness(40);                                // ~15% — не жжём глаза и БП
  ledRainbowSweep();
  ledFill(0);

  bool pcm = initPCM();
  Serial.printf("[AUX] I2S PCM5102A (BCK %d, LCK %d, DIN %d): %s\n", PCM_BCK, PCM_LCK, PCM_DIN, pcm ? "OK" : "FAIL");
  delay(500);
  if (pcm) beepMelody();                                  // сразу при старте — AUX должен пискнуть

  readBattery();
  Serial.printf("[BAT] %.2f V\n", batmV / 1000.0f);
  Serial2.println("HELLO coproc");
  Serial.println("[SYS] готово. Трогай сенсоры, жми кнопки на экране S3.");
}

unsigned long tStat = 0, tBat = 0, tLog = 0;

void loop() {
  pollLink();
  pollInputs();

  if (millis() - tBat > 1000) { tBat = millis(); readBattery(); }
  if (millis() - tStat > 200) {
    tStat = millis();
    Serial2.printf("S,%d,%d,%d,%d,%d,%lu\n", t[0], t[1], t[2], micBtn, batmV, millis() / 1000);
  }
  if (millis() - tLog > 2000) {
    tLog = millis();
    Serial.printf("[STAT] T:%d%d%d micbtn:%d bat:%.2fV | от S3: %s (PING %lu)\n",
                  t[0], t[1], t[2], micBtn, batmV / 1000.0f,
                  (lastPing && millis() - lastPing < 2500) ? "OK" : "НЕТ (провод S3 8 -> ESP 16?)", pings);
  }
}