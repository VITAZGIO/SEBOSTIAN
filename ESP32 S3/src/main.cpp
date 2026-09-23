// =====================================================================
//  СЕБАСТЬЯН — стендовый тест, ГОЛОВА (ESP32-S3)  v3
//  Дисплей (перевёрнут на 180°) + тач, SD+MP3 -> MAX98357A (I2S1, как в v1),
//  микрофон INMP441 (I2S0),
//  мьют динамиков (BC547, GPIO2), мьют AUX (XSMT, GPIO39),
//  UART-связь с со-процессором с проверкой ОБОИХ направлений.
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <driver/i2s.h>
#include "AudioOutput.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"

// ------------------------- ПИНЫ S3 -----------------------------------
#define SD_SCK       6
#define SD_MOSI      7
#define SD_MISO      15
#define SD_CS        5
#define SPK_BCLK     40     // I2S на оба MAX98357A
#define SPK_LRC      41
#define SPK_DIN      42
#define MIC_SCK      47     // INMP441
#define MIC_WS       48
#define MIC_SD       38     // на схеме нарисован 37 — он занят PSRAM, реально 38
#define PIN_SPK_MUTE 2      // базы BC547 через 1к: LOW = динамики играют, HIGH = молчат
#define PIN_XSMT     39     // PCM5102A XSMT: HIGH = AUX играет, LOW = тишина
#define T_IRQ        17     // прерывание тача (LOW = касание)
#define LINK_RX      18     // UART к со-процессору: RX S3 <- TX2 (GPIO17) ESP32
#define LINK_TX      8      //                       TX S3 -> RX2 (GPIO16) ESP32

// ------------------------- НАСТРОЙКИ ---------------------------------
static const char *MP3_FILE = "/test.mp3";
static float volume = 0.20f;            // громкость динамиков
static bool loopMp3 = true;             // трек по кругу, пока не нажмёшь STOP
static uint16_t calData[5] = {460, 3320, 340, 3430, 1};   // калибровка тача (снята при rotation 0)

TFT_eSPI tft;
SPIClass sdSPI(HSPI);                   // SD на своей шине (дисплей на FSPI)

// =====================================================================
//  I2S-выход для ESP8266Audio (как в v1, где музыка играла): драйвер на
//  порту 1 ставится один раз и больше не пересоздаётся.
// =====================================================================
class I2SOut : public AudioOutput {
 public:
  explicit I2SOut(i2s_port_t p) : port(p) { hertz = 44100; bps = 16; channels = 2; SetGain(1.0f); }
  bool installed = false;

  bool install() {
    if (installed) return true;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = hertz;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 16;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;
    if (i2s_driver_install(port, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = SPK_BCLK;
    pins.ws_io_num = SPK_LRC;
    pins.data_out_num = SPK_DIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(port, &pins);
    installed = true;
    return true;
  }
  bool SetRate(int hz) override { hertz = hz; if (installed) i2s_set_sample_rates(port, hz); return true; }
  bool begin() override { if (!install()) return false; i2s_set_sample_rates(port, hertz); return true; }
  bool ConsumeSample(int16_t s[2]) override {
    MakeSampleStereo16(s);
    int16_t o[2] = {Amplify(s[0]), Amplify(s[1])};
    size_t bw = 0;
    i2s_write(port, o, sizeof(o), &bw, 0);
    return bw == sizeof(o);
  }
  bool stop() override { if (installed) i2s_zero_dma_buffer(port); return true; }

 private:
  i2s_port_t port;
};

I2SOut *out = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *mp3File = nullptr;
AudioFileSourceBuffer *mp3Buf = nullptr;

#define MIC_PORT I2S_NUM_0               // микрофон на I2S0, динамики на I2S1 (как в v1)

// ------------------------- СОСТОЯНИЕ ---------------------------------
bool sdOk = false, fileOk = false, micOk = false;
bool spkMuted = false, auxMuted = false;
uint64_t sdSizeMB = 0;
float micLevel = 0;

// связь с со-процессором
unsigned long lastRxMs = 0;       // когда последний раз пришла строка от ESP
unsigned long rxLines = 0;        // сколько строк пришло всего
unsigned long rxBytes = 0;        // сколько байт пришло (даже мусор)
unsigned long pingSent = 0;       // сколько PING отправили
unsigned long lastAck = 0;        // номер последнего PING, на который ESP ответила
unsigned long lastAckMs = 0;
int cpT1 = 0, cpT2 = 0, cpT3 = 0, cpMicBtn = 0, cpBat = 0;
unsigned long cpUp = 0;

// =====================================================================
//  MP3
// =====================================================================
bool isPlaying() { return mp3 && mp3->isRunning(); }

void stopMp3() {
  if (mp3) { if (mp3->isRunning()) mp3->stop(); delete mp3; mp3 = nullptr; }
  if (mp3Buf) { delete mp3Buf; mp3Buf = nullptr; }
  if (mp3File) { delete mp3File; mp3File = nullptr; }
}

bool startMp3() {
  stopMp3();
  if (!sdOk) { Serial.println("[MP3] нет SD — играть нечего"); return false; }
  mp3File = new AudioFileSourceSD(MP3_FILE);
  if (!mp3File->isOpen()) {
    Serial.printf("[MP3] файл %s не открылся\n", MP3_FILE);
    delete mp3File; mp3File = nullptr;
    return false;
  }
  mp3Buf = new AudioFileSourceBuffer(mp3File, 32768);   // 32 КБ буфер — без треска
  mp3 = new AudioGeneratorMP3();
  out->SetGain(volume);
  bool ok = mp3->begin(mp3Buf, out);
  Serial.printf("[MP3] старт %s: %s (vol %.2f)\n", MP3_FILE, ok ? "OK" : "FAIL", volume);
  return ok;
}

// =====================================================================
//  ТОН В ДИНАМИКИ (проверка усилителей без SD и MP3)
// =====================================================================
void speakerTone() {
  stopMp3();
  Serial.println("[SPK] тон 1 кГц 1 с в динамики");
  out->SetGain(volume);
  out->SetBitsPerSample(16);
  out->SetChannels(2);
  out->SetRate(44100);
  out->begin();
  const int N = 44100;
  float ph = 0, inc = 2.0f * PI * 1000.0f / 44100.0f;
  for (int i = 0; i < N; i++) {
    float env = (i < 441) ? i / 441.0f : (i > N - 441 ? (N - i) / 441.0f : 1.0f);
    int16_t v = (int16_t)(12000 * env * sinf(ph));
    ph += inc; if (ph > 2 * PI) ph -= 2 * PI;
    int16_t s[2] = {v, v};
    while (!out->ConsumeSample(s)) delay(1);
  }
  delay(200);
  out->stop();
}

// =====================================================================
//  МИКРОФОН INMP441 (I2S0, вход)
// =====================================================================
bool initMic() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;    // INMP441: 24 бит в 32-битном слоте
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;      // L/R микрофона на GND
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;
  if (i2s_driver_install(MIC_PORT, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = MIC_SCK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD;
  return i2s_set_pin(MIC_PORT, &pins) == ESP_OK;
}

static inline int32_t micTo16(int32_t raw) {
  int32_t v = raw >> 14;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return v;
}

void micMeter() {
  if (!micOk) return;
  static int32_t buf[256];
  size_t br = 0;
  i2s_read(MIC_PORT, buf, sizeof(buf), &br, 0);
  int n = br / 4;
  int32_t peak = 0;
  for (int i = 0; i < n; i++) {
    int32_t v = abs(micTo16(buf[i]));
    if (v > peak) peak = v;
  }
  micLevel = max((float)peak, micLevel * 0.85f);
}

// =====================================================================
//  ЭКРАН (rotation 2 = перевёрнут на 180°)
// =====================================================================
struct Btn { int x, y, w, h; const char *label; };
Btn btns[8];
const int BTN_TOP = 156, BTN_H = 37, BTN_GAP = 4;

void setupButtons() {
  const char *labels[8] = {"PLAY / STOP", "MIC TEST 3s", "SPK MUTE", "AUX MUTE",
                           "AUX BEEP", "LED NEXT", "SPK TONE", "VOLUME +"};
  for (int i = 0; i < 8; i++) {
    int col = i % 2, row = i / 2;
    btns[i] = {3 + col * 119, BTN_TOP + row * (BTN_H + BTN_GAP), 115, BTN_H, labels[i]};
  }
}

uint16_t btnColor(int i) {
  if (i == 0 && isPlaying()) return TFT_DARKGREEN;
  if (i == 2 && spkMuted) return TFT_RED;
  if (i == 3 && auxMuted) return TFT_RED;
  return 0x3186;
}

void drawButton(int i, bool pressed = false) {
  Btn &b = btns[i];
  uint16_t bg = pressed ? TFT_ORANGE : btnColor(i);
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_LIGHTGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawAllButtons() { for (int i = 0; i < 8; i++) drawButton(i); }

String lastLine[9];
void statusLine(int idx, const String &txt, uint16_t col) {
  if (lastLine[idx] == txt) return;
  lastLine[idx] = txt;
  int y = 22 + idx * 16;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextPadding(236);
  tft.drawString(txt, 2, y, 2);
  tft.setTextPadding(0);
}

void drawHeader() {
  tft.fillRect(0, 0, 240, 20, TFT_NAVY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("SEBASTIAN  bench test v3", 4, 2, 2);
}

void flashMsg(const char *msg, uint16_t col) {
  tft.fillRect(0, 0, 240, 20, col);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, col);
  tft.drawString(msg, 4, 2, 2);
}

void drawMicBar() {
  int y = 22 + 2 * 16;
  int w = (int)(micLevel / 40.0f);
  if (w > 150) w = 150;
  tft.fillRect(80, y + 3, w, 10, w > 120 ? TFT_RED : TFT_GREEN);
  tft.fillRect(80 + w, y + 3, 150 - w, 10, 0x2104);
}

bool rxOk() { return lastRxMs && millis() - lastRxMs < 1500; }      // ESP -> S3 живо
bool txOk() { return lastAckMs && millis() - lastAckMs < 3000; }    // S3 -> ESP живо (ESP отвечает на PING)

void updateStatus() {
  char s[64];
  // 0: SD
  if (!sdOk) statusLine(0, "SD: FAIL (FAT32? CS=5)", TFT_RED);
  else {
    snprintf(s, sizeof(s), "SD: OK %lluMB  test.mp3:%s", (unsigned long long)sdSizeMB, fileOk ? "YES" : "NO");
    statusLine(0, s, fileOk ? TFT_GREEN : TFT_YELLOW);
  }
  // 1: AUDIO
  snprintf(s, sizeof(s), "AUDIO: %s  vol %.2f", isPlaying() ? "PLAYING" : "stop", volume);
  statusLine(1, s, isPlaying() ? TFT_GREEN : TFT_WHITE);
  // 2: MIC
  if (!micOk) statusLine(2, "MIC: I2S FAIL", TFT_RED);
  else { snprintf(s, sizeof(s), "MIC %5d", (int)micLevel); statusLine(2, s, TFT_CYAN); drawMicBar(); }
  // 3: ESP -> S3
  if (rxOk()) { snprintf(s, sizeof(s), "ESP->S3: OK  %lu str, %lums", rxLines, millis() - lastRxMs); statusLine(3, s, TFT_GREEN); }
  else if (rxBytes > 0) { snprintf(s, sizeof(s), "ESP->S3: MUSOR %lu b (GND?)", rxBytes); statusLine(3, s, TFT_YELLOW); }
  else statusLine(3, "ESP->S3: NET (ESP 17 -> S3 18)", TFT_RED);
  // 4: S3 -> ESP
  if (txOk()) { snprintf(s, sizeof(s), "S3->ESP: OK  ping %lu/%lu", lastAck, pingSent); statusLine(4, s, TFT_GREEN); }
  else { snprintf(s, sizeof(s), "S3->ESP: NET (S3 8 -> ESP 16) %lu", pingSent); statusLine(4, s, TFT_RED); }
  // 5: сенсоры
  bool link = rxOk();
  snprintf(s, sizeof(s), "TOUCH 1:%c 2:%c 3:%c  MICBTN:%c",
           link && cpT1 ? 'X' : '-', link && cpT2 ? 'X' : '-', link && cpT3 ? 'X' : '-', link && cpMicBtn ? 'X' : '-');
  statusLine(5, s, TFT_WHITE);
  // 6: батарея
  if (!link) statusLine(6, "BAT: ---", TFT_DARKGREY);
  else if (cpBat < 2500) { snprintf(s, sizeof(s), "BAT: %.2f V (net akuma)", cpBat / 1000.0f); statusLine(6, s, TFT_DARKGREY); }
  else { snprintf(s, sizeof(s), "BAT: %.2f V", cpBat / 1000.0f); statusLine(6, s, TFT_WHITE); }
  // 7: выходы
  snprintf(s, sizeof(s), "SPK:%s (GPIO2=%s)  AUX:%s", spkMuted ? "OFF" : "ON", spkMuted ? "H" : "L", auxMuted ? "OFF" : "ON");
  statusLine(7, s, TFT_WHITE);
}

// =====================================================================
//  ТЕСТ МИКРОФОНА: 3 с записи -> воспроизведение в динамики
// =====================================================================
void micTest() {
  if (!micOk) return;
  stopMp3();
  const int SR = 16000, N = SR * 3;
  int16_t *rec = (int16_t *)ps_malloc(N * sizeof(int16_t));
  if (!rec) { Serial.println("[MIC] нет PSRAM"); return; }

  flashMsg("REC 3s... govori!", TFT_RED);
  Serial.println("[MIC] запись 3 с...");
  static int32_t raw[256];
  size_t br;
  for (int k = 0; k < 16; k++) i2s_read(MIC_PORT, raw, sizeof(raw), &br, 0);   // слить старое
  int idx = 0; int32_t peak = 0;
  while (idx < N) {
    i2s_read(MIC_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    int n = br / 4;
    for (int i = 0; i < n && idx < N; i++) {
      int32_t v = micTo16(raw[i]);
      if (abs(v) > peak) peak = abs(v);
      rec[idx++] = (int16_t)(v * 2 / 5);
    }
  }
  Serial.printf("[MIC] записано, peak=%ld (около 0 = микрофон не слышит)\n", (long)peak);

  flashMsg("PLAY...", TFT_DARKGREEN);
  out->SetGain(volume);
  out->SetBitsPerSample(16);
  out->SetChannels(2);
  out->SetRate(SR);
  out->begin();
  for (int i = 0; i < N; i++) {
    int16_t s[2] = {rec[i], rec[i]};
    while (!out->ConsumeSample(s)) delay(1);
  }
  delay(300);
  out->stop();
  free(rec);
  Serial.println("[MIC] готово");
  drawHeader();
}

// =====================================================================
//  ВЫХОДЫ
// =====================================================================
void applyMutes() {
  digitalWrite(PIN_SPK_MUTE, spkMuted ? HIGH : LOW);
  digitalWrite(PIN_XSMT, auxMuted ? LOW : HIGH);
  Serial.printf("[OUT] динамики %s (GPIO2=%s), AUX %s\n", spkMuted ? "OFF" : "ON",
                spkMuted ? "HIGH" : "LOW", auxMuted ? "OFF" : "ON");
}

static const float VOLS[] = {0.10f, 0.15f, 0.20f, 0.30f, 0.45f, 0.60f};
static const int NVOL = sizeof(VOLS) / sizeof(VOLS[0]);
int volIndex() { int c = 0; for (int i = 0; i < NVOL; i++) if (fabsf(VOLS[i] - volume) < 0.01f) c = i; return c; }
void setVolume(float v) { volume = v; if (out) out->SetGain(volume); Serial.printf("[VOL] %.2f\n", volume); }
void volumeUp()    { int i = volIndex(); if (i < NVOL - 1) setVolume(VOLS[i + 1]); }
void volumeDown()  { int i = volIndex(); if (i > 0) setVolume(VOLS[i - 1]); }
void volumeCycle() { setVolume(VOLS[(volIndex() + 1) % NVOL]); }

void togglePlay() {
  if (isPlaying()) { loopMp3 = false; stopMp3(); Serial.println("[MP3] стоп"); }
  else { loopMp3 = true; startMp3(); }
  drawButton(0);
}

// =====================================================================
//  КНОПКИ НА ЭКРАНЕ
// =====================================================================
void onButton(int i) {
  Serial.printf("[UI] кнопка: %s\n", btns[i].label);
  switch (i) {
    case 0: togglePlay(); break;
    case 1: { bool was = loopMp3; micTest(); if (was) startMp3(); } break;
    case 2: spkMuted = !spkMuted; applyMutes(); break;
    case 3: auxMuted = !auxMuted; applyMutes(); break;
    case 4: Serial1.println("BEEP"); break;
    case 5: Serial1.println("LED"); break;
    case 6: { bool was = loopMp3; speakerTone(); if (was) startMp3(); } break;
    case 7: volumeCycle(); break;
  }
  drawButton(i);
}

void onHwTouch(int n) {
  Serial.printf("[TOUCH] сенсор %d -> %s\n", n, n == 1 ? "тише" : n == 2 ? "play/stop" : "громче");
  if (n == 1) volumeDown();
  if (n == 2) togglePlay();
  if (n == 3) volumeUp();
}

bool touchDown = false;
unsigned long lastIrqLow = 0;
void pollTouch() {
  if (digitalRead(T_IRQ) == HIGH) {
    if (touchDown && millis() - lastIrqLow > 120) touchDown = false;
    return;
  }
  lastIrqLow = millis();
  if (touchDown) return;
  uint16_t x, y;
  bool ok = tft.getTouch(&x, &y);
  tft.drawPixel(239, 319, TFT_BLACK);            // «жертвенная» запись после getTouch
  if (!ok) return;
  touchDown = true;
  // калибровка снята при rotation 0, экран сейчас перевёрнут (rotation 2) — отзеркаливаем
  x = 239 - x;
  y = 319 - y;
  for (int i = 0; i < 8; i++) {
    Btn &b = btns[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
      drawButton(i, true);
      onButton(i);
      return;
    }
  }
  Serial.printf("[UI] тап x=%d y=%d (мимо кнопок)\n", x, y);
}

// =====================================================================
//  UART-ЛИНК
//  S3 -> ESP:  PING n  (ESP отвечает P,n)  |  BEEP  |  LED
//  ESP -> S3:  S,t1,t2,t3,micbtn,bat_mV,uptime  |  T,n  |  P,n
// =====================================================================
char lb[160];
int ll = 0;

void handleLine(char *s) {
  lastRxMs = millis();
  rxLines++;
  if (s[0] == 'S' && s[1] == ',') {
    sscanf(s + 2, "%d,%d,%d,%d,%d,%lu", &cpT1, &cpT2, &cpT3, &cpMicBtn, &cpBat, &cpUp);
  } else if (s[0] == 'T' && s[1] == ',') {
    onHwTouch(atoi(s + 2));
  } else if (s[0] == 'P' && s[1] == ',') {
    lastAck = strtoul(s + 2, nullptr, 10);
    lastAckMs = millis();
  } else {
    Serial.printf("[LINK] от ESP: %s\n", s);
  }
}

void pollLink() {
  while (Serial1.available()) {
    char c = Serial1.read();
    rxBytes++;
    if (c == '\n') { lb[ll] = 0; if (ll) handleLine(lb); ll = 0; }
    else if (c != '\r' && ll < (int)sizeof(lb) - 1) lb[ll++] = c;
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n===== SEBASTIAN BENCH v3: HEAD (ESP32-S3) =====");

  pinMode(PIN_SPK_MUTE, OUTPUT);
  pinMode(PIN_XSMT, OUTPUT);
  applyMutes();
  pinMode(T_IRQ, INPUT_PULLUP);

  Serial.printf("[SYS] PSRAM: %s (%u KB)\n", psramFound() ? "OK" : "НЕТ", ESP.getPsramSize() / 1024);

  tft.init();
  tft.setRotation(2);                       // перевёрнут на 180°
  tft.setTouch(calData);
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  setupButtons();
  drawAllButtons();

  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, 10000000);
  if (sdOk) {
    sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    fileOk = SD.exists(MP3_FILE);
    Serial.printf("[SD] OK, %llu MB, %s: %s\n", (unsigned long long)sdSizeMB, MP3_FILE, fileOk ? "есть" : "НЕТ");
  } else {
    Serial.println("[SD] FAIL — карта FAT32? провода 6/7/15/5?");
  }

  // динамики: I2S1, как в v1 (там музыка играла)
  out = new I2SOut(I2S_NUM_1);
  out->SetGain(volume);
  Serial.printf("[I2S] динамики: %s\n", out->begin() ? "OK" : "FAIL");

  micOk = initMic();
  Serial.printf("[MIC] I2S0: %s (SCK %d, WS %d, SD %d)\n", micOk ? "OK" : "FAIL", MIC_SCK, MIC_WS, MIC_SD);

  updateStatus();
  if (fileOk) startMp3();                   // сразу играем test.mp3, по кругу
  drawButton(0);
  Serial.println("[SYS] готово. Сенсоры: 1=тише 2=play/stop 3=громче");
}

unsigned long tUi = 0, tLog = 0, tPing = 0;
bool wasPlaying = false;

void loop() {
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("[MP3] трек закончился");
      if (loopMp3) startMp3();              // по кругу
    }
  }
  if (wasPlaying != isPlaying()) { wasPlaying = isPlaying(); drawButton(0); }

  pollLink();
  pollTouch();
  micMeter();

  if (millis() - tUi > 150) { tUi = millis(); updateStatus(); }
  if (millis() - tPing > 1000) { tPing = millis(); Serial1.printf("PING %lu\n", ++pingSent); }
  if (millis() - tLog > 2000) {
    tLog = millis();
    Serial.printf("[STAT] mp3:%s vol:%.2f mic:%d | ESP->S3:%s (%lu str) S3->ESP:%s (ack %lu/%lu) | T:%d%d%d bat:%dmV\n",
                  isPlaying() ? "play" : "stop", volume, (int)micLevel,
                  rxOk() ? "OK" : "NET", rxLines, txOk() ? "OK" : "NET", lastAck, pingSent,
                  cpT1, cpT2, cpT3, cpBat);
  }
}