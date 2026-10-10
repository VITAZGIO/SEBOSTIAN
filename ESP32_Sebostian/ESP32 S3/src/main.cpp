// =====================================================================
//  СЕБАСТЬЯН — стендовый тест, ГОЛОВА (ESP32-S3)  v21
//  v21: страница OUTPUT — куда идёт звук: DINAMIKI / AUX / OBA (запоминается во флеше).
//       AUX без штекера -> играют динамики. Переключатель источника 74HC4053 на со-процессоре:
//       голова шлёт "S3B 1/0" (S3 сейчас звучит / молчит), со-процессор сам выбирает S3 или Bluetooth.
//  v20: 1) Включил тумблером, а аккум «сел» (со-процессор шлёт "G,mV") -> экран с кнопкой
//          VKLYUCHIT' на 5 с (или сенсор 2). Нажал -> "FORCE", работаем (для зарядки).
//          Не нажал -> спим дальше.
//       2) Лог аккума на SD: раз в секунду время + напряжение + %, только при 100-70% и 30-0%.
//          Файлы по часам: /batlog/ГГГГММДД_ЧЧ.csv. С ПК: http://<IP головы>/bat.tar
//  v19: защита аккума (вместе с со-процессором v7). Пришло "B,2" (аккум < 3.0 В) ->
//       ответ "SLEEP", звук/Wi-Fi выкл, экран «BATAREYA SELA», глубокий сон.
//       Будит: со-процессор (зарядился >= 3.55 В) или касание экрана (покажет, что сел).
//       Запасной путь: связь пропала > 30 с, а аккум был < 3.15 В -> тоже спать.
//       < 3.3 В — красная надпись LOW BAT раз в минуту, строка BAT цветная.
//  v18: страница TRAIN — запись сэмплов для обучения своего слова «Себастьян» (на SD):
//       POS x10 (слово), NEG x10 (другие слова), FON 30 s (фон), PLAY/DEL последнего.
//       Всё скачивается с ПК одним файлом: http://<IP головы>/kws.tar (веб-сервер на :80).
//  v17: пока Whisper распознаёт «Себастьян», слушатель НЕ глохнет и пишет следующую фразу.
//       «Себастьян … (пауза) … включи фикус» больше не теряет команду: если после слова уже
//       говоришь — без «дзынь», фраза сразу идёт командой. Ожидание команды 6 -> 8 с.
//  v16: WS микрофона перенесён 48 -> 4 (ПРОВОД ПЕРЕКИНУТЬ!). На GPIO48 сидит белый
//       RGB-светодиод платы (WS2812): такт мика слепил его «прожектором». Теперь 48
//       свободен и светодиод гасится при старте. Красный (питание) — железный, остаётся.
//  v15: слово-активатор «Себастьян». Задача-слушатель всё время ловит фразы (VAD — детектор
//       речи по громкости), фразу шлём в Whisper (/stt). Есть «Себастьян» в тексте:
//       «Себастьян, включи свет» -> команда сразу текстом в /ask_text_stream;
//       просто «Себастьян» -> «дзынь», следующая фраза = команда (/ask_stream).
//       Пока играет звук — не слушаем (эхоподавления нет). Кнопка WAKE — вкл/выкл.
//       Слушатель стартует через 20 с после загрузки и НЕ стартует сам после сбоя (OTA цела).
//  v14: страница VOICE — первый живой круг колонка <-> сервер Себастьяна.
//       PING = GET /health. ASK = запись с INMP441 (тап по экрану = стоп, макс 8 с)
//       -> WAV -> POST /ask_stream -> поток SSE -> куски wav играем в динамики ПО МЕРЕ
//       прихода (отдельная задача-плеер). Кнопка мика на со-процессоре = держи и говори.
//       Сервер НЕ трогаем: тот же эндпоинт, что у stream.sh.
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
#include <esp_sleep.h>
#include <Preferences.h>
#include <vector>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include "AudioOutput.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <freertos/queue.h>
#include <WebServer.h>
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
#define MIC_WS       4      // было 48: на 48 висит RGB-светодиод платы, такт мика его зажигал
#define RGB_LED      48     // WS2812 на плате — гасим при старте
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

// Батарея (напряжение меряет со-процессор, сюда приходит среднее за 10 с)
#define BAT_NONE 2500                    // ниже = аккума нет (стенд от USB)
#define BAT_LOW  3300                    // предупреждение LOW BAT
RTC_DATA_ATTR bool headBatSleep = false; // true = уснули из-за аккума (переживает глубокий сон)
extern uint32_t blLines;                 // строк лога аккума с запуска (определено ниже)

// Сервер Себастьяна (Ubuntu VM): голосовой контур sebastian-voice
#define SEB_HOST   "192.168.1.201"
#define SEB_PORT   9010
#define REC_MAX_S  8                    // максимум записи вопроса, с

// Слово «Себастьян»: детектор речи по громкости (VAD). Уровни видно внизу страницы VOICE.
#define VAD_MIN      40.0f              // ниже этого уровня — никогда не речь
#define VAD_START_K  3.0f               // речь: громче шума в 3 раза 60 мс подряд
#define VAD_END_K    2.0f               // тишина: тише шума×2 ...
#define VAD_SIL_MS   700                // ... столько мс подряд = фраза кончилась
#define VAD_MIN_SPEECH_MS 400           // короче — щелчок/стук, не шлём
#define PRE_MS       400                // сколько звука ДО начала речи прихватить
#define CMD_WAIT_MS  8000               // после «дзынь» ждём команду столько мс

// ------------------------- СОСТОЯНИЕ ---------------------------------
bool sdOk = false, fileOk = false, micOk = false;
bool spkMuted = false, auxMuted = false;
// куда выводим звук (страница OUTPUT): динамики / AUX / оба
enum { OUT_SPK, OUT_AUX, OUT_BOTH };
int outMode = OUT_BOTH;
const char *OUT_NAMES[3] = {"DINAMIKI", "AUX", "OBA"};
Preferences prefs;                       // NVS — хранилище во флеше (переживает выключение)
volatile bool testBusy = false;          // играет тест/запись (L/R, MIC, TRAIN) — для переключателя источника
int cpMux = -1;                          // что выбрал 74HC4053 на со-процессоре: 0 = S3, 1 = Bluetooth
bool spkOn(); bool auxOn();            // объявления — определены ниже (у applyMutes)
void s3BusyReport(bool force = false);
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
void pollLink();
void pageVoice();
void pageTrain();
void kwsScan();
void srvPing();
void voiceAsk(bool ptt);
void micHold();
void wakeTick();
extern bool wakeOn;
extern TaskHandle_t lsTask;
bool listenStart();
volatile bool voiceBusy = false;         // идёт разговор с Себастьяном
volatile bool pttReq = false;            // нажата кнопка мика на со-процессоре
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
  micHold();                                    // стуки не должны уйти в распознавание
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
    case ESP_RST_DEEPSLEEP: return "SLEEP WAKE";
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
enum { PG_HOME, PG_VOICE, PG_TRAIN, PG_AUDIO, PG_OUT, PG_SENS, PG_RADIO, PG_SYS, PG_COUNT };
const char *PG_NAMES[PG_COUNT] = {"HOME", "VOICE", "TRAIN", "AUDIO", "OUTPUT", "SENSORS", "RADIO", "SYSTEM"};
int page = PG_HOME;

enum {
  B_PREV = 1, B_NEXT, B_PLAY, B_VOLM, B_VOLP, B_SPK, B_AUX, B_AUXMUS, B_LR, B_MIC,
  B_LED, B_BEEP, B_CPOTA, B_REBOOT, B_WIFI, B_ASK, B_SRV, B_WAKE, B_KPOS, B_KNEG, B_KBG, B_KPLAY, B_KDEL, B_KTIP, B_OSPK, B_OAUX, B_OBOTH, B_RF = 100    // B_RF+0..20 коды, B_RF+21 HOLD
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
    case B_ASK:    return TFT_DARKGREEN;
    case B_WAKE:   return wakeOn ? TFT_DARKGREEN : 0x4208;
    case B_RF + 21: return 0x7800;
    case B_OSPK:   return outMode == OUT_SPK ? TFT_DARKGREEN : 0x3186;
    case B_OAUX:   return outMode == OUT_AUX ? TFT_DARKGREEN : 0x3186;
    case B_OBOTH:  return outMode == OUT_BOTH ? TFT_DARKGREEN : 0x3186;
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
    int x = 120 - (PG_COUNT - 1) * 7 + i * 14;
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
    case PG_VOICE: {
      addBtn(3, 234, 112, 48, B_ASK, "ASK (tap=stop)");
      addBtn(118, 234, 66, 48, B_WAKE, wakeOn ? "WAKE ON" : "WAKE off");
      addBtn(187, 234, 50, 48, B_SRV, "PING");
    } break;
    case PG_TRAIN: {
      kwsScan();
      int ids[6] = {B_KPOS, B_KNEG, B_KBG, B_KPLAY, B_KDEL, B_KTIP};
      const char *lb[6] = {"POS x10", "NEG x10", "FON 30 s", "PLAY", "DEL", "SOVET"};
      grid2(178, 32, ids, lb, 6);
    } break;
    case PG_AUDIO: {
      int ids[8] = {B_PLAY, B_MIC, B_VOLM, B_VOLP, B_SPK, B_AUX, B_AUXMUS, B_LR};
      const char *lb[8] = {"PLAY / STOP", "MIC TEST 2s", "VOL -", "VOL +", "SPK MUTE", "AUX MUTE", "AUX MUSIC", "L / R TEST"};
      grid2(112, 40, ids, lb, 8);
    } break;
    case PG_OUT: {
      addBtn(10, 120, 220, 46, B_OSPK, "DINAMIKI");
      addBtn(10, 172, 220, 46, B_OAUX, "AUX");
      addBtn(10, 224, 220, 46, B_OBOTH, "OBA");
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
uint16_t batCol() {
  if (!rxOk() || cpBat < BAT_NONE) return TFT_WHITE;
  return cpBat < BAT_LOW ? TFT_RED : cpBat < 3600 ? TFT_YELLOW : TFT_GREEN;
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
  line(7, batText(), batCol());
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
  snprintf(s, sizeof(s), "OUT:%s SPK:%s AUX:%s JACK:%s", OUT_NAMES[outMode], spkOn() ? "ON" : "OFF", auxOn() ? "ON" : "OFF",
           auxPlug ? "IN" : "--");
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
  line(2, batText(), batCol());
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

void pageOut() {
  char s[64];
  snprintf(s, sizeof(s), "VYVOD: %s", OUT_NAMES[outMode]);
  line(0, s, TFT_CYAN);
  snprintf(s, sizeof(s), "dinamiki: %s   AUX: %s", spkOn() ? "IGRAYUT" : "molchat", auxOn() ? "IGRAET" : "molchit");
  line(1, s, TFT_WHITE);
  if (outMode != OUT_SPK && !auxPlug) line(2, "shteker AUX ne vstavlen -> dinamiki", TFT_YELLOW);
  else line(2, String("shteker AUX: ") + (auxPlug ? "vstavlen" : "net"), auxPlug ? TFT_GREEN : TFT_DARKGREY);
  const char *src = !rxOk() ? "---" : cpMux == 1 ? "BLUETOOTH" : cpMux == 0 ? "S3 (golova)" : "? (staryi coproc)";
  line(3, String("istochnik: ") + src, cpMux == 1 ? TFT_CYAN : TFT_WHITE);
  line(4, (spkMuted || auxMuted) ? "vnimanie: vklyuchen MUTE na AUDIO" : "", TFT_RED);
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
  line(7, String("v21  " __DATE__ "  batlog ") + blLines, TFT_DARKGREY);
}

void updatePage() {
  switch (page) {
    case PG_HOME:  pageHome(); break;
    case PG_VOICE: pageVoice(); break;
    case PG_TRAIN: pageTrain(); break;
    case PG_AUDIO: pageAudio(); break;
    case PG_SENS:  pageSens(); break;
    case PG_RADIO: pageRadio(); break;
    case PG_OUT:   pageOut(); break;
    case PG_SYS:   pageSys(); break;
  }
}
// =====================================================================
//  ТЕСТ МИКРОФОНА: 3 с записи -> воспроизведение в динамики
// =====================================================================
void micTest() {
  if (!micOk) return;
  micHold();                                    // микрофон у слушателя «Себастьяна» — забираем
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
//  ГОЛОС: колонка <-> сервер Себастьяна (v14 ASK, v15 слово «Себастьян»)
//  запись INMP441 16 кГц -> WAV -> POST /ask_stream (multipart, поле "file")
//  ответ — SSE (поток строк "data: {...}"): stt | text | audio | error | done
//  audio.audio_b64 = кусок wav (XTTS, 24 кГц моно) -> очередь -> задача-плеер -> I2S1
// =====================================================================
String vSrv = "SRV: nazhmi PING", vState = "gotov", vYou = "", vSeb = "", vT1 = "", vT2 = "";
uint16_t vStateCol = TFT_WHITE;

// ---------- кириллица -> латиница (шрифт экрана без кириллицы) ----------
String translit(const String &u) {
  static const char *LAT[32] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
                                "r", "s", "t", "u", "f", "h", "ts", "ch", "sh", "sch", "", "y", "'", "e", "yu", "ya"};
  String o;
  const uint8_t *p = (const uint8_t *)u.c_str();
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) cp = *p++;
    else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else { p++; continue; }
    if (cp < 0x80) { o += (char)cp; continue; }
    bool up = false;
    if (cp == 0x401 || cp == 0x451) { o += (cp == 0x401) ? "Yo" : "yo"; continue; }
    if (cp >= 0x410 && cp <= 0x42F) { up = true; cp += 0x20; }
    if (cp >= 0x430 && cp <= 0x44F) {
      String t = LAT[cp - 0x430];
      if (up && t.length()) t.setCharAt(0, toupper(t[0]));
      o += t;
    } else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D || cp == 0x201E) o += '"';
    else if (cp == 0x2013 || cp == 0x2014) o += '-';
    else if (cp == 0x2026) o += "...";
    else if (cp == 0xA0) o += ' ';
    else o += '?';
  }
  return o;
}

// ---------- мини-разбор JSON (без библиотек): строка / число по ключу ----------
const char *jsonVal(const char *s, const char *key) {           // указатель на начало значения
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(s, pat);
  while (p) {
    const char *q = p + strlen(pat);
    while (*q == ' ') q++;
    if (*q == ':') { q++; while (*q == ' ') q++; return q; }
    p = strstr(q, pat);
  }
  return nullptr;
}
bool jsonNum(const char *s, const char *key, float &v) {
  const char *q = jsonVal(s, key);
  if (!q) return false;
  v = strtof(q, nullptr);
  return true;
}
static void putUtf8(String &o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
bool jsonStr(const char *s, const char *key, String &o) {       // понимает \n \" \\ \uXXXX (ensure_ascii)
  const char *q = jsonVal(s, key);
  if (!q || *q != '"') return false;
  q++;
  o = "";
  while (*q && *q != '"') {
    if (*q != '\\') { o += *q++; continue; }
    q++;
    switch (*q) {
      case 'n': case 'r': case 't': o += ' '; q++; break;
      case 'u': {
        char h[5] = {0}; for (int i = 0; i < 4 && q[1 + i]; i++) h[i] = q[1 + i];
        uint32_t cp = strtoul(h, nullptr, 16);
        q += 5;
        if (cp >= 0xD800 && cp <= 0xDFFF) { o += '?'; if (q[0] == '\\' && q[1] == 'u') q += 6; }  // эмодзи и т.п.
        else putUtf8(o, cp);
      } break;
      default: if (*q) o += *q++; break;
    }
  }
  return true;
}

// ---------- base64 -> байты (на месте, выход короче входа) ----------
static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
size_t b64decodeInPlace(char *s, size_t n) {
  uint8_t *o = (uint8_t *)s;
  size_t w = 0; uint32_t acc = 0; int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64v(s[i]);
    if (v < 0) continue;                           // '=' и мусор пропускаем
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; o[w++] = (acc >> bits) & 0xFF; }
  }
  return w;
}

// ---------- задача-плеер: играет куски PCM из очереди, пока мы читаем сеть ----------
struct PcmJob { int16_t *pcm; size_t n; int rate; };
QueueHandle_t pcmQ = nullptr;
volatile bool plBusy = false;                      // плеер сейчас играет кусок
volatile unsigned long plFirstMs = 0, plLastMs = 0; // первый звук / конец последнего куска
volatile int plGaps = 0, plPieces = 0;
int plRate = 0;

void playerTask(void *) {
  static int16_t st[512];                          // 256 стерео-кадров
  PcmJob j;
  for (;;) {
    if (xQueueReceive(pcmQ, &j, portMAX_DELAY) != pdTRUE) continue;
    plBusy = true;
    if (j.rate != plRate) { plRate = j.rate; i2s_set_sample_rates(I2S_NUM_1, plRate); }
    // DMA держит 16×256 кадров; если кусок пришёл позже, чем это доиграло, — слышна пауза
    unsigned long dmaMs = 16UL * 256UL * 1000UL / (unsigned long)plRate;
    if (plPieces > 0 && millis() - plLastMs > dmaMs) plGaps++;
    float g = volume;
    for (size_t i = 0; i < j.n; i += 256) {
      size_t k = min((size_t)256, j.n - i);
      for (size_t m = 0; m < k; m++) {
        int32_t v = (int32_t)(j.pcm[i + m] * g);
        int16_t s = (int16_t)constrain(v, -32768, 32767);
        st[2 * m] = s; st[2 * m + 1] = s;          // моно -> оба динамика
      }
      size_t bw;
      i2s_write(I2S_NUM_1, st, k * 4, &bw, portMAX_DELAY);
      if (!plFirstMs) plFirstMs = millis();
    }
    free(j.pcm);
    plPieces++;
    plLastMs = millis();
    plBusy = false;
  }
}

bool playerStart() {
  if (pcmQ) return true;
  pcmQ = xQueueCreate(24, sizeof(PcmJob));
  if (!pcmQ) return false;
  return xTaskCreatePinnedToCore(playerTask, "seb_player", 4096, nullptr, 3, nullptr, 1) == pdPASS;
}

// кусок wav (после base64) -> PCM в PSRAM -> в очередь плеера
bool queueWav(const uint8_t *w, size_t n) {
  int rate = 24000;                                // XTTS отдаёт 24 кГц моно 16 бит
  const uint8_t *data = w; size_t dlen = n;
  if (n > 44 && !memcmp(w, "RIFF", 4) && !memcmp(w + 8, "WAVE", 4)) {
    size_t p = 12; data = nullptr;
    int ch = 1, bits = 16;
    while (p + 8 <= n) {
      uint32_t sz = w[p + 4] | (w[p + 5] << 8) | (w[p + 6] << 16) | ((uint32_t)w[p + 7] << 24);
      if (!memcmp(w + p, "fmt ", 4)) {
        ch = w[p + 10] | (w[p + 11] << 8);
        rate = w[p + 12] | (w[p + 13] << 8) | (w[p + 14] << 16) | ((uint32_t)w[p + 15] << 24);
        bits = w[p + 22] | (w[p + 23] << 8);
      } else if (!memcmp(w + p, "data", 4)) {
        data = w + p + 8; dlen = min((size_t)sz, n - p - 8); break;
      }
      p += 8 + sz + (sz & 1);
    }
    if (!data || bits != 16) { Serial.printf("[VOICE] wav не понял: bits=%d\n", bits); return false; }
    if (ch == 2) {                                 // на всякий случай: стерео -> моно
      size_t fr = dlen / 4;
      int16_t *pcm = (int16_t *)heap_caps_malloc(fr * 2 + 2, MALLOC_CAP_SPIRAM);
      if (!pcm) return false;
      const int16_t *s = (const int16_t *)data;
      for (size_t i = 0; i < fr; i++) pcm[i] = (s[2 * i] + s[2 * i + 1]) / 2;
      PcmJob j = {pcm, fr, rate};
      return xQueueSend(pcmQ, &j, pdMS_TO_TICKS(5000)) == pdTRUE;
    }
  }
  size_t ns = dlen / 2;
  int16_t *pcm = (int16_t *)heap_caps_malloc(ns * 2 + 2, MALLOC_CAP_SPIRAM);
  if (!pcm) { Serial.println("[VOICE] нет PSRAM под кусок"); return false; }
  memcpy(pcm, data, ns * 2);
  PcmJob j = {pcm, ns, rate};
  return xQueueSend(pcmQ, &j, pdMS_TO_TICKS(5000)) == pdTRUE;
}

// ---------- чтение ответа сервера: обычный или chunked ----------
struct NetRd {
  WiFiClient *c; bool chunked = false; long left = 0; bool eof = false; bool timeout = false;
  uint8_t buf[2048]; int pos = 0, len = 0;
  unsigned long waitMs = 90000;                    // первый ответ после простоя ~25 с (холодный Ollama)
  void reset(WiFiClient *cl) { c = cl; chunked = false; left = 0; eof = false; timeout = false; pos = len = 0; }
  int raw() {
    if (pos < len) return buf[pos++];
    unsigned long t0 = millis();
    while (!c->available()) {
      if (!c->connected()) { eof = true; return -1; }
      if (millis() - t0 > waitMs) { timeout = true; eof = true; return -1; }
      pollLink();
      delay(2);
    }
    len = c->read(buf, sizeof(buf)); pos = 0;
    if (len <= 0) { eof = true; return -1; }
    return buf[pos++];
  }
  int get() {
    if (!chunked) return raw();
    if (left == 0) {
      char h[16]; int hl = 0, b;
      while ((b = raw()) >= 0 && b != '\n') if (b != '\r' && hl < 15) h[hl++] = b;
      h[hl] = 0;
      if (b < 0) return -1;
      left = strtol(h, nullptr, 16);
      if (left == 0) { eof = true; return -1; }
    }
    int b = raw();
    if (b >= 0 && --left == 0) { raw(); raw(); }   // \r\n после куска
    return b;
  }
  // строка без \r\n; -1 = конец потока, -2 = не влезла в буфер
  long line(char *o, size_t cap) {
    size_t n = 0; int b;
    bool over = false;
    while ((b = get()) >= 0 && b != '\n') {
      if (b == '\r') continue;
      if (n < cap - 1) o[n++] = b; else over = true;
    }
    o[n] = 0;
    if (b < 0 && n == 0) return -1;
    return over ? -2 : (long)n;
  }
};

void vSet(const String &st, uint16_t col);
// ---------- экран VOICE ----------
void wrapLines(const String &txt, int first, int count, uint16_t col) {
  String rest = txt;
  for (int i = 0; i < count; i++) {
    String part = rest;
    if ((int)rest.length() > 34) {
      int cut = rest.lastIndexOf(' ', 34);
      if (cut < 10) cut = 34;
      part = rest.substring(0, cut);
      rest = rest.substring(cut); rest.trim();
    } else rest = "";
    if (i == count - 1 && rest.length()) part = part.substring(0, 31) + "...";
    line(first + i, part, col);
  }
}

// ---------- слушатель: ждём слово «Себастьян» ----------
// Задача постоянно читает микрофон, считает громкость кусками по 20 мс и ловит фразы:
// громче шума ×VAD_START_K 60 мс подряд = начало, тише шума ×VAD_END_K VAD_SIL_MS = конец.
// Готовую фразу loop() шлёт в Whisper (/stt); если в тексте есть «Себастьян» — это к нам.
TaskHandle_t lsTask = nullptr;
volatile bool lsPause = true, lsParked = true;
volatile int lsState = 0;                          // 0 ждём речь, 1 идёт фраза, 2 фраза готова
volatile int lsLen = 0;
volatile unsigned long lsEndMs = 0;                // когда реально кончилась речь
volatile float lsNf = VAD_MIN, lsLvl = 0;          // шум (адаптивный) и текущий уровень
enum { LM_WAKE, LM_CMD };
volatile int lsMode = LM_WAKE;                     // CMD = после «Себастьян» без команды ждём команду
unsigned long cmdDeadline = 0;
bool wakeOn = true;
String vHeard = "";
int16_t *lsSeg = nullptr, *lsRing = nullptr;
const int LS_RING = 16000 * PRE_MS / 1000;        // кусок ДО начала речи (чтобы не съесть «Се...»)
const int LS_SEG = 16000 * REC_MAX_S;

void listenTask(void *) {
  static int32_t raw[320];                         // 20 мс
  float hp = 0, prev = 0;
  int ringPos = 0, above = 0, silMs = 0, spMs = 0, n = 0;
  bool wasParked = true;
  for (;;) {
    if (lsPause) { lsParked = true; wasParked = true; vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    lsParked = false;
    size_t br = 0;
    if (wasParked) {                               // после паузы: слить старое (эхо ответа) и начать заново
      for (int k = 0; k < 16; k++) i2s_read(MIC_PORT, raw, sizeof(raw), &br, 0);
      wasParked = false; lsState = 0; above = 0;
    }
    i2s_read(MIC_PORT, raw, sizeof(raw), &br, pdMS_TO_TICKS(100));
    int cnt = br / 4;
    if (cnt <= 0 || lsState == 2) continue;        // фраза ждёт, пока loop() её заберёт
    double sq = 0; int32_t pk = 0;
    for (int i = 0; i < cnt; i++) {
      int16_t v = (int16_t)micTo16(raw[i]);
      float x = v; hp = 0.954f * (hp + x - prev); prev = x; sq += (double)hp * hp;
      if (abs(v) > pk) pk = abs(v);
      if (lsState == 0) { lsRing[ringPos] = v; ringPos = (ringPos + 1) % LS_RING; }
      else if (n < LS_SEG) lsSeg[n++] = v;
    }
    int fms = cnt * 1000 / 16000;
    float rms = sqrtf(sq / cnt), nf = lsNf;
    lsLvl = rms;
    micLevel = max((float)pk, micLevel * 0.85f);   // полоска MIC на AUDIO живёт отсюда
    if (lsState == 0) {
      if (rms > max(nf * VAD_START_K, VAD_MIN)) {
        if (++above >= 3) {                        // 60 мс громко -> фраза
          n = 0;
          for (int i = 0; i < LS_RING; i++) lsSeg[n++] = lsRing[(ringPos + i) % LS_RING];
          lsState = 1; silMs = 0; spMs = above * fms;
        }
      } else {
        above = 0;
        lsNf = max(nf * 0.97f + rms * 0.03f, 5.0f); // шум подстраивается только в тишине
      }
    } else if (lsState == 1) {
      spMs += fms;
      if (rms < max(nf * VAD_END_K, VAD_MIN * 0.7f)) silMs += fms; else silMs = 0;
      if (n >= LS_SEG) {                           // 8 с без паузы — это не фраза, а шум: поднимаем порог
        lsNf = max(nf, rms) * 1.3f; lsState = 0; above = 0;
      } else if (silMs >= VAD_SIL_MS) {
        if (spMs - silMs >= VAD_MIN_SPEECH_MS) { lsLen = n; lsEndMs = millis() - silMs; lsState = 2; }
        else { lsState = 0; above = 0; }           // щелчок / стук — не речь
      }
    }
  }
}

bool listenStart() {
  if (lsTask) return true;
  lsSeg = (int16_t *)heap_caps_malloc(LS_SEG * 2, MALLOC_CAP_SPIRAM);
  lsRing = (int16_t *)heap_caps_calloc(LS_RING, 2, MALLOC_CAP_SPIRAM);
  if (!lsSeg || !lsRing) { free(lsSeg); free(lsRing); lsSeg = lsRing = nullptr; return false; }
  lsPause = true;
  bool ok = xTaskCreatePinnedToCore(listenTask, "seb_listen", 4096, nullptr, 2, &lsTask, 1) == pdPASS;
  Serial.printf("[WAKE] слушатель: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

void micHold() {                                   // забрать микрофон у слушателя (MIC TEST, ASK)
  lsPause = true;
  unsigned long t = millis();
  while (lsTask && !lsParked && millis() - t < 400) delay(5);
}

// ---------- WAV и сеть ----------
size_t buildWav(uint8_t *wav, int n) {             // wav+44 уже содержит n сэмплов 16 кГц
  int16_t *rec = (int16_t *)(wav + 44);
  // фильтр ~120 Гц + усиление по RMS + мягкий лимитер (для Whisper)
  float hp = 0, prevIn = 0; double sq = 0;
  for (int i = 0; i < n; i++) { float x = rec[i]; hp = 0.954f * (hp + x - prevIn); prevIn = x; rec[i] = (int16_t)constrain((int)hp, -32768, 32767); sq += (double)hp * hp; }
  float g = 3000.0f / (sqrtf(sq / n) + 1);
  g = constrain(g, 1.0f, 30.0f);
  for (int i = 0; i < n; i++) rec[i] = (int16_t)(tanhf(rec[i] * g / 32767.0f) * 30000.0f);
  uint32_t dataB = n * 2, riff = 36 + dataB, sr = 16000, byteRate = 32000;
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                   0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
  memcpy(h + 4, &riff, 4); memcpy(h + 24, &sr, 4); memcpy(h + 28, &byteRate, 4); memcpy(h + 40, &dataB, 4);
  memcpy(wav, h, 44);
  return 44 + dataB;
}

// POST multipart (поле "file") — тело: head + wav + tail
bool postWav(WiFiClient &cl, const char *path, const uint8_t *wav, size_t wavLen, const char *accept) {
  if (!cl.connect(SEB_HOST, SEB_PORT, 3000)) return false;
  cl.setNoDelay(true);
  const char *B = "----sebastianS3";
  String head = String("--") + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"q.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + B + "--\r\n";
  size_t clen = head.length() + wavLen + tail.length();
  cl.printf("POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %u\r\nAccept: %s\r\nConnection: close\r\n\r\n", path, SEB_HOST, SEB_PORT, B, (unsigned)clen, accept);
  cl.print(head);
  for (size_t off = 0; off < wavLen && cl.connected();) {
    size_t w = cl.write(wav + off, min((size_t)4096, wavLen - off));
    if (w == 0) { delay(2); continue; }
    off += w;
  }
  cl.print(tail);
  return true;
}

// читает статус и заголовки; возвращает HTTP-код (или -1)
int readHead(NetRd &rd, char *buf) {
  long L = rd.line(buf, 256);
  if (L < 0 || strncmp(buf, "HTTP/1.", 7) != 0) return -1;
  int code = atoi(buf + 9);
  while ((L = rd.line(buf, 512)) > 0) {
    String hl = buf; hl.toLowerCase();
    if (hl.startsWith("transfer-encoding:") && hl.indexOf("chunked") > 0) rd.chunked = true;
  }
  return code;
}

static char *lbuf = nullptr;                       // строка SSE: кусок wav в base64 бывает 300+ КБ
const size_t LCAP = 2 * 1024 * 1024;

// ---------- приём ответа (SSE) и игра по кусочкам — общий для голоса и текста ----------
// tRef = момент конца речи; tUp = сколько заняла отправка; espStt = распознавание на нашей стороне (для wake)
void streamAnswer(WiFiClient &cl, unsigned long tRef, float recS, unsigned long tUp, float espStt) {
  float sStt = -1, sBrain = -1, sAudio1 = -1, sTotal = -1;
  int chunks = 0, recvPieces = 0;
  bool ok = false;
  String err = "";
  static NetRd rd;                                 // static: 2 КБ буфер не на стеке loop()
  rd.reset(&cl);
  vSet("DUMAET... (1-y raz posle prostoya ~25 s)", TFT_YELLOW);
  int code = readHead(rd, lbuf);
  if (code < 0) err = rd.timeout ? "server molchit (timeout)" : "plohoy otvet servera";
  else if (code != 200) { rd.line(lbuf, 200); err = "HTTP " + String(code) + " " + String(lbuf).substring(0, 20); }
  else {
    while (true) {
      long L = rd.line(lbuf, LCAP);
      if (L == -1) break;
      if (L == -2) { err = "stroka SSE ne vlezla"; continue; }
      if (strncmp(lbuf, "data:", 5) != 0) continue;
      const char *js = lbuf + 5;
      String type; jsonStr(js, "type", type);
      if (type == "stt") {
        jsonStr(js, "text", vYou); vYou = translit(vYou); jsonNum(js, "t", sStt);
        vSet("DUMAET...", TFT_YELLOW);
      } else if (type == "text") {
        jsonStr(js, "text", vSeb); vSeb = translit(vSeb); jsonNum(js, "t", sBrain);
        float c; if (jsonNum(js, "chunks", c)) chunks = (int)c;
        vSet("GOVORIT...", TFT_GREEN);
      } else if (type == "audio") {
        if (sAudio1 < 0) jsonNum(js, "t_audio", sAudio1);
        if (!vSeb.length()) { String t; jsonStr(js, "text", t); vSeb = translit(t); }
        const char *q = jsonVal(js, "audio_b64");
        if (q && *q == '"') {
          char *b = (char *)q + 1;
          char *e = strchr(b, '"');
          if (e) {
            size_t bytes = b64decodeInPlace(b, e - b);
            if (queueWav((uint8_t *)b, bytes)) recvPieces++;
          }
        }
        vSet("GOVORIT... kusok " + String(recvPieces) + (chunks ? "/" + String(chunks) : String("")), TFT_GREEN);
      } else if (type == "error") {
        String m; jsonStr(js, "message", m); err = translit(m);
      } else if (type == "done") {
        jsonNum(js, "total", sTotal);
        ok = true;
        break;
      }
    }
    if (!ok && err == "") err = rd.timeout ? "server zamolchal (timeout)" : "obryv svyazi";
  }
  cl.stop();

  // дождаться, пока плеер доиграет всё
  unsigned long tw = millis();
  while ((uxQueueMessagesWaiting(pcmQ) > 0 || plBusy) && millis() - tw < 120000) { pollLink(); delay(10); }
  delay(250);                                      // хвост из DMA
  i2s_zero_dma_buffer(I2S_NUM_1);

  float first = plFirstMs ? (plFirstMs - tRef) / 1000.0f : -1;
  float all = plLastMs ? (plLastMs - tRef) / 1000.0f : -1;
  char m[64];
  snprintf(m, sizeof(m), "ZAP %.1f|1y ZVUK %.1f|VSE %.1f", recS, first, all);
  vT1 = m;
  if (espStt >= 0) snprintf(m, sizeof(m), "stt %.1f mozg %.1f zvuk %.1f", espStt, sBrain, sAudio1);
  else snprintf(m, sizeof(m), "stt %.1f mozg %.1f zvuk %.1f up %.1f", sStt, sBrain, sAudio1, tUp / 1000.0f);
  vT2 = m;
  if (err.length()) vSet("OSHIBKA: " + err, TFT_RED);
  else { snprintf(m, sizeof(m), "GOTOVO: kuskov %d, pauz %d", plPieces, plGaps); vSet(m, plGaps ? TFT_YELLOW : TFT_GREEN); }
  Serial.printf("[VOICE] %s | %s | %s\n", vState.c_str(), vT1.c_str(), vT2.c_str());
  plFirstMs = 0; plLastMs = 0; plGaps = 0; plPieces = 0;
}

// голос -> /ask_stream (сервер сам распознаёт)
void askAudio(const uint8_t *wav, size_t wavLen, unsigned long tRef, float recS) {
  vSet("OTPRAVKA na server...", TFT_YELLOW);
  WiFiClient cl;
  if (!postWav(cl, "/ask_stream", wav, wavLen, "text/event-stream")) { vSet("OSHIBKA: net svyazi s " SEB_HOST, TFT_RED); return; }
  streamAnswer(cl, tRef, recS, millis() - tRef, -1);
}

// текст -> /ask_text_stream (после «Себастьян, ...» — Whisper уже отработал)
String jsonEsc(const String &s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) o += ' ';
    else o += (char)c;
  }
  return o;
}
void askText(const String &text, unsigned long tRef, float recS, float espStt) {
  WiFiClient cl;
  if (!cl.connect(SEB_HOST, SEB_PORT, 3000)) { vSet("OSHIBKA: net svyazi s " SEB_HOST, TFT_RED); return; }
  String body = "{\"text\":\"" + jsonEsc(text) + "\"}";
  cl.printf("POST /ask_text_stream HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
            "Content-Length: %u\r\nAccept: text/event-stream\r\nConnection: close\r\n\r\n", SEB_HOST, SEB_PORT, (unsigned)body.length());
  cl.print(body);
  streamAnswer(cl, tRef, recS, 0, espStt);
}

// голос -> /stt -> текст (для поиска слова «Себастьян»)
String sttRequest(const uint8_t *wav, size_t wavLen, int &code) {
  WiFiClient cl;
  code = -1;
  if (!postWav(cl, "/stt", wav, wavLen, "application/json")) return "";
  static NetRd rd;
  rd.reset(&cl);
  rd.waitMs = 20000;
  code = readHead(rd, lbuf);
  String body = "";
  if (code > 0) {
    long L;
    while ((L = rd.line(lbuf, 8192)) >= 0 || L == -2) { body += lbuf; body += ' '; if (body.length() > 6000) break; }
  }
  cl.stop();
  rd.waitMs = 90000;
  body.trim();
  if (code != 200) return body;
  String t;
  if (body.startsWith("{") && jsonStr(body.c_str(), "text", t)) return t;   // {"text": "..."}
  if (body.startsWith("\"")) { body.remove(0, 1); if (body.endsWith("\"")) body.remove(body.length() - 1); }
  return body;
}

// есть ли в фразе «Себастьян»; cmd = всё остальное (сама команда)
bool splitWake(const String &txt, String &cmd) {
  static const char *KEYS[] = {"sebas", "sebos", "sevas", "sebus", "sabas", "sebes"};
  cmd = "";
  bool found = false;
  int start = 0;
  String s = txt + " ";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] != ' ') continue;
    String w = s.substring(start, i);
    start = i + 1;
    if (!w.length()) continue;
    String t = translit(w); t.toLowerCase();
    String letters = "";
    for (unsigned k = 0; k < t.length(); k++) if (isalpha((uint8_t)t[k])) letters += t[k];
    bool isKey = false;
    if (!found) for (auto k : KEYS) if (letters.startsWith(k)) isKey = true;
    if (isKey) { found = true; continue; }
    cmd += w + " ";
  }
  // убрать знаки по краям: «Себастьян, включи свет.» -> «включи свет»
  cmd.trim();
  while (cmd.length() && strchr(",.!?-:; ", cmd[0])) cmd.remove(0, 1);
  while (cmd.length() && strchr(",.!?-:; ", cmd[cmd.length() - 1])) cmd.remove(cmd.length() - 1);
  int letters = 0;
  for (unsigned k = 0; k < cmd.length(); k++) if (isalpha((uint8_t)cmd[k]) || (uint8_t)cmd[k] >= 0x80) letters++;
  if (letters < 2) cmd = "";
  return found;
}

void queueBeep() {                                 // «дзынь» — услышал, говори команду
  const int R = 24000, n1 = R * 9 / 100, gap = R * 4 / 100, n2 = R * 12 / 100, N = n1 + gap + n2;
  int16_t *p = (int16_t *)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
  if (!p) return;
  for (int i = 0; i < N; i++) {
    float f = i < n1 ? 660 : (i < n1 + gap ? 0 : 990);
    int k = i < n1 ? i : i - n1 - gap, len = i < n1 ? n1 : n2;
    float env = (k < 200) ? k / 200.0f : (k > len - 200 ? (len - k) / 200.0f : 1.0f);
    p[i] = f > 0 ? (int16_t)(20000 * env * sinf(2 * PI * f * k / R)) : 0;
  }
  PcmJob j = {p, (size_t)N, R};
  if (xQueueSend(pcmQ, &j, pdMS_TO_TICKS(500)) != pdTRUE) free(p);
}

void pageVoice() {
  line(0, vSrv, vSrv.startsWith("SRV: OK") ? TFT_GREEN : (vSrv.indexOf("OSHIB") >= 0 ? TFT_RED : TFT_WHITE));
  line(1, vState, vStateCol);
  wrapLines(vYou.length() ? "TY: " + vYou : String("TY: -"), 2, 2, TFT_CYAN);
  wrapLines(vSeb.length() ? "SEB: " + vSeb : String("SEB: -"), 4, 3, TFT_YELLOW);
  wrapLines(vHeard, 7, 1, TFT_DARKGREY);
  line(8, vT1, TFT_GREEN);
  line(9, vT2, TFT_DARKGREY);
  // статус слушателя + уровни (для настройки порогов VAD_*)
  String ls; uint16_t lc = TFT_WHITE;
  if (!wakeOn) { ls = "WAKE: vykl"; lc = TFT_DARKGREY; }
  else if (!lsTask) { ls = badReset() ? "WAKE: posle sboya - nazhmi WAKE" : "WAKE: start cherez 20 s..."; lc = TFT_DARKGREY; }
  else if (lsPause) { ls = "WAKE: pauza (zvuk/zanyat)"; lc = TFT_DARKGREY; }
  else if (lsState == 1) { ls = "WAKE: RECH... zapisyvayu"; lc = TFT_RED; }
  else if (lsState == 2) { ls = "WAKE: raspoznayu..."; lc = TFT_YELLOW; }
  else if (lsMode == LM_CMD) { ls = "WAKE: SLUSHAYU KOMANDU"; lc = TFT_CYAN; }
  else ls = "WAKE: zhdu 'Sebastyan'";
  line(10, ls, lc);
  char s[48];
  snprintf(s, sizeof(s), "shum %d  uroven %d  porog %d", (int)lsNf, (int)lsLvl, (int)max(lsNf * VAD_START_K, VAD_MIN));
  line(11, lsTask ? String(s) : String(""), TFT_DARKGREY);
}

void vSet(const String &st, uint16_t col) {       // статус + сразу на экран (мы в долгой функции)
  vState = st; vStateCol = col;
  if (page == PG_VOICE) { updateHeader(); pageVoice(); }
}

// ---------- PING: GET /health ----------
void srvPing() {
  if (WiFi.status() != WL_CONNECTED) { vSrv = "SRV: OSHIBKA - net WiFi"; return; }
  vSrv = "SRV: ping..."; if (page == PG_VOICE) pageVoice();
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  unsigned long t0 = millis();
  http.begin("http://" SEB_HOST ":" + String(SEB_PORT) + "/health");
  int code = http.GET();
  unsigned long dt = millis() - t0;
  if (code == 200) {
    String b = http.getString();
    String model;
    jsonStr(b.c_str(), "model", model);
    vSrv = "SRV: OK " + String(dt) + "ms  " + model;
  } else vSrv = "SRV: OSHIBKA " + String(code) + " " + http.errorToString(code);
  http.end();
  Serial.printf("[VOICE] /health -> %d за %lu мс\n", code, dt);
}

bool voicePrep() {                                 // общее: плеер, буфер строк, Wi-Fi
  if (WiFi.status() != WL_CONNECTED) { vSet("OSHIBKA: net WiFi", TFT_RED); return false; }
  if (!playerStart()) { vSet("OSHIBKA: pleer ne startoval", TFT_RED); return false; }
  if (!lbuf) lbuf = (char *)heap_caps_malloc(LCAP, MALLOC_CAP_SPIRAM);
  if (!lbuf) { vSet("OSHIBKA: net PSRAM", TFT_RED); return false; }
  plRate = 0;                                      // MP3/тесты могли сменить частоту I2S — выставим заново
  return true;
}

// ---------- кнопка ASK / кнопка мика: вопрос голосом -> ответ голосом ----------
void voiceAsk(bool ptt) {
  if (voiceBusy || !micOk) return;
  if (!voicePrep()) return;
  const int SR = 16000, NMAX = SR * REC_MAX_S;
  uint8_t *wav = (uint8_t *)heap_caps_malloc(44 + NMAX * 2, MALLOC_CAP_SPIRAM);
  if (!wav) { vSet("OSHIBKA: net PSRAM", TFT_RED); return; }

  voiceBusy = true; s3BusyReport();   // 74HC4053 -> S3
  micHold();                                       // микрофон — наш, слушатель ждёт
  loopMp3 = false; stopMp3();
  lsMode = LM_WAKE;
  if (page != PG_VOICE) goPage(PG_VOICE);
  vYou = ""; vSeb = ""; vT1 = ""; vT2 = "";

  if (!ptt) {                                      // дождаться, пока палец отпустит кнопку ASK
    unsigned long hi = millis(), t0 = millis();
    while (millis() - hi < 150 && millis() - t0 < 3000) { if (digitalRead(T_IRQ) == LOW) hi = millis(); delay(5); }
  }
  int16_t *rec = (int16_t *)(wav + 44);
  static int32_t raw[256];
  size_t br;
  for (int k = 0; k < 16; k++) i2s_read(MIC_PORT, raw, sizeof(raw), &br, 0);   // слить старое
  vSet(ptt ? "ZAPIS... (otpusti knopku = stop)" : "ZAPIS... govori (tap = stop)", TFT_RED);
  unsigned long tRec0 = millis(), tShow = 0;
  int n = 0;
  while (n < NMAX) {
    i2s_read(MIC_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    for (int i = 0; i < (int)(br / 4) && n < NMAX; i++) rec[n++] = (int16_t)micTo16(raw[i]);
    unsigned long el = millis() - tRec0;
    if (millis() - tShow > 300) {
      tShow = millis();
      char m[40]; snprintf(m, sizeof(m), "ZAPIS %.1f s  (maks %d)", el / 1000.0f, REC_MAX_S);
      vSet(m, TFT_RED);
    }
    if (ptt) { pollLink(); if (el > 300 && !cpMicBtn) break; if (!rxOk()) break; }
    else if (el > 500 && digitalRead(T_IRQ) == LOW) break;
  }
  unsigned long tRecEnd = millis();
  if (n < SR / 2) { free(wav); voiceBusy = false; vSet("slishkom korotko (<0.5 s)", TFT_YELLOW); return; }
  size_t wavLen = buildWav(wav, n);
  askAudio(wav, wavLen, tRecEnd, n / (float)SR);
  free(wav);
  voiceBusy = false;
  drawHeader();
}

// ---------- фраза от слушателя: ищем «Себастьян» ----------
void wakeProcess() {
  int len = lsLen, mode = lsMode;
  unsigned long tEnd = lsEndMs;
  float recS = len / 16000.0f;
  if (!voicePrep()) { lsState = 0; return; }
  uint8_t *wav = (uint8_t *)heap_caps_malloc(44 + len * 2, MALLOC_CAP_SPIRAM);
  if (!wav) { lsState = 0; return; }
  memcpy(wav + 44, lsSeg, len * 2);
  lsState = 0;                                     // слушатель на паузе (voiceBusy), буфер свободен
  voiceBusy = true; s3BusyReport();   // 74HC4053 -> S3
  size_t wl = buildWav(wav, len);

  if (mode == LM_CMD) {                            // после «дзынь»: это сама команда
    lsMode = LM_WAKE;
    vYou = ""; vSeb = ""; vT1 = ""; vT2 = "";
    askAudio(wav, wl, tEnd, recS);
  } else {
    int code;
    unsigned long t0 = millis();
    lsPause = false;                               // фраза уже скопирована — слушатель пишет дальше, пока ждём Whisper
    String txt = sttRequest(wav, wl, code);
    float sttS = (millis() - t0) / 1000.0f;
    if (code != 200) vHeard = "STT: HTTP " + String(code) + " " + translit(txt).substring(0, 20);
    else {
      vHeard = "slyshal: " + translit(txt);
      Serial.printf("[WAKE] %.1f с речи, stt %.1f с: %s\n", recS, sttS, txt.c_str());
      String cmd;
      if (splitWake(txt, cmd)) {
        loopMp3 = false; stopMp3();
        if (page != PG_VOICE) goPage(PG_VOICE);
        if (cmd.length()) {                        // «Себастьян, включи свет» — сразу в мозг текстом
          micHold();                               // сейчас будет ответ — не слушаем (эхо)
          vYou = translit(cmd); vSeb = ""; vT1 = ""; vT2 = "";
          askText(cmd, tEnd, recS, sttS);
        } else if (lsState == 1 || lsState == 2) {  // «Себастьян» … и уже говоришь дальше — это и есть команда
          lsMode = LM_CMD;
          cmdDeadline = millis() + CMD_WAIT_MS;
          vSet("SLUSHAYU komandu...", TFT_CYAN);
        } else {                                   // просто «Себастьян» и тишина — дзынь и ждём команду
          micHold();
          queueBeep();
          unsigned long tw = millis();
          while ((uxQueueMessagesWaiting(pcmQ) > 0 || plBusy) && millis() - tw < 2000) delay(10);
          delay(150);
          i2s_zero_dma_buffer(I2S_NUM_1);
          plFirstMs = 0; plLastMs = 0; plGaps = 0; plPieces = 0;
          lsMode = LM_CMD;
          cmdDeadline = millis() + CMD_WAIT_MS;
          vSet("DA, SER? govori komandu", TFT_CYAN);
        }
      }
    }
  }
  free(wav);
  voiceBusy = false;
  drawHeader();
}

// вызывается из loop(): запуск слушателя, паузы, обработка готовых фраз
void wakeTick() {
  if (!lsTask && wakeOn && micOk && millis() > 20000 && !badReset()) listenStart();
  if (!lsTask) return;
  bool hold = !wakeOn || voiceBusy || isPlaying() || plBusy || (pcmQ && uxQueueMessagesWaiting(pcmQ) > 0) ||
              cpBt == 2 || cpAuxMusic || headOtaPct >= 0 || WiFi.status() != WL_CONNECTED;
  lsPause = hold;                                  // без эхоподавления: пока играет звук — не слушаем
  if (lsState == 2) { lsPause = true; wakeProcess(); return; }
  if (lsMode == LM_CMD && lsState == 0 && !hold && millis() > cmdDeadline) {
    lsMode = LM_WAKE;
    vSet("komandu ne uslyshal", TFT_YELLOW);
  }
}

// =====================================================================
//  TRAIN (v18): запись сэмплов для обучения слова «Себастьян» прямо на плате
//  /kws/pos/p_NNNN.wav — «Себастьян», /kws/neg/n_NNNN.wav — другие слова,
//  /kws/bg/b_NNNN.wav — фон 30 с. 16 кГц, 16 бит, моно, СЫРОЙ звук (без усиления и
//  фильтров) — ровно такой, какой потом будет слушать модель на плате.
//  Скачать всё одним файлом с ПК: http://<IP головы>/kws.tar
// =====================================================================
#define KWS_CLIP_MS  2000               // длина одного сэмпла
#define KWS_BG_S     30                 // длина записи фона
#define KWS_SERIES   10                 // сэмплов за одно нажатие

int kwsPos = 0, kwsNeg = 0, kwsBg = 0;          // сколько файлов
int kwsPosMax = 0, kwsNegMax = 0, kwsBgMax = 0; // последний номер
String kwsLast = "", kwsState = "vyberi: POS / NEG / FON", kwsInfo = "";
uint16_t kwsStateCol = TFT_WHITE;
int kwsTip = -1;
void kwsSet(const String &s, uint16_t col);

// похожие слова и обычная речь — чтобы модель НЕ срабатывала на них
const char *NEG_WORDS[] = {
  "Sevastopol'", "sebestoimost'", "bastion", "Kristian", "Stepan", "Stanislav",
  "Svyatoslav", "basseyn", "sebe", "sebya", "segodnya", "Seva, stoy",
  "Tat'yana", "vse yasno", "vklyuchi svet", "kak dela", "Sergey", "sebe stanu",
  "bastard", "Sabina", "est' chto poest'", "lyuboe slovo"
};
const int NEG_N = sizeof(NEG_WORDS) / sizeof(NEG_WORDS[0]);

const char *KWS_TIPS[] = {
  "POS: 100+ raz. Normal'no, tiho, gromko",
  "POS: blizko, 1 m, 2-3 m, s raznyh storon",
  "POS: bystro/medlenno, s raznoy intonaciey",
  "POS: pri shume: TV, muzyka, razgovor",
  "NEG: 60+ raz, govori slovo s ekrana",
  "FON: 5-10 raz po 30 s - tishina, TV, muzyka",
  "Drug skazhet 'Sebastyan' - tozhe v POS",
};
const int KWS_TIPS_N = sizeof(KWS_TIPS) / sizeof(KWS_TIPS[0]);

int kwsCount(const char *dir, int &maxIdx) {
  maxIdx = 0;
  File d = SD.open(dir);
  if (!d) return 0;
  int c = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (n.endsWith(".wav") && n.length() >= 8) {
        c++;
        int k = n.substring(n.length() - 8, n.length() - 4).toInt();
        if (k > maxIdx) maxIdx = k;
      }
    }
    f.close();
  }
  d.close();
  return c;
}

void kwsScan() {
  if (!sdOk) return;
  if (!SD.exists("/kws")) SD.mkdir("/kws");
  if (!SD.exists("/kws/pos")) SD.mkdir("/kws/pos");
  if (!SD.exists("/kws/neg")) SD.mkdir("/kws/neg");
  if (!SD.exists("/kws/bg")) SD.mkdir("/kws/bg");
  kwsPos = kwsCount("/kws/pos", kwsPosMax);
  kwsNeg = kwsCount("/kws/neg", kwsNegMax);
  kwsBg  = kwsCount("/kws/bg",  kwsBgMax);
}

bool kwsWrite(const String &path, const int16_t *pcm, int n) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  uint32_t dataB = n * 2, riff = 36 + dataB, sr = 16000, byteRate = 32000;
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                   0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
  memcpy(h + 4, &riff, 4); memcpy(h + 24, &sr, 4); memcpy(h + 28, &byteRate, 4); memcpy(h + 40, &dataB, 4);
  bool ok = f.write(h, 44) == 44;
  const uint8_t *p = (const uint8_t *)pcm;
  for (uint32_t off = 0; ok && off < dataB; off += 4096) {
    uint32_t k = min((uint32_t)4096, dataB - off);
    ok = f.write(p + off, k) == k;
  }
  f.close();
  return ok;
}

void kwsRecord(int16_t *buf, int n) {             // сырой звук, как у слушателя (micTo16)
  static int32_t raw[256];
  size_t br;
  for (int k = 0; k < 16; k++) i2s_read(MIC_PORT, raw, sizeof(raw), &br, 0);   // слить старое
  int i = 0;
  while (i < n) {
    i2s_read(MIC_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    for (int j = 0; j < (int)(br / 4) && i < n; j++) buf[i++] = (int16_t)micTo16(raw[j]);
  }
}

void kwsWaitPlayer(int maxMs) {
  unsigned long t = millis();
  while ((uxQueueMessagesWaiting(pcmQ) > 0 || plBusy) && millis() - t < (unsigned long)maxMs) delay(5);
}

void kwsBeep(int hz, int ms) {                    // короткий «пик» перед записью
  if (!playerStart()) return;
  plRate = 0;
  const int R = 24000, N = R * ms / 1000;
  int16_t *p = (int16_t *)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
  if (!p) return;
  for (int i = 0; i < N; i++) {
    float env = i < 200 ? i / 200.0f : (i > N - 200 ? (N - i) / 200.0f : 1.0f);
    p[i] = (int16_t)(18000 * env * sinf(2 * PI * hz * i / R));
  }
  PcmJob j = {p, (size_t)N, R};
  if (xQueueSend(pcmQ, &j, pdMS_TO_TICKS(300)) != pdTRUE) { free(p); return; }
  kwsWaitPlayer(1000);
  delay(180);                                      // хвост из DMA, чтобы «пик» не попал в запись
  i2s_zero_dma_buffer(I2S_NUM_1);
  plFirstMs = 0; plLastMs = 0; plGaps = 0; plPieces = 0;
}

void kwsSet(const String &s, uint16_t col) {
  kwsState = s; kwsStateCol = col;
  if (page == PG_TRAIN) { updateHeader(); pageTrain(); }
}

void kwsPrompt(const String &big, const String &small, uint16_t col) {   // крупная подсказка по центру
  if (page != PG_TRAIN) return;
  tft.fillRect(0, 92, 240, 60, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(big, 120, 96, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(small, 120, 128, 2);
  tft.setTextDatum(TL_DATUM);
}

bool kwsTapAbort(int ms) {                        // ждём ms; тап по экрану = стоп серии
  unsigned long t = millis();
  while (millis() - t < (unsigned long)ms) {
    if (digitalRead(T_IRQ) == LOW) return true;
    delay(10);
  }
  return false;
}

void kwsLevel(const int16_t *b, int n, int &pk, int &rms) {
  double sq = 0; pk = 0;
  for (int i = 0; i < n; i++) { int v = abs(b[i]); if (v > pk) pk = v; sq += (double)b[i] * b[i]; }
  rms = (int)sqrt(sq / n);
}

// серия из KWS_SERIES сэмплов: kind 0 = «Себастьян», 1 = другие слова
void kwsSeries(int kind) {
  if (!sdOk) { kwsSet("NET SD karty!", TFT_RED); return; }
  if (!micOk || voiceBusy) return;
  const int N = 16000 * KWS_CLIP_MS / 1000;
  int16_t *buf = (int16_t *)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
  if (!buf) { kwsSet("net PSRAM", TFT_RED); return; }
  voiceBusy = true; s3BusyReport();   // 74HC4053 -> S3
  micHold();                                       // микрофон забираем у слушателя
  loopMp3 = false; stopMp3();
  kwsScan();
  { unsigned long hi = millis(), t0 = millis();    // дождаться, пока отпустишь кнопку
    while (millis() - hi < 200 && millis() - t0 < 3000) { if (digitalRead(T_IRQ) == LOW) hi = millis(); delay(5); } }
  int done = 0;
  for (int s = 0; s < KWS_SERIES; s++) {
    String word = kind == 0 ? String("SEBASTYAN") : String(NEG_WORDS[(kwsNeg + s) % NEG_N]);
    kwsPrompt(word, "posle 'pik' - " + String(s + 1) + "/" + String(KWS_SERIES) + "  (tap = stop)",
              kind == 0 ? TFT_GREEN : TFT_YELLOW);
    kwsSet(kind == 0 ? "POS: skazhi posle pika" : "NEG: skazhi slovo s ekrana", TFT_CYAN);
    if (kwsTapAbort(500)) break;
    kwsBeep(kind == 0 ? 1000 : 700, 90);
    kwsSet("ZAPIS' 2 s...", TFT_RED);
    kwsRecord(buf, N);
    int pk, rms; kwsLevel(buf, N, pk, rms);
    char name[40];
    if (kind == 0) snprintf(name, sizeof(name), "/kws/pos/p_%04d.wav", ++kwsPosMax);
    else           snprintf(name, sizeof(name), "/kws/neg/n_%04d.wav", ++kwsNegMax);
    if (!kwsWrite(name, buf, N)) { kwsSet("oshibka zapisi na SD", TFT_RED); break; }
    kwsLast = name;
    if (kind == 0) kwsPos++; else kwsNeg++;
    done++;
    kwsInfo = String(name + 5) + "  pik " + String(pk) + (pk < 300 ? "  TIHO!" : pk > 32000 ? "  PEREGRUZ!" : "  ok");
    Serial.printf("[KWS] %s pik %d rms %d\n", name, pk, rms);
    if (kwsTapAbort(300)) break;
  }
  free(buf);
  kwsPrompt("", "", TFT_WHITE);
  kwsSet("zapisano " + String(done) + ". Eshche? POS / NEG / FON", TFT_GREEN);
  voiceBusy = false;
  drawHeader();
}

// фон 30 с — просто жизнь комнаты, без слова «Себастьян»
void kwsBackground() {
  if (!sdOk) { kwsSet("NET SD karty!", TFT_RED); return; }
  if (!micOk || voiceBusy) return;
  const int N = 16000 * KWS_BG_S;
  int16_t *buf = (int16_t *)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
  if (!buf) { kwsSet("net PSRAM", TFT_RED); return; }
  voiceBusy = true; s3BusyReport();   // 74HC4053 -> S3
  micHold();
  loopMp3 = false; stopMp3();
  kwsScan();
  kwsPrompt("FON 30 s", "NE govori 'Sebastyan'. TV/muzyka/razgovor - mozhno", TFT_ORANGE);
  kwsSet("ZAPIS' FONA 30 s...", TFT_RED);
  delay(300);
  kwsRecord(buf, N);
  int pk, rms; kwsLevel(buf, N, pk, rms);
  char name[40];
  snprintf(name, sizeof(name), "/kws/bg/b_%04d.wav", ++kwsBgMax);
  bool ok = kwsWrite(name, buf, N);
  free(buf);
  if (ok) { kwsBg++; kwsLast = name; kwsInfo = String(name + 5) + "  pik " + String(pk) + "  rms " + String(rms); }
  kwsPrompt("", "", TFT_WHITE);
  kwsSet(ok ? "fon zapisan" : "oshibka zapisi na SD", ok ? TFT_GREEN : TFT_RED);
  voiceBusy = false;
  drawHeader();
}

void kwsPlayLast() {                               // послушать последний сэмпл (громкость подтянута)
  if (!kwsLast.length() || voiceBusy) return;
  File f = SD.open(kwsLast);
  if (!f) { kwsSet("fayl ne otkrylsya", TFT_RED); return; }
  size_t sz = f.size();
  if (sz <= 44 || sz > 2 * 1024 * 1024) { f.close(); return; }
  int n = (sz - 44) / 2;
  int16_t *p = (int16_t *)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  if (!p) { f.close(); return; }
  f.seek(44); f.read((uint8_t *)p, n * 2); f.close();
  int pk = 1; for (int i = 0; i < n; i++) pk = max(pk, abs((int)p[i]));
  float g = min(20000.0f / pk, 40.0f);
  for (int i = 0; i < n; i++) p[i] = (int16_t)constrain((int)(p[i] * g), -32768, 32767);
  if (!playerStart()) { free(p); return; }
  loopMp3 = false; stopMp3();
  plRate = 0;
  PcmJob j = {p, (size_t)n, 16000};
  if (xQueueSend(pcmQ, &j, pdMS_TO_TICKS(300)) != pdTRUE) { free(p); return; }
  kwsSet("igraet " + kwsLast.substring(5), TFT_CYAN);
  kwsWaitPlayer(35000);
  delay(200);
  i2s_zero_dma_buffer(I2S_NUM_1);
  plFirstMs = 0; plLastMs = 0; plGaps = 0; plPieces = 0;
  kwsSet("gotovo", TFT_WHITE);
}

void kwsDelLast() {
  if (!kwsLast.length()) { kwsSet("nechego udalyat'", TFT_YELLOW); return; }
  if (SD.remove(kwsLast)) { kwsSet("udaleno " + kwsLast.substring(5), TFT_YELLOW); kwsLast = ""; kwsInfo = ""; }
  else kwsSet("ne udalos' udalit'", TFT_RED);
  kwsScan();
}

void pageTrain() {
  char s[64];
  if (!sdOk) line(0, "SD: NET KARTY (FAT32)", TFT_RED);
  else line(0, "SD: OK   slovo: SEBASTYAN", TFT_GREEN);
  snprintf(s, sizeof(s), "POS %d/100  NEG %d/60  FON %d/5", kwsPos, kwsNeg, kwsBg);
  line(1, s, (kwsPos >= 100 && kwsNeg >= 60 && kwsBg >= 5) ? TFT_GREEN : TFT_WHITE);
  line(2, kwsState, kwsStateCol);
  line(3, kwsInfo, TFT_DARKGREY);
  bool wc = WiFi.status() == WL_CONNECTED;
  line(7, wc ? "PC: http://" + WiFi.localIP().toString() + "/kws.tar" : String("PC: net WiFi"), wc ? TFT_CYAN : TFT_RED);
  line(8, kwsTip >= 0 ? String(KWS_TIPS[kwsTip]) : String("SOVET - podskazki kak pisat'"), TFT_DARKGREY);
}

// ---------- лог аккума на SD: /batlog/ГГГГММДД_ЧЧ.csv ----------
// раз в секунду: время, мВ (среднее за 10 с от со-процессора), %, что нагружает (mp3/BT/голос/громкость).
// Пишем только 100–70% и 30–0% — там видна форма кривой разряда. На SD сбрасываем раз в 10 с.
String blBuf, blFile;
unsigned long tBl = 0, tBlFlush = 0;
uint32_t blLines = 0;

void batLogFlush() {
  if (!sdOk || !blBuf.length() || !blFile.length()) return;
  bool isNew = !SD.exists(blFile);
  File f = SD.open(blFile, FILE_APPEND);
  if (f) {
    if (isNew) f.print("time,mV,pct,mp3,bt,voice,vol\n");
    f.print(blBuf);
    f.close();
  }
  blBuf = "";
}

void batLogTick() {
  if (!sdOk || millis() - tBl < 1000) return;
  tBl = millis();
  if (!rxOk() || cpBat < BAT_NONE) return;         // нет связи / нет аккума (стенд от USB)
  int p = batPercent(cpBat);
  if (p > 30 && p < 70) { batLogFlush(); return; } // середину не пишем
  struct tm t;
  char fn[40], ts[16];
  if (timeValid(t)) {
    strftime(fn, sizeof(fn), "/batlog/%Y%m%d_%H.csv", &t);
    strftime(ts, sizeof(ts), "%H:%M:%S", &t);
  } else {                                          // нет времени (нет Wi-Fi) — по аптайму
    unsigned long sec = millis() / 1000;
    snprintf(fn, sizeof(fn), "/batlog/notime_b%lu_%02lu.csv", (unsigned long)bootCount, sec / 3600);
    snprintf(ts, sizeof(ts), "+%lu", sec);
  }
  if (blFile != fn) {                               // новый час — новый файл
    batLogFlush();
    blFile = fn;
    if (!SD.exists("/batlog")) SD.mkdir("/batlog");
  }
  char l[64];
  snprintf(l, sizeof(l), "%s,%d,%d,%d,%d,%d,%.2f\n", ts, cpBat, p, isPlaying() ? 1 : 0, cpBt,
           (voiceBusy || plBusy) ? 1 : 0, volume);
  blBuf += l;
  blLines++;
  if (millis() - tBlFlush > 10000) { tBlFlush = millis(); batLogFlush(); }
}

// ---------- веб: скачать все сэмплы одним tar ----------
WebServer web(80);
bool webStarted = false;

static void tarHeader(uint8_t *h, const String &name, uint32_t size) {
  memset(h, 0, 512);
  strncpy((char *)h, name.c_str(), 99);
  memcpy(h + 100, "0000644", 7);
  memcpy(h + 108, "0000000", 7);
  memcpy(h + 116, "0000000", 7);
  snprintf((char *)h + 124, 12, "%011o", (unsigned)size);
  snprintf((char *)h + 136, 12, "%011o", 1790000000u);
  h[156] = '0';
  memcpy(h + 257, "ustar", 5);
  h[263] = '0'; h[264] = '0';
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  h[155] = ' ';
}

static const char *KWS_DIRS[3] = {"/kws/pos", "/kws/neg", "/kws/bg"};
static const char *BAT_DIRS[1] = {"/batlog"};

// отдать папки с SD одним tar-архивом (tar — «склейка» файлов в один без сжатия)
void sendTar(const char *const *dirs, int nd, const char *fname) {
  if (!sdOk) { web.send(503, "text/plain", "no SD"); return; }
  uint32_t total = 1024;
  for (int di = 0; di < nd; di++) {
    const char *dir = dirs[di];
    File d = SD.open(dir);
    if (!d) continue;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (!f.isDirectory()) total += 512 + ((f.size() + 511) / 512) * 512;
      f.close();
    }
    d.close();
  }
  web.setContentLength(total);
  web.sendHeader("Content-Disposition", String("attachment; filename=") + fname);
  web.send(200, "application/x-tar", "");
  static uint8_t buf[4096], hdr[512];
  static const uint8_t zeros[512] = {0};
  for (int di = 0; di < nd; di++) {
    const char *dir = dirs[di];
    File d = SD.open(dir);
    if (!d) continue;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (f.isDirectory()) { f.close(); continue; }
      uint32_t sz = f.size();
      tarHeader(hdr, String(dir + 1) + "/" + f.name(), sz);
      web.sendContent((const char *)hdr, 512);
      int n;
      while ((n = f.read(buf, sizeof(buf))) > 0) web.sendContent((const char *)buf, n);
      if (sz % 512) web.sendContent((const char *)zeros, 512 - sz % 512);
      f.close();
    }
    d.close();
  }
  web.sendContent((const char *)zeros, 512);
  web.sendContent((const char *)zeros, 512);
  Serial.printf("[WEB] %s отдан, %u байт\n", fname, (unsigned)total);
}
void webKwsTar() { sendTar(KWS_DIRS, 3, "kws.tar"); }
void webBatTar() { batLogFlush(); sendTar(BAT_DIRS, 1, "bat.tar"); }
void webBatClear() {                               // http://<IP>/bat_clear?yes=1 — стереть лог аккума
  if (web.arg("yes") != "1") { web.send(200, "text/plain", "dobav ?yes=1 chtoby steret' /batlog"); return; }
  batLogFlush();
  int n = 0;
  File d = SD.open("/batlog");
  if (d) {
    std::vector<String> names;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) { if (!f.isDirectory()) names.push_back(String("/batlog/") + f.name()); f.close(); }
    d.close();
    for (auto &p : names) if (SD.remove(p)) n++;
  }
  blLines = 0;
  web.send(200, "text/plain", String("udaleno faylov: ") + n);
}

void webRoot() {
  kwsScan();
  String h = "<html><head><meta charset='utf-8'><title>Sebastian head</title></head><body style='font-family:sans-serif'>"
             "<h2>Себастьян — голова</h2><p>Сэмплы слова: POS " + String(kwsPos) + ", NEG " + String(kwsNeg) +
             ", FON " + String(kwsBg) + "</p><p><a href='/kws.tar'>Скачать все сэмплы (kws.tar)</a></p>"
             "<p>Лог аккума: " + String(blLines) + " строк с запуска. <a href='/bat.tar'>Скачать (bat.tar)</a> · "
             "<a href='/bat_clear?yes=1'>стереть</a></p></body></html>";
  web.send(200, "text/html; charset=utf-8", h);
}

void webSetup() {
  web.on("/", webRoot);
  web.on("/kws.tar", webKwsTar);
  web.on("/bat.tar", webBatTar);
  web.on("/bat_clear", webBatClear);
  web.begin();
  webStarted = true;
  Serial.println("[WEB] http-сервер на порту 80");
}

// =====================================================================
//  ВЫХОДЫ, ГРОМКОСТЬ, ДЕТЕКТ AUX
// =====================================================================
// Динамики: режим DINAMIKI или OBA; в режиме AUX — только если штекера нет (запасной вариант, чтобы не было тишины)
bool spkOn() { return !spkMuted && (outMode != OUT_AUX || !auxPlug); }
// AUX: штекер вставлен и режим AUX или OBA
bool auxOn() { return !auxMuted && auxPlug && outMode != OUT_SPK; }

void applyMutes() {
  digitalWrite(PIN_SPK_MUTE, spkOn() ? LOW : HIGH);
  digitalWrite(PIN_XSMT, auxOn() ? HIGH : LOW);
  Serial.printf("[OUT] режим %s: динамики %s, AUX %s (штекер %s)\n", OUT_NAMES[outMode],
                spkOn() ? "ON" : "OFF", auxOn() ? "ON" : "OFF", auxPlug ? "есть" : "нет");
  refreshBtn(B_SPK); refreshBtn(B_AUX);
}

void setOutMode(int m) {
  if (m < OUT_SPK || m > OUT_BOTH) return;
  outMode = m;
  prefs.begin("seb", false);
  if (prefs.getInt("out", -1) != m) prefs.putInt("out", m);   // пишем во флеш только если изменилось
  prefs.end();
  applyMutes();
  refreshBtn(B_OSPK); refreshBtn(B_OAUX); refreshBtn(B_OBOTH);
}

// ---------- переключатель источника 74HC4053 (сам чип на со-процессоре) ----------
// Голова говорит со-процессору, звучит ли S3 сейчас. Звучит -> 4053 на S3, иначе можно Bluetooth.
bool s3BusyNow() { return isPlaying() || voiceBusy || plBusy || testBusy; }
int s3BusySent = -1;
void s3BusyReport(bool force) {
  int b = s3BusyNow() ? 1 : 0;
  if (!force && b == s3BusySent) return;
  s3BusySent = b;
  Serial1.printf("S3B %d\n", b);
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
  if (up && !webStarted) webSetup();
  if (webStarted) web.handleClient();
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
    case B_OSPK:   setOutMode(OUT_SPK); break;
    case B_OAUX:   setOutMode(OUT_AUX); break;
    case B_OBOTH:  setOutMode(OUT_BOTH); break;
    case B_SPK:    spkMuted = !spkMuted; applyMutes(); break;
    case B_AUX:    auxMuted = !auxMuted; applyMutes(); break;
    case B_AUXMUS: Serial1.println("MUSIC"); break;
    case B_LR:     { bool was = loopMp3; testBusy = true; s3BusyReport(); lrTest(); testBusy = false; if (was) startMp3(); } break;
    case B_MIC:    { bool was = loopMp3; testBusy = true; s3BusyReport(); micTest(); testBusy = false; if (was) startMp3(); } break;
    case B_LED:    Serial1.println("LED"); break;
    case B_BEEP:   Serial1.println("BEEP"); break;
    case B_CPOTA:  Serial1.println("OTA"); cpOta = 1; cpIp = ""; Serial.println("[OTA] -> со-процессор: режим прошивки"); break;
    case B_REBOOT: flashMsg("REBOOT...", TFT_RED); delay(300); ESP.restart(); break;
    case B_SRV:    srvPing(); break;
    case B_ASK:    { bool was = loopMp3; testBusy = true; s3BusyReport(); voiceAsk(false); testBusy = false; if (was) startMp3(); } break;
    case B_KPOS:   testBusy = true; s3BusyReport(); kwsSeries(0); testBusy = false; break;
    case B_KNEG:   testBusy = true; s3BusyReport(); kwsSeries(1); testBusy = false; break;
    case B_KBG:    testBusy = true; s3BusyReport(); kwsBackground(); testBusy = false; break;
    case B_KPLAY:  testBusy = true; s3BusyReport(); kwsPlayLast(); testBusy = false; break;
    case B_KDEL:   kwsDelLast(); break;
    case B_KTIP:   kwsTip = (kwsTip + 1) % KWS_TIPS_N; break;
    case B_WAKE:   wakeOn = !wakeOn; if (wakeOn && !lsTask) listenStart(); goPage(page); break;
  }
}

void onHwTouch(int n) {
  if (voiceBusy) return;                         // во время разговора сенсоры не трогают музыку
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
//  ЗАЩИТА АККУМА: глубокий сон по команде со-процессора
// =====================================================================
void sleepNow() {
  pinMode(PIN_SPK_MUTE, OUTPUT); digitalWrite(PIN_SPK_MUTE, HIGH);   // MAX98357A молчат
  pinMode(PIN_XSMT, OUTPUT);     digitalWrite(PIN_XSMT, LOW);        // PCM5102A молчит
  pinMode(TFT_BL, OUTPUT);       digitalWrite(TFT_BL, LOW);          // подсветка экрана выкл
  gpio_hold_en((gpio_num_t)PIN_SPK_MUTE);                            // держать эти уровни и во сне
  gpio_hold_en((gpio_num_t)PIN_XSMT);
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_deep_sleep_hold_en();
  // ждём, пока со-процессор замолчит (иначе его строки сразу нас разбудят)
  unsigned long t0 = millis(), quiet = millis();
  while (millis() - t0 < 10000 && millis() - quiet < 1500) {
    while (Serial1.available()) { Serial1.read(); quiet = millis(); }
    delay(5);
  }
  headBatSleep = true;
  // будильник: LOW на RX от со-процессора (он проснулся и что-то шлёт) или касание экрана (T_IRQ)
  esp_sleep_enable_ext1_wakeup((1ULL << LINK_RX) | (1ULL << T_IRQ), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // подтяжки работают во сне
  rtc_gpio_pullup_en((gpio_num_t)LINK_RX);   rtc_gpio_pulldown_dis((gpio_num_t)LINK_RX);
  rtc_gpio_pullup_en((gpio_num_t)T_IRQ);     rtc_gpio_pulldown_dis((gpio_num_t)T_IRQ);
  Serial.println("[BAT] сплю. Разбудит со-процессор (зарядка) или касание экрана.");
  Serial.flush();
  esp_deep_sleep_start();
}

void drawBatDead(int mv) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("BATAREYA", 120, 90, 4);
  tft.drawString("SELA", 120, 120, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (mv > 0) { char b[16]; snprintf(b, sizeof(b), "%.2f V", mv / 1000.0f); tft.drawString(b, 120, 165, 4); }
  tft.drawString("postav na zaryadku", 120, 210, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("vklyuchus sama ot 3.55 V", 120, 232, 2);
  tft.setTextDatum(TL_DATUM);
}

void batShutdown(int mv) {                         // "B,2,mV" от со-процессора (или связь пропала на низком)
  Serial.printf("[BAT] аккум сел (%.2f В) — выключаюсь\n", mv / 1000.0f);
  Serial1.print("SLEEP\n");                        // со-процессору: понял, можешь спать
  Serial1.flush();
  batLogFlush();                                   // дописать лог аккума на SD
  wakeOn = false;                                  // слушатель больше не шлёт фразы
  loopMp3 = false;
  stopMp3();
  digitalWrite(PIN_SPK_MUTE, HIGH);
  digitalWrite(PIN_XSMT, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  drawBatDead(mv);
  delay(3000);
  sleepNow();
}

// Включили тумблером, а аккум «сел»: со-процессор шлёт "G,mV" и ждёт 12 с.
// Экран с кнопкой на 5 с: нажал (или сенсор 2 на корпусе) -> "FORCE", работаем. Нет -> спать.
unsigned long graceDoneMs = 0;
void graceScreen(int mv) {
  Serial.printf("[BAT] включили, аккум сел (%.2f В): 5 с на кнопку VKLYUCHIT'\n", mv / 1000.0f);
  loopMp3 = false;
  stopMp3();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("BATAREYA SELA", 120, 40, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char b[24]; snprintf(b, sizeof(b), "%.2f V", mv / 1000.0f);
  tft.drawString(b, 120, 75, 4);
  tft.drawString("zaryadka podklyuchena?", 120, 115, 2);
  tft.fillRoundRect(20, 160, 200, 80, 10, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("VKLYUCHIT'", 120, 188, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("ili sensor 2 na korpuse", 120, 250, 2);
  unsigned long t0 = millis();
  int lastSec = -1;
  bool forced = false;
  char lb2[16]; int n = 0;
  while (!forced && millis() - t0 < 5000) {
    int sec = 5 - (millis() - t0) / 1000;
    if (sec != lastSec) {
      lastSec = sec;
      char c[32]; snprintf(c, sizeof(c), "  inache splyu cherez %d s  ", sec);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString(c, 120, 280, 2);
    }
    if (digitalRead(T_IRQ) == LOW) {
      uint16_t x, y;
      bool ok = tft.getTouch(&x, &y);
      tft.drawPixel(239, 319, TFT_BLACK);          // «жертвенная» запись после getTouch
      if (ok) { y = 319 - y; if (y >= 150 && y <= 250) forced = true; }
    }
    while (Serial1.available()) {                   // со-процессор: "F" = нажали сенсор 2
      char c = Serial1.read();
      if (c == '\n') { lb2[n] = 0; if (!strcmp(lb2, "F")) forced = true; n = 0; }
      else if (c != '\r' && n < 15) lb2[n++] = c;
    }
    delay(20);
  }
  tft.setTextDatum(TL_DATUM);
  graceDoneMs = millis();
  if (forced) {
    Serial1.print("FORCE\n");
    Serial.println("[BAT] FORCE — работаем от зарядки (выключусь, если аккум < 3.0 В)");
    goPage(page);
    flashMsg("FORCE: rabotayu ot zaryadki", TFT_DARKGREEN);
    return;
  }
  batShutdown(mv);
}

// Самое начало setup(): проснулись после сна из-за аккума — просыпаться совсем или спать дальше
void bootGate() {
  gpio_deep_sleep_hold_dis();                      // отпускаем пины, которые держали во сне
  gpio_hold_dis((gpio_num_t)PIN_SPK_MUTE);
  gpio_hold_dis((gpio_num_t)PIN_XSMT);
  gpio_hold_dis((gpio_num_t)TFT_BL);
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) { headBatSleep = false; return; }
  bool byTouch = esp_sleep_get_ext1_wakeup_status() & (1ULL << T_IRQ);
  rtc_gpio_deinit((gpio_num_t)LINK_RX);            // вернуть пины из режима «будильника» в обычный
  rtc_gpio_deinit((gpio_num_t)T_IRQ);
  if (!headBatSleep) return;
  Serial.printf("[BAT] проснулся: %s\n", byTouch ? "касание экрана" : "сигнал от со-процессора");
  pinMode(PIN_SPK_MUTE, OUTPUT); digitalWrite(PIN_SPK_MUTE, HIGH);
  pinMode(PIN_XSMT, OUTPUT);     digitalWrite(PIN_XSMT, LOW);
  if (byTouch) { tft.init(); tft.setRotation(2); drawBatDead(0); }
  else { pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, LOW); }
  // со-процессор в обычном режиме шлёт "S,..." 5 раз в секунду — ждём до 4 с
  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  char b[16]; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 4000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\n') {
        b[n] = 0;
        if (!strncmp(b, "S,", 2) || !strncmp(b, "HELLO", 5)) {
          headBatSleep = false;
          Serial.println("[BAT] со-процессор проснулся (зарядка) — обычный старт");
          return;
        }
        n = 0;
      } else if (c != '\r' && n < 15) b[n++] = c;
    }
    delay(5);
  }
  Serial.println("[BAT] со-процессор спит — аккум всё ещё сел, сплю дальше");
  sleepNow();
}

// =====================================================================
//  UART-ЛИНК с со-процессором
//  S3 -> ESP: PING n | BEEP | LED | MUSIC | RF n | RFHOLD | OTA
//  ESP -> S3: S,... | T,n | P,n | R,n,code | L,n,code | U,code | X,n,code | W,st,pct,ip | B,2,mV
//  S3 -> ESP (ответ на B,2): SLEEP
//  ESP -> S3: G,mV (включили на севшем аккуме, 5 с на кнопку) | F (нажали сенсор 2)
//  S3 -> ESP: FORCE (нажали кнопку на экране) | SLEEP (не нажали)
//  S3 -> ESP: S3B 1/0 — S3 сейчас звучит / молчит (для 74HC4053); S,... 9-е поле = mux (0 S3, 1 BT)
// =====================================================================
char lb[160];
int ll = 0;

void handleLine(char *s) {
  lastRxMs = millis();
  rxLines++;
  if (s[0] == 'S' && s[1] == ',') {
    int oldMus = cpAuxMusic, oldMic = cpMicBtn;
    int mux = -1;
    sscanf(s + 2, "%d,%d,%d,%d,%d,%lu,%d,%d,%d", &cpT1, &cpT2, &cpT3, &cpMicBtn, &cpBat, &cpUp, &cpBt, &cpAuxMusic, &mux);
    cpMux = mux;                                      // 9-е поле есть только у со-процессора v7.2+
    if (cpMicBtn && !oldMic && !voiceBusy) pttReq = true;   // нажали кнопку мика -> разговор
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
  } else if (s[0] == 'G' && s[1] == ',') {          // G,mV — включили тумблером на севшем аккуме
    if (!graceDoneMs || millis() - graceDoneMs > 15000) graceScreen(atoi(s + 2));
  } else if (s[0] == 'B' && s[1] == ',') {          // B,2,mV — аккум сел, спать
    int lvl = 0, mv = 0;
    sscanf(s + 2, "%d,%d", &lvl, &mv);
    if (lvl == 2) batShutdown(mv);
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
  Serial.println("\n\n===== SEBASTIAN BENCH v21: HEAD (ESP32-S3) =====");
  Serial.printf("[SYS] причина старта: %s, перезапуск #%lu\n", rstName(rstReason), (unsigned long)bootCount);
  bootGate();                                      // спали из-за аккума? может, спим дальше

  pinMode(PIN_SPK_MUTE, OUTPUT);
  pinMode(PIN_XSMT, OUTPUT);
  pinMode(AUX_DET, INPUT_PULLUP);
  prefs.begin("seb", true);                        // режим вывода звука из флеша (по умолчанию OBA)
  outMode = constrain(prefs.getInt("out", OUT_BOTH), OUT_SPK, OUT_BOTH);
  prefs.end();
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
  neopixelWrite(RGB_LED, 0, 0, 0);                 // белый светодиод платы — выкл
  loopMp3 = false;                                 // музыка при старте не играет
  Serial.println("[SYS] готово. Сенсоры: 1=тише 2=play/stop 3=громче");
}

unsigned long tUi = 0, tLog = 0, tPing = 0, tLowBat = 0;
int lastBat = 0;                                   // последнее известное напряжение аккума
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
  if (!lsTask) micMeter();                         // иначе микрофон читает слушатель
  wakeTick();
  pollAuxDetect();
  if (hdrRestoreAt && millis() > hdrRestoreAt) { hdrRestoreAt = 0; drawHeader(); }
  if (pttReq) { pttReq = false; bool was = loopMp3; voiceAsk(true); if (was) startMp3(); }

  if (millis() - tUi > 150) { tUi = millis(); updateHeader(); updatePage(); }
  if (millis() - tPing > 1000) { tPing = millis(); Serial1.printf("PING %lu\n", ++pingSent); s3BusyReport(true); }
  s3BusyReport();                                  // сразу сообщить, если S3 начал/перестал звучать
  if (rxOk()) lastBat = cpBat;
  batLogTick();
  if (rxOk() && cpBat >= BAT_NONE && cpBat < BAT_LOW && millis() - tLowBat > 60000) {
    tLowBat = millis();
    char m[32]; snprintf(m, sizeof(m), "LOW BAT %.2fV - zaryadi!", cpBat / 1000.0f);
    flashMsg(m, TFT_RED);
  }
  // запасной путь: "B,2" потерялся (голова была занята), со-процессор уже спит
  if (!rxOk() && lastRxMs && millis() - lastRxMs > 30000 && lastBat >= BAT_NONE && lastBat < 3150) batShutdown(lastBat);
  if (millis() - tLog > 5000) {
    tLog = millis();
    Serial.printf("[STAT] mp3:%s vol:%.2f | link %s/%s | bat:%dmV | WiFi:%s\n",
                  isPlaying() ? "play" : "stop", volume, rxOk() ? "OK" : "NET", txOk() ? "OK" : "NET", cpBat,
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "net");
  }
}