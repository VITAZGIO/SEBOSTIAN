// =====================================================================
//  СЕБАСТЬЯН — стендовый тест, ГОЛОВА (ESP32-S3)  v13
//  v13: экраны-страницы (листать < >): HOME (часы), AUDIO, SENSORS, RADIO, SYSTEM.
//       Wi-Fi включается сам при старте: часы по интернету (NTP) и прошивка по Wi-Fi
//       ВСЕГДА готова (кнопку жать не надо). Со-процессор — кнопкой на SYSTEM.
//  v12: без автостарта музыки. v11: OTA. v10: детект AUX. v9: пульт RADIO. v8: радио 433.
//  Дисплей перевёрнут на 180°, тач XPT2046, SD+MP3 -> 2×MAX98357A (I2S1), INMP441 (I2S0),
//  мьют динамиков (BC547, GPIO2), мьют AUX (XSMT, GPIO39), UART-связь с со-процессором.
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <driver/i2s.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "AudioOutput.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "secrets.h"      // WIFI_SSID, WIFI_PASS, OTA_PASS — впиши свои

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
#define AUX_DET      3      // детект штекера: пин 4 гнезда -> 10к -> GPIO3 (LOW = нет штекера, HIGH = вставлен)
#define T_IRQ        17     // прерывание тача (LOW = касание)
#define LINK_RX      18     // UART к со-процессору: RX S3 <- TX2 (GPIO17) ESP32
#define LINK_TX      8      //                       TX S3 -> RX2 (GPIO16) ESP32

// ------------------------- НАСТРОЙКИ ---------------------------------
static const char *MP3_FILE = "/test.mp3";
static float volume = 0.30f;            // громкость динамиков
static bool loopMp3 = false;            // трек по кругу после PLAY, пока не нажмёшь STOP
static uint16_t calData[5] = {460, 3320, 340, 3430, 1};   // калибровка тача (снята при rotation 0)

// Часовой пояс для часов (строка POSIX). Москва UTC+3 = "MSK-3".
// Другие: Самара "<+04>-4", Екатеринбург "<+05>-5", Новосибирск "<+07>-7", Владивосток "<+10>-10".
#define TZ_INFO "MSK-3"

// ------------------------- СОСТОЯНИЕ ---------------------------------
bool sdOk = false, fileOk = false, micOk = false;
bool spkMuted = false, auxMuted = false;
bool auxPlug = false;                    // штекер AUX вставлен?
uint64_t sdSizeMB = 0;
float micLevel = 0;

// связь с со-процессором
unsigned long lastRxMs = 0, rxLines = 0, rxBytes = 0, pingSent = 0, lastAck = 0, lastAckMs = 0;
int cpT1 = 0, cpT2 = 0, cpT3 = 0, cpMicBtn = 0, cpBat = 0;
unsigned long cpUp = 0;
int cpBt = 0, cpAuxMusic = 0;            // BT: 0 ждёт, 1 подключён, 2 играет; мелодия в AUX 0/1
bool rxOk() { return lastRxMs && millis() - lastRxMs < 1500; }      // ESP -> S3 живо
bool txOk() { return lastAckMs && millis() - lastAckMs < 3000; }    // S3 -> ESP живо (ESP отвечает на PING)

// Wi-Fi / часы / OTA
bool otaReady = false;                   // ArduinoOTA запущен (после первого подключения Wi-Fi)
int headOtaPct = -1;                     // -1 = не прошиваемся
bool timeStarted = false;
int cpOta = 0, cpOtaPct = 0;             // статус OTA со-процессора (W,...)
String cpIp = "";

// радио 433 (сам модуль на со-процессоре)
// Подписи кнопок пульта: латиница/цифры, до ~5 символов. Номер лучше оставить.
const char *RF_NAMES[21] = {
  "1 ON", "2", "3", "4", "5", "6", "7",
  "8", "9", "10", "11", "12", "13", "14",
  "15", "16", "17", "18", "19", "20", "21"
};
char rfRxText[40] = "RF RX: ---";
unsigned long rfRxMs = 0;
char rfTxText[24] = "";
unsigned long hdrRestoreAt = 0;          // когда вернуть шапку после вспышки

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
    int16_t o[2] = {Amplify(s[0]), Amplify(s[1])};   // {лево, право} — стандартный порядок I2S
    size_t bw = 0;
    i2s_write(port, o, sizeof(o), &bw, 0);
    return bw == sizeof(o);
  }
  bool stop() override { if (installed) i2s_zero_dma_buffer(port); return true; }

 private:
  i2s_port_t port;
};

I2SOut *out = nullptr;
void flashMsg(const char *msg, uint16_t col);   // объявления — определены ниже
void drawHeader();
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *mp3File = nullptr;
AudioFileSourceBuffer *mp3Buf = nullptr;

#define MIC_PORT I2S_NUM_0               // микрофон на I2S0, динамики на I2S1 (как в v1)
void goPage(int p);
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
  Serial.println("[MP3] 1/4 открываю файл"); Serial.flush();
  mp3File = new AudioFileSourceSD(MP3_FILE);
  if (!mp3File->isOpen()) {
    Serial.printf("[MP3] файл %s не открылся\n", MP3_FILE);
    delete mp3File; mp3File = nullptr;
    return false;
  }
  Serial.printf("[MP3] 2/4 буфер 32К (свободно внутр. RAM %u)\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); Serial.flush();
  mp3Buf = new AudioFileSourceBuffer(mp3File, 32768);   // 32 КБ буфер — без треска
  Serial.println("[MP3] 3/4 декодер"); Serial.flush();
  mp3 = new AudioGeneratorMP3();
  out->SetGain(volume);
  Serial.println("[MP3] 4/4 старт"); Serial.flush();
  bool ok = mp3->begin(mp3Buf, out);
  Serial.printf("[MP3] старт %s: %s (vol %.2f)\n", MP3_FILE, ok ? "OK" : "FAIL", volume);
  return ok;
}

// =====================================================================
//  L/R ТЕСТ: 3 стука только в ЛЕВЫЙ канал, потом 2 стука только в ПРАВЫЙ
// =====================================================================
void toneChan(float freq, int ms, bool left, bool right) {
  const int N = 44100 * ms / 1000;
  float ph = 0, inc = 2.0f * PI * freq / 44100.0f;
  for (int i = 0; i < N; i++) {
    float env = (i < 441) ? i / 441.0f : (i > N - 441 ? (N - i) / 441.0f : 1.0f);
    int16_t v = (int16_t)(12000 * env * sinf(ph));
    ph += inc; if (ph > 2 * PI) ph -= 2 * PI;
    int16_t smp[2] = {left ? v : (int16_t)0, right ? v : (int16_t)0};
    while (!out->ConsumeSample(smp)) delay(1);
  }
}

void knocks(int n, bool left, bool right) {
  for (int k = 0; k < n; k++) {
    toneChan(700, 130, left, right);   // «тук»
    toneChan(0, 220, false, false);    // пауза между стуками
  }
}

void lrTest() {
  stopMp3();
  Serial.println("[SPK] L/R: снач. ОБА (проверка что живы), потом ЛЕВЫЙ=3 стука, ПРАВЫЙ=2 стука");
  out->SetGain(volume);
  out->SetBitsPerSample(16);
  out->SetChannels(2);
  out->SetRate(44100);
  out->begin();
  flashMsg("BOTH: 1 stuk", TFT_DARKGREEN);
  knocks(1, true, true);
  toneChan(0, 700, false, false);
  flashMsg("LEFT: 3 stuka", TFT_DARKGREEN);
  knocks(3, true, false);
  toneChan(0, 900, false, false);
  flashMsg("RIGHT: 2 stuka", TFT_DARKGREEN);
  knocks(2, false, true);
  delay(200);
  out->stop();
  drawHeader();
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

unsigned long micSamples = 0, micNonZero = 0;      // за текущую секунду
unsigned long micSamplesShown = 0, micNonZeroShown = 0, micWin = 0;
void micMeter() {
  if (!micOk) return;
  static int32_t buf[256];
  size_t br = 0;
  i2s_read(MIC_PORT, buf, sizeof(buf), &br, 0);
  int n = br / 4;
  int32_t peak = 0;
  for (int i = 0; i < n; i++) {
    if (buf[i] != 0) micNonZero++;
    int32_t v = abs(micTo16(buf[i]));
    if (v > peak) peak = v;
  }
  micSamples += n;
  if (millis() - micWin > 1000) {
    micWin = millis();
    micSamplesShown = micSamples; micNonZeroShown = micNonZero;
    micSamples = 0; micNonZero = 0;
  }
  micLevel = max((float)peak, micLevel * 0.85f);
}


// =====================================================================
//  СИСТЕМНОЕ: причина старта, счётчик перезапусков
// =====================================================================
RTC_NOINIT_ATTR uint32_t bootCount;       // переживает перезагрузки (не выключение питания)
esp_reset_reason_t rstReason;
const char *rstName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWER ON";
    case ESP_RST_SW:       return "SOFT";
    case ESP_RST_PANIC:    return "CRASH";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_EXT:      return "RESET BTN";
    default:               return "OTHER";
  }
}
bool badReset() { return rstReason == ESP_RST_BROWNOUT || rstReason == ESP_RST_PANIC ||
                         rstReason == ESP_RST_INT_WDT || rstReason == ESP_RST_TASK_WDT || rstReason == ESP_RST_WDT; }

// процент заряда Li-ion по напряжению (примерно, под нагрузкой врёт на 5-10%)
int batPercent(int mv) {
  static const int V[] = {3000, 3500, 3700, 3800, 3900, 4000, 4100, 4200};
  static const int P[] = {0,    8,    35,   55,   70,   82,   92,   100};
  if (mv <= V[0]) return 0;
  if (mv >= V[7]) return 100;
  for (int i = 1; i < 8; i++)
    if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

String uptimeStr(unsigned long sec) {
  char b[24];
  if (sec < 3600) snprintf(b, sizeof(b), "%lum %02lus", sec / 60, sec % 60);
  else if (sec < 86400) snprintf(b, sizeof(b), "%luh %02lum", sec / 3600, (sec / 60) % 60);
  else snprintf(b, sizeof(b), "%lud %02luh", sec / 86400, (sec / 3600) % 24);
  return b;
}

bool timeValid(struct tm &t) { return timeStarted && getLocalTime(&t, 0) && t.tm_year > 120; }

// =====================================================================
//  ЭКРАН: страницы
//  шапка (0..20) | содержимое (22..282) | навигация < > (286..318)
// =====================================================================
enum { PG_HOME, PG_AUDIO, PG_SENS, PG_RADIO, PG_SYS, PG_COUNT };
const char *PG_NAMES[PG_COUNT] = {"HOME", "AUDIO", "SENSORS", "RADIO", "SYSTEM"};
int page = PG_HOME;

enum {
  B_PREV = 1, B_NEXT, B_PLAY, B_VOLM, B_VOLP, B_SPK, B_AUX, B_AUXMUS, B_LR, B_MIC,
  B_LED, B_BEEP, B_CPOTA, B_REBOOT, B_WIFI, B_RF = 100    // B_RF+0..20 коды, B_RF+21 HOLD
};
struct Btn { int x, y, w, h, id; const char *label; };
Btn pb[30];                    // кнопки текущей страницы
int npb = 0;
String lines[12];              // кэш строк статуса (не перерисовываем то, что не изменилось)
String clockCache = "";        // кэш часов на HOME
int touchCache = -1;           // кэш кругов-сенсоров на SENSORS

void addBtn(int x, int y, int w, int h, int id, const char *label) {
  if (npb < 30) pb[npb++] = {x, y, w, h, id, label};
}

uint16_t btnColor(int id) {
  switch (id) {
    case B_PLAY:   return isPlaying() ? TFT_DARKGREEN : 0x3186;
    case B_SPK:    return spkMuted ? TFT_RED : 0x3186;
    case B_AUX:    return auxMuted ? TFT_RED : (auxPlug ? 0x3186 : 0x4208);
    case B_AUXMUS: return cpAuxMusic ? TFT_DARKGREEN : 0x3186;
    case B_PREV: case B_NEXT: return TFT_NAVY;
    case B_CPOTA:  return (cpOta >= 1 && cpOta <= 3) ? TFT_DARKGREEN : 0x3186;
    case B_REBOOT: return 0x7800;
    case B_RF + 21: return 0x7800;
    default:       return 0x3186;
  }
}

void drawBtn(Btn &b, bool pressed = false) {
  uint16_t bg = pressed ? TFT_ORANGE : btnColor(b.id);
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_LIGHTGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2 + 1, 2);
  tft.setTextDatum(TL_DATUM);
}

void refreshBtn(int id) {       // перерисовать кнопку, если она на текущей странице
  for (int i = 0; i < npb; i++) if (pb[i].id == id) drawBtn(pb[i]);
}

// строка статуса №idx (y = 24 + idx*17), перерисовка только при изменении
void line(int idx, const String &txt, uint16_t col = TFT_WHITE) {
  if (lines[idx] == txt) return;
  lines[idx] = txt;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextPadding(236);
  tft.drawString(txt, 2, 24 + idx * 17, 2);
  tft.setTextPadding(0);
}

// ---------- шапка: время | страница | Wi-Fi ----------
String hdrCache = "";
void drawHeader() {
  hdrCache = "";                                   // принудительно перерисовать
}
void updateHeader() {
  if (hdrRestoreAt) return;                        // сейчас показывается вспышка
  struct tm t;
  char tm_s[8] = "--:--";
  if (timeValid(t)) strftime(tm_s, sizeof(tm_s), "%H:%M", &t);
  String w;
  if (headOtaPct >= 0) w = "OTA";
  else if (WiFi.status() == WL_CONNECTED) { int r = WiFi.RSSI(); w = r > -60 ? "WiFi+++" : r > -75 ? "WiFi++" : "WiFi+"; }
  else w = "WiFi-";
  String h = String(tm_s) + "|" + PG_NAMES[page] + "|" + w + "|" + (badReset() ? "!" : "");
  if (h == hdrCache) return;
  hdrCache = h;
  uint16_t bg = badReset() ? 0x7800 : TFT_NAVY;
  tft.fillRect(0, 0, 240, 20, bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);  tft.drawString(tm_s, 4, 2, 2);
  tft.setTextDatum(TC_DATUM);  tft.drawString(PG_NAMES[page], 120, 2, 2);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, bg);
  tft.setTextDatum(TR_DATUM);  tft.drawString(w, 236, 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void flashMsg(const char *msg, uint16_t col) {
  tft.fillRect(0, 0, 240, 20, col);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, col);
  tft.drawString(msg, 4, 2, 2);
  hdrRestoreAt = millis() + 2000;
  hdrCache = "";
}

// ---------- построение страниц ----------
void drawNav() {
  addBtn(2, 286, 60, 32, B_PREV, "<");
  addBtn(178, 286, 60, 32, B_NEXT, ">");
  tft.fillRect(64, 286, 112, 32, TFT_BLACK);
  // точки страниц
  for (int i = 0; i < PG_COUNT; i++) {
    int x = 120 - (PG_COUNT - 1) * 9 + i * 18;
    if (i == page) tft.fillCircle(x, 302, 5, TFT_WHITE);
    else tft.drawCircle(x, 302, 5, TFT_DARKGREY);
  }
}

void grid2(int y0, int h, const int *ids, const char **labels, int n) {   // кнопки в 2 столбца
  for (int i = 0; i < n; i++) addBtn(3 + (i % 2) * 119, y0 + (i / 2) * (h + 4), 115, h, ids[i], labels[i]);
}

void buildPage() {
  npb = 0;
  for (auto &l : lines) l = "";
  clockCache = ""; touchCache = -1;
  tft.fillRect(0, 20, 240, 300, TFT_BLACK);
  drawHeader();
  switch (page) {
    case PG_HOME: {
      int ids[3] = {B_VOLM, B_PLAY, B_VOLP};
      const char *lb[3] = {"VOL -", "PLAY/STOP", "VOL +"};
      for (int i = 0; i < 3; i++) addBtn(3 + i * 79, 240, 75, 40, ids[i], lb[i]);
    } break;
    case PG_AUDIO: {
      int ids[8] = {B_PLAY, B_MIC, B_VOLM, B_VOLP, B_SPK, B_AUX, B_AUXMUS, B_LR};
      const char *lb[8] = {"PLAY / STOP", "MIC TEST 2s", "VOL -", "VOL +", "SPK MUTE", "AUX MUTE", "AUX MUSIC", "L / R TEST"};
      grid2(112, 40, ids, lb, 8);
    } break;
    case PG_SENS: {
      int ids[2] = {B_LED, B_BEEP};
      const char *lb[2] = {"LED NEXT", "BEEP (AUX)"};
      grid2(240, 40, ids, lb, 2);
    } break;
    case PG_RADIO: {
      for (int i = 0; i < 22; i++)
        addBtn(2 + (i % 4) * 60, 62 + (i / 4) * 37, 56, 33, B_RF + i, i < 21 ? RF_NAMES[i] : "HOLD");
    } break;
    case PG_SYS: {
      int ids[2] = {B_CPOTA, B_REBOOT};
      const char *lb[2] = {"COPROC OTA", "REBOOT HEAD"};
      grid2(240, 40, ids, lb, 2);
    } break;
  }
  drawNav();
  for (int i = 0; i < npb; i++) drawBtn(pb[i]);
}

void goPage(int p) {
  page = (p + PG_COUNT) % PG_COUNT;
  Serial.printf("[UI] страница %s\n", PG_NAMES[page]);
  buildPage();
}

// ---------- содержимое страниц (обновляется ~7 раз в секунду) ----------
void micBar(int y) {
  int w = constrain((int)(micLevel / 40.0f), 0, 150);
  tft.fillRect(80, y + 3, w, 10, w > 120 ? TFT_RED : TFT_GREEN);
  tft.fillRect(80 + w, y + 3, 150 - w, 10, 0x2104);
}

String batText() {
  if (!rxOk()) return "BAT: ---";
  if (cpBat < 2500) return "BAT: net akkuma";
  char b[40]; snprintf(b, sizeof(b), "BAT: %.2fV  %d%%", cpBat / 1000.0f, batPercent(cpBat));
  return b;
}
const char *btText() { return !rxOk() ? "---" : cpBt == 2 ? "PLAY" : cpBt == 1 ? "CONNECTED" : "zhdet"; }

void pageHome() {
  struct tm t;
  bool ok = timeValid(t);
  char hm[8] = "--:--", ss[4] = "", dt[32] = "net vremeni (WiFi?)";
  if (ok) {
    strftime(hm, sizeof(hm), "%H:%M", &t);
    strftime(ss, sizeof(ss), "%S", &t);
    static const char *DN[7] = {"Vs", "Pn", "Vt", "Sr", "Cht", "Pt", "Sb"};
    snprintf(dt, sizeof(dt), "%s  %02d.%02d.%04d", DN[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  }
  String c = String(hm) + ss;
  if (c != clockCache) {
    clockCache = c;
    tft.setTextColor(ok ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextPadding(200);
    tft.drawString(hm, 108, 30, 7);                // большие цифры
    tft.setTextPadding(30);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(ss, 200, 58, 4);                // секунды
    tft.setTextPadding(0);
  }
  line(4, dt, TFT_CYAN);
  line(6, String("WiFi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() + "  " + WiFi.RSSI() + "dBm" : String("net (") + WIFI_SSID + ")"),
       WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED);
  line(7, batText(), TFT_WHITE);
  line(8, String("BT: ") + btText() + "   LINK: " + (rxOk() && txOk() ? "OK" : "NET"), rxOk() && txOk() ? TFT_WHITE : TFT_RED);
  char s[48]; snprintf(s, sizeof(s), "AUDIO: %s  vol %.2f", isPlaying() ? "PLAY" : "stop", volume);
  line(9, s, isPlaying() ? TFT_GREEN : TFT_WHITE);
  line(10, rfRxText, rfRxMs && millis() - rfRxMs < 3000 ? TFT_YELLOW : TFT_DARKGREY);
}

void pageAudio() {
  char s[64];
  snprintf(s, sizeof(s), "AUDIO: %s  vol %.2f", isPlaying() ? "PLAYING" : "stop", volume);
  line(0, s, isPlaying() ? TFT_GREEN : TFT_WHITE);
  if (!sdOk) line(1, "SD: FAIL (FAT32? CS=5)", TFT_RED);
  else { snprintf(s, sizeof(s), "SD: %lluMB  test.mp3:%s", (unsigned long long)sdSizeMB, fileOk ? "YES" : "NO"); line(1, s, fileOk ? TFT_GREEN : TFT_YELLOW); }
  snprintf(s, sizeof(s), "SPK:%s  AUX:%s  JACK:%s%s", spkMuted ? "OFF" : "ON", (auxPlug && !auxMuted) ? "ON" : "OFF",
           auxPlug ? "IN" : "--", cpAuxMusic ? " music" : "");
  line(2, s);
  if (!micOk) line(3, "MIC: I2S FAIL", TFT_RED);
  else { snprintf(s, sizeof(s), "MIC %5d", (int)micLevel); line(3, s, TFT_CYAN); micBar(24 + 3 * 17); }
  line(4, String("BT: ") + btText(), cpBt ? TFT_CYAN : TFT_WHITE);
}

void pageSens() {
  char s[64];
  if (rxOk()) snprintf(s, sizeof(s), "ESP->S3: OK  %lu str", rxLines);
  else if (rxBytes) snprintf(s, sizeof(s), "ESP->S3: MUSOR %lu b (GND?)", rxBytes);
  else snprintf(s, sizeof(s), "ESP->S3: NET (ESP 17 -> S3 18)");
  line(0, s, rxOk() ? TFT_GREEN : TFT_RED);
  if (txOk()) snprintf(s, sizeof(s), "S3->ESP: OK  ping %lu/%lu", lastAck, pingSent);
  else snprintf(s, sizeof(s), "S3->ESP: NET (S3 8 -> ESP 16)");
  line(1, s, txOk() ? TFT_GREEN : TFT_RED);
  line(2, batText());
  line(3, String("BT: ") + btText() + "   coproc up " + (rxOk() ? uptimeStr(cpUp) : String("---")));
  line(4, String("AUX JACK: ") + (auxPlug ? "vstavlen" : "net"), auxPlug ? TFT_GREEN : TFT_WHITE);
  line(5, rfRxText, rfRxMs && millis() - rfRxMs < 3000 ? TFT_YELLOW : TFT_WHITE);
  // индикаторы сенсоров: круги T1 T2 T3 MIC
  bool l = rxOk();
  int st = (l && cpT1) | (l && cpT2) << 1 | (l && cpT3) << 2 | (l && cpMicBtn) << 3 | l << 4;
  if (st != touchCache) {
    touchCache = st;
    const char *nm[4] = {"T1", "T2", "T3", "MIC"};
    for (int i = 0; i < 4; i++) {
      int x = 30 + i * 60, y = 190;
      bool on = st & (1 << i);
      tft.fillCircle(x, y, 22, on ? TFT_GREEN : (l ? 0x2104 : 0x4000));
      tft.drawCircle(x, y, 22, TFT_LIGHTGREY);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(on ? TFT_BLACK : TFT_WHITE, on ? TFT_GREEN : (l ? 0x2104 : 0x4000));
      tft.drawString(nm[i], x, y, 2);
      tft.setTextDatum(TL_DATUM);
    }
  }
}

void pageRadio() {
  line(0, rfRxText, rfRxMs && millis() - rfRxMs < 3000 ? TFT_YELLOW : TFT_WHITE);
  line(1, String("RF TX: ") + (rfTxText[0] ? rfTxText : "---"), TFT_CYAN);
}

String otaText(const char *who, int st, int pct, const String &ip) {
  switch (st) {
    case 0: return String(who) + ": vykl";
    case 1: return String(who) + ": podkl. k WiFi...";
    case 2: return String(who) + ": ZHDU " + ip;
    case 3: return String(who) + ": proshivka " + pct + "%";
    case 4: return String(who) + ": gotovo, reboot";
    default: return String(who) + ": OSHIBKA";
  }
}

void pageSys() {
  bool wc = WiFi.status() == WL_CONNECTED;
  line(0, String("WiFi: ") + WIFI_SSID + (wc ? "  " + String(WiFi.RSSI()) + "dBm" : "  NET"), wc ? TFT_GREEN : TFT_RED);
  line(1, String("IP: ") + (wc ? WiFi.localIP().toString() : String("---")), wc ? TFT_WHITE : TFT_DARKGREY);
  line(2, String("HEAD OTA: ") + (otaReady ? "gotov (sebastian-head)" : "zhdu WiFi"), otaReady ? TFT_GREEN : TFT_YELLOW);
  line(3, otaText("COPROC OTA", cpOta, cpOtaPct, cpIp), cpOta == 2 ? TFT_GREEN : (cpOta == 9 ? TFT_RED : TFT_WHITE));
  char s[64];
  snprintf(s, sizeof(s), "RAM: %uK  PSRAM: %uK svob.", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  line(4, s);
  line(5, String("uptime: ") + uptimeStr(millis() / 1000));
  snprintf(s, sizeof(s), "start: %s  boot#%lu", rstName(rstReason), (unsigned long)bootCount);
  line(6, s, badReset() ? TFT_RED : TFT_WHITE);
  line(7, "v13  " __DATE__, TFT_DARKGREY);
}

void updatePage() {
  switch (page) {
    case PG_HOME:  pageHome(); break;
    case PG_AUDIO: pageAudio(); break;
    case PG_SENS:  pageSens(); break;
    case PG_RADIO: pageRadio(); break;
    case PG_SYS:   pageSys(); break;
  }
}
// =====================================================================
//  ТЕСТ МИКРОФОНА: 3 с записи -> воспроизведение в динамики
// =====================================================================
void micTest() {
  if (!micOk) return;
  stopMp3();
  const int SR = 16000, N = SR * 2;
  int16_t *rec = (int16_t *)heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!rec) { Serial.println("[MIC] не хватило памяти"); return; }

  flashMsg("REC 2s... govori!", TFT_RED);
  Serial.println("[MIC] запись 2 с...");
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
      rec[idx++] = (int16_t)v;
    }
  }
  // 1) фильтр верхних частот ~120 Гц: убираем постоянку и гул (он съедает громкость)
  float hp = 0, prevIn = 0;
  const float a = 0.954f;                       // 16 кГц, срез ~120 Гц
  double sq = 0;
  for (int i = 0; i < N; i++) {
    float x = rec[i];
    hp = a * (hp + x - prevIn);
    prevIn = x;
    rec[i] = (int16_t)constrain((int)hp, -32768, 32767);
    sq += (double)hp * hp;
  }
  // 2) усиление по СРЕДНЕЙ громкости (RMS), а не по пику — речь станет громче
  float rms = sqrt(sq / N) + 1;
  float g = 7000.0f / rms;
  if (g > 60.0f) g = 60.0f;             // тишину не раздуваем до шипения
  if (g < 1.0f) g = 1.0f;
  // 3) мягкий ограничитель: громкие места не хрипят, а плавно поджимаются
  for (int i = 0; i < N; i++) {
    float v = rec[i] * g / 32767.0f;
    rec[i] = (int16_t)(tanhf(v) * 30000.0f);
  }
  Serial.printf("[MIC] записано, peak=%ld, усиление x%.1f (peak около 0 = микрофон не слышит)\n", (long)peak, g);

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
//  ВЫХОДЫ, ГРОМКОСТЬ, ДЕТЕКТ AUX
// =====================================================================
void applyMutes() {
  digitalWrite(PIN_SPK_MUTE, spkMuted ? HIGH : LOW);
  bool auxOn = auxPlug && !auxMuted;               // AUX играет только если штекер вставлен и не заглушён кнопкой
  digitalWrite(PIN_XSMT, auxOn ? HIGH : LOW);
  Serial.printf("[OUT] динамики %s, AUX %s (штекер %s)\n", spkMuted ? "OFF" : "ON", auxOn ? "ON" : "OFF", auxPlug ? "есть" : "нет");
  refreshBtn(B_SPK); refreshBtn(B_AUX);
}

unsigned long auxDetChange = 0;
bool auxDetRaw = false;
void pollAuxDetect() {                             // 150 мс антидребезга
  bool r = digitalRead(AUX_DET) == HIGH;
  if (r != auxDetRaw) { auxDetRaw = r; auxDetChange = millis(); return; }
  if (r != auxPlug && millis() - auxDetChange > 150) {
    auxPlug = r;
    Serial.printf("[AUX] штекер %s\n", auxPlug ? "ВСТАВЛЕН -> AUX вкл" : "вынут -> AUX выкл");
    applyMutes();
  }
}

static const float VOLS[] = {0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
static const int NVOL = sizeof(VOLS) / sizeof(VOLS[0]);
int volIndex() { int c = 0; for (int i = 0; i < NVOL; i++) if (fabsf(VOLS[i] - volume) < 0.01f) c = i; return c; }
void setVolume(float v) { volume = v; if (out) out->SetGain(volume); Serial.printf("[VOL] %.2f\n", volume); }
void volumeUp()    { int i = volIndex(); if (i < NVOL - 1) setVolume(VOLS[i + 1]); }
void volumeDown()  { int i = volIndex(); if (i > 0) setVolume(VOLS[i - 1]); }

void togglePlay() {
  if (isPlaying()) { loopMp3 = false; stopMp3(); Serial.println("[MP3] стоп"); }
  else { loopMp3 = true; startMp3(); }
  refreshBtn(B_PLAY);
}

// =====================================================================
//  Wi-Fi: часы (NTP) + прошивка по воздуху (всегда готова)
// =====================================================================
void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("sebastian-head");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] подключаюсь к \"%s\"...\n", WIFI_SSID);
}

void otaSetup() {
  ArduinoOTA.setHostname("sebastian-head");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    loopMp3 = false; stopMp3();
    headOtaPct = 0;
    Serial.println("[OTA] приём прошивки...");
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("PROSHIVKA PO WiFi", 120, 120, 4);
    tft.drawRect(19, 159, 202, 22, TFT_WHITE);
    tft.setTextDatum(TL_DATUM);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int p = total ? done * 100 / total : 0;
    if (p == headOtaPct) return;
    headOtaPct = p;
    tft.fillRect(20, 160, p * 2, 20, TFT_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextPadding(80);
    tft.drawString(String(p) + "%", 120, 205, 4);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] готово, перезагрузка");
    tft.setTextDatum(MC_DATUM);
    tft.drawString("GOTOVO, REBOOT", 120, 245, 2);
    tft.setTextDatum(TL_DATUM);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] ошибка %u\n", e);
    headOtaPct = -1;
    goPage(page);                                  // вернуть экран
    flashMsg("OTA OSHIBKA", TFT_RED);
  });
  ArduinoOTA.begin();
  otaReady = true;
}

bool wifiWasUp = false;
void wifiTick() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiWasUp) {
    Serial.printf("[WIFI] подключён: IP %s, %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!timeStarted) { configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "ntp1.stratum2.ru"); timeStarted = true; }
    if (!otaReady) otaSetup();
  }
  if (!up && wifiWasUp) Serial.println("[WIFI] связь потеряна, переподключаюсь...");
  wifiWasUp = up;
  if (otaReady) ArduinoOTA.handle();
}

// =====================================================================
//  НАЖАТИЯ
// =====================================================================
void onBtn(Btn &b) {
  Serial.printf("[UI] %s: %s\n", PG_NAMES[page], b.label);
  int id = b.id;
  if (id == B_PREV) { goPage(page - 1); return; }
  if (id == B_NEXT) { goPage(page + 1); return; }
  if (id >= B_RF && id <= B_RF + 21) {
    int i = id - B_RF;
    if (i < 21) { Serial1.printf("RF %d\n", i + 1); snprintf(rfTxText, sizeof(rfTxText), "#%s ...", RF_NAMES[i]); }
    else { Serial1.println("RFHOLD"); snprintf(rfTxText, sizeof(rfTxText), "HOLD ..."); }
    return;
  }
  switch (id) {
    case B_PLAY:   togglePlay(); break;
    case B_VOLM:   volumeDown(); break;
    case B_VOLP:   volumeUp(); break;
    case B_SPK:    spkMuted = !spkMuted; applyMutes(); break;
    case B_AUX:    auxMuted = !auxMuted; applyMutes(); break;
    case B_AUXMUS: Serial1.println("MUSIC"); break;
    case B_LR:     { bool was = loopMp3; lrTest(); if (was) startMp3(); } break;
    case B_MIC:    { bool was = loopMp3; micTest(); if (was) startMp3(); } break;
    case B_LED:    Serial1.println("LED"); break;
    case B_BEEP:   Serial1.println("BEEP"); break;
    case B_CPOTA:  Serial1.println("OTA"); cpOta = 1; cpIp = ""; Serial.println("[OTA] -> со-процессор: режим прошивки"); break;
    case B_REBOOT: flashMsg("REBOOT...", TFT_RED); delay(300); ESP.restart(); break;
  }
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
  tft.drawPixel(239, 319, TFT_BLACK);            // «жертвенная» запись после getTouch (общая SPI)
  if (!ok) return;
  touchDown = true;
  x = 239 - x;                                   // калибровка при rotation 0, экран в rotation 2
  y = 319 - y;
  for (int i = 0; i < npb; i++) {
    Btn &b = pb[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
      if (b.id != B_PREV && b.id != B_NEXT) drawBtn(b, true);
      Btn copy = b;                              // страница может смениться внутри onBtn
      onBtn(copy);
      if (copy.id != B_PREV && copy.id != B_NEXT) { delay(80); refreshBtn(copy.id); }
      return;
    }
  }
}

// =====================================================================
//  UART-ЛИНК с со-процессором
//  S3 -> ESP: PING n | BEEP | LED | MUSIC | RF n | RFHOLD | OTA
//  ESP -> S3: S,... | T,n | P,n | R,n,code | L,n,code | U,code | X,n,code | W,st,pct,ip
// =====================================================================
char lb[160];
int ll = 0;

void handleLine(char *s) {
  lastRxMs = millis();
  rxLines++;
  if (s[0] == 'S' && s[1] == ',') {
    int oldMus = cpAuxMusic;
    sscanf(s + 2, "%d,%d,%d,%d,%d,%lu,%d,%d", &cpT1, &cpT2, &cpT3, &cpMicBtn, &cpBat, &cpUp, &cpBt, &cpAuxMusic);
    if (oldMus != cpAuxMusic) refreshBtn(B_AUXMUS);
  } else if (s[0] == 'T' && s[1] == ',') {
    onHwTouch(atoi(s + 2));
  } else if (s[0] == 'R' && s[1] == ',') {          // R,n,code — пойман триггер n (1..8)
    int n = 0; unsigned long code = 0;
    sscanf(s + 2, "%d,%lu", &n, &code);
    snprintf(rfRxText, sizeof(rfRxText), "RF RX: TRIG %d", n);
    rfRxMs = millis();
    Serial.printf("[RF] ПОЙМАН ТРИГГЕР %d (код %lu)\n", n, code);
    char m[24]; snprintf(m, sizeof(m), "RF TRIGGER %d", n);
    flashMsg(m, TFT_ORANGE);
  } else if (s[0] == 'L' && s[1] == ',') {          // L,n,code — код подсветки (родной пульт)
    int n = 0; unsigned long code = 0;
    sscanf(s + 2, "%d,%lu", &n, &code);
    snprintf(rfRxText, sizeof(rfRxText), "RF RX: pult #%d", n);
    rfRxMs = millis();
  } else if (s[0] == 'U' && s[1] == ',') {          // U,code — чужой код
    snprintf(rfRxText, sizeof(rfRxText), "RF RX: ?%s", s + 2);
    rfRxMs = millis();
  } else if (s[0] == 'X' && s[1] == ',') {          // X,n,code — со-процессор отправил
    int n = 0; unsigned long code = 0;
    sscanf(s + 2, "%d,%lu", &n, &code);
    if (n == 22) snprintf(rfTxText, sizeof(rfTxText), "HOLD ok");
    else if (n >= 1 && n <= 21) snprintf(rfTxText, sizeof(rfTxText), "#%s ok", RF_NAMES[n - 1]);
  } else if (s[0] == 'W' && s[1] == ',') {          // W,state,pct,ip — OTA со-процессора
    char ip[20] = "";
    int old = cpOta;
    sscanf(s + 2, "%d,%d,%19s", &cpOta, &cpOtaPct, ip);
    cpIp = ip;
    if (old != cpOta) refreshBtn(B_CPOTA);
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
  heap_caps_malloc_extmem_enable(64 * 1024);       // всё до 64 КБ — во внутренней RAM
  rstReason = esp_reset_reason();
  if (rstReason == ESP_RST_POWERON) bootCount = 0;
  bootCount++;
  Serial.println("\n\n===== SEBASTIAN BENCH v13: HEAD (ESP32-S3) =====");
  Serial.printf("[SYS] причина старта: %s, перезапуск #%lu\n", rstName(rstReason), (unsigned long)bootCount);

  pinMode(PIN_SPK_MUTE, OUTPUT);
  pinMode(PIN_XSMT, OUTPUT);
  pinMode(AUX_DET, INPUT_PULLUP);
  auxPlug = auxDetRaw = digitalRead(AUX_DET) == HIGH;
  applyMutes();
  pinMode(T_IRQ, INPUT_PULLUP);

  wifiBegin();                                     // Wi-Fi подключается в фоне

  tft.init();
  tft.setRotation(2);                              // перевёрнут на 180°
  tft.setTouch(calData);
  tft.fillScreen(TFT_BLACK);
  goPage(PG_HOME);

  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, 10000000);
  if (sdOk) {
    sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    fileOk = SD.exists(MP3_FILE);
    Serial.printf("[SD] OK, %llu MB, %s: %s\n", (unsigned long long)sdSizeMB, MP3_FILE, fileOk ? "есть" : "НЕТ");
  } else Serial.println("[SD] FAIL — карта FAT32? провода 6/7/15/5?");

  out = new I2SOut(I2S_NUM_1);                     // динамики: I2S1
  out->SetGain(volume);
  Serial.printf("[I2S] динамики: %s\n", out->begin() ? "OK" : "FAIL");

  micOk = initMic();
  Serial.printf("[MIC] I2S0: %s\n", micOk ? "OK" : "FAIL");
  loopMp3 = false;                                 // музыка при старте не играет
  Serial.println("[SYS] готово. Сенсоры: 1=тише 2=play/stop 3=громче");
}

unsigned long tUi = 0, tLog = 0, tPing = 0;
bool wasPlaying = false;

void loop() {
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("[MP3] трек закончился");
      if (loopMp3) startMp3();                     // по кругу
    }
  }
  if (wasPlaying != isPlaying()) { wasPlaying = isPlaying(); refreshBtn(B_PLAY); }

  wifiTick();
  if (headOtaPct >= 0) return;                     // идёт прошивка — экран и всё остальное не трогаем
  pollLink();
  pollTouch();
  micMeter();
  pollAuxDetect();
  if (hdrRestoreAt && millis() > hdrRestoreAt) { hdrRestoreAt = 0; drawHeader(); }

  if (millis() - tUi > 150) { tUi = millis(); updateHeader(); updatePage(); }
  if (millis() - tPing > 1000) { tPing = millis(); Serial1.printf("PING %lu\n", ++pingSent); }
  if (millis() - tLog > 5000) {
    tLog = millis();
    Serial.printf("[STAT] mp3:%s vol:%.2f | link %s/%s | bat:%dmV | WiFi:%s\n",
                  isPlaying() ? "play" : "stop", volume, rxOk() ? "OK" : "NET", txOk() ? "OK" : "NET", cpBat,
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "net");
  }
}