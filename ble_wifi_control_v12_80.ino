#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <Wire.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===== Версія прошивки — міняти тут при кожному релізі =====
#define FW_VERSION "v12.80"
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <WebSocketsServer.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include "esp_task_wdt.h"

// FastLED до Adafruit щоб уникнути конфлікту типів
#define FASTLED_RMT_BUILTIN_DRIVER 1
#define FASTLED_ESP32_RAW_RMT
#define FASTLED_INTERNAL
#include <FastLED.h>

// Adafruit після FastLED
#include <OLEDDisplay.h>
#include <SSD1306Wire.h>
#include <SH1106Wire.h>
#include <arduinoFFT.h>

// ===================== OLED SSD1306/SH1106 128x64 =====================
// oledType: 0 = SSD1306, 1 = SH1106
int oledType = 1; // 0=SSD1306, 1=SH1106 (за замовчуванням більший)
int oledRotate = 0; // 0=нормально, 1=перевернуто
int specStyle = 0;  // 0=полоски 1=крапки 2=дзеркало 3=перев. 4=VU 5=очі 6=бобіни 7=VU2 8=Lissajous 9=осцил. 10=horizVU 11=vertVU 12=авто
int specPeaks = 1;  // 0=без піків, 1=падіння, 2=улітання, 3=зависання
unsigned long autoStyleTimer = 0;
int autoStyleCurrent = 0;
int autoStyleTime = 4;   // час перемикання ефектів в авто-режимі (секунди, 2-10)
uint16_t autoStyleMask = 0x0FFF; // біти 0-11: які стилі включені в авто (за замовч. всі)
float peakBars[16] = {0};      // позиції піків
float peakVelBars[16] = {0};   // швидкість піків (для улітання)
SSD1306Wire displaySSD(0x3C, 17, 18);
SH1106Wire  displaySH (0x3C, 17, 18);
OLEDDisplay* display = &displaySSD; // вказівник на активний дисплей

enum OledMode { OLED_OFF, OLED_REELS, OLED_SPECTRUM, OLED_EYES };
OledMode oledMode = OLED_EYES; // при старті — очі

bool     oledActive    = false; // резерв — не використовується наразі
float    reelAngleL    = 0.0f;
float    reelAngleR    = 0.0f;
unsigned long lastOledUpdate = 0;
volatile int  reelPulseCount = 0;
SemaphoreHandle_t i2cMutex   = NULL;

// FFT для спектроаналізатора
#define FFT_SAMPLES 512
double fftReal[FFT_SAMPLES];
double fftImag[FFT_SAMPLES];
ArduinoFFT<double> FFT(fftReal, fftImag, FFT_SAMPLES, 44100.0);
volatile float spectrumBars[16] = {0};
float spectrumBarsBuf[16] = {0}; // буфер Core 0
volatile bool spectrumReady = false;
unsigned long lastFFTTime = 0;
int32_t i2sBuffer[FFT_SAMPLES * 2]; // стерео буфер L+R
volatile int32_t i2sLastL = 0, i2sLastR = 0; // максимум за буфер для VFD
volatile int32_t i2sInstL = 0, i2sInstR = 0; // миттєве значення зі знаком для гоніометра/осцилографа
TaskHandle_t fftTaskHandle = NULL;
// Циклічний буфер для FFT
int32_t circBufL[FFT_SAMPLES] = {0};
int32_t circBufR[FFT_SAMPLES] = {0};
int circBufPos = 0;
volatile bool circBufReady = false;

// Очі
float eyeX1 = 20, eyeX2 = 90;
float eyeVX1 = 1.5f, eyeVX2 = 1.5f;
unsigned long eyeLastBlink = 0;
bool eyeBlink = false;

// Command queue for BLE (called from BLE callback on Core 0)
volatile char bleCmd[16] = {0};
volatile bool bleJustConnected = false;
volatile bool bleCmdPending = false;

// ===================== BLE =====================
#define BLE_SERVICE_UUID     "12345678-1234-1234-1234-123456789abc"
#define BLE_CMD_CHAR_UUID    "12345678-1234-1234-1234-123456789abd"
#define BLE_STATUS_CHAR_UUID "12345678-1234-1234-1234-123456789abe"

BLEServer*         bleServer     = nullptr;
BLECharacteristic* bleCmdChar    = nullptr;
BLECharacteristic* bleStatusChar = nullptr;
bool bleDeviceConnected = false;
bool blePrevConnected   = false;

class BLEConnectCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) {
    bleDeviceConnected = true;
    bleJustConnected = true;
    Serial.println("BLE connected!");
  }
  void onDisconnect(BLEServer* s) {
    bleDeviceConnected = false;
    Serial.println("BLE disconnected!");
  }
};

class BLECmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    std::string val = c->getValue();
    Serial.print("BLE onWrite len="); Serial.println(val.length());
    if(val.length() > 0 && val.length() < 15) {
      strncpy((char*)bleCmd, val.c_str(), 15);
      Serial.print("BLE stored: "); Serial.println((char*)bleCmd);
    }
  }
};

// ===================== PCF8574 =====================
// P0=PLAY, P1=PAUSE, P2=FORWARD, P3=REC
// P4=HX Pro (вхід, активний HIGH)
// P5=REWIND_SENSE, P6=NORM_MET, P7=CrO2
// Large Hub (VFD bit28) керується програмно через setBit
#define PCF8574_ADDR 0x27
#define PCF_I2C_SDA  17
#define PCF_I2C_SCL  18

uint8_t pcfShadow = 0xFF;

// Стан транспорту з PCF — оновлюється в loop(), читається з wifiTask (mqttPublish/wsSendVU)
volatile bool ws_pcf_PLAY    = false;
volatile bool ws_pcf_PAUSE   = false;
volatile bool ws_pcf_FORWARD = false;
volatile bool ws_pcf_REWIND  = false;
volatile bool ws_pcf_REC     = false;

// Прапор для відновлення I2C поза mutex
static volatile bool i2cNeedsRecovery = false;

uint8_t pcfRead() {
  uint8_t val = 0xFF;
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
    Wire.requestFrom(PCF8574_ADDR, 1);
    if (Wire.available()) {
      val = Wire.read();
    } else {
      // Позначаємо що треба відновити I2C — але не робимо це під mutex
      static unsigned long lastI2CError = 0;
      if (millis() - lastI2CError > 5000) {
        lastI2CError = millis();
        i2cNeedsRecovery = true;
      }
    }
    xSemaphoreGive(i2cMutex);
  }
  return val;
}

void pcfSetBit(int bit, bool state) {
  if (state) pcfShadow |=  (1 << bit);
  else       pcfShadow &= ~(1 << bit);
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
    Wire.beginTransmission(PCF8574_ADDR);
    Wire.write(pcfShadow);
    Wire.endTransmission();
    xSemaphoreGive(i2cMutex);
  }
  // якщо mutex не взято — shadow вже оновлений, запишемо наступного разу
}

bool pcfBit(uint8_t data, int bit) { return (data >> bit) & 1; }

// ===================== IR =====================
#define IR_PIN 21
IRrecv irrecv(IR_PIN);
decode_results irResults;

// Захист від race condition між Core 0 (wifiTask/IR) та Core 1 (loop)
portMUX_TYPE irMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lastIRCode = 0;
volatile bool     newIRCode  = false;
// Центральні частоти аналізатора: 125,175,250,350,500,700,1k,1.4k,2k,2.8k,4k,5.6k,8k,11k,14k,16k Гц
const int fftBinLow[]  = { 2,  4,  6,  8, 10, 12, 14, 16, 20, 26, 34, 44,  57,  74,  96, 125};
const int fftBinHigh[] = { 3,  5,  7,  9, 11, 13, 15, 19, 25, 33, 43, 56,  73,  95, 124, 162};
const int totalBinsFFT = 64; // FFT_SAMPLES/2
volatile unsigned long lastIRTime = 0; // час останнього IR імпульсу
unsigned long lastToggleTime = 0;      // debounce для mpx/hx/dolby
#define TOGGLE_DEBOUNCE 600            // мс між перемиканнями

// 3 коди на кожну кнопку
uint32_t irPlay[3]    = {0,0,0};
uint32_t irStop[3]    = {0,0,0};
uint32_t irRew[3]     = {0,0,0};
uint32_t irFwd[3]     = {0,0,0};
uint32_t irDolby[3]   = {0,0,0};

bool irMatchAny(uint32_t* arr, uint32_t code) {
  for (int i=0;i<3;i++) if (arr[i]!=0 && arr[i]==code) return true;
  return false;
}

// ===================== WiFi / MQTT =====================
#define WIFI_AP_SSID "TapeControl"
#define WIFI_AP_PASS "12345678"

enum WifiStatus { WIFI_SEARCHING, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_AP_MODE };
WifiStatus wifiStatus = WIFI_AP_MODE;

String wifiSSID="", wifiPass="";
bool useStaticIP=false;
IPAddress staticIP, staticGW, staticMask;
WebServer server(80);
WebSocketsServer wsServer(81);
DNSServer dnsServer;
bool captivePortalActive=false;
unsigned long lastWifiCheck=0;

// MQTT
String mqttServer="", mqttUser="", mqttPass="";
int mqttPort=1883;
bool mqttEnabled=false;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
// 28 топіків в mqttPublish — збільшуємо буфер щоб не втрачати повідомлення
// (дефолтний 256 байт недостатній)
static const uint16_t MQTT_BUFFER = 512;
unsigned long lastMqttPublish=0;
#define MQTT_PREFIX "homeassistant/tape/"
#define MQTT_DEVICE_ID "tape_deck_1"

// ===================== Константи =====================
Preferences prefs;

// Debounce збереження налаштувань — щоб не зношувати Flash при русі слайдерів
unsigned long settingsDirtyTime = 0;
bool          settingsDirty     = false;

void markSettingsDirty() {
  settingsDirty     = true;
  settingsDirtyTime = millis();
}
constexpr int SOLENOID_PULSE     = 900;
constexpr int SOLENOID_PULSE_WEB = 900;
constexpr int DOUBLE_CLICK_GAP   = 400;
constexpr int SILENCE_HYSTERESIS = 500;
constexpr int REWIND_STOP_DELAY  = 3000;
constexpr int TIMER_TIMEOUT      = 40000;
constexpr int LED_UPDATE_INTERVAL= 40;

#define LED_PIN     48
#define PIN_BUZZER  45  // динамік 64 Ом через резистор 100 Ом
#define LED_COUNT   40
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
CRGB leds[LED_COUNT];

// ===================== GPIO =====================
const int G_SOURCE = 38;
const int G_FILTER = 37, G_DOLBY_B = 35, G_DOLBY_C = 36;
const int PIN_CK = 15, PIN_DATA = 7, PIN_BRIGHTNESS_PWM = 16;
const int latchPins[] = {6, 5, 4};
const int PIN_AUDIO_L = 1, PIN_AUDIO_R = 2, PIN_PULSE = 46, PIN_RESET = 0;
// I2S PCM1808
#define I2S_BCK   39
#define I2S_LRC   40
#define I2S_DIN   41
#define I2S_MCLK  42
const int ENC_CLK = 13, ENC_DT = 14;
const int OUT_REWIND = 11, OUT_STOP = 9, OUT_PLAY = 10, OUT_FORWARD = 12;

// ===================== Меню =====================
// ===================== Меню з групами =====================
enum MenuGroup { GRP_NONE, GRP_VFD, GRP_OLED, GRP_LED, GRP_BIIP, GRP_WIFI };
enum MenuState {
  WORK,
  // VFD
  MENU_SCALE, MENU_VIEWS, MENU_PULSES, MENU_ANIM,
  MENU_PEAK_HOLD, MENU_RELEASE, MENU_RED_RELEASE, MENU_BALANCE,
  MENU_GAIN, MENU_BRIGHTNESS, MENU_SILENCE,
  MENU_ARU, MENU_CALIB,
  // OLED
  MENU_SPEC_STYLE, MENU_AUTO_TIME, MENU_OLED_THR, MENU_SPEC_GAIN, MENU_OLED_BRIGHT, MENU_OLED_TYPE, MENU_OLED_ROT,
  MENU_SPEC_PEAKS, MENU_STANDBY,
  // LED
  MENU_LED_C1, MENU_LED_C2, MENU_LED_BRIGHT,
  // BiiP
  MENU_BIIP_START, MENU_BIIP_OUT, MENU_BIIP_MENU, MENU_BIIP_WEB,
  // WiFi
  MENU_WIFI_DB, MENU_WIFI_IP, MENU_WIFI_RESET
};
MenuState currentState = WORK;
MenuGroup currentGroup = GRP_NONE; // GRP_NONE = вибір групи
bool inGroupSelect = false; // true = показуємо назву групи

// ===================== Змінні =====================
int dolbyMode=0;
bool mpxFilter=false, sourceMonitorActive=false, arrowState=false;
int peakHoldTime=1200, releaseLevel=6, redReleaseLevel=4, gainLevel=25, balanceVal=0;
int aruLevel=0; // 0=вимк, 1=повільне, 2=середнє, 3=швидке
float aruMax=0.0f; // поточний максимум для АРУ
bool autoCalibActive=false; // автокалібрування активне
bool calibReset=false;      // скидання до дефолту активне
unsigned long calibResetTimer=0;
int displayMode=0, brightnessLevel=10, viewMode=1, pulsesSetting=1, animMode=1, silenceThreshold=60, oledThreshold=2, specGain=100, oledBright=99;
int biipStart=1, biipOut=1, biipMenu=1, biipWeb=1;
int standbyTime=0; // хвилини до standby (0=вимкнено)
unsigned long standbyTimer = 0;
bool standbyActive = false;
bool standbyTimerInited = false;
float currentDecay=0.03f, redDecay=0.02f, smoothL=0, smoothR=0, smoothRed=0;
uint8_t ledColor1=160, ledColor2=0, ledBright=100;
unsigned long lastLedUpdate=0, btnTimer=0, lastMoveTime=0, timeTickMillis=0, lastBlinkMillis=0, silenceStart=0, lastMenuAction=0;
bool btnPressed=false, silenceDetected=false, actionPerformedWhilePressed=false, blinkMute=false, oledMute=true;
float peakPosL=0, peakPosR=0, peakVelL=0, peakVelR=0;
unsigned long peakT_L=0, peakT_R=0;
uint64_t chipBuffers[3]={0,0,0};
volatile int tapeCounter=0, pulseSubCounter=0;
volatile unsigned long lastPulseTime=0;
volatile int encCounter=0;
int lastTapeValue=-999;
long totalSeconds=0;
unsigned long lastClickTime=0, solenoidTimer=0;
int clickCount=0, activeSolenoidPin=-1;
bool solenoidIsWeb=false;
bool tapeReturnActive=false, zeroPulseRunning=false, zeroCrossBlocked=true, trIndicatorState=false;
unsigned long zeroPulseTimer=0, trBlinkTimer=0;
int trBlinkCount=0, lastTapePos=0;
bool remainModeActive=false;
enum RemainState { WAIT_FOR_SOUND, WAIT_FOR_SILENCE, REWINDING, STOPPING, START_PLAY };
RemainState remainStep=WAIT_FOR_SOUND;
unsigned long soundStartTimer=0, remainSequenceTimer=0, glitchTimer=0;
int wifiResetCounter=0;
bool wifiResetBlinking=false;
unsigned long wifiResetBlinkTimer=0;
bool irBindMode=false;
String irBindTarget="";
int irBindSlot=0; // поточний слот прив'язки (0,1,2)
bool vfdReady=false;
bool hxPro=false; // HX Pro — читається з PCF P4, глобально для веб

const int digitSegments[4][7]={
  {30,36,35,34,33,32,37},{0,5,4,3,2,1,6},{8,13,12,11,10,9,14},{16,21,20,19,18,17,22}
};
const byte numbers[10]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
const byte letter_S=0x6D,letter_C=0x39,letter_P=0x73,letter_H=0x76,letter_r=0x50;
const byte letter_G=0x3D,letter_n=0x54,letter_b=0x7C,letter_L=0x38,letter_E=0x79;
const byte letter_V=0x3E,letter_i=0x06,letter_u=0x1C,letter_A=0x77,letter_d=0x5E,letter_O=0x3F,letter_F=0x71,letter_t=0x78;

// ===================== Соленоїд =====================
void triggerSolenoid(int pin, bool webCmd=false) {
  if (activeSolenoidPin!=-1) digitalWrite(activeSolenoidPin,LOW);
  digitalWrite(pin,HIGH);
  activeSolenoidPin=pin; solenoidTimer=millis(); solenoidIsWeb=webCmd;
}
void tapeStop(bool w=false) { triggerSolenoid(OUT_STOP,   w); }
void tapePlay(bool w=false) { triggerSolenoid(OUT_PLAY,   w); }
void tapeREW(bool w=false)  { triggerSolenoid(OUT_REWIND, w); }
void tapeFwd(bool w=false)  { triggerSolenoid(OUT_FORWARD,w); }

// ===================== Збереження =====================
void saveSettings() {
  if (!prefs.begin("settings",false)) return;
  prefs.putInt("mode",displayMode);  prefs.putInt("peak",peakHoldTime);
  prefs.putInt("balance",balanceVal);
  prefs.putInt("rel",releaseLevel);  prefs.putInt("redRel",redReleaseLevel);
  prefs.putInt("gain",gainLevel);    prefs.putInt("bright",brightnessLevel);
  prefs.putInt("aru",aruLevel);
  prefs.putInt("lc1",ledColor1);     prefs.putInt("lc2",ledColor2);
  prefs.putInt("lbr",ledBright);     prefs.putInt("view",viewMode);
  prefs.putInt("pulseS",pulsesSetting); prefs.putInt("anim",animMode);
  prefs.putInt("dolby",dolbyMode);   prefs.putBool("mpx",mpxFilter);
  prefs.putInt("silence",silenceThreshold);
  prefs.putInt("oledThr",oledThreshold);
  prefs.putInt("specGain",specGain);
  prefs.putInt("oledBr",oledBright);
  prefs.putInt("oledType",oledType);
  prefs.putInt("oledRot",oledRotate);
  prefs.putInt("specStyle",specStyle);
  prefs.putInt("specPeaks",specPeaks);
  prefs.putInt("autoTime", autoStyleTime);
  prefs.putInt("biipS", biipStart);
  prefs.putInt("biipO", biipOut);
  prefs.putInt("biipM", biipMenu);
  prefs.putInt("biipW", biipWeb);
  prefs.putInt("standby", standbyTime);
  prefs.putUInt("autoMask", autoStyleMask);
  prefs.end();
}

void loadSettings() {
  if (!prefs.begin("settings",true)) return;
  displayMode=prefs.getInt("mode",0);      peakHoldTime=prefs.getInt("peak",1200);
  balanceVal=prefs.getInt("balance",0);
  releaseLevel=prefs.getInt("rel",6);      redReleaseLevel=prefs.getInt("redRel",4);
  gainLevel=prefs.getInt("gain",25);       brightnessLevel=prefs.getInt("bright",10);
  aruLevel=prefs.getInt("aru",0);
  ledColor1=prefs.getInt("lc1",160);       ledColor2=prefs.getInt("lc2",0);
  ledBright=prefs.getInt("lbr",100);       viewMode=prefs.getInt("view",1);
  pulsesSetting=prefs.getInt("pulseS",1);  animMode=prefs.getInt("anim",1);
  dolbyMode=prefs.getInt("dolby",0);       mpxFilter=prefs.getBool("mpx",false);
  silenceThreshold=prefs.getInt("silence",60);
  oledThreshold=prefs.getInt("oledThr",2);
  specGain=prefs.getInt("specGain",200);
  oledBright=prefs.getInt("oledBr",99);
  oledType=prefs.getInt("oledType",1);
  oledRotate=prefs.getInt("oledRot",0);
  specStyle=prefs.getInt("specStyle",0);
  specPeaks=prefs.getInt("specPeaks",1);
  autoStyleTime=(int)prefs.getInt("autoTime", 4);
  biipStart=prefs.getInt("biipS", 1);
  biipOut=prefs.getInt("biipO", 1);
  biipMenu=prefs.getInt("biipM", 1);
  biipWeb=prefs.getInt("biipW", 1);
  standbyTime=prefs.getInt("standby", 0);
  autoStyleMask=(uint16_t)prefs.getUInt("autoMask", 0x0FFF);
  prefs.end();
  currentDecay=(float)releaseLevel*0.005f; redDecay=(float)redReleaseLevel*0.005f;
}

void saveWifiSettings() {
  if (!prefs.begin("wifi",false)) return;
  prefs.putString("ssid",wifiSSID); prefs.putString("pass",wifiPass);
  prefs.putBool("static",useStaticIP);
  if (useStaticIP) {
    prefs.putUInt("sip",(uint32_t)staticIP);
    prefs.putUInt("sgw",(uint32_t)staticGW);
    prefs.putUInt("smsk",(uint32_t)staticMask);
  }
  prefs.end();
}

void loadWifiSettings() {
  if (!prefs.begin("wifi",true)) return;
  wifiSSID=prefs.getString("ssid",""); wifiPass=prefs.getString("pass","");
  useStaticIP=prefs.getBool("static",false);
  if (useStaticIP) {
    staticIP=IPAddress(prefs.getUInt("sip",0));
    staticGW=IPAddress(prefs.getUInt("sgw",0));
    staticMask=IPAddress(prefs.getUInt("smsk",0));
  }
  prefs.end();
}

void saveMqttSettings() {
  if (!prefs.begin("mqtt",false)) return;
  prefs.putString("srv",mqttServer); prefs.putInt("port",mqttPort);
  prefs.putString("user",mqttUser); prefs.putString("pass",mqttPass);
  prefs.putBool("en",mqttEnabled);
  prefs.end();
}

void loadMqttSettings() {
  if (!prefs.begin("mqtt",true)) return;
  mqttServer=prefs.getString("srv",""); mqttPort=prefs.getInt("port",1883);
  mqttUser=prefs.getString("user",""); mqttPass=prefs.getString("pass","");
  mqttEnabled=prefs.getBool("en",false);
  prefs.end();
}

void saveIRSettings() {
  if (!prefs.begin("ir",false)) return;
  for (int i=0;i<3;i++) {
    prefs.putUInt(("pl"+String(i)).c_str(), irPlay[i]);
    prefs.putUInt(("st"+String(i)).c_str(), irStop[i]);
    prefs.putUInt(("rw"+String(i)).c_str(), irRew[i]);
    prefs.putUInt(("fw"+String(i)).c_str(), irFwd[i]);
    prefs.putUInt(("dl"+String(i)).c_str(), irDolby[i]);
  }
  prefs.end();
}

void loadIRSettings() {
  if (!prefs.begin("ir",true)) return;
  for (int i=0;i<3;i++) {
    irPlay[i] =prefs.getUInt(("pl"+String(i)).c_str(),0);
    irStop[i] =prefs.getUInt(("st"+String(i)).c_str(),0);
    irRew[i]  =prefs.getUInt(("rw"+String(i)).c_str(),0);
    irFwd[i]  =prefs.getUInt(("fw"+String(i)).c_str(),0);
    irDolby[i]=prefs.getUInt(("dl"+String(i)).c_str(),0);
  }
  prefs.end();
}

void resetWifiSettings() {
  if (prefs.begin("wifi",false)){prefs.clear();prefs.end();}
  wifiSSID=""; wifiPass=""; useStaticIP=false;
}

// ===================== MQTT =====================
// Forward declarations
void applyBrightness();
void applyOledBrightness();
void buzzMenuEnter();
void buzzMenuExit();
void buzzToneDecay(int freq, int dur);

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t=String(topic), msg="";
  for (unsigned int i=0;i<len;i++) msg+=(char)payload[i];

  // Транспорт — HA button надсилає порожній payload, також підтримуємо "ON"
  if (t.endsWith("/play/set")   && (msg==""||msg=="ON")) tapePlay(true);
  if (t.endsWith("/stop/set")   && (msg==""||msg=="ON")) tapeStop(true);
  if (t.endsWith("/rew/set")    && (msg==""||msg=="ON")) tapeREW(true);
  if (t.endsWith("/fwd/set")    && (msg==""||msg=="ON")) tapeFwd(true);

  // LED окремі параметри
  if (t.endsWith("/led_c1/set"))     { ledColor1 = constrain(msg.toInt(),0,255); markSettingsDirty(); }
  if (t.endsWith("/led_c2/set"))     { ledColor2 = constrain(msg.toInt(),0,255); markSettingsDirty(); }
  if (t.endsWith("/led_bright/set")) { ledBright  = constrain(msg.toInt(),0,255); markSettingsDirty(); }

  // OLED режим
  if (t.endsWith("/oled/set")) {
    if      (msg=="off")      oledMode = OLED_OFF;
    else if (msg=="reels")    oledMode = OLED_REELS;
    else if (msg=="spectrum") oledMode = OLED_SPECTRUM;
    else if (msg=="eyes")     oledMode = OLED_EYES;
  }
  // OLED стиль спектру 0-8
  if (t.endsWith("/spec_style/set")) {
    specStyle = constrain(msg.toInt(), 0, 13);
    markSettingsDirty();
  }
  // OLED піки 0-3
  if (t.endsWith("/oled_peaks/set")) {
    specPeaks = constrain(msg.toInt(), 0, 3);
    markSettingsDirty();
  }
  // OLED яскравість 0-99
  if (t.endsWith("/oled_bright/set")) {
    oledBright = constrain(msg.toInt(), 0, 99);
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50))==pdTRUE) {
      applyOledBrightness();
      xSemaphoreGive(i2cMutex);
    }
    markSettingsDirty();
  }
  // VFD яскравість 1-15
  if (t.endsWith("/vfd_bright/set")) {
    brightnessLevel = constrain(msg.toInt(), 1, 15);
    applyBrightness();
    markSettingsDirty();
  }
}

void mqttDiscovery() {
  String dev = String(",\"device\":{\"identifiers\":[\"") + MQTT_DEVICE_ID + "\"],\"name\":\"Tape Deck\",\"model\":\"ESP32-S3\"}";

  // --- Кнопки транспорту ---
  // payload_press="" — HA button надсилає порожній рядок, саме це і приймаємо
  struct { const char* name; const char* id; } btns[] = {
    {"Play","play"},{"Stop","stop"},{"Rewind","rew"},{"Forward","fwd"}
  };
  for (auto& b : btns) {
    String t = String("homeassistant/button/") + MQTT_DEVICE_ID + "_" + b.id + "/config";
    String p = String("{\"name\":\"Tape ") + b.name + "\",\"unique_id\":\"" + MQTT_DEVICE_ID + "_" + b.id +
      "\",\"command_topic\":\"" + MQTT_PREFIX + b.id + "/set" +
      "\",\"payload_press\":\"\"" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true);
  }

  // --- Стиль спектру OLED (select) ---
  { String t = String("homeassistant/select/") + MQTT_DEVICE_ID + "_spec_style/config";
    String p = String("{\"name\":\"OLED Spectrum Style\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_spec_style" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "spec_style/set" +
      "\",\"state_topic\":\"" + MQTT_PREFIX + "spec_style/state" +
      "\",\"options\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\"]" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- Сенсори ---
  struct { const char* name; const char* id; const char* unit; const char* cls; } sens[] = {
    {"VU Left","vu_l","",""},{"VU Right","vu_r","",""},
    {"Tape Counter","counter","",""},{"WiFi RSSI","rssi","dBm","signal_strength"},
    {"Dolby","dolby","",""},{"MPX","mpx","",""}
  };
  for (auto& s : sens) {
    String t = String("homeassistant/sensor/") + MQTT_DEVICE_ID + "_" + s.id + "/config";
    String p = String("{\"name\":\"") + s.name + "\",\"unique_id\":\"" + MQTT_DEVICE_ID + "_" + s.id +
      "\",\"state_topic\":\"" + MQTT_PREFIX + s.id + "\"" +
      (strlen(s.unit)>0 ? String(",\"unit_of_measurement\":\"") + s.unit + "\"" : "") +
      (strlen(s.cls)>0  ? String(",\"device_class\":\"")        + s.cls  + "\"" : "") +
      dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true);
  }

  // --- OLED режим (select) ---
  { String t = String("homeassistant/select/") + MQTT_DEVICE_ID + "_oled_mode/config";
    String p = String("{\"name\":\"OLED Mode\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_oled_mode" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "oled/set\",\"state_topic\":\"" + MQTT_PREFIX + "oled/state" +
      "\",\"options\":[\"off\",\"reels\",\"spectrum\",\"eyes\"]" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- OLED піки (select) ---
  { String t = String("homeassistant/select/") + MQTT_DEVICE_ID + "_oled_peaks/config";
    String p = String("{\"name\":\"OLED Peaks\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_oled_peaks" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "oled_peaks/set\",\"state_topic\":\"" + MQTT_PREFIX + "oled_peaks/state" +
      "\",\"options\":[\"0\",\"1\",\"2\",\"3\"]" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- OLED яскравість (number) ---
  { String t = String("homeassistant/number/") + MQTT_DEVICE_ID + "_oled_bright/config";
    String p = String("{\"name\":\"OLED Brightness\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_oled_bright" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "oled_bright/set\",\"state_topic\":\"" + MQTT_PREFIX + "oled_bright/state" +
      "\",\"min\":0,\"max\":99,\"step\":1" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- LED Color Play hue (number) ---
  { String t = String("homeassistant/number/") + MQTT_DEVICE_ID + "_led_c1/config";
    String p = String("{\"name\":\"LED Color Play\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_led_c1" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "led_c1/set\",\"state_topic\":\"" + MQTT_PREFIX + "led_c1/state" +
      "\",\"min\":0,\"max\":255,\"step\":1" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- LED Color Rec hue (number) ---
  { String t = String("homeassistant/number/") + MQTT_DEVICE_ID + "_led_c2/config";
    String p = String("{\"name\":\"LED Color Rec\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_led_c2" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "led_c2/set\",\"state_topic\":\"" + MQTT_PREFIX + "led_c2/state" +
      "\",\"min\":0,\"max\":255,\"step\":1" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- LED Brightness (number) ---
  { String t = String("homeassistant/number/") + MQTT_DEVICE_ID + "_led_bright/config";
    String p = String("{\"name\":\"LED Brightness\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_led_bright" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "led_bright/set\",\"state_topic\":\"" + MQTT_PREFIX + "led_bright/state" +
      "\",\"min\":0,\"max\":255,\"step\":1" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- VFD яскравість (number) ---
  { String t = String("homeassistant/number/") + MQTT_DEVICE_ID + "_vfd_bright/config";
    String p = String("{\"name\":\"VFD Brightness\",\"unique_id\":\"") + MQTT_DEVICE_ID + "_vfd_bright" +
      "\",\"command_topic\":\"" + MQTT_PREFIX + "vfd_bright/set\",\"state_topic\":\"" + MQTT_PREFIX + "vfd_bright/state" +
      "\",\"min\":1,\"max\":15,\"step\":1,\"icon\":\"mdi:brightness-6\"" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true); }

  // --- VFD транспорт режими (binary_sensor) ---
  struct { const char* name; const char* id; const char* icon; } modes[] = {
    {"VFD Rewind",  "vfd_rew", "mdi:rewind"},
    {"VFD Stop",    "vfd_stp", "mdi:stop"},
    {"VFD Play",    "vfd_ply", "mdi:play"},
    {"VFD Pause",   "vfd_pau", "mdi:pause"},
    {"VFD Forward", "vfd_fwd", "mdi:fast-forward"}
  };
  for (auto& m : modes) {
    String t = String("homeassistant/binary_sensor/") + MQTT_DEVICE_ID + "_" + m.id + "/config";
    String p = String("{\"name\":\"") + m.name +
      "\",\"unique_id\":\"" + MQTT_DEVICE_ID + "_" + m.id +
      "\",\"state_topic\":\"" + MQTT_PREFIX + m.id + "/state" +
      "\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"" + m.icon + "\"" + dev + "}";
    mqttClient.publish(t.c_str(), p.c_str(), true);
  }
}

void mqttConnect() {
  if (!mqttEnabled || mqttServer.length()==0 || wifiStatus!=WIFI_CONNECTED) return;
  mqttClient.setBufferSize(MQTT_BUFFER); // збільшений буфер для 28 топіків
  mqttClient.setServer(mqttServer.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  if (mqttClient.connect(MQTT_DEVICE_ID, mqttUser.c_str(), mqttPass.c_str())) {
    mqttClient.subscribe((String(MQTT_PREFIX)+"play/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"stop/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"rew/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"fwd/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"led_c1/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"led_c2/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"led_bright/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"oled/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"oled_peaks/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"oled_bright/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"spec_style/set").c_str());
    mqttClient.subscribe((String(MQTT_PREFIX)+"vfd_bright/set").c_str());
    mqttDiscovery();
  }
}

void mqttPublish() {
  if (!mqttEnabled || !mqttClient.connected()) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", (int)smoothL);
  mqttClient.publish((String(MQTT_PREFIX)+"vu_l").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", (int)smoothR);
  mqttClient.publish((String(MQTT_PREFIX)+"vu_r").c_str(), buf);
  // tapeCounter змінюється в ISR на Core 1 — читаємо безпечно
  int safeCounter;
  portENTER_CRITICAL(&irMux); safeCounter = tapeCounter; portEXIT_CRITICAL(&irMux);
  snprintf(buf, sizeof(buf), "%d", safeCounter);
  mqttClient.publish((String(MQTT_PREFIX)+"counter").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
  mqttClient.publish((String(MQTT_PREFIX)+"rssi").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", dolbyMode);
  mqttClient.publish((String(MQTT_PREFIX)+"dolby").c_str(), buf);
  mqttClient.publish((String(MQTT_PREFIX)+"mpx").c_str(), mpxFilter ? "ON" : "OFF");

  // LED стан
  snprintf(buf, sizeof(buf), "%d", ledColor1);
  mqttClient.publish((String(MQTT_PREFIX)+"led_c1/state").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", ledColor2);
  mqttClient.publish((String(MQTT_PREFIX)+"led_c2/state").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", ledBright);
  mqttClient.publish((String(MQTT_PREFIX)+"led_bright/state").c_str(), buf);

  // OLED стан
  const char* oledStr = "eyes";
  if      (oledMode==OLED_OFF)      oledStr = "off";
  else if (oledMode==OLED_REELS)    oledStr = "reels";
  else if (oledMode==OLED_SPECTRUM) oledStr = "spectrum";
  mqttClient.publish((String(MQTT_PREFIX)+"oled/state").c_str(), oledStr);
  snprintf(buf, sizeof(buf), "%d", specPeaks);
  mqttClient.publish((String(MQTT_PREFIX)+"oled_peaks/state").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", specStyle);
  mqttClient.publish((String(MQTT_PREFIX)+"spec_style/state").c_str(), buf);
  snprintf(buf, sizeof(buf), "%d", oledBright);
  mqttClient.publish((String(MQTT_PREFIX)+"oled_bright/state").c_str(), buf);

  // VFD яскравість
  snprintf(buf, sizeof(buf), "%d", brightnessLevel);
  mqttClient.publish((String(MQTT_PREFIX)+"vfd_bright/state").c_str(), buf);
  // VFD транспорт режими публікуються одразу при зміні стану (в loop)
}

// ===================== WiFi =====================
void startAP() {
  WiFi.disconnect(true); WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID,WIFI_AP_PASS);
  dnsServer.start(53,"*",WiFi.softAPIP()); // Captive Portal — всі DNS на себе
  captivePortalActive=true;
  wifiStatus=WIFI_AP_MODE;
}
void startSTA() {
  if (wifiSSID.length()==0){startAP();return;}
  dnsServer.stop(); captivePortalActive=false;
  WiFi.mode(WIFI_STA);
  if (useStaticIP) WiFi.config(staticIP,staticGW,staticMask);
  WiFi.begin(wifiSSID.c_str(),wifiPass.c_str());
  wifiStatus=WIFI_CONNECTING; lastWifiCheck=millis();
}

// ===================== Веб сторінка =====================
String irCodesHtml(uint32_t* arr, const char* name) {
  String s = String("<div class=\"ir-slots\">");
  for (int i=0;i<3;i++) {
    if(arr[i]!=0) {
      s += "<span class=\"slot filled\">0x"+String(arr[i],HEX);
      s += "<button class=\"clrbtn\" style=\"font-size:9px;padding:1px 4px;margin-left:3px\" onclick=\"clearOne('"+String(name)+"',"+String(i)+")\">x</button>";
      s += "</span>";
    } else {
      s += "<span class=\"slot\">---</span>";
    }
  }
  s += "<button class=\"clrbtn\" onclick=\"clearIR('"+String(name)+"')\">&#x2715;</button></div>";
  return s;
}

String webPage() {
  String ip   = (wifiStatus==WIFI_CONNECTED)?WiFi.localIP().toString():WiFi.softAPIP().toString();
  int    rssi = (wifiStatus==WIFI_CONNECTED)?WiFi.RSSI():0;
  String modeStr = wifiStatus==WIFI_AP_MODE?"Точка доступу":wifiStatus==WIFI_CONNECTING?"Підключення...":"Підключено";
  String irDisp  = newIRCode?("0x"+String(lastIRCode,HEX)):"---";
  String dolbyStr= dolbyMode==0?"Вимк":dolbyMode==1?"B":dolbyMode==2?"C":"B+C";

  String html;
  html += R"(<!DOCTYPE html>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<meta name='mobile-web-app-capable' content='yes'>
<meta name='apple-mobile-web-app-capable' content='yes'>
<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>
<meta name='apple-mobile-web-app-title' content='Tape Control'>
<meta name='theme-color' content='#0f3460'>
<link rel='manifest' href='/manifest.json'>
<title>Tape Control</title><style>
*{box-sizing:border-box}
body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:0}
h2{color:#e94560;margin:8px 0;font-size:20px}
.tabs{display:flex;background:#0f3460;position:sticky;top:0;z-index:10}
.tab{flex:1;padding:12px 4px;text-align:center;cursor:pointer;font-size:13px;
     border-bottom:3px solid transparent;transition:all .2s}
.tab.active{border-bottom:3px solid #e94560;color:#e94560;font-weight:bold}
.page{display:none;padding:16px;max-width:480px;margin:0 auto}
.page.active{display:block}
.block{background:#16213e;border-radius:12px;padding:14px;margin:10px 0}
.block b{font-size:15px}
.btn{padding:13px 16px;margin:4px;border-radius:8px;border:none;font-size:14px;
     font-weight:bold;cursor:pointer;color:#fff}
.play{background:#27ae60}.stop{background:#e74c3c}
.rew{background:#2980b9}.fwd{background:#8e44ad}
.tog{background:#555}.tog.on{background:#e67e22}
.bind{background:#f39c12}.bind.on{background:#c0392b;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
.sbtn{background:#e94560;padding:9px 18px;border:none;border-radius:8px;color:#fff;cursor:pointer;margin:4px}
.gbtn{background:#27ae60;padding:9px 18px;border:none;border-radius:8px;color:#fff;cursor:pointer;margin:4px}
.info{font-size:13px;color:#aaa;margin:3px 0}
.ir-code{font-size:20px;font-weight:bold;color:#f39c12;padding:8px;
  background:#0f3460;border-radius:8px;margin:6px 0;letter-spacing:2px}
.ir-slots{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin:4px 0}
.slot{padding:4px 8px;border-radius:6px;background:#0f3460;font-size:12px;color:#aaa}
.slot.filled{color:#2ecc71;font-weight:bold}
.clrbtn{background:#c0392b;border:none;color:#fff;border-radius:6px;
        padding:3px 8px;cursor:pointer;font-size:12px}
.ir-row{display:flex;align-items:center;justify-content:space-between;
        padding:6px 0;border-bottom:1px solid #2a3a5e}
.ir-row:last-child{border:none}
.ir-label{font-weight:bold;min-width:60px}
.bind-btns{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
input[type=text],input[type=password],input[type=number]{
  width:100%;padding:8px;border-radius:6px;border:1px solid #444;
  background:#0f3460;color:#fff;margin:4px 0;font-size:14px}
label{font-size:13px;display:block;margin:4px 0}
.slider-row{display:flex;align-items:center;gap:8px;margin:6px 0}
.slider-row input[type=range]{flex:1}
.slider-val{min-width:36px;text-align:right;font-size:13px;color:#f39c12}
.status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}
.dot-green{background:#2ecc71}.dot-yellow{background:#f39c12}.dot-red{background:#e74c3c}
.tp-icons{display:flex;gap:10px;align-items:center;margin-left:10px}
.tp-icon svg{display:block}
.tp-icon path,.tp-icon rect,.tp-icon polygon{transition:fill .2s}
.tp-icon.active path,.tp-icon.active rect,.tp-icon.active polygon{fill:#2ecc71}
.tp-icon path,.tp-icon rect,.tp-icon polygon{fill:#2a3a5e}
@keyframes recBlink{0%,100%{color:#e74c3c}50%{color:transparent}}
#ic_rec.active{animation:recBlink 0.6s infinite;color:#e74c3c}
.ir-dot{width:10px;height:10px;border-radius:50%;background:#1a2a3a;display:inline-block;margin-left:4px;transition:background 0.15s}
.ir-dot.active{background:#2980b9}
.acc-hdr{display:flex;justify-content:space-between;align-items:center;
  padding:12px 14px;cursor:pointer;font-weight:bold;font-size:15px;
  border-radius:12px;background:#16213e;user-select:none}
.acc-hdr:active{background:#1a2a4a}
.acc-arr{font-size:12px;color:#aaa;transition:transform .2s}
.acc-body{padding:10px 14px 14px}
.slider-row label{font-size:13px;min-width:110px;color:#ccc}
</style></head><body>
<div style='text-align:center;padding:10px 0 0'>
  <h2>🎛 Tape Control</h2>
  <button id='pwa_install' onclick='installPWA()' style='display:none;margin-top:6px;padding:6px 16px;background:#e94560;color:#fff;border:none;border-radius:20px;font-size:12px;cursor:pointer'>📲 Додати на екран</button>
</div>
<div class='tabs'>
  <div class='tab active' onclick='showTab(0)'>📻 Керування</div>
  <div class='tab' onclick='showTab(1)'>⚙️ Меню</div>
  <div class='tab' onclick='showTab(2)'>📡 IR</div>
  <div class='tab' onclick='showTab(3)'>🌐 Мережа</div>
</div>

<!-- ===== TAB 0: КЕРУВАННЯ ===== -->
<div class='page active' id='p0'>
  <div class='block'>
    <div style='display:flex;align-items:center;margin-bottom:10px'>
      <b>Транспорт</b>
      <div class='tp-icons'>
        <!-- REW -->
        <div class='tp-icon' id='ic_rew' title='REW'>
          <svg width='22' height='18' viewBox='0 0 22 18'>
            <polygon points='11,9 22,0 22,18'/>
            <polygon points='0,9 11,0 11,18'/>
          </svg>
        </div>
        <!-- STOP -->
        <div class='tp-icon' id='ic_stp' title='STOP'>
          <svg width='16' height='16' viewBox='0 0 16 16'>
            <rect x='0' y='0' width='16' height='16'/>
          </svg>
        </div>
        <!-- PLAY -->
        <div class='tp-icon' id='ic_ply' title='PLAY'>
          <svg width='16' height='18' viewBox='0 0 16 18'>
            <polygon points='0,0 16,9 0,18'/>
          </svg>
        </div>
        <!-- PAUSE -->
        <div class='tp-icon' id='ic_pau' title='PAUSE'>
          <svg width='14' height='18' viewBox='0 0 14 18'>
            <rect x='0' y='0' width='5' height='18'/>
            <rect x='9' y='0' width='5' height='18'/>
          </svg>
        </div>
        <!-- FWD -->
        <div class='tp-icon' id='ic_fwd' title='FWD'>
          <svg width='22' height='18' viewBox='0 0 22 18'>
            <polygon points='0,0 11,9 0,18'/>
            <polygon points='11,0 22,9 11,18'/>
          </svg>
        </div>
        <span id='ic_rec' style='font-size:13px;font-weight:bold;color:#333;margin-left:4px'>REC</span>
      </div>
      <div style='margin-left:auto;display:flex;align-items:center;gap:4px;font-size:12px;color:#fff'>
        IR<span class='ir-dot' id='ir_dot_1'></span>
      </div>
    </div>
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>
      <button class='btn play' style='width:100%;padding:18px 0;font-size:16px' onclick='cmd("play")'>▶ PLAY</button>
      <button class='btn stop' style='width:100%;padding:18px 0;font-size:16px' onclick='cmd("stop")'>■ STOP</button>
      <button class='btn rew'  style='width:100%;padding:18px 0;font-size:16px' onclick='cmd("rew")'>⏮ REW</button>
      <button class='btn fwd'  style='width:100%;padding:18px 0;font-size:16px' onclick='cmd("fwd")'>⏭ FWD</button>
    </div>
  </div>
  <div class='block' style='padding:8px 14px'>
    <div style='display:flex;align-items:center;gap:8px;flex-wrap:wrap'>
      <b style='margin-right:4px'>Dolby:</b>
      <button class='btn tog)" + String(dolbyMode==0?" on":"") + R"(' onclick='setDolby(0)'>Вимк</button>
      <button class='btn tog)" + String(dolbyMode==1?" on":"") + R"(' onclick='setDolby(1)'>B</button>
      <button class='btn tog)" + String(dolbyMode==2?" on":"") + R"(' onclick='setDolby(2)'>C</button>
      <button class='btn tog)" + String(dolbyMode==3?" on":"") + R"(' onclick='setDolby(3)'>B+C</button>
    </div>
  </div>
  <div class='block'>
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:center'>
      <div>
        <div style='font-size:13px;color:#aaa;margin-bottom:6px'>MPX фільтр</div>
        <button id='mpx_btn' class='btn tog)" + String(mpxFilter?" on":"") + R"(' style='width:100%' onclick='toggleMPX()'>
          )" + String(mpxFilter?"🟠 Увімк":"⚪ Вимк") + R"(
        </button>
      </div>
      <div>
        <div style='font-size:13px;color:#aaa;margin-bottom:6px'>HX Pro</div>
        <div id='hxpro' style='padding:10px;background:#0f3460;border-radius:8px;text-align:center;font-weight:bold'>
          ⚫ Вимк
        </div>
      </div>
    </div>
  </div>
  <div class='block'>
    <b>VU метр</b>
    <canvas id='vu' width='380' height='60' style='width:100%;border-radius:8px;background:#0f3460;margin-top:8px'></canvas>
  </div>
  <div class='block' style='padding:6px 14px 8px'>
    <div id='cnt-wrap' style='background:#000;border-radius:8px;padding:5px 10px;text-align:center'>
      <svg id='cnt-svg' viewBox='0 0 242 60' style='width:100%;max-width:255px'>)" +
      [](int val) -> String {
        bool neg = val < 0;
        int v = abs(val) % 10000;
        int digits[4] = {v/1000, (v/100)%10, (v/10)%10, v%10};
        const bool segs[10][7] = {
          {1,1,1,1,1,1,0},{0,1,1,0,0,0,0},{1,1,0,1,1,0,1},{1,1,1,1,0,0,1},
          {0,1,1,0,0,1,1},{1,0,1,1,0,1,1},{1,0,1,1,1,1,1},{1,1,1,0,0,0,0},
          {1,1,1,1,1,1,1},{1,1,1,1,0,1,1},
        };
        String svg = "";
        String on = "#2ecc71", off = "#0a1a0a";
        // Мінус — завжди присутній зліва, колір залежить від знаку
        svg += "<rect x='2' y='27' width='16' height='6' rx='3' fill='" + (neg?String("#e74c3c"):off) + "'/>";
        // Цифри зміщені на 22px щоб не перекриватись з мінусом
        for (int d=0; d<4; d++) {
          int x = d * 55 + 22;
          int n = digits[d];
          svg += "<rect x='"+String(x+4)+"' y='2'  width='36' height='6' rx='3' fill='"+(segs[n][0]?on:off)+"'/>";
          svg += "<rect x='"+String(x+40)+"' y='4'  width='6' height='22' rx='3' fill='"+(segs[n][1]?on:off)+"'/>";
          svg += "<rect x='"+String(x+40)+"' y='34' width='6' height='22' rx='3' fill='"+(segs[n][2]?on:off)+"'/>";
          svg += "<rect x='"+String(x+4)+"' y='52' width='36' height='6' rx='3' fill='"+(segs[n][3]?on:off)+"'/>";
          svg += "<rect x='"+String(x)+"'   y='34' width='6' height='22' rx='3' fill='"+(segs[n][4]?on:off)+"'/>";
          svg += "<rect x='"+String(x)+"'   y='4'  width='6' height='22' rx='3' fill='"+(segs[n][5]?on:off)+"'/>";
          svg += "<rect x='"+String(x+4)+"' y='27' width='36' height='6' rx='3' fill='"+(segs[n][6]?on:off)+"'/>";
        }
        return svg;
      }(tapeCounter) + R"(
      </svg>
    </div>
  </div>
</div>
<!-- ===== TAB 1: МЕНЮ ===== -->
<div class='page' id='p1'>

  <!-- VFD -->
  <div class='block' style='padding:0'>
    <div class='acc-hdr' onclick='toggleAcc("vfd")'>
      <span>📺 VFD</span><span class='acc-arr' id='arr_vfd'>▼</span>
    </div>
    <div class='acc-body' id='acc_vfd'>
      <div style='display:flex;gap:6px;margin:4px 0 10px;flex-wrap:wrap'>
        <span id='vfd_rew' style='padding:3px 8px;border-radius:5px;background:#1a2a4a;font-size:12px;color:#888'>⏮ REW</span>
        <span id='vfd_stp' style='padding:3px 8px;border-radius:5px;background:#1a2a4a;font-size:12px;color:#888'>■ STOP</span>
        <span id='vfd_ply' style='padding:3px 8px;border-radius:5px;background:#1a2a4a;font-size:12px;color:#888'>▶ PLAY</span>
        <span id='vfd_pau' style='padding:3px 8px;border-radius:5px;background:#1a2a4a;font-size:12px;color:#888'>⏸ PAUSE</span>
        <span id='vfd_fwd' style='padding:3px 8px;border-radius:5px;background:#1a2a4a;font-size:12px;color:#888'>⏭ FWD</span>
      </div>
      <div class='slider-row'><label>Яскравість (br)</label>
        <input type='range' min='1' max='15' value=')" + String(brightnessLevel) + R"(' oninput='setSetting("bright",this.value)'>
        <span class='slider-val' id='sv_bright'>)" + String(brightnessLevel) + R"(</span></div>
      <div class='slider-row'><label>Шкала (SC)</label>
        <input type='range' min='0' max='2' value=')" + String(displayMode) + R"(' oninput='setSetting("mode",this.value)'>
        <span class='slider-val' id='sv_mode'>)" + String(displayMode) + R"(</span></div>
      <div class='slider-row'><label>Вигляд (Vi)</label>
        <input type='range' min='1' max='4' value=')" + String(viewMode) + R"(' oninput='setSetting("view",this.value)'>
        <span class='slider-val' id='sv_view'>)" + String(viewMode) + R"(</span></div>
      <div class='slider-row'><label>Імпульси (Pu)</label>
        <input type='range' min='1' max='20' value=')" + String(pulsesSetting) + R"(' oninput='setSetting("pulseS",this.value)'>
        <span class='slider-val' id='sv_pulseS'>)" + String(pulsesSetting) + R"(</span></div>
      <div class='slider-row'><label>Анімація (An)</label>
        <input type='range' min='1' max='6' value=')" + String(animMode) + R"(' oninput='setSetting("anim",this.value)'>
        <span class='slider-val' id='sv_anim'>)" + String(animMode) + R"(</span></div>
      <div style='font-size:11px;color:#888;margin:-4px 0 8px'>1=падіння 2=відскок 3=вильот 4=стоп 5=скид 6=авто демо</div>
      <div class='slider-row'><label>Peak Hold (PH)</label>
        <input type='range' min='0' max='40' value=')" + String(peakHoldTime/100) + R"(' oninput='setSetting("peak",this.value*100)'>
        <span class='slider-val' id='sv_peak'>)" + String(peakHoldTime/100) + R"(</span></div>
      <div class='slider-row'><label>Release (rS)</label>
        <input type='range' min='1' max='99' value=')" + String(releaseLevel) + R"(' oninput='setSetting("rel",this.value)'>
        <span class='slider-val' id='sv_rel'>)" + String(releaseLevel) + R"(</span></div>
      <div class='slider-row'><label>Red Release (rE)</label>
        <input type='range' min='1' max='99' value=')" + String(redReleaseLevel) + R"(' oninput='setSetting("redRel",this.value)'>
        <span class='slider-val' id='sv_redRel'>)" + String(redReleaseLevel) + R"(</span></div>
      <div class='slider-row'><label>Баланс (bL)</label>
        <input type='range' min='-5' max='5' value=')" + String(balanceVal) + R"(' oninput='setSetting("balance",this.value)'>
        <span class='slider-val' id='sv_balance'>)" + String(balanceVal) + R"(</span></div>
      <div style='font-size:11px;color:#888;margin:-4px 0 8px'>-5=L5 (лівий -2.5дБ) .. 0=рівно .. +5=R5 (правий -2.5дБ)</div>
      <div class='slider-row'><label>Підсилення (Gn)</label>
        <input type='range' min='1' max='200' value=')" + String(gainLevel) + R"(' oninput='setSetting("gain",this.value)'>
        <span class='slider-val' id='sv_gain'>)" + String(gainLevel) + R"(</span></div>
      <div class='slider-row'><label>Тиша (SL)</label>
        <input type='range' min='5' max='400' step='5' value=')" + String(silenceThreshold) + R"(' oninput='setSetting("silence",this.value)'>
        <span class='slider-val' id='sv_silence'>)" + String(silenceThreshold) + R"(</span></div>
      <div class='slider-row'><label>АРУ (Ar)</label>
        <input type='range' min='0' max='3' value=')" + String(aruLevel) + R"(' oninput='setSetting("aru",this.value)'>
        <span class='slider-val' id='sv_aru'>)" + String(aruLevel) + R"(</span></div>
      <button class='gbtn' style='margin-top:8px' onclick='saveAll()'>💾 Зберегти</button>
    </div>
  </div>

  <!-- OLED -->
  <div class='block' style='padding:0;margin-top:8px'>
    <div class='acc-hdr' onclick='toggleAcc("oled")'>
      <span>🖥 OLED</span><span class='acc-arr' id='arr_oled'>▶</span>
    </div>
    <div class='acc-body' id='acc_oled' style='display:none'>
      <div class='slider-row'><label>Яскравість (Ob)</label>
        <input type='range' min='0' max='99' value=')" + String(oledBright) + R"(' oninput='setSetting("oledBr",this.value)'>
        <span class='slider-val' id='sv_oledBr'>)" + String(oledBright) + R"(</span></div>
      <div class='slider-row'><label>Поріг тиші (OL)</label>
        <input type='range' min='0' max='16' value=')" + String(oledThreshold) + R"(' oninput='setSetting("oledThr",this.value)'>
        <span class='slider-val' id='sv_oledThr'>)" + String(oledThreshold) + R"(</span></div>
      <div class='slider-row'><label>Підсилення FFT (SG)</label>
        <input type='range' min='1' max='100' value=')" + String(specGain/20) + R"(' oninput='setSetting("specGain",this.value*20)'>
        <span class='slider-val' id='sv_specGain'>)" + String(specGain/20) + R"(</span></div>
      <div class='slider-row'><label>Стиль (Sd) 0-13</label>
        <input type='range' min='0' max='13' value=')" + String(specStyle) + R"(' oninput='setSetting("specStyle",this.value)'>
        <span class='slider-val' id='sv_specStyle'>)" + String(specStyle) + R"(</span></div>
      <div style='font-size:11px;color:#888;margin:-4px 0 8px'>0=полоски 1=крапки 2=дзеркало 3=перев. 4=VU 5=очі 6=бобіни 7=VU2 8=Ліссажу 9=осцил. 10=HorizVU 11=VertVU 12=0A(авто) 13=2A(авто без захоплення)</div>
      <div class='slider-row'><label>Час авто (At)</label>
        <input type='range' min='2' max='10' value=')" + String(autoStyleTime) + R"(' oninput='setSetting("autoTime",this.value)'>
        <span class='slider-val' id='sv_autoTime'>)" + String(autoStyleTime) + R"(</span></div>
      <div style='font-size:12px;color:#aaa;margin:6px 0 4px'>Авто — вибір ефектів:</div>
      <div id='auto-checks' style='display:flex;flex-wrap:wrap;gap:4px 8px;margin-bottom:8px'>)" +
      [](uint16_t mask) -> String {
        const char* names[] = {"Пол","Крп","Дзр","Прв","VU1","Очі","Боб","VU2","Ліс","Осц","HVU","VVU"};
        String s = "";
        for (int i = 0; i < 12; i++) {
          bool on = (mask >> i) & 1;
          s += "<label style='display:flex;align-items:center;gap:3px;font-size:12px;color:#ccc;cursor:pointer'>"
               "<input type='checkbox' id='am" + String(i) + "'" + (on?" checked":"") +
               " onchange='setAutoMask(" + String(i) + ",this.checked)'>" +
               String(names[i]) + "</label>";
        }
        return s;
      }(autoStyleMask) + R"(
      </div>
      <div class='slider-row'><label>Піки (Pb) 0-3</label>
        <input type='range' min='0' max='3' value=')" + String(specPeaks) + R"(' oninput='setSetting("specPeaks",this.value)'>
        <span class='slider-val' id='sv_specPeaks'>)" + String(specPeaks) + R"(</span></div>
      <div class='slider-row'><label>Standby (Sb) хв</label>
        <input type='range' min='0' max='20' value=')" + String(standbyTime) + R"(' oninput='setSetting("standby",this.value)'>
        <span class='slider-val' id='sv_standby'>)" + String(standbyTime) + R"(</span></div>
      <div style='font-size:11px;color:#888;margin:-4px 0 8px'>0=вимк, 1-20 хвилин до вимкнення дисплеїв при тиші</div>
      <div style='margin:8px 0 4px;font-size:13px;color:#aaa'>Тип дисплея:</div>
      <button id='btn_ssd' class='btn tog)" + String(oledType==0?" on":"") + R"(' onclick='setOledType(0)'>SSD1306</button>
      <button id='btn_sh'  class='btn tog)" + String(oledType==1?" on":"") + R"(' onclick='setOledType(1)'>SH1106</button>
      <div style='margin:8px 0 4px;font-size:13px;color:#aaa'>Поворот:</div>
      <button id='btn_rot0' class='btn tog)" + String(oledRotate==0?" on":"") + R"(' onclick='setOledRot(0)'>Норм</button>
      <button id='btn_rot1' class='btn tog)" + String(oledRotate==1?" on":"") + R"(' onclick='setOledRot(1)'>180°</button>
      <button class='gbtn' style='margin-top:8px;display:block' onclick='saveAll()'>💾 Зберегти</button>
    </div>
  </div>

  <!-- LED -->
  <div class='block' style='padding:0;margin-top:8px'>
    <div class='acc-hdr' onclick='toggleAcc("led")'>
      <span>💡 LED</span><span class='acc-arr' id='arr_led'>▶</span>
    </div>
    <div class='acc-body' id='acc_led' style='display:none'>
      <div class='slider-row'><label>Колір Play (Hue)</label>
        <input type='range' min='0' max='255' value=')" + String(ledColor1) + R"(' oninput='setSetting("lc1",this.value)'>
        <span class='slider-val' id='sv_lc1'>)" + String(ledColor1) + R"(</span></div>
      <div class='slider-row'><label>Колір Rec (Hue)</label>
        <input type='range' min='0' max='255' value=')" + String(ledColor2) + R"(' oninput='setSetting("lc2",this.value)'>
        <span class='slider-val' id='sv_lc2'>)" + String(ledColor2) + R"(</span></div>
      <div class='slider-row'><label>Яскравість</label>
        <input type='range' min='0' max='255' value=')" + String(ledBright) + R"(' oninput='setSetting("lbr",this.value)'>
        <span class='slider-val' id='sv_lbr'>)" + String(ledBright) + R"(</span></div>
      <button class='gbtn' style='margin-top:8px' onclick='saveAll()'>💾 Зберегти</button>
    </div>
  </div>

  <!-- BiiP -->
  <div class='acc-item'>
    <div class='acc-hdr' onclick='toggleAcc("biip")'>
      <span>🔔 BiiP (звуки)</span><span class='acc-arr' id='arr_biip'>▶</span>
    </div>
    <div class='acc-body' id='acc_biip' style='display:none'>
      <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px'>
        <div>
          <div style='font-size:12px;color:#aaa;margin-bottom:4px'>bS Старт</div>
          <button id='biip_s' class='btn tog)" + String(biipStart?" on":"") + R"(' style='width:100%' onclick='toggleBiip("biipS","biip_s")'>
            )" + String(biipStart?"🔔 Увімк":"🔕 Вимк") + R"(
          </button>
        </div>
        <div>
          <div style='font-size:12px;color:#aaa;margin-bottom:4px'>bP Поза меню</div>
          <button id='biip_p' class='btn tog)" + String(biipOut?" on":"") + R"(' style='width:100%' onclick='toggleBiip("biipP","biip_p")'>
            )" + String(biipOut?"🔔 Увімк":"🔕 Вимк") + R"(
          </button>
        </div>
        <div>
          <div style='font-size:12px;color:#aaa;margin-bottom:4px'>bn Меню</div>
          <button id='biip_n' class='btn tog)" + String(biipMenu?" on":"") + R"(' style='width:100%' onclick='toggleBiip("biipN","biip_n")'>
            )" + String(biipMenu?"🔔 Увімк":"🔕 Вимк") + R"(
          </button>
        </div>
        <div>
          <div style='font-size:12px;color:#aaa;margin-bottom:4px'>bu Web/BLE</div>
          <button id='biip_w' class='btn tog)" + String(biipWeb?" on":"") + R"(' style='width:100%' onclick='toggleBiip("biipW","biip_w")'>
            )" + String(biipWeb?"🔔 Увімк":"🔕 Вимк") + R"(
          </button>
        </div>
      </div>
      <div style='font-size:11px;color:#888;margin-bottom:8px'>Звук підключення BLE завжди активний.</div>
      <button class='gbtn' onclick='saveAll()'>💾 Зберегти</button>
    </div>
  </div>

</div>
<div class='page' id='p2'>
  <div class='block'>
    <div style='display:flex;align-items:center;margin-bottom:8px'>
      <b>Прийнятий код</b>
      <div style='margin-left:auto;display:flex;align-items:center;gap:4px;font-size:12px;color:#fff'>
        Активність IR<span class='ir-dot' id='ir_dot_2'></span>
      </div>
    </div>
    <div class='ir-code' id='irc'>)" + irDisp + R"(</div>
    <div style='display:flex;gap:8px;flex-wrap:wrap'>
    <button class='btn bind)" + String(irBindMode?" on":"") + R"(' id='bb' onclick='toggleBind()' style='min-width:200px' data-off=')" + String("Прив'язати код") + R"(' data-on='Виберіть кнопку...'>
      )" + String(irBindMode?"Виберіть кнопку...":"Прив'язати код") + R"(
    </button>
    <button class='btn' onclick='showSysInfo()' style='background:#1a3a5c'>⚙ Система</button>
    </div>
  </div>

  <!-- Модальне вікно системних параметрів -->
  <div id='sysmodal' style='display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:1000;align-items:center;justify-content:center'>
    <div style='background:#0f3460;border-radius:12px;padding:20px;min-width:280px;max-width:90%;position:relative'>
      <div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:16px'>
        <b style='font-size:16px'>⚙ Системні параметри</b>
        <button onclick='hideSysInfo()' style='background:none;border:none;color:#fff;font-size:20px;cursor:pointer'>✕</button>
      </div>
      <div id='sysinfo_body' style='font-size:13px;line-height:2'>
        <div style='color:#aaa'>Завантаження...</div>
      </div>
      <button class='gbtn' style='margin-top:12px;width:100%' onclick='hideSysInfo()'>Закрити</button>
    </div>
  </div>
  <div class='block'>
    <b>Прив'язки</b>
    <div class='ir-row' id='ir_row_play'>
      <span class='ir-label'>▶ PLAY</span>
      <span id='ir_codes_play'>)" + irCodesHtml(irPlay,"play") + R"(</span>
      <button id='ir_add_play' class='btn play' style='padding:6px 10px;font-size:12px;display:)" + String(irBindMode?"inline-block":"none") + R"(' onclick='bindTo("play")'>+</button>
    </div>
    <div class='ir-row' id='ir_row_stop'>
      <span class='ir-label'>■ STOP</span>
      <span id='ir_codes_stop'>)" + irCodesHtml(irStop,"stop") + R"(</span>
      <button id='ir_add_stop' class='btn stop' style='padding:6px 10px;font-size:12px;display:)" + String(irBindMode?"inline-block":"none") + R"(' onclick='bindTo("stop")'>+</button>
    </div>
    <div class='ir-row' id='ir_row_rew'>
      <span class='ir-label'>⏮ REW</span>
      <span id='ir_codes_rew'>)" + irCodesHtml(irRew,"rew") + R"(</span>
      <button id='ir_add_rew' class='btn rew' style='padding:6px 10px;font-size:12px;display:)" + String(irBindMode?"inline-block":"none") + R"(' onclick='bindTo("rew")'>+</button>
    </div>
    <div class='ir-row' id='ir_row_fwd'>
      <span class='ir-label'>⏭ FWD</span>
      <span id='ir_codes_fwd'>)" + irCodesHtml(irFwd,"fwd") + R"(</span>
      <button id='ir_add_fwd' class='btn fwd' style='padding:6px 10px;font-size:12px;display:)" + String(irBindMode?"inline-block":"none") + R"(' onclick='bindTo("fwd")'>+</button>
    </div>
    <div class='ir-row' id='ir_row_dolby'>
      <span class='ir-label'>🎵 Dolby</span>
      <span id='ir_codes_dolby'>)" + irCodesHtml(irDolby,"dolby") + R"(</span>
      <button id='ir_add_dolby' class='btn tog' style='padding:6px 10px;font-size:12px;display:)" + String(irBindMode?"inline-block":"none") + R"(' onclick='bindTo("dolby")'>+</button>
    </div>
  </div>
</div>

<!-- ===== TAB 3: МЕРЕЖА ===== -->
<div class='page' id='p3'>
  <div class='block'>
    <b>WiFi статус</b><br>
    <div class='info'>
      <span class='status-dot )" + String(wifiStatus==WIFI_CONNECTED?"dot-green":wifiStatus==WIFI_CONNECTING?"dot-yellow":"dot-red") + R"('></span>
      )" + modeStr + R"(
    </div>
    <div class='info'>IP: <b>)" + ip + R"(</b></div>
    <div class='info'>Режим IP: <b>)" + String(useStaticIP ? "🔒 Статична" : "🔄 DHCP") + R"(</b></div>
    )" + (wifiStatus==WIFI_CONNECTED ? String(
      "<div class='info' style='display:flex;align-items:center;gap:8px'>Сигнал: <b>" + String(rssi) + " dBm</b>"
      "<div style='display:flex;align-items:flex-end;gap:2px;height:16px;margin-left:4px'>" +
      [](int r) -> String {
        int bars = r>=-55?5:r>=-65?4:r>=-75?3:r>=-85?2:1;
        String s="";
        for(int i=1;i<=5;i++){
          String col = i<=bars?"#2ecc71":"#2a3a5e";
          s+="<div style='width:5px;height:"+String(i*3)+"px;background:"+col+";border-radius:1px 1px 0 0'></div>";
        }
        return s;
      }(rssi) +
      "</div></div>"
    ) : "") + R"(
  </div>
  <div class='block'>
    <b>Підключення до мережі</b>
    <input type='text'     id='ss' placeholder='Назва мережі (SSID)'>
    <input type='password' id='pw' placeholder='Пароль'>
    <label><input type='checkbox' id='us' onchange="document.getElementById('sb').style.display=this.checked?'block':'none'"> Статична IP</label>
    <div id='sb' style='display:none'>
      <input type='text' id='si' placeholder='IP (192.168.1.100)'>
      <input type='text' id='sg' placeholder='Шлюз (192.168.1.1)'>
      <input type='text' id='sm' placeholder='Маска (255.255.255.0)'>
    </div>
    <button class='sbtn' onclick='saveWifi()'>💾 Зберегти та підключити</button>
  </div>
  <div class='block'>
    <b>MQTT (Home Assistant)</b>
    <div class='info' style='margin:4px 0 8px'>Статус: <b>)" + String(mqttClient.connected()?"🟢 Підключено":"🔴 Відключено") + R"(</b></div>
    <label><input type='checkbox' id='mqen')" + String(mqttEnabled?" checked":"") + R"(> Увімкнути MQTT</label>
    <input type='text'   id='msrv' placeholder='Адреса брокера' value=')" + mqttServer + R"('>
    <input type='number' id='mprt' placeholder='Порт' value=')" + String(mqttPort) + R"('>
    <input type='text'   id='musr' placeholder='Логін (якщо є)' value=')" + mqttUser + R"('>
    <input type='password' id='mpas' placeholder='Пароль (якщо є)'>
    <button class='gbtn' onclick='saveMqtt()'>💾 Зберегти MQTT</button>
  </div>
  <div class='block'>
    <b>OTA оновлення прошивки</b><br>
    <input type='file' id='fw' accept='.bin' style='color:#eee;margin:8px 0'>
    <button class='sbtn' onclick='uploadFW()'>⬆️ Завантажити</button>
    <div id='ota-progress' style='display:none;margin-top:8px'>
      <div style='background:#0f3460;border-radius:6px;height:16px'>
        <div id='ota-bar' style='background:#27ae60;height:16px;border-radius:6px;width:0%;transition:width .3s'></div>
      </div>
      <div id='ota-msg' style='font-size:13px;margin-top:4px'>0%</div>
    </div>
  </div>
</div>

<script>
var tabs,pages;
function showTab(i){
  if(!tabs) return;
  tabs.forEach((t,j)=>{t.classList.toggle('active',i==j)});
  pages.forEach((p,j)=>{p.classList.toggle('active',i==j)});
  sessionStorage.setItem('tab',i);
}
window.onload=function(){
  tabs=document.querySelectorAll('.tab');
  pages=document.querySelectorAll('.page');
  var savedTab=parseInt(sessionStorage.getItem('tab')||'0');
  showTab(savedTab);
};
function cmd(c){fetch('/buzz');fetch('/cmd?a='+c)}
function setDolby(m){
  fetch('/buzz');
  fetch('/setting?k=dolby&v='+m).then(()=>{
    document.querySelectorAll('[onclick^="setDolby"]').forEach(function(b,i){b.classList.toggle('on',i==m);});
  });
}
function toggleMPX(){
  fetch('/setting?k=mpx&v=toggle').then(()=>{
    fetch('/status').then(r=>r.json()).then(d=>{
      var mpxEl=document.getElementById('mpx_btn');
      if(mpxEl){
        mpxEl.innerHTML=d.mpx?'🟠 Увімк':'⚪ Вимк';
        mpxEl.classList.toggle('on',!!d.mpx);
      }
    });
  });
}

function setSetting(k,v){
  var el=document.getElementById('sv_'+k);
  if(el) el.innerText=v;
  fetch('/setting?k='+k+'&v='+v);
}
function saveAll(){fetch('/savesettings').then(()=>alert('Збережено!'))}
function toggleBind(){
  fetch('/ir?bind=1').then(r=>r.json()).then(d=>updateIR(d));
}
function bindTo(t){
  fetch('/ir?to='+t).then(r=>r.json()).then(d=>updateIR(d));
}
function clearIR(t){
  fetch('/ir?clear='+t).then(r=>r.json()).then(d=>updateIR(d));
}
function clearOne(t,idx){
  fetch('/ir?clearone='+t+'&idx='+idx).then(r=>r.json()).then(d=>updateIR(d));
}
function updateIR(d){
  var bb=document.getElementById("bb");
  if(bb){
    bb.className="btn bind"+(d.bind?" on":"");
    bb.innerHTML=d.bind?"Виберіть кнопку...": "Прив&#39;язати код";
  }
  var actions=["play","stop","rew","fwd","dolby"];
  actions.forEach(function(a){
    var row=document.getElementById("ir_row_"+a);
    if(!row) return;
    var codesDiv=document.getElementById("ir_codes_"+a);
    if(codesDiv){
      var html="";
      var i=0;
      d[a].forEach(function(c){
        if(c) html+=["<span class=\"slot filled\">",c,"<button class=\"clrbtn\" style=\"font-size:9px;padding:1px 4px;margin-left:3px\" onclick=\"clearOne(\'"+a+"\',"+i+")\" >x</button></span>"].join("");
        else html+="<span class=\"slot\">---</span>";
        i++;
      });
      html+="<button class=\"clrbtn\" onclick=\"clearIR(\'"+a+"\')\">&#x2715;</button>";
      codesDiv.innerHTML=html;
    }
    var addBtn=document.getElementById("ir_add_"+a);
    if(addBtn) addBtn.style.display=d.bind?"inline-block":"none";
  });
}
function saveWifi(){
  var p=new URLSearchParams({
    ssid:document.getElementById('ss').value,
    pass:document.getElementById('pw').value,
    us:document.getElementById('us').checked?1:0,
    si:document.getElementById('si').value,
    sg:document.getElementById('sg').value,
    sm:document.getElementById('sm').value
  });
  fetch('/setwifi?'+p).then(()=>alert('Збережено! Перезавантаження...'))
}
function saveMqtt(){
  var p=new URLSearchParams({
    en:document.getElementById('mqen').checked?1:0,
    srv:document.getElementById('msrv').value,
    port:document.getElementById('mprt').value,
    user:document.getElementById('musr').value,
    pass:document.getElementById('mpas').value
  });
  fetch('/setmqtt?'+p).then(()=>alert('MQTT збережено!'))
}
function uploadFW(){
  var f=document.getElementById('fw').files[0];
  if(!f){alert('Оберіть .bin файл!');return;}
  document.getElementById('ota-progress').style.display='block';
  var fd=new FormData(); fd.append('firmware',f);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){
    var pct=Math.round(e.loaded/e.total*100);
    document.getElementById('ota-bar').style.width=pct+'%';
    document.getElementById('ota-msg').innerText=pct+'%';
  };
  xhr.onload=function(){document.getElementById('ota-msg').innerText='Готово! Перезавантаження...';setTimeout(()=>location.reload(),5000);};
  xhr.open('POST','/ota'); xhr.send(fd);
}

// ===== WebSocket VU =====
var canvas=document.getElementById('vu');
var ctx=canvas?canvas.getContext('2d'):null;
var vuL=0,vuR=0;

function drawVU(l,r,pl,pr){
  if(!ctx) return;
  var w=canvas.width,h=canvas.height,bw=w/16-2;
  ctx.clearRect(0,0,w,h);
  for(var i=0;i<16;i++){
    var x=i*(bw+2);
    // Лівий канал (верхня половина)
    ctx.fillStyle=i<l?(i<10?'#2ecc71':i<13?'#f39c12':'#e74c3c'):'#1a2a4a';
    ctx.fillRect(x,2,bw,h/2-4);
    // Правий канал (нижня половина)
    ctx.fillStyle=i<r?(i<10?'#2ecc71':i<13?'#f39c12':'#e74c3c'):'#1a2a4a';
    ctx.fillRect(x,h/2+2,bw,h/2-4);
  }
  // Піки лівий
  if(pl>0&&pl<=16){
    var px=(pl-1)*(bw+2);
    ctx.fillStyle=pl<10?'#2ecc71':pl<13?'#f39c12':'#e74c3c';
    ctx.fillRect(px,2,bw,h/2-4);
  }
  // Піки правий
  if(pr>0&&pr<=16){
    var px=(pr-1)*(bw+2);
    ctx.fillStyle=pr<10?'#2ecc71':pr<13?'#f39c12':'#e74c3c';
    ctx.fillRect(px,h/2+2,bw,h/2-4);
  }
}

function drawCounter(val){
  var svg=document.getElementById('cnt-svg');
  if(!svg) return;
  var neg=val<0, v=Math.abs(val)%10000;
  var digits=[Math.floor(v/1000),Math.floor(v/100)%10,Math.floor(v/10)%10,v%10];
  var segs=[[1,1,1,1,1,1,0],[0,1,1,0,0,0,0],[1,1,0,1,1,0,1],[1,1,1,1,0,0,1],
            [0,1,1,0,0,1,1],[1,0,1,1,0,1,1],[1,0,1,1,1,1,1],[1,1,1,0,0,0,0],
            [1,1,1,1,1,1,1],[1,1,1,1,0,1,1]];
  var on='#2ecc71', off='#0a1a0a';
  // Мінус — завжди є, колір залежить від знаку
  var html='<rect x="2" y="27" width="16" height="6" rx="3" fill="'+(neg?'#e74c3c':off)+'"/>';
  for(var d=0;d<4;d++){
    var x=d*55+22, n=digits[d], s=segs[n];
    html+='<rect x="'+(x+4)+'" y="2"  width="36" height="6" rx="3" fill="'+(s[0]?on:off)+'"/>';
    html+='<rect x="'+(x+40)+'" y="4"  width="6" height="22" rx="3" fill="'+(s[1]?on:off)+'"/>';
    html+='<rect x="'+(x+40)+'" y="34" width="6" height="22" rx="3" fill="'+(s[2]?on:off)+'"/>';
    html+='<rect x="'+(x+4)+'" y="52" width="36" height="6" rx="3" fill="'+(s[3]?on:off)+'"/>';
    html+='<rect x="'+x+'" y="34" width="6" height="22" rx="3" fill="'+(s[4]?on:off)+'"/>';
    html+='<rect x="'+x+'" y="4"  width="6" height="22" rx="3" fill="'+(s[5]?on:off)+'"/>';
    html+='<rect x="'+(x+4)+'" y="27" width="36" height="6" rx="3" fill="'+(s[6]?on:off)+'"/>';
  }
  svg.innerHTML=html;
}
function setOledType(v){
  setSetting('oledType',v);
  document.getElementById('btn_ssd').classList.toggle('on',v==0);
  document.getElementById('btn_sh').classList.toggle('on',v==1);
}
function setOledRot(v){
  setSetting('oledRot',v);
  document.getElementById('btn_rot0').classList.toggle('on',v==0);
  document.getElementById('btn_rot1').classList.toggle('on',v==1);
}
var autoMask=)" + String(autoStyleMask) + R"(;
function setAutoMask(bit,on){
  if(on) autoMask|=(1<<bit); else autoMask&=~(1<<bit);
  setSetting('autoMask',autoMask);
}
function toggleAcc(id){
  var b=document.getElementById('acc_'+id);
  var a=document.getElementById('arr_'+id);
  if(!b) return;
  var open=b.style.display!='none';
  b.style.display=open?'none':'block';
  if(a) a.style.transform=open?'':'rotate(180deg)';
}
var wsHost=location.hostname;
var wsTimer=null;
function connectWS(){
  var ws=new WebSocket('ws://'+wsHost+':81/');
  ws.onopen=function(){if(wsTimer){clearTimeout(wsTimer);wsTimer=null;}};
  ws.onmessage=function(e){
    try{
      var d=JSON.parse(e.data);
      vuL=d.l||0; vuR=d.r||0;
      drawVU(vuL,vuR,d.pl||0,d.pr||0);
      if(d.cnt!==undefined) drawCounter(d.cnt);
      // Оновлюємо іконки транспорту (TAB 0)
      var ids=['ic_rew','ic_stp','ic_ply','ic_pau','ic_fwd'];
      var vals=[d.rew,d.stp,d.ply,d.pau,d.fwd];
      for(var i=0;i<5;i++){
        var el=document.getElementById(ids[i]);
        if(el) el.classList.toggle('active', !!vals[i]);
      }
      // Оновлюємо VFD індикатори в меню (TAB 1)
      var vmap={vfd_rew:d.rew,vfd_stp:d.stp,vfd_ply:d.ply,vfd_pau:d.pau,vfd_fwd:d.fwd};
      for(var k in vmap){
        var ve=document.getElementById(k);
        if(ve){
          ve.style.background=vmap[k]?'#27ae60':'#1a2a4a';
          ve.style.color=vmap[k]?'#fff':'#888';
        }
      }
      // Оновлюємо HX Pro в реальному часі
      var hxEl=document.getElementById('hxpro');
      if(hxEl) hxEl.innerHTML=d.hx?'🟢 Активний':'⚫ Вимк';
      // Оновлюємо REC індикатор
      var recEl=document.getElementById('ic_rec');
      if(recEl) recEl.classList.toggle('active',!!d.rec);
      var irAct=!!d.ir;
      var d1=document.getElementById('ir_dot_1');
      var d2=document.getElementById('ir_dot_2');
      if(d1) d1.classList.toggle('active',irAct);
      if(d2) d2.classList.toggle('active',irAct);
      // Оновлюємо Dolby кнопки
      if(d.dlb!==undefined){
        var dlbBtns=document.querySelectorAll("[onclick^=\"setDolby\"]");
        dlbBtns.forEach(function(b,i){b.classList.toggle("on",i==d.dlb);});
      }
      // Оновлюємо MPX кнопку
      if(d.mpx!==undefined){
        var mpxEl=document.querySelector("[onclick=\"toggleMPX()\"]");
        if(mpxEl) mpxEl.innerHTML=d.mpx?"🟠 Увімк":"⚪ Вимк";
        if(mpxEl) mpxEl.classList.toggle("on",!!d.mpx);
      }
    }catch(err){}
  };
  ws.onclose=function(){wsTimer=setTimeout(connectWS,2000);};
  ws.onerror=function(){ws.close();};
}
connectWS();

// Fallback polling для IR коду
setInterval(()=>{
  fetch('/status').then(r=>r.json()).then(d=>{
    if(d.ir&&d.ir!='---') document.getElementById('irc').innerText=d.ir;
  });
},2000);
var sysInfoTimer=null;
function showSysInfo(){
  var m=document.getElementById('sysmodal');
  m.style.display='flex';
  fetchSysInfo();
  sysInfoTimer=setInterval(fetchSysInfo,2000);
}
function hideSysInfo(){
  document.getElementById('sysmodal').style.display='none';
  if(sysInfoTimer){clearInterval(sysInfoTimer);sysInfoTimer=null;}
}
function fetchSysInfo(){
  fetch('/sysinfo').then(r=>r.json()).then(d=>{
    var h=document.getElementById('sysinfo_body');
    if(!h)return;
    h.innerHTML=
      '<div>🧠 <b>CPU:</b> '+d.cpu+' МГц</div>'+
      '<div>🌡 <b>Температура:</b> '+d.temp+' °C</div>'+
      '<div>💾 <b>RAM вільна:</b> '+d.freeHeap+' байт</div>'+
      '<div>💾 <b>RAM загальна:</b> '+d.totalHeap+' байт</div>'+
      '<div>📊 <b>RAM зайнята:</b> '+d.usedPct+' %</div>'+
      '<div>📶 <b>WiFi сигнал:</b> '+d.rssi+' dBm</div>'+
      '<div>⏱ <b>Uptime:</b> '+d.uptime+'</div>'+
      '<div>🔢 <b>FFT семплів:</b> '+d.fftSamples+'</div>';
  }).catch(function(){
    var h=document.getElementById('sysinfo_body');
    if(h) h.innerHTML='<div style="color:#e94560">Помилка отримання даних</div>';
  });
}
function toggleBiip(key, btnId){
  var btn=document.getElementById(btnId);
  var isOn=btn.classList.contains('on');
  var newVal=isOn?0:1;
  fetch('/setting?k='+key+'&v='+newVal);
  btn.classList.toggle('on',!isOn);
  btn.innerHTML=newVal?'🔔 Увімк':'🔕 Вимк';
}
// PWA install prompt
var deferredPrompt;
window.addEventListener('beforeinstallprompt',function(e){
  e.preventDefault();
  deferredPrompt=e;
  var btn=document.getElementById('pwa_install');
  if(btn) btn.style.display='block';
});
function installPWA(){
  if(deferredPrompt){
    deferredPrompt.prompt();
    deferredPrompt.userChoice.then(function(){deferredPrompt=null;});
  }
}
</script></body></html>)";
  return html;
}

// ===================== Web Handlers =====================
void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  String page = webPage();
  int len = page.length();
  int pos = 0;
  while (pos < len) {
    int chunk = min(1024, len - pos);
    server.sendContent(page.substring(pos, pos + chunk));
    pos += chunk;
    esp_task_wdt_reset();
  }
  page = String();
  server.sendContent("");
}


void execCommand(String a) {
  if      (a=="play")  tapePlay(true);
  else if (a=="stop")  tapeStop(true);
  else if (a=="rew")   tapeREW(true);
  else if (a=="fwd")   tapeFwd(true);
  else if (a=="dolby" || a=="mpx" || a=="hx") {
    // Debounce — ігноруємо повторні команди протягом TOGGLE_DEBOUNCE мс
    if (millis() - lastToggleTime < TOGGLE_DEBOUNCE) return;
    lastToggleTime = millis();
    if      (a=="dolby") dolbyMode=(dolbyMode+1)%4;
    else if (a=="mpx") {
      mpxFilter=!mpxFilter;
      if(biipWeb) { if(mpxFilter) buzzMenuEnter(); else buzzMenuExit(); }
    }
    else if (a=="hx")    hxPro=!hxPro;
  }
}

void handleCmd() {
  execCommand(server.arg("a"));
  server.send(200,"text/plain","ok");
}

void handleSetting() {
  String k=server.arg("k"), v=server.arg("v");
  if      (k=="mode")      displayMode=constrain(v.toInt(),0,2);
  else if (k=="view")      viewMode=constrain(v.toInt(),1,4);
  else if (k=="pulseS")    pulsesSetting=constrain(v.toInt(),1,20);
  else if (k=="anim")      animMode=constrain(v.toInt(),1,6);
  else if (k=="peak")      peakHoldTime=constrain(v.toInt(),0,4000);
  else if (k=="rel")     { releaseLevel=constrain(v.toInt(),1,99); currentDecay=releaseLevel*0.005f; }
  else if (k=="redRel")  { redReleaseLevel=constrain(v.toInt(),1,99); redDecay=redReleaseLevel*0.005f; }
  else if (k=="balance")   balanceVal=constrain(v.toInt(),-5,5);
  else if (k=="gain")      gainLevel=constrain(v.toInt(),1,200);
  else if (k=="bright")  { brightnessLevel=constrain(v.toInt(),1,15); applyBrightness(); }
  else if (k=="silence")   silenceThreshold=constrain(v.toInt(),5,400);
  else if (k=="dolby")     dolbyMode=constrain(v.toInt(),0,3);
  else if (k=="mpx")       execCommand("mpx");
  else if (k=="aru")       aruLevel=constrain(v.toInt(),0,3);
  // OLED
  else if (k=="oledBr")  {
    oledBright=constrain(v.toInt(),0,99);
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50))==pdTRUE) {
      applyOledBrightness();
      xSemaphoreGive(i2cMutex);
    }
  }
  else if (k=="oledThr")   oledThreshold=constrain(v.toInt(),0,16);
  else if (k=="specGain")  specGain=constrain(v.toInt(),20,2000);
  else if (k=="specStyle") specStyle=constrain(v.toInt(),0,13);
  else if (k=="autoTime") { autoStyleTime=constrain(v.toInt(),2,10); }
  else if (k=="autoMask") { autoStyleMask=(uint16_t)constrain(v.toInt(),0,0x0FFF); }
  else if (k=="specPeaks") specPeaks=constrain(v.toInt(),0,3);
  else if (k=="standby")   standbyTime=constrain(v.toInt(),0,20);
  else if (k=="biipS")     biipStart=constrain(v.toInt(),0,1);
  else if (k=="biipP")     biipOut=constrain(v.toInt(),0,1);
  else if (k=="biipN")     biipMenu=constrain(v.toInt(),0,1);
  else if (k=="biipW")     biipWeb=constrain(v.toInt(),0,1);
  else if (k=="oledType") {
    oledType=constrain(v.toInt(),0,1);
    display=(oledType==0)?(OLEDDisplay*)&displaySSD:(OLEDDisplay*)&displaySH;
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))==pdTRUE) {
      display->init();
      if (oledRotate) display->flipScreenVertically();
      applyOledBrightness();
      display->clear(); display->display();
      xSemaphoreGive(i2cMutex);
    }
  }
  else if (k=="oledRot") {
    oledRotate=constrain(v.toInt(),0,1);
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))==pdTRUE) {
      if (oledRotate) display->flipScreenVertically();
      else { display->init(); applyOledBrightness(); }
      display->clear(); display->display();
      xSemaphoreGive(i2cMutex);
    }
  }
  // LED
  else if (k=="lc1")  ledColor1=constrain(v.toInt(),0,255);
  else if (k=="lc2")  ledColor2=constrain(v.toInt(),0,255);
  else if (k=="lbr")  ledBright=constrain(v.toInt(),0,255);
  markSettingsDirty();
  server.send(200,"text/plain","ok");
}

void handleSaveSettings() { markSettingsDirty(); server.send(200,"text/plain","ok"); }

void handleIR() {
  if (server.hasArg("bind")) irBindMode=!irBindMode;
  if (server.hasArg("to")) {
    uint32_t irCode = 0; bool hasIR = false;
    portENTER_CRITICAL(&irMux);
    if (newIRCode) { irCode = lastIRCode; hasIR = true; }
    portEXIT_CRITICAL(&irMux);
    if (hasIR) {
      String t=server.arg("to");
      uint32_t* arr = t=="play"?irPlay:t=="stop"?irStop:t=="rew"?irRew:t=="fwd"?irFwd:irDolby;
      for (int i=0;i<3;i++) {
        if (arr[i]==0 || arr[i]==irCode) { arr[i]=irCode; break; }
      }
      saveIRSettings(); irBindMode=false;
      portENTER_CRITICAL(&irMux); newIRCode=false; portEXIT_CRITICAL(&irMux);
    }
  }
  if (server.hasArg("clear")) {
    String t=server.arg("clear");
    uint32_t* arr = t=="play"?irPlay:t=="stop"?irStop:t=="rew"?irRew:t=="fwd"?irFwd:irDolby;
    for (int i=0;i<3;i++) arr[i]=0;
    saveIRSettings();
  }
  if (server.hasArg("clearone")) {
    String t=server.arg("clearone");
    int idx=server.arg("idx").toInt();
    uint32_t* arr = t=="play"?irPlay:t=="stop"?irStop:t=="rew"?irRew:t=="fwd"?irFwd:irDolby;
    if(idx>=0&&idx<3){
      arr[idx]=0;
      // Зсуваємо щоб не було дірок
      for(int i=idx;i<2;i++) arr[i]=arr[i+1];
      arr[2]=0;
    }
    saveIRSettings();
  }
  // Повертаємо JSON зі станом IR
  auto codeStr = [](uint32_t c) -> String {
    return c ? "0x"+String(c,HEX) : "";
  };
  String json = "{\"bind\":" + String(irBindMode?1:0) +
    ",\"play\":[\"" + codeStr(irPlay[0]) + "\",\"" + codeStr(irPlay[1]) + "\",\"" + codeStr(irPlay[2]) + "\"]" +
    ",\"stop\":[\"" + codeStr(irStop[0]) + "\",\"" + codeStr(irStop[1]) + "\",\"" + codeStr(irStop[2]) + "\"]" +
    ",\"rew\":[\"" + codeStr(irRew[0])  + "\",\"" + codeStr(irRew[1])  + "\",\"" + codeStr(irRew[2])  + "\"]" +
    ",\"fwd\":[\"" + codeStr(irFwd[0])  + "\",\"" + codeStr(irFwd[1])  + "\",\"" + codeStr(irFwd[2])  + "\"]" +
    ",\"dolby\":[\"" + codeStr(irDolby[0]) + "\",\"" + codeStr(irDolby[1]) + "\",\"" + codeStr(irDolby[2]) + "\"]}";
  server.send(200,"application/json",json);
}

void handleStatus() {
  char irBuf[12];
  uint32_t irCode = 0;
  bool hasIR = false;
  portENTER_CRITICAL(&irMux);
  if (newIRCode) { irCode = lastIRCode; hasIR = true; }
  portEXIT_CRITICAL(&irMux);
  if (hasIR) snprintf(irBuf, sizeof(irBuf), "0x%X", irCode);
  else       strncpy(irBuf, "---", sizeof(irBuf));

  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"ir\":\"%s\",\"counter\":%d,\"vu_l\":%d,\"vu_r\":%d}",
    irBuf, tapeCounter, (int)smoothL, (int)smoothR);
  server.send(200, "application/json", buf);
}

void handleSetWifi() {
  wifiSSID=server.arg("ssid"); wifiPass=server.arg("pass");
  useStaticIP=(server.arg("us")=="1");
  if (useStaticIP) {
    staticIP.fromString(server.arg("si"));
    staticGW.fromString(server.arg("sg"));
    staticMask.fromString(server.arg("sm"));
  }
  saveWifiSettings();
  server.send(200,"text/plain","ok");
  analogWrite(PIN_BRIGHTNESS_PWM,0);
  delay(500); ESP.restart();
}

void handleSetMqtt() {
  mqttEnabled=(server.arg("en")=="1");
  mqttServer=server.arg("srv");
  mqttPort=server.arg("port").toInt();
  mqttUser=server.arg("user");
  mqttPass=server.arg("pass");
  saveMqttSettings();
  server.send(200,"text/plain","ok");
  if (mqttEnabled) mqttConnect();
}

void handleOTA() {
  HTTPUpload& upload=server.upload();
  if (upload.status==UPLOAD_FILE_START) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status==UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status==UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      Update.printError(Serial);
    }
  }
}
void handleOTADone() {
  server.send(200,"text/plain", Update.hasError()?"FAIL":"OK");
  delay(500);
  analogWrite(PIN_BRIGHTNESS_PWM,0);
  ESP.restart();
}

// ===================== VFD =====================
void applyBrightness() { analogWrite(PIN_BRIGHTNESS_PWM,map(brightnessLevel,1,15,10,255)); }

// ===================== Buzzer =====================
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_E6  1319
#define NOTE_G6  1568
#define NOTE_A6  1760
#define NOTE_B6  1976
#define NOTE_C7  2093
#define NOTE_E7  2637
#define NOTE_GS5 831


const uint8_t sinTable[64] = {
  128,140,152,165,176,187,197,206,214,221,227,231,235,237,238,238,
  236,233,228,222,215,207,198,188,177,166,154,142,130,117,105, 93,
   81, 70, 59, 49, 40, 32, 25, 19, 14, 10,  7,  5,  4,  4,  6,  9,
   13, 18, 25, 32, 41, 50, 60, 71, 82, 94,106,118,130,140,152,128
};
struct PolyNote { int freqA; int freqB; int duration; };

// Старт — Samsung стиль
const PolyNote samsungStartup[] = {
  {NOTE_G4,  NOTE_B4,  150},
  {NOTE_B4,  NOTE_D5,  150},
  {NOTE_D5,  NOTE_G5,  150},
  {NOTE_G5,  NOTE_B5,  450},
  {NOTE_D5,  NOTE_FS5, 150},
  {NOTE_B4,  NOTE_D5,  150},
  {NOTE_G4,  NOTE_B4,  450},
};

// Псевдо-поліфонія через ledc — чергування двох частот
void buzzPolyNote(int freqA, int freqB, int dur) {
  unsigned long start = millis();
  int periodA = freqA > 0 ? 1000000 / freqA / 2 : 0;
  int periodB = freqB > 0 ? 1000000 / freqB / 2 : 0;
  unsigned long tA = 0, tB = 0;
  bool stateA = false, stateB = false;
  while (millis() - start < (unsigned long)dur) {
    unsigned long us = micros();
    if (freqA > 0 && us - tA >= (unsigned long)periodA) { stateA = !stateA; tA = us; }
    if (freqB > 0 && us - tB >= (unsigned long)periodB) { stateB = !stateB; tB = us; }
    digitalWrite(PIN_BUZZER, (stateA || stateB) ? HIGH : LOW);
  }
  digitalWrite(PIN_BUZZER, LOW);
}

// Поліфонія з затуханням через ledc
void buzzPolyNoteDecay(int freqA, int freqB, int dur) {
  // Використовуємо середню частоту між двома нотами
  int freq = (freqA + freqB) / 2;
  ledcSetup(0, freq, 8);
  ledcAttachPin(PIN_BUZZER, 0);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)dur) {
    float vol = 1.0f - (float)(millis() - start) / (float)dur;
    int duty = (int)(vol * vol * 180);
    ledcWrite(0, duty);
    delay(4);
  }
  ledcWrite(0, 0);
  ledcDetachPin(PIN_BUZZER);
  digitalWrite(PIN_BUZZER, LOW);
}

// Таблиця синусу 64 точки, 8-bit (0-255)

// Нота з затуханням через ledc PWM
void buzzToneDecay(int freq, int dur) {
  ledcSetup(0, freq, 8);
  ledcAttachPin(PIN_BUZZER, 0);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)dur) {
    float vol = 1.0f - (float)(millis() - start) / (float)dur;
    int duty = (int)(vol * vol * 200);
    ledcWrite(0, duty);
    delay(4);
  }
  ledcWrite(0, 0);
  ledcDetachPin(PIN_BUZZER);
  digitalWrite(PIN_BUZZER, LOW);
}

// 1. Старт — з затуханням
void buzzStartup() {
  int n = sizeof(samsungStartup) / sizeof(PolyNote);
  for (int i = 0; i < n; i++) {
    buzzPolyNoteDecay(samsungStartup[i].freqA, samsungStartup[i].freqB, samsungStartup[i].duration);
    delay(10);
  }
}

// 2. Клік енкодера — "Пілім" з затуханням
void buzzClick() {
  buzzPolyNoteDecay(NOTE_C6, NOTE_E5, 80);
  buzzPolyNoteDecay(NOTE_G5, NOTE_B4, 120);
}

// 3. Перехід між групами — "Пі лі лім" з затуханням
void buzzGroup() {
  buzzPolyNoteDecay(NOTE_E6, NOTE_C6, 100);
  buzzPolyNoteDecay(NOTE_C6, NOTE_G5, 100);
  buzzPolyNoteDecay(NOTE_G5, NOTE_E5, 200);
}

// 4. Вхід в меню — висхідна A6→B6→E7
void buzzMenuEnter() {
  buzzToneDecay(NOTE_A6, 120);
  buzzToneDecay(NOTE_B6, 60);
  buzzToneDecay(NOTE_E7, 270);
}

// 5. Вихід з меню — ноти дзеркально, тривалості ті самі
void buzzMenuExit() {
  buzzToneDecay(NOTE_E7, 120);
  buzzToneDecay(NOTE_B6, 60);
  buzzToneDecay(NOTE_A6, 270);
}

// 6. Поворот енкодера — C7 з затуханням
void buzzEncoderTurn() {
  buzzToneDecay(NOTE_C7, 80);
}


void applyOledBrightness() {
  // Плавний діапазон 0-99
  // 0 = мінімум (dim режим), 99 = максимум
  int c = map(oledBright, 0, 99, 0, 255);
  if (oledBright < 10) {
    display->setContrast(c, 5, 0);   // дуже темно
  } else {
    display->setContrast(c, 241, 64); // нормально
  }
}

volatile bool cached_REWIND_SENSE=false;

void IRAM_ATTR handlePulse() {
  unsigned long now=millis();
  if (now-lastPulseTime>5) {
    bool reverse=cached_REWIND_SENSE;
    pulseSubCounter++;
    reelPulseCount++;
    if (pulseSubCounter>=pulsesSetting){pulseSubCounter=0;if(reverse)tapeCounter--;else tapeCounter++;}
    lastPulseTime=now;
  }
}

void IRAM_ATTR readEncoder() {
  static uint8_t old_AB=0;
  old_AB<<=2; old_AB|=(digitalRead(ENC_CLK)<<1)|digitalRead(ENC_DT);
  const int8_t states[]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
  int delta=states[old_AB&0x0F];
  if (delta!=0&&encCounter<1000&&encCounter>-1000) encCounter+=delta;
}

void setBit(int chipNum,int bit,bool state) {
  int idx=chipNum-1; if(idx<0||idx>2) return;
  if(state) chipBuffers[idx]|=(1ULL<<bit); else chipBuffers[idx]&=~(1ULL<<bit);
}

void vfdSend() {
  for(int c=0;c<3;c++){
    digitalWrite(latchPins[c],LOW);
    for(int i=0;i<40;i++){
      digitalWrite(PIN_DATA,(chipBuffers[c]&(1ULL<<i))?HIGH:LOW);
      digitalWrite(PIN_CK,HIGH); delayMicroseconds(5);
      digitalWrite(PIN_CK,LOW);  delayMicroseconds(5);
    }
    digitalWrite(latchPins[c],HIGH); delayMicroseconds(10);
    digitalWrite(latchPins[c],LOW);
  }
}

void renderBar(int val,float& pPos,float& pVel,unsigned long& t,int channel) {
  const int leftL[]={5,6,8,9,10,11,12,13,14,16,17,18,22,21,20,19};
  const int rC2[]={2,1,0},rC3[]={37,36,35,34,33,32,30,29,28,24,25,26,27};
  if(channel==0) for(int i=0;i<16;i++) setBit(3,leftL[i],false);
  else{for(int i=0;i<3;i++) setBit(2,rC2[i],false);for(int i=0;i<13;i++) setBit(3,rC3[i],false);}
  for(int i=0;i<val&&i<16;i++){
    if(channel==0) setBit(3,leftL[i],true);
    else{if(i<3) setBit(2,rC2[i],true);else setBit(3,rC3[i-3],true);}
  }
  if(animMode==4) return;

  // Для режиму відскоку — не скидаємо pVel поки відскок активний
  static bool bouncing[2] = {false, false};
  bool& isBouncing = bouncing[channel];

  if((float)val>=pPos && !isBouncing){
    pPos=(float)val; pVel=0; t=millis();
  } else {
    if(millis()-t>(unsigned long)peakHoldTime){
      static unsigned long demoTimer=0;
      static int demoAnim=1;
      if(animMode==6 && millis()-demoTimer>4000){demoTimer=millis();demoAnim=demoAnim%5+1;}
      int anim = (animMode==6) ? demoAnim : animMode;

      if(anim==1){ pVel=0; pPos-=0.25f; isBouncing=false; }
      else if(anim==2){
        isBouncing=true;
        pVel+=0.2f;             // прискорення вниз
        pPos-=pVel;
        float floor = (float)val - 2.0f; // трохи нижче рівня
        if(pPos<=floor){
          if(pVel>1.0f){
            pVel=-pVel*0.45f;   // відскок вгору
            pPos=floor;
          } else {
            pPos=floor; pVel=0; isBouncing=false;
          }
        }
      }
      else if(anim==3){pVel+=0.06f;pPos+=pVel;if(pPos>30){pPos=0;pVel=0;} isBouncing=false;}
      else if(anim==4){ return; }
      else if(anim==5){ pPos=0; isBouncing=false; }
    }
  }
  if(pPos<0){pPos=0;pVel=0;}
  int pIdx=(int)pPos-1;
  if(pIdx>=0&&pIdx<16){
    if(channel==0) setBit(3,leftL[pIdx],true);
    else{if(pIdx<3) setBit(2,rC2[pIdx],true);else setBit(3,rC3[pIdx-3],true);}
  }
}

// ===================== OLED =====================
void drawReel(int cx, int cy, int r, float angle) {
  // Зовнішнє коло
  display->drawCircle(cx, cy, r);
  // Три заповнених білих сектори як у Revox
  for (int i = 0; i < 3; i++) {
    float a = angle + i * (2.0f * M_PI / 3.0f);
    float a1 = a - 0.45f, a2 = a + 0.45f;
    int ri = 5, ro = r - 1;
    // Заповнюємо сектор білим — багато ліній від втулки до краю
    for (float t = 0; t <= 1.0f; t += 0.05f) {
      float aa = a1 + (a2 - a1) * t;
      int x1 = cx + (int)(ri * cosf(aa));
      int y1 = cy + (int)(ri * sinf(aa));
      int x2 = cx + (int)(ro * cosf(aa));
      int y2 = cy + (int)(ro * sinf(aa));
      display->drawLine(x1, y1, x2, y2);
    }
  }
  // Втулка — чорне коло з білим контуром
  display->setColor(BLACK); display->fillCircle(cx, cy, 5);
  display->drawCircle(cx, cy, 5);
  display->setColor(WHITE); display->fillCircle(cx, cy, 2);
}

void drawOLEDVUbar(int x, int y, int w, int h, int val, int peak) {
  display->setColor(BLACK);
  display->fillRect(x, y, w, h);
  display->setColor(WHITE);
  int filled = (val * h) / 16;
  if (filled > 0) { display->fillRect(x, y + h - filled, w, filled); }
  if (peak > 0 && peak <= 16) {
    int py = y + h - (peak * h) / 16;
    display->drawHorizontalLine(x, py, w);
  }
}

void drawReelsMode() {
  drawReel(22, 26, 20, reelAngleL);
  drawReel(106, 26, 20, reelAngleR);
  drawOLEDVUbar(50, 2, 7, 44, (int)smoothL, (int)peakPosL);
  drawOLEDVUbar(70, 2, 7, 44, (int)smoothR, (int)peakPosR);
  display->setFont(ArialMT_Plain_10); display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(51, 47, "L");
  display->drawString(71, 47, "R");
  display->drawLine(22, 46, 42, 62);
  display->drawLine(42, 62, 86, 62);
  display->drawLine(86, 62, 106, 46);
}

void updateSpectrum() {
  if (specGain <= 0) specGain = 1;
  if (!circBufReady) return;
  circBufReady = false;

  for (int i = 0; i < FFT_SAMPLES; i++) {
    fftReal[i] = (double)(circBufL[i] + circBufR[i]) / 65536.0;
    fftImag[i] = 0;
  }
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();
  fftReal[0] = 0; // DC складова
  fftReal[1] = 0; // шум 50 Гц від мережі

  // Логарифмічний розподіл бінів по 16 смугах
  // Діапазон: 250 Гц - 16000 Гц (при sample rate 32000 Гц)
  // FFT_SAMPLES/2 = 128 корисних бінів, кожен = 32000/256 = 125 Гц
  const int totalBins = FFT_SAMPLES / 2; // 256

  for (int b = 0; b < 16; b++) {
    int bLow  = fftBinLow[b];
    int bHigh = min(fftBinHigh[b], totalBins - 1);

    float sum = 0;
    int count = 0;
    for (int i = bLow; i <= bHigh; i++) {
      sum += fftReal[i];
      count++;
    }
    float val = constrain(sum / max(count, 1) / (float)specGain, 0, 1.0f);
    if (val < 0.15f) val = 0; // поріг шуму для PCM1808
    spectrumBarsBuf[b] += (val - spectrumBarsBuf[b]) * 0.3f;
  }
  // Копіюємо в основний масив атомарно
  for (int i = 0; i < 16; i++) spectrumBars[i] = spectrumBarsBuf[i];
}

void updatePeaks() {
  for (int i = 0; i < 16; i++) {
    float bar = spectrumBars[i];
    float barH = bar * 62;
    if (specPeaks == 1) {
      // Падіння вниз
      if (barH > peakBars[i]) { peakBars[i] = barH; peakVelBars[i] = 0; }
      else { peakVelBars[i] += 0.3f; if(peakVelBars[i]>10) peakVelBars[i]=10; peakBars[i] -= peakVelBars[i]; }
      if (peakBars[i] < 0) { peakBars[i] = 0; peakVelBars[i] = 0; }
    } else if (specPeaks == 2) {
      // Улітання вгору — мінімальна швидкість щоб пік завжди перелетів екран
      if (barH > peakBars[i]) { peakBars[i] = barH; peakVelBars[i] = max(4.0f, barH * 0.2f); }
      else { peakBars[i] += peakVelBars[i]; peakVelBars[i] *= 0.92f; }
      if (peakBars[i] > 63 || peakVelBars[i] < 0.1f) { peakBars[i] = 0; peakVelBars[i] = 0; }
    } else if (specPeaks == 3) {
      // Зависання — тримається на максимумі
      if (barH > peakBars[i]) peakBars[i] = barH;
      else peakBars[i] -= 0.05f;
      if (peakBars[i] < 0) peakBars[i] = 0;
    }
  }
}

void drawPeaks() {
  if (specPeaks == 0) return;
  display->setColor(WHITE);
  for (int i = 0; i < 16; i++) {
    int px = i * 8;
    if (specPeaks == 2) {
      // Улітання — пік летить вгору
      int py = 63 - (int)peakBars[i] - 4;
      if (py >= 0 && py < 63) display->drawHorizontalLine(px, py, 6);
    } else {
      // Падіння або зависання — пік зверху стовпця
      int py = 63 - (int)peakBars[i] - 1;
      if (py >= 0 && py < 63) display->drawHorizontalLine(px, py, 6);
    }
  }
}

void drawSpectrumBars() {
  int activeStyle = (specStyle == 12 || specStyle == 13) ? autoStyleCurrent : specStyle;
  display->setColor(BLACK); display->fillRect(0, 0, 128, 64);
  display->setColor(WHITE);

  for (int i = 0; i < 16; i++) {
    int h = (int)(spectrumBars[i] * 62);
    int x = i * 8;
    if (activeStyle == 0) {
      if (h > 0) display->fillRect(x, 63 - h, 6, h);
    } else if (activeStyle == 1) {
      for (int j = 0; j < h; j += 4)
        display->fillRect(x, 63 - j, 6, 2);
    } else if (activeStyle == 2) {
      int half = h / 2;
      if (half > 0) {
        display->fillRect(x, 32 - half, 6, half);
        display->fillRect(x, 32, 6, half);
      }
    } else if (activeStyle == 3) {
      if (h > 0) display->fillRect(x, 0, 6, h);
    }
  }
  drawPeaks();
}

// VU метр — дві стрілки L і R зі шкалами
void drawVUMeter() {
  // Два VU метри: лівий центр cx1=32, правий cx2=96
  const int cx1=32, cx2=96, cy=68, R=58;
  // Кути: від -2.35 (ліво) до -0.79 (право) — верхня півкола
  const float aMin=-2.35f, aMax=-0.79f;

  // Мітки шкали: значення в dB та відносна позиція 0.0-1.0
  struct Mark { float pos; const char* label; bool big; };
  const Mark marks[] = {
    {0.0f,  "-20", true},
    {0.2f,  "-10", true},
    {0.4f,  "-7",  true},
    {0.5f,  "",    false},
    {0.6f,  "-3",  true},
    {0.7f,  "0",   true},
    {0.85f, "+3",  true},
    {1.0f,  "+5",  true},
    // Короткі риски між
    {0.1f, "", false},{0.15f,"",false},{0.25f,"",false},{0.3f,"",false},
    {0.35f,"",false},{0.45f,"",false},{0.55f,"",false},{0.65f,"",false},
    {0.75f,"",false},{0.8f, "",false},{0.9f, "",false},{0.95f,"",false},
  };
  const int nMarks = sizeof(marks)/sizeof(marks[0]);

  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);

  for (int ci = 0; ci < 2; ci++) {
    int cx = (ci==0) ? cx1 : cx2;
    float sig = (ci==0) ? smoothL : smoothR;
    float angle = aMin + (sig/16.0f) * (aMax - aMin);

    // Малюємо дугу шкали
    for (float a = aMin; a <= aMax; a += 0.03f) {
      int x = cx + (int)(R * cosf(a));
      int y = cy + (int)(R * sinf(a));
      if (x>=0&&x<128&&y>=0&&y<64) display->setPixel(x, y);
    }

    // Мітки
    for (int m = 0; m < nMarks; m++) {
      float a = aMin + marks[m].pos * (aMax - aMin);
      int rOuter = R;
      int rInner = marks[m].big ? R-6 : R-3;
      int x1 = cx + (int)(rOuter * cosf(a));
      int y1 = cy + (int)(rOuter * sinf(a));
      int x2 = cx + (int)(rInner * cosf(a));
      int y2 = cy + (int)(rInner * sinf(a));
      display->drawLine(x1, y1, x2, y2);

      // Підпис
      if (marks[m].big && marks[m].label[0] != '\0') {
        int rLabel = R - 14;
        int lx = cx + (int)(rLabel * cosf(a));
        int ly = cy + (int)(rLabel * sinf(a)) - 4;
        if (ly < 0) ly = 0;
        if (lx>=4&&lx<=123&&ly>=0&&ly<54)
          display->drawString(lx, ly, marks[m].label);
      }
    }

    // Pivot — верхнє півколо ⌓ знизу екрану
    int pivX = cx, pivY = 63;
    for (float a = M_PI; a <= 2*M_PI; a += 0.1f) {
      int px = pivX + (int)(5 * cosf(a));
      int py = pivY + (int)(5 * sinf(a));
      if (px>=0&&px<128&&py>=0&&py<64) display->setPixel(px, py);
    }
    display->fillCircle(pivX, pivY, 2);

    // Стрілка від pivot
    int ax = cx + (int)(52 * cosf(angle));
    int ay = cy + (int)(52 * sinf(angle));
    display->drawLine(pivX, pivY, ax, ay);
    // Коротка протилежна частина
    int bx = pivX - (int)(6 * cosf(angle));
    int by = pivY - (int)(6 * sinf(angle));
    if (by >= 0 && by < 64) display->drawLine(pivX, pivY, bx, by);
  }

  // Підписи — L лівіше від pivot, VU правіше
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(cx1-12, 54, "L");
  display->drawString(cx2-12, 54, "R");
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(cx1+9, 54, "VU");
  display->drawString(cx2+9, 54, "VU");
}

// Очі в такт музиці
void drawEyesBeat() {
  int ex1 = 32, ex2 = 96, ey = 26;
  int eyeR = 13; // +15% (було 11)
  int pupil = constrain(2 + (int)(smoothRed * 0.35f), 2, 6);
  float level = constrain(smoothRed / 12.0f, 0.0f, 1.0f);
  float levelBM = level * 0.8f; // -20% чутливість для бров і рота

  // Брови
  display->setColor(WHITE);
  int browY = 4 - (int)(levelBM * 5.0f);
  display->drawLine(ex1-9, browY+3, ex1+1, browY);
  display->drawLine(ex1-9, browY+4, ex1+1, browY+1);
  display->drawLine(ex2-1, browY,   ex2+9,  browY+3);
  display->drawLine(ex2-1, browY+1, ex2+9,  browY+4);

  // Очі
  display->setColor(WHITE);
  display->fillCircle(ex1, ey, eyeR);
  display->fillCircle(ex2, ey, eyeR);
  display->setColor(BLACK);
  display->fillCircle(ex1, ey, pupil);
  display->fillCircle(ex2, ey, pupil);
  display->setColor(WHITE);
  display->fillCircle(ex1+3, ey-3, 2);
  display->fillCircle(ex2+3, ey-3, 2);

  // Носик — піднімається коли рот відкривається
  display->setColor(WHITE);
  int noseY = (int)(levelBM * 3.0f); // піднімається при сигналі
  display->drawLine(64-2, 37-noseY, 64-4, 43-noseY);
  display->drawLine(64+2, 37-noseY, 64+4, 43-noseY);
  display->drawLine(64-4, 43-noseY, 64+4, 43-noseY);

  // Рот — посмішка яка відкривається
  display->setColor(WHITE);
  int mouthCX = 64, mouthY = 52;
  int mouthW = 18 + (int)(levelBM * 8.0f); // ширший рот

  // Нижня дуга — посмішка
  for (float a = 0.15f; a <= M_PI - 0.15f; a += 0.05f) {
    int x = mouthCX + (int)(mouthW * cosf(a));
    int y = mouthY  + (int)((5.0f + levelBM * 6.0f) * sinf(a));
    if (x>=0&&x<128&&y>=0&&y<64) display->setPixel(x, y);
  }
  // При гучному сигналі — верхня дуга
  if (levelBM > 0.25f) {
    float openH = levelBM * 7.0f;
    for (float a = 0.15f; a <= M_PI - 0.15f; a += 0.05f) {
      int x = mouthCX + (int)(mouthW * cosf(a));
      int y = mouthY  - (int)(openH * sinf(a));
      if (x>=0&&x<128&&y>=0&&y<64) display->setPixel(x, y);
    }
  }
}

void drawVUMeter2() {
  const float aMin=-M_PI, aMax=-M_PI/2;
  const int R=44;

  struct Mark { float pos; const char* label; bool big; int rL; };
  const Mark marks[] = {
    {0.15f, "-20",true,  R+16},
    {0.37f, "-8", true,  R+16},
    {0.52f, "-3", true,  R+16},
    {0.66f, "0",  true,  R+16},
    {0.8f,  "+3", true,  R+15},
    {0.1f,"",false,0},{0.27f,"",false,0},{0.42f,"",false,0},
    {0.57f,"",false,0},{0.72f,"",false,0},{0.9f,"",false,0},
  };
  const int nMarks = sizeof(marks)/sizeof(marks[0]);

  display->setColor(WHITE);
  display->setFont(ArialMT_Plain_10);

  for (int ci=0; ci<2; ci++) {
    int cx = (ci==0) ? 64 : 126;
    int cy = 63;
    float sig = (ci==0) ? smoothL : smoothR;

    // Дуга — товста від pos=0.66 ("0") до кінця ("+3")
    for (float a=aMin; a<=aMax; a+=0.02f) {
      float pos = (a - aMin) / (aMax - aMin);
      int x=cx+(int)(R*cosf(a)), y=cy+(int)(R*sinf(a));
      if (x<0||x>=128||y<0||y>=64) continue;
      display->setPixel(x,y);
      if (pos >= 0.66f) {
        int x2=cx+(int)((R+1)*cosf(a)), y2=cy+(int)((R+1)*sinf(a));
        int x3=cx+(int)((R-1)*cosf(a)), y3=cy+(int)((R-1)*sinf(a));
        if (x2>=0&&x2<128&&y2>=0&&y2<64) display->setPixel(x2,y2);
        if (x3>=0&&x3<128&&y3>=0&&y3<64) display->setPixel(x3,y3);
      }
    }

    // Риски і цифри
    for (int m=0; m<nMarks; m++) {
      float a   = aMin + marks[m].pos*(aMax-aMin);
      int rIn   = R;
      int rOut  = marks[m].big ? R+8 : R+4;
      int x1=cx+(int)(rIn *cosf(a)), y1=cy+(int)(rIn *sinf(a));
      int x2=cx+(int)(rOut*cosf(a)), y2=cy+(int)(rOut*sinf(a));
      if (x1>=0&&x1<128&&y1>=0&&y1<64) display->drawLine(x1,y1,x2,y2);
      if (marks[m].big && marks[m].label[0]!='\0') {
        int lx=cx+(int)(marks[m].rL*cosf(a));
        int ly=cy+(int)(marks[m].rL*sinf(a))-4;
        if (lx>=4&&lx<=123&&ly>=0&&ly<=53) {
          display->setTextAlignment(TEXT_ALIGN_CENTER);
          display->drawString(lx, ly, marks[m].label);
        }
      }
    }

    // Стрілка
    float angle = aMin + (sig/16.0f)*(aMax-aMin);
    int ax=cx+(int)(52*cosf(angle)), ay=cy+(int)(52*sinf(angle));
    int bx=cx-(int)(8*cosf(angle)),  by=cy-(int)(8*sinf(angle));
    ax=constrain(ax,0,127); ay=constrain(ay,0,63);
    display->drawLine(cx,cy,ax,ay);
    // Хвостик прибрано

    // Центральна точка
    for (float a=aMin; a<=aMax; a+=0.1f)
      for (int r=0; r<=5; r++) {
        int px=cx+(int)(r*cosf(a)), py=cy+(int)(r*sinf(a));
        if (px>=0&&px<128&&py>=0&&py<64) display->setPixel(px,py);
      }
  }
}

// ===== Стиль 8: Стереогоніометр + індикатор кореляції =====
void drawLissajous() {
  static int16_t gonX[128] = {};
  static int16_t gonY[128] = {};
  static int gonIdx = 0;
  static float corrSmooth = 0.0f;

  int rawL = (int)(i2sInstL / 1600);
  int rawR = (int)(i2sInstR / 1600);

  // Гоніометрична проекція (центр 64,24 — верхня частина)
  int px = 64 + (rawL - rawR) * 22 / 2048;
  int py = 24 - (rawL + rawR) * 22 / 2048;
  gonX[gonIdx] = (int16_t)constrain(px, 0, 127);
  gonY[gonIdx] = (int16_t)constrain(py, 2, 46);
  gonIdx = (gonIdx + 1) % 128;

  // Кореляція: C = (L*R) / sqrt(L²+1) / sqrt(R²+1), діапазон -1..+1
  float fL = (float)rawL / 2048.0f;
  float fR = (float)rawR / 2048.0f;
  float num = fL * fR;
  float den = sqrtf(fL*fL + 0.001f) * sqrtf(fR*fR + 0.001f);
  float corr = (den > 0.01f) ? constrain(num/den, -1.0f, 1.0f) : 0.0f;
  corrSmooth += (corr - corrSmooth) * 0.05f;

  display->setFont(ArialMT_Plain_10);

  // === Підписи L і R зверху ===
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 0, "L");
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(127, 0, "R");

  // Числове значення кореляції — праворуч y=20
  char corrBuf[8];
  snprintf(corrBuf, sizeof(corrBuf), "%.1f", corrSmooth);
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(127, 20, corrBuf);

  // Ширина стерео бази у % — ліворуч y=20
  int widthPct = (int)((1.0f - corrSmooth) * 50.0f);
  widthPct = constrain(widthPct, 0, 100);
  char widBuf[8];
  snprintf(widBuf, sizeof(widBuf), "%d%%", widthPct);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 20, widBuf);

  // === Діагональ L: (0,0)→(127,47) ===
  for (int x = 10; x < 118; x += 3) {
    int y = (x * 47) / 127;
    if (y >= 2 && y <= 46) display->setPixel(x, y);
  }
  // === Діагональ R: (127,0)→(0,47) ===
  for (int x = 10; x < 118; x += 3) {
    int y = ((127-x) * 47) / 127;
    if (y >= 2 && y <= 46) display->setPixel(x, y);
  }
  // === Вертикаль: y=0..46, пунктир від самого верху ===
  for (int y = 0; y < 47; y += 4) display->setPixel(64, y);

  // === Слід гоніометра ===
  for (int i = 0; i < 128; i++) {
    int idx = (gonIdx + i) % 128;
    if (i > 20) display->setPixel(gonX[idx], gonY[idx]);
  }

  // === Індикатор кореляції знизу ===
  // x=16..111 = 96px, симетрично з обох боків, висота 11px
  const int barX = 16, barW = 96, barY = 51, barH = 11;
  const int barCX = barX + barW/2; // центр = 64
  display->drawRect(barX, barY, barW, barH);
  display->drawVerticalLine(barCX, barY, barH); // центральна риска

  // Підписи -1 і +1
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, barY+1, "-1");
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(127, barY+1, "+1");

  // Заповнення від центру
  int corrPx = (int)(corrSmooth * (barW/2 - 1));
  if (corrPx > 0) {
    display->fillRect(barCX+1, barY+1, constrain(corrPx, 0, barW/2-2), barH-2);
  } else if (corrPx < 0) {
    int w = constrain(-corrPx, 0, barW/2-2);
    display->fillRect(barCX-w, barY+1, w, barH-2);
  }
}

// ===== Стиль 9: Осцилограф =====
// L/R підписи ліворуч, хвилі починаються після підписів
void drawOscilloscope() {
  static int8_t oscL[128] = {};
  static int8_t oscR[128] = {};
  static int oscPos = 0;

  int rawL = (int)(i2sInstL / 1600);
  int rawR = (int)(i2sInstR / 1600);
  int scale = constrain(gainLevel * 16 / 25, 5, 17); // +30% розмах
  oscL[oscPos] = (int8_t)constrain(rawL * scale / 2048, -13, 13);
  oscR[oscPos] = (int8_t)constrain(rawR * scale / 2048, -13, 13);
  oscPos = (oscPos + 1) % 128;

  // Підписи ліворуч
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 0,  "L");
  display->drawString(0, 33, "R");

  // Роздільник
  display->drawHorizontalLine(0, 31, 128);

  // Нульові лінії (пунктир) — від x=12 щоб не перекривати L/R
  for (int x = 12; x < 128; x += 4) {
    display->setPixel(x, 15);
    display->setPixel(x, 47);
  }

  // Хвилі від x=12 (після підписів L/R) до x=127
  for (int x = 12; x < 127; x++) {
    int i1 = (oscPos + x)     % 128;
    int i2 = (oscPos + x + 1) % 128;
    int y1L = constrain(15 - oscL[i1], 2,  28);
    int y2L = constrain(15 - oscL[i2], 2,  28);
    int y1R = constrain(47 - oscR[i1], 34, 61);
    int y2R = constrain(47 - oscR[i2], 34, 61);
    display->drawLine(x, y1L, x+1, y2L);
    display->drawLine(x, y1R, x+1, y2R);
  }
}

// ===== Стиль 10: Горизонтальний VU метр з dB шкалою =====
void drawHorizVUMeter() {
  // Весь блок: L(11)+gap(3)+line+ticks(6)+digits(10)+ticks(6)+line+gap(3)+R(11) = 51px
  // Центрування: (64-51)/2 = 6px зверху
  const int Y0     = 6;   // відступ зверху (64-52)/2
  const int yL     = Y0;       // y=6,  L смуга 11px → y=6..16
  const int yLine1 = Y0+13;    // y=19, верхня лінія (відступ 2px після смуги)
  const int yTick1 = Y0+14;    // y=20, риски вниз 5px → y=20..24
  const int yDig   = Y0+21;    // y=27, цифри 10px → y=27..36 (відступ 2px після рисок)
  const int yTick2 = Y0+38;    // y=44, риски вгору 5px → y=44..48 (відступ 2px після цифр)
  const int yLine2 = Y0+43;    // y=49, нижня лінія (після рисок)
  const int yR     = Y0+46;    // y=52, R смуга 11px → y=52..62 (відступ 2px)

  // Смуги і шкала однакової ширини — до позиції +3
  // +3dB риска на x=12+107=119, текст центр там же
  // Рамка смуги від x=11, ширина 109 → до x=119 включно
  auto dbToX = [](float db) -> int {
    return constrain((int)((db + 20.0f) * 107.0f / 23.0f), 0, 107);
  };
  auto valToDB = [](float val) -> float {
    return (val < 0.1f) ? -40.0f : 20.0f * log10f(val / 16.0f);
  };

  int xL  = dbToX(valToDB(smoothL));
  int xR  = dbToX(valToDB(smoothR));
  int xPL = dbToX(valToDB(peakPosL));
  int xPR = dbToX(valToDB(peakPosR));

  display->setFont(ArialMT_Plain_10);

  // L смуга — ширина 109 = від x=11 до x=119 (як лінії шкали)
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, yL, "L");
  display->drawRect(11, yL, 109, 11);
  if (xL > 0) display->fillRect(12, yL+1, xL, 9);
  if (xPL > 0 && xPL < 107) display->drawVerticalLine(12+xPL, yL+1, 9);

  // Верхня лінія + риски вниз
  display->drawHorizontalLine(11, yLine1, 109);
  const float dbV[] = {-20,-10,-6,-3, 0, 3};
  const char* dbN[] = {"-20","-10","-6","-3","0","+3"};
  for (int i = 0; i < 6; i++) {
    int x = 12 + dbToX(dbV[i]);
    display->drawVerticalLine(x, yTick1, 5);
  }

  // Цифри
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  for (int i = 0; i < 6; i++) {
    int x = 12 + dbToX(dbV[i]);
    display->drawString(x, yDig, dbN[i]);
  }

  // Риски вгору + нижня лінія
  for (int i = 0; i < 6; i++) {
    int x = 12 + dbToX(dbV[i]);
    display->drawVerticalLine(x, yTick2, 5);
  }
  display->drawHorizontalLine(11, yLine2, 109);

  // R смуга — ширина 109
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, yR, "R");
  display->drawRect(11, yR, 109, 11);
  if (xR > 0) display->fillRect(12, yR+1, xR, 9);
  if (xPR > 0 && xPR < 107) display->drawVerticalLine(12+xPR, yR+1, 9);
}

// ===== Стиль 11: Вертикальний VU метр =====
// L ліворуч, R праворуч, шкала по центру з вертикальними лініями
void drawVertVUMeter() {
  // Розміщення по горизонталі:
  // L смуга:     x=0..37   (38px)
  // зазор:       x=38..40  (3px)
  // ліва лінія:  x=41
  // риски+цифри: x=42..85
  // права лінія: x=86
  // зазор:       x=87..89  (3px)
  // R смуга:     x=90..127 (38px)

  const int xL1 = 0,  wBar = 38; // L смуга
  const int xLL = 41;             // ліва вертикальна лінія
  const int xRL = 86;             // права вертикальна лінія
  const int xR1 = 90;             // R смуга

  // Шкала: +3, 0, -3, -8, -20 dB
  // 5 довгих рисок, 4 проміжки по 2 короткі = 13 рисок всього
  // Висота шкали: підбираємо щоб вмістилось у 64px
  // Підписи "L VU" і "R VU" зверху — 10px
  // Шкала: y=12..62 = 50px

  const int yTop = 12; // верхня риска (+3)
  const int yBot = 58; // нижня риска (-20) — підняли щоб не обрізало
  const int yH   = yBot - yTop; // 50px

  // Позиції 5 довгих рисок (рівномірно)
  // +3=0, 0=1, -3=2, -8=3, -20=4
  const int yMain[5] = {
    yTop,
    yTop + yH*1/4,
    yTop + yH*2/4,
    yTop + yH*3/4,
    yBot
  };
  const char* lbl[5] = {"+3","0","-3","-8","-20"};

  auto valToDB = [](float val) -> float {
    return (val < 0.1f) ? -40.0f : 20.0f * log10f(val / 16.0f);
  };
  auto dbToY = [&](float db) -> int {
    // +3dB=yTop, -20dB=yBot
    return constrain((int)(yTop + (3.0f - db) * yH / 23.0f), yTop, yBot);
  };

  int hL = constrain(yBot - dbToY(valToDB(smoothL)), 0, yH);
  int hR = constrain(yBot - dbToY(valToDB(smoothR)), 0, yH);
  int pyL = dbToY(valToDB(peakPosL));
  int pyR = dbToY(valToDB(peakPosR));

  // Підписи зверху
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(xL1 + wBar/2, 0, "L VU");
  display->drawString(xR1 + wBar/2, 0, "R VU");

  // L смуга (знизу вгору)
  display->drawRect(xL1, yTop, wBar, yH+1);
  if (hL > 0) display->fillRect(xL1+1, yBot-hL+1, wBar-2, hL);
  if (pyL > yTop && pyL < yBot)
    display->drawHorizontalLine(xL1+1, pyL, wBar-2);

  // R смуга (знизу вгору)
  display->drawRect(xR1, yTop, wBar, yH+1);
  if (hR > 0) display->fillRect(xR1+1, yBot-hR+1, wBar-2, hR);
  if (pyR > yTop && pyR < yBot)
    display->drawHorizontalLine(xR1+1, pyR, wBar-2);

  // Вертикальні лінії — від yTop до yBot (не виступають)
  display->drawVerticalLine(xLL, yTop, yH+1);
  display->drawVerticalLine(xRL, yTop, yH+1);

  // Довгі риски (8px) і цифри
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  for (int i = 0; i < 5; i++) {
    int y = yMain[i];
    display->drawHorizontalLine(xLL+1, y, 8);   // ліва довга риска всередину
    display->drawHorizontalLine(xRL-8, y, 8);   // права довга риска всередину
    display->drawString(63, y-4, lbl[i]);        // цифра по центру
  }

  // Короткі риски (4px) між довгими
  for (int i = 0; i < 4; i++) {
    // 2 короткі риски між кожною парою довгих
    int y1 = yMain[i] + (yMain[i+1]-yMain[i])/3;
    int y2 = yMain[i] + (yMain[i+1]-yMain[i])*2/3;
    for (int yi = 0; yi < 2; yi++) {
      int y = (yi==0) ? y1 : y2;
      display->drawHorizontalLine(xLL+1, y, 4);
      display->drawHorizontalLine(xRL-4, y, 4);
    }
  }
}

void drawSpectrumMode() {
  // Авто (12) — перебирає стилі 0-11 кожні Nс
  // Авто2 (13) — те саме, але бобіни не перехоплюють екран від імпульсів
  if (specStyle == 12 || specStyle == 13) {
    if (millis() - autoStyleTimer > (unsigned long)(autoStyleTime * 1000)) {
      autoStyleTimer = millis();
      uint16_t mask = autoStyleMask & 0x0FFF;
      if (mask == 0) mask = 0x0FFF;
      int prevStyle = autoStyleCurrent;
      for (int i = 1; i <= 12; i++) {
        int next = (autoStyleCurrent + i) % 12;
        if (mask & (1 << next)) { autoStyleCurrent = next; break; }
      }
      // Скидаємо піки при зміні стилю
      if (autoStyleCurrent != prevStyle) {
        for (int i = 0; i < 16; i++) { peakBars[i] = 0; peakVelBars[i] = 0; }
      }
    }
  }

  int activeStyle = (specStyle == 12 || specStyle == 13) ? autoStyleCurrent : specStyle;

  display->setColor(BLACK); display->fillRect(0, 0, 128, 64);
  display->setColor(WHITE);

  if      (activeStyle == 4)  { drawVUMeter(); }
  else if (activeStyle == 5)  { drawEyesBeat(); }
  else if (activeStyle == 6)  { drawReelsMode(); }
  else if (activeStyle == 7)  { drawVUMeter2(); }
  else if (activeStyle == 8)  { drawLissajous(); }
  else if (activeStyle == 9)  { drawOscilloscope(); }
  else if (activeStyle == 10) { drawHorizVUMeter(); }
  else if (activeStyle == 11) { drawVertVUMeter(); }
  else                        { drawSpectrumBars(); } // стилі 0-3
}

void fillRoundRectOLED(int x, int y, int w, int h, int r) {
  display->fillRect(x+r, y, w-2*r, h);
  display->fillRect(x, y+r, r, h-2*r);
  display->fillRect(x+w-r, y+r, r, h-2*r);
  display->fillCircle(x+r,     y+r,     r);
  display->fillCircle(x+w-r-1, y+r,     r);
  display->fillCircle(x+r,     y+h-r-1, r);
  display->fillCircle(x+w-r-1, y+h-r-1, r);
}

void drawEyesMode() {
  unsigned long now = millis();
  eyeX1 += eyeVX1; eyeX2 += eyeVX2;
  if (eyeX1 < 14 || eyeX1 > 48) eyeVX1 = -eyeVX1;
  if (eyeX2 < 76 || eyeX2 > 110) eyeVX2 = -eyeVX2;
  if (now - eyeLastBlink > 3000) { eyeLastBlink = now; eyeBlink = true; }
  if (eyeBlink && now - eyeLastBlink > 200) eyeBlink = false;

  // Рівень сигналу для реакції на музику (0.0 - 1.0)
  float musicLevel = constrain((float)(i2sLastL + i2sLastR) / 2.0f / 50000.0f, 0.0f, 1.0f);
  static float smoothMusic = 0.0f;
  smoothMusic += (musicLevel - smoothMusic) * 0.2f;

  int ex1 = (int)eyeX1, ex2 = (int)eyeX2, ey = 22;

  // Брови — піднімаються при гучному сигналі
  display->setColor(WHITE);
  int browY = 4 - (int)(smoothMusic * 6.0f); // чим гучніше — тим вище
  // Ліва брова
  display->drawLine(ex1-10, browY+3, ex1, browY);
  display->drawLine(ex1-10, browY+4, ex1, browY+1);
  // Права брова  
  display->drawLine(ex2, browY, ex2+10, browY+3);
  display->drawLine(ex2, browY+1, ex2+10, browY+4);

  if (!eyeBlink) {
    // Ліве око
    display->setColor(WHITE);
    display->fillCircle(ex1, ey, 13);
    display->setColor(BLACK);
    display->fillCircle(ex1+2, ey+2, 6);
    display->setColor(WHITE);
    display->fillCircle(ex1+4, ey, 2);
    // Праве око
    display->setColor(WHITE);
    display->fillCircle(ex2, ey, 13);
    display->setColor(BLACK);
    display->fillCircle(ex2+2, ey+2, 6);
    display->setColor(WHITE);
    display->fillCircle(ex2+4, ey, 2);
  } else {
    // Закриті очі
    display->setColor(WHITE);
    for (int i = -13; i <= 13; i++) {
      int yy = ey + (i*i)/26;
      display->setPixel(ex1+i, yy);
      display->setPixel(ex1+i, yy+1);
      display->setPixel(ex2+i, yy);
      display->setPixel(ex2+i, yy+1);
    }
  }

  // Жива усмішка — слідує за очима, реагує на моргання
  static float smileAmp = 8.0f;
  static float smileW   = 22.0f;
  static float smileMX  = 64.0f; // X центр усмішки

  float targetAmp = eyeBlink ? 14.0f : 7.0f + sinf(now * 0.0015f) * 2.0f;
  float targetW   = eyeBlink ? 28.0f : 22.0f + sinf(now * 0.001f) * 2.0f;
  // Усмішка слідує за середнім положенням очей
  float targetMX  = (eyeX1 + eyeX2) * 0.5f;

  smileAmp += (targetAmp - smileAmp) * 0.12f;
  smileW   += (targetW   - smileW)   * 0.12f;
  smileMX  += (targetMX  - smileMX)  * 0.08f;

  display->setColor(WHITE);
  int my = 52;
  int prevX = -1, prevY = -1;
  for (float a = 0.2f; a <= M_PI - 0.2f; a += 0.07f) {
    int x = (int)smileMX + (int)(smileW * cosf(a));
    int y = my + (int)(smileAmp * sinf(a)); // плюс = дуга вниз = усмішка
    if (x>=0&&x<128&&y>=0&&y<64) {
      if (prevX >= 0) {
        display->drawLine(prevX, prevY,   x, y);
        display->drawLine(prevX, prevY+1, x, y+1);
      }
      prevX = x; prevY = y;
    }
  }
}

void updateOLED() {
  if (!i2cMutex) return;

  // FFT семплування поза мютексом — не блокуємо I2C
  // FFT виконується на Core 0 (wifiTask) — тут тільки читаємо готові spectrumBars[]

  static unsigned long lastSuccessfulUpdate = 0;
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    // Mutex не вдалося взяти — якщо давно не оновлювались, відновлюємо I2C
    if (millis() - lastSuccessfulUpdate > 5000) {
      Wire.end();
      delay(10);
      Wire.begin(PCF_I2C_SDA, PCF_I2C_SCL);
      display->init();
      if (oledRotate) display->flipScreenVertically();
      applyOledBrightness();
      lastSuccessfulUpdate = millis();
    }
    return;
  }
  display->clear();

  switch (oledMode) {
    case OLED_REELS:    drawReelsMode();    break;
    case OLED_SPECTRUM: drawSpectrumMode(); break;
    case OLED_EYES:     drawEyesMode();     break;
    case OLED_OFF:      break;
  }

  display->display();
  lastSuccessfulUpdate = millis();
  xSemaphoreGive(i2cMutex);
}

// ===================== WebSocket =====================
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  // Приймаємо підключення — нічого не робимо, тільки надсилаємо дані
}

// Надсилаємо VU дані + стани транспорту всім підключеним клієнтам
void wsSendVU() {
  char buf[200];
  int st_rew  = ws_pcf_REWIND  ? 1 : 0;
  int st_stop = (!ws_pcf_PLAY && !ws_pcf_FORWARD && !ws_pcf_REWIND) ? 1 : 0;
  int st_play = ws_pcf_PLAY    ? 1 : 0;
  int st_pau  = ws_pcf_PAUSE   ? 1 : 0;
  int st_fwd  = ws_pcf_FORWARD ? 1 : 0;
  int ir_act  = (millis() - lastIRTime < 500) ? 1 : 0;
  snprintf(buf, sizeof(buf),
    "{\"l\":%d,\"r\":%d,\"pl\":%d,\"pr\":%d,\"cnt\":%d,\"rew\":%d,\"stp\":%d,\"ply\":%d,\"pau\":%d,\"fwd\":%d,\"hx\":%d,\"dlb\":%d,\"mpx\":%d,\"rec\":%d,\"ir\":%d}",
    (int)smoothL, (int)smoothR,
    (int)peakPosL, (int)peakPosR,
    tapeCounter,
    st_rew, st_stop, st_play, st_pau, st_fwd,
    hxPro ? 1 : 0,
    dolbyMode,
    mpxFilter ? 1 : 0,
    ws_pcf_REC ? 1 : 0,
    ir_act);
  wsServer.broadcastTXT(buf);

  // BLE NOTIFY статус — кожен раз разом з WS
  if (bleDeviceConnected && bleStatusChar) {
    char ble[80];
    snprintf(ble, sizeof(ble),
      "{\"rew\":%d,\"stp\":%d,\"ply\":%d,\"fwd\":%d,\"rec\":%d,\"dlb\":%d,\"mpx\":%d,\"hx\":%d,\"ir\":%d}",
      st_rew, st_stop, st_play, st_fwd,
      ws_pcf_REC ? 1 : 0,
      dolbyMode,
      mpxFilter ? 1 : 0,
      hxPro ? 1 : 0,
      ir_act);
    bleStatusChar->setValue(ble);
    bleStatusChar->notify();
  }
}

// ===================== WiFi Task (Core 0) =====================
void fftTask(void* param) {
  for (;;) {
    // Читаємо маленькими порціями — миттєво
    int32_t smallBuf[16];
    size_t sbBytes = 0;
    i2s_read(I2S_NUM_1, smallBuf, sizeof(smallBuf), &sbBytes, 0);
    int sbSamples = sbBytes / 8;
    if (sbSamples > 0) {
      int32_t maxL = 0, maxR = 0;
      for (int i = 0; i < sbSamples; i++) {
        int32_t L = abs(smallBuf[i*2] >> 8);
        int32_t R = abs(smallBuf[i*2+1] >> 8);
        if (L > maxL) maxL = L;
        if (R > maxR) maxR = R;
        // Миттєве значення зі знаком для гоніометра
        i2sInstL = smallBuf[i*2] >> 8;
        i2sInstR = smallBuf[i*2+1] >> 8;
        if (!circBufReady && circBufPos < FFT_SAMPLES) {
          circBufL[circBufPos] = smallBuf[i*2] >> 8;
          circBufR[circBufPos] = smallBuf[i*2+1] >> 8;
          circBufPos++;
          if (circBufPos >= FFT_SAMPLES) {
            circBufPos = 0;
            circBufReady = true;
          }
        }
      }
      i2sLastL = maxL;
      i2sLastR = maxR;
    }
    // FFT коли буфер готовий
    if (circBufReady) {
      updateSpectrum();
      bool forceShow = (currentState == MENU_SPEC_STYLE || currentState == MENU_SPEC_PEAKS);
      // Спектр завжди оновлюється — oledMute тільки для переключення в очі
      if (oledMode == OLED_SPECTRUM || forceShow) {
        updatePeaks();
      } else {
        // Тільки згасання коли не в режимі спектру (очі/бобіни)
        for (int i = 0; i < 16; i++) {
          spectrumBarsBuf[i] *= 0.85f;
          spectrumBars[i] = spectrumBarsBuf[i];
          peakBars[i] *= 0.85f;
        }
      }
    }
    vTaskDelay(1);
  }
}

void wifiTask(void* param) {
  esp_task_wdt_add(NULL);
  loadWifiSettings(); loadMqttSettings();
  if (wifiSSID.length()>0) startSTA(); else startAP();

  // Captive Portal — перенаправляємо всі запити на головну
  server.onNotFound([](){
    server.sendHeader("Location","http://"+WiFi.softAPIP().toString(),true);
    server.send(302,"text/plain","");
  });
  server.on("/",           handleRoot);
  server.on("/sysinfo", [](){
    uint32_t freeHeap  = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint8_t  usedPct   = (uint8_t)(100 - (freeHeap * 100 / totalHeap));
    uint32_t upSec     = millis() / 1000;
    uint32_t upMin     = upSec / 60;
    uint32_t upHr      = upMin / 60;
    char uptimeBuf[20];
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%02d:%02d:%02d", upHr, upMin%60, upSec%60);
    float temp = temperatureRead();
    String json = "{";
    json += "\"cpu\":"    + String(ESP.getCpuFreqMHz())  + ",";
    json += "\"temp\":"   + String(temp, 1)               + ",";
    json += "\"freeHeap\":"  + String(freeHeap)           + ",";
    json += "\"totalHeap\":" + String(totalHeap)          + ",";
    json += "\"usedPct\":"   + String(usedPct)            + ",";
    json += "\"rssi\":"      + String(WiFi.RSSI())        + ",";
    json += "\"uptime\":\"" + String(uptimeBuf)           + "\",";
    json += "\"fftSamples\":" + String(FFT_SAMPLES);
    json += "}";
    server.send(200, "application/json", json);
  });
  server.on("/manifest.json", [](){
    String m = "{";
    m += "\"name\":\"Tape Control\",";
    m += "\"short_name\":\"TapeCtrl\",";
    m += "\"start_url\":\"/\",";
    m += "\"display\":\"standalone\",";
    m += "\"background_color\":\"#0f3460\",";
    m += "\"theme_color\":\"#0f3460\",";
    m += "\"orientation\":\"portrait\",";
    m += "\"icons\":[{";
    m += "\"src\":\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'%3E";
    m += "%3Crect width='192' height='192' fill='%230f3460'/%3E";
    m += "%3Ccircle cx='64' cy='96' r='36' fill='none' stroke='%23e94560' stroke-width='8'/%3E";
    m += "%3Ccircle cx='128' cy='96' r='36' fill='none' stroke='%23e94560' stroke-width='8'/%3E";
    m += "%3Ccircle cx='64' cy='96' r='12' fill='%23e94560'/%3E";
    m += "%3Ccircle cx='128' cy='96' r='12' fill='%23e94560'/%3E";
    m += "%3Crect x='64' y='88' width='64' height='16' fill='%230f3460'/%3E";
    m += "\",\"sizes\":\"192x192\",\"type\":\"image/svg+xml\"}]";
    m += "}";
    server.send(200,"application/manifest+json",m);
  });
  server.on("/cmd",        handleCmd);
  server.on("/buzz", [](){
    if(biipWeb) { buzzToneDecay(NOTE_C7, 80); buzzToneDecay(NOTE_C7, 80); }
    server.send(200,"text/plain","ok");
  });
  server.on("/setting",    handleSetting);
  server.on("/savesettings",handleSaveSettings);
  server.on("/ir",         handleIR);
  server.on("/status",     handleStatus);
  server.on("/setwifi",    handleSetWifi);
  server.on("/setmqtt",    handleSetMqtt);
  server.on("/ota", HTTP_POST, handleOTADone, handleOTA);
  server.begin();

  wsServer.begin();
  wsServer.onEvent(wsEvent);

  // ArduinoOTA
  ArduinoOTA.setHostname("TapeDeck");
  ArduinoOTA.onStart([](){
    Serial.printf("OTA Start! heap=%d psram=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());
    analogWrite(PIN_BRIGHTNESS_PWM, 0);
    settingsDirty = false;
    if (fftTaskHandle) vTaskSuspend(fftTaskHandle);
    irrecv.disableIRIn();
    wsServer.disconnect();
    mqttClient.disconnect();
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_1);
    // Повністю вимикаємо watchdog на час OTA
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    esp_task_wdt_delete(xTaskGetHandle("loopTask"));
    esp_task_wdt_deinit();
    Serial.println("OTA ready!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total){
    static unsigned int lastPct = 0;
    unsigned int pct = progress * 100 / total;
    if (pct != lastPct && pct % 10 == 0) {
      Serial.printf("OTA %d%% heap=%d\n", pct, ESP.getFreeHeap());
      lastPct = pct;
    }
  });
  ArduinoOTA.onEnd([](){
    Serial.println("OTA Done!");
    ESP.restart();
  });
  ArduinoOTA.onError([](ota_error_t error){
    Serial.printf("OTA Error[%u]: ", error);
    if(error==OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if(error==OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if(error==OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if(error==OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if(error==OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  unsigned long lastWsSend=0;

  for(;;) {
    if (captivePortalActive) dnsServer.processNextRequest();
    server.handleClient();
    wsServer.loop();
    ArduinoOTA.handle();

    // BLE — перезапуск реклами після відключення
    if (!bleDeviceConnected && blePrevConnected) {
      delay(500);
      bleServer->startAdvertising();
      blePrevConnected = false;
    }
    if (bleDeviceConnected && !blePrevConnected) {
      blePrevConnected = true;
      bleCmdChar->setValue(""); // очищаємо старе значення
    }

    // BLE polling — читаємо значення характеристики кожні 100мс
    static unsigned long lastBlePoll = 0;
    if (bleDeviceConnected && bleCmdChar && millis()-lastBlePoll >= 100) {
      lastBlePoll = millis();
      if (!bleCmdPending) {
        std::string val = bleCmdChar->getValue();
        if (val.length() > 0 && val.length() < 15) {
          strncpy((char*)bleCmd, val.c_str(), 15);
          bleCmdChar->setValue("");
          bleCmdPending = true;
          Serial.print("BLE poll: "); Serial.println((char*)bleCmd);
        }
      }
    }

    // WebSocket VU — кожні 50мс
    if (millis()-lastWsSend>=50) { lastWsSend=millis(); wsSendVU(); }

    // WiFi стан
    if (wifiStatus==WIFI_CONNECTING) {
      if (WiFi.status()==WL_CONNECTED) {
        wifiStatus=WIFI_CONNECTED;
        lastWifiCheck=millis();
        MDNS.begin("tape");
        mqttConnect();
      } else if (millis()-lastWifiCheck>15000) {
        // Не підключились за 15с — переходимо в AP
        lastWifiCheck=millis(); startAP();
      }
    }
    if (wifiStatus==WIFI_CONNECTED) {
      if (WiFi.status()!=WL_CONNECTED) {
        // WiFi відвалився — спробуємо перепідключитись
        wifiStatus=WIFI_CONNECTING;
        lastWifiCheck=millis();
        WiFi.reconnect();
      } else {
        // Перевіряємо RSSI кожні 30с — якщо сигнал зник перепідключаємось
        if (millis()-lastWifiCheck>30000) {
          lastWifiCheck=millis();
          if (WiFi.RSSI() < -90) { // дуже слабкий сигнал
            wifiStatus=WIFI_CONNECTING;
            WiFi.reconnect();
          }
        }
      }
    }

    // MQTT
    if (mqttEnabled && wifiStatus==WIFI_CONNECTED) {
      if (!mqttClient.connected()) mqttConnect();
      else {
        mqttClient.loop();
        if (millis()-lastMqttPublish>5000) {
          lastMqttPublish=millis();
          mqttPublish();
        }
      }
    }

    // IR
    if (irrecv.decode(&irResults)) {
      uint32_t code=irResults.value;
      if (code!=0xFFFFFFFF && code!=0) {
        // Атомарний запис — захист від race condition з Core 1 (loop)
        portENTER_CRITICAL(&irMux);
        lastIRCode=code; newIRCode=true; lastIRTime=millis();
        portEXIT_CRITICAL(&irMux);
        if (!irBindMode) {
          if      (irMatchAny(irPlay,  code)) execCommand("play");
          else if (irMatchAny(irStop,  code)) execCommand("stop");
          else if (irMatchAny(irRew,   code)) execCommand("rew");
          else if (irMatchAny(irFwd,   code)) execCommand("fwd");
          else if (irMatchAny(irDolby, code)) execCommand("dolby");
        }
      }
      irrecv.resume();
    }
    vTaskDelay(1);
    esp_task_wdt_reset(); // Скидаємо watchdog Core 0
  }
}

// ===================== Стартова анімація OLED =====================
void drawStartAnimation() {
  const char* FW_VER = FW_VERSION;
  const char* AUTHOR = "ble_wifi_control";

  // Загальний час анімації: 2500+500+1000+1500+2000 = 7500мс
  const unsigned long TOTAL_MS = 7500;
  unsigned long animStart = millis();

  // Кільця сегментів: a→f→e→d→c→b
  const byte ring[6] = {0x01,0x20,0x10,0x08,0x04,0x02};

  // VFD анімація — викликається кожен кадр
  auto vfdAnim = [&](unsigned long now) {
    // Кільця на 4 цифрах — кожна зі зсувом фази
    int ringStep = (int)((now - animStart) / 80) % 6;
    for (int d = 0; d < 4; d++) {
      int step = (ringStep + d) % 6;
      for (int s = 0; s < 7; s++) setBit(1, digitSegments[d][s], false);
      byte seg = ring[step];
      for (int s = 0; s < 6; s++)
        if (seg & (1<<s)) setBit(1, digitSegments[d][s], true);
    }
    // Смуги — один цикл за весь час анімації (7500мс)
    // 0→16 за першу половину, 16→0 за другу
    unsigned long elapsed = now - animStart;
    if (elapsed > TOTAL_MS) elapsed = TOTAL_MS;
    int barVal;
    if (elapsed < TOTAL_MS/2)
      barVal = (int)(elapsed * 16 / (TOTAL_MS/2));
    else
      barVal = (int)((TOTAL_MS - elapsed) * 16 / (TOTAL_MS/2));
    barVal = constrain(barVal, 0, 16);

    // Обидві смуги однаково як в renderBar
    const int leftL[]={5,6,8,9,10,11,12,13,14,16,17,18,22,21,20,19};
    const int rC2[]={2,1,0};
    const int rC3[]={37,36,35,34,33,32,30,29,28,24,25,26,27};
    // Очищаємо
    for (int i=0;i<16;i++) setBit(3, leftL[i], false);
    for (int i=0;i<3;i++)  setBit(2, rC2[i],  false);
    for (int i=0;i<13;i++) setBit(3, rC3[i],  false);
    // Заповнюємо обидві однаково
    for (int i=0;i<barVal&&i<16;i++) {
      setBit(3, leftL[i], true);
      if (i<3) setBit(2, rC2[i], true);
      else     setBit(3, rC3[i-3], true);
    }
    vfdSend();
  };

  // === Фаза 1: Квадратики злітаються з кутів і будують лева (3с) ===
  // Морда лева — bitmap 12x10 блоків по 8px (64x80 але ми на 128x64)
  // Спрощена морда лева з блоків 8x8
  // 1=блок є, 0=пусто — рядки зверху вниз, стовпці зліва
  const uint8_t lion[8][16] = {
    {0,0,0,1,1,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,1,1,1,1,0,0,1,1,1,1,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,1,1,0,1,1,1,1,1,1,0,1,1,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,1,1,0,1,1,1,1,0,1,1,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0},
  };
  const int BW = 8; // ширина блоку
  const int BH = 7; // висота блоку
  const int COLS = 14, ROWS = 8;
  const int offX = (128 - COLS*BW) / 2; // центр по X
  const int offY = (64  - ROWS*BH) / 2; // центр по Y

  unsigned long t0 = millis();
  const int BUILD_MS = 2500;
  while (millis() - t0 < BUILD_MS) {
    float prog = (float)(millis() - t0) / BUILD_MS;
    display->clear();
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        if (!lion[r][c]) continue;
        int fx = offX + c * BW;
        int fy = offY + r * BH;
        int sx = (c < COLS/2) ? -BW : 128;
        int sy = (r < ROWS/2) ? -BH : 64;
        float delay = 0.3f * (abs(c - COLS/2) + abs(r - ROWS/2)) / (COLS/2 + ROWS/2);
        float lp = constrain((prog - delay) / (1.0f - delay), 0.0f, 1.0f);
        float le = lp < 0.5f ? 2*lp*lp : 1-2*(1-lp)*(1-lp);
        int x = sx + (int)((fx - sx) * le);
        int y = sy + (int)((fy - sy) * le);
        display->fillRect(x, y, BW-1, BH-1);
      }
    }
    display->display();
    vfdAnim(millis());
  }

  // === Фаза 2: Лев стоїть 0.5с ===
  display->clear();
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (lion[r][c]) display->fillRect(offX+c*BW, offY+r*BH, BW-1, BH-1);
  display->display();
  { unsigned long t2=millis(); while(millis()-t2<500) vfdAnim(millis()); }

  // === Фаза 3: Квадратики розлітаються (1с) ===
  t0 = millis();
  const int EXPLODE_MS = 1000;
  while (millis() - t0 < EXPLODE_MS) {
    float prog = (float)(millis() - t0) / EXPLODE_MS;
    display->clear();
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        if (!lion[r][c]) continue;
        int fx = offX + c * BW;
        int fy = offY + r * BH;
        int dx = fx - 64;
        int dy = fy - 32;
        float e = prog * prog;
        int x = fx + (int)(dx * e * 3);
        int y = fy + (int)(dy * e * 3);
        if (x > -BW && x < 128 && y > -BH && y < 64)
          display->fillRect(x, y, BW-1, BH-1);
      }
    }
    display->display();
    vfdAnim(millis());
  }

  // === Фаза 4: Напис з'являється (1.5с) ===
  t0 = millis();
  const int TEXT_MS = 1500;
  while (millis() - t0 < TEXT_MS) {
    float prog = (float)(millis() - t0) / TEXT_MS;
    display->clear();
    if (prog > 0.3f || ((int)(millis()/80) % 2 == 0)) {
      display->setFont(ArialMT_Plain_16);
      display->setTextAlignment(TEXT_ALIGN_CENTER);
      display->drawString(64, 10, AUTHOR);
      display->setFont(ArialMT_Plain_10);
      display->drawString(64, 34, "Tape Controller");
      display->drawString(64, 48, FW_VER);
    }
    display->display();
    vfdAnim(millis());
  }

  // === Фаза 5: Фінальний екран 2с ===
  display->clear();
  display->setFont(ArialMT_Plain_16);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64, 10, AUTHOR);
  display->setFont(ArialMT_Plain_10);
  display->drawString(64, 34, "Tape Controller");
  display->drawString(64, 48, FW_VER);
  display->display();
  { unsigned long t5=millis(); while(millis()-t5<2000) vfdAnim(millis()); }

  display->clear();
  display->display();

  // Очищаємо VFD буфери після анімації
  for (int c = 0; c < 3; c++) chipBuffers[c] = 0;
  vfdSend();
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("Chip: %s Rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("Flash: %d MB\n", ESP.getFlashChipSize()/1024/1024);
  Serial.printf("PSRAM found: %d, size: %d\n", psramFound(), ESP.getPsramSize());
  Serial.printf("Free heap: %d\n", ESP.getFreeHeap());
  Serial.printf("Free heap at start: %d\n", ESP.getFreeHeap());
  esp_task_wdt_init(60, true); // Watchdog 60 секунд — достатньо для OTA
  esp_task_wdt_add(NULL);
  i2cMutex = xSemaphoreCreateMutex();
  Wire.begin(PCF_I2C_SDA,PCF_I2C_SCL);
  Wire.beginTransmission(PCF8574_ADDR); Wire.write(0xFF); Wire.endTransmission();

  // I2S0 — генерує MCLK без APLL (APLL конфліктує з WiFi!)
  i2s_config_t cfg0;
  memset(&cfg0, 0, sizeof(cfg0));
  cfg0.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg0.sample_rate = 44100;
  cfg0.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg0.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg0.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg0.dma_buf_count = 2;
  cfg0.dma_buf_len = 8;
  cfg0.use_apll = false;
  cfg0.fixed_mclk = 0;
  i2s_driver_install(I2S_NUM_0, &cfg0, 0, NULL);
  i2s_pin_config_t pins0;
  memset(&pins0, 0, sizeof(pins0));
  pins0.mck_io_num = I2S_MCLK;
  pins0.bck_io_num = I2S_PIN_NO_CHANGE;
  pins0.ws_io_num  = I2S_PIN_NO_CHANGE;
  pins0.data_out_num = I2S_PIN_NO_CHANGE;
  pins0.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins0);

  // I2S1 — читає дані з PCM1808
  i2s_config_t cfg1;
  memset(&cfg1, 0, sizeof(cfg1));
  cfg1.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg1.sample_rate = 44100;
  cfg1.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg1.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg1.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg1.dma_buf_count = 4;
  cfg1.dma_buf_len = FFT_SAMPLES;
  cfg1.use_apll = false;
  i2s_driver_install(I2S_NUM_1, &cfg1, 0, NULL);
  i2s_pin_config_t pins1;
  memset(&pins1, 0, sizeof(pins1));
  pins1.mck_io_num = I2S_PIN_NO_CHANGE;
  pins1.bck_io_num = I2S_BCK;
  pins1.ws_io_num  = I2S_LRC;
  pins1.data_out_num = I2S_PIN_NO_CHANGE;
  pins1.data_in_num  = I2S_DIN;
  i2s_set_pin(I2S_NUM_1, &pins1);

  // Глушимо VFD одразу
  pinMode(PIN_BRIGHTNESS_PWM,OUTPUT); analogWrite(PIN_BRIGHTNESS_PWM,0);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_CK,OUTPUT);  digitalWrite(PIN_CK,LOW);
  pinMode(PIN_DATA,OUTPUT); digitalWrite(PIN_DATA,LOW);
  for(int i=0;i<3;i++){pinMode(latchPins[i],OUTPUT);digitalWrite(latchPins[i],HIGH);}

  FastLED.addLeds<LED_TYPE,LED_PIN,COLOR_ORDER>(leds,LED_COUNT);
  pinMode(PIN_RESET,INPUT_PULLUP);
  pinMode(ENC_CLK,INPUT_PULLUP); pinMode(ENC_DT,INPUT_PULLUP);
  pinMode(PIN_PULSE,INPUT_PULLUP);
  pinMode(OUT_REWIND,OUTPUT);  digitalWrite(OUT_REWIND,LOW);
  pinMode(OUT_STOP,OUTPUT);    digitalWrite(OUT_STOP,LOW);
  pinMode(OUT_PLAY,OUTPUT);    digitalWrite(OUT_PLAY,LOW);
  pinMode(OUT_FORWARD,OUTPUT); digitalWrite(OUT_FORWARD,LOW);
  pinMode(G_SOURCE,OUTPUT);  pinMode(G_DOLBY_B,OUTPUT);
  pinMode(G_DOLBY_C,OUTPUT); pinMode(G_FILTER,OUTPUT);
  // GPIO 38 = G_SOURCE, GPIO 39 = I2S_BCK (PCM1808)

  irrecv.enableIRIn();
  loadSettings(); loadIRSettings();

  // Чекаємо 1.5с — система стабілізується
  delay(1500);
  applyBrightness();
  vfdReady=true;
  // OLED ініціалізація після стабілізації системи
  display = (oledType==0) ? (OLEDDisplay*)&displaySSD : (OLEDDisplay*)&displaySH;
  if (!display->init()) {
    delay(1000);       // Повторна спроба через 1с
    display->init();
  }
  if (oledRotate) display->flipScreenVertically();
  applyOledBrightness();
  display->clear();
  display->display();

  // ===== Стартова анімація =====
  // Тимчасово вимикаємо watchdog — анімація ~8с > WDT таймаут 10с
  esp_task_wdt_delete(NULL);
  analogWrite(PIN_BRIGHTNESS_PWM, 128); // VFD увімкнено на час анімації
  drawStartAnimation();
  esp_task_wdt_add(NULL);
  applyBrightness(); // відновлюємо правильну яскравість VFD після анімації
  if(biipStart) buzzStartup();     // звук при старті

  attachInterrupt(digitalPinToInterrupt(PIN_PULSE),handlePulse,FALLING);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK),  readEncoder,CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),   readEncoder,CHANGE);
  analogReadResolution(12);
  autoStyleTimer = millis(); // Fix: ініціалізуємо щоб авто-стиль не спрацював одразу

  // Ініціалізація BLE в setup() на Core 1
  BLEDevice::init("TapeControl");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BLEConnectCallbacks());
  BLEService* bleService = bleServer->createService(BLE_SERVICE_UUID);
  bleCmdChar = bleService->createCharacteristic(
    BLE_CMD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  bleCmdChar->setCallbacks(new BLECmdCallbacks());
  bleStatusChar = bleService->createCharacteristic(
    BLE_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleStatusChar->addDescriptor(new BLE2902());
  bleService->start();
  BLEAdvertising* bleAdv = BLEDevice::getAdvertising();
  bleAdv->addServiceUUID(BLE_SERVICE_UUID);
  bleAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE started!");

  xTaskCreatePinnedToCore(wifiTask,"wifiTask",16384,NULL,1,NULL,0);
  xTaskCreatePinnedToCore(fftTask,"fftTask",4096,NULL,1,&fftTaskHandle,0);
}

// ===================== LOOP (Core 1) =====================
void loop() {
  esp_task_wdt_reset();
  if (!vfdReady) return;
  // BLE підключення — звук завжди
  if (bleJustConnected) {
    bleJustConnected = false;
    buzzToneDecay(NOTE_E7, 200);
  }

  // BLE команда з черги
  if (bleCmd[0] != 0) {
    String cmd = String((char*)bleCmd);
    cmd.trim();
    Serial.print("BLE cmd: ["); Serial.print(cmd); Serial.println("]");
    memset((void*)bleCmd, 0, sizeof(bleCmd));
    bleCmdPending = false;
    if(biipWeb) buzzToneDecay(NOTE_C7, 80);
    execCommand(cmd);
  }

  // Відновлення I2C поза mutex — безпечно без delay під семафором
  if (i2cNeedsRecovery) {
    i2cNeedsRecovery = false;
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Wire.end(); delay(10);
      Wire.begin(PCF_I2C_SDA, PCF_I2C_SCL);
      Wire.beginTransmission(PCF8574_ADDR);
      Wire.write(pcfShadow);
      Wire.endTransmission();
      xSemaphoreGive(i2cMutex);
    }
  }

  unsigned long curM=millis();

  // Соленоїд
  if (activeSolenoidPin!=-1) {
    if (curM-solenoidTimer>=(unsigned long)SOLENOID_PULSE) {
      digitalWrite(activeSolenoidPin,LOW); activeSolenoidPin=-1;
    }
  }

  // PCF8574
  uint8_t pcf=pcfRead();
  bool pcf_PLAY         =pcfBit(pcf,0);
  bool pcf_PAUSE        =pcfBit(pcf,1);
  bool pcf_FORWARD      =pcfBit(pcf,2);
  bool isRec            =pcfBit(pcf,3);
  // P4 = HX Pro (вхід — тільки читаємо, не пишемо)
  bool pcf_REWIND_SENSE =pcfBit(pcf,5);
  bool pcf_NORM_MET     =pcfBit(pcf,6);
  bool pcf_CR02         =pcfBit(pcf,7);
  hxPro                 =pcfBit(pcf,4); // HX Pro на PCF P4

  // WiFi індикатор Large Hub — тільки VFD bit28, PCF P4 більше не чіпаємо
  bool lhState=false;
  if      (wifiStatus==WIFI_CONNECTED)  lhState=true;
  else if (wifiStatus==WIFI_AP_MODE)    lhState=(curM%200<100);
  else if (wifiStatus==WIFI_CONNECTING) lhState=(curM%1000<500);

  // PCM1808: peak hold для VFD — тримаємо максимум 80мс щоб не пропускати піки
  static int peakHoldL = 0, peakHoldR = 0;
  static unsigned long vfdPeakTimer = 0;
  int instL = (int)(i2sLastL / 1600);
  int instR = (int)(i2sLastR / 1600);
  if (instL > peakHoldL || instR > peakHoldR) {
    peakHoldL = instL; peakHoldR = instR;
    vfdPeakTimer = curM;
  } else if (curM - vfdPeakTimer > 80) {
    peakHoldL = instL; peakHoldR = instR;
    vfdPeakTimer = curM;
  }
  int rawL = peakHoldL, rawR = peakHoldR;
  // Баланс: крок 0.5 дБ = множник ~0.944 за крок
  if (balanceVal < 0) rawL = (int)(rawL * pow(0.944f, abs(balanceVal))); // приглушуємо L
  if (balanceVal > 0) rawR = (int)(rawR * pow(0.944f, balanceVal));      // приглушуємо R
  int currentMax=max(rawL,rawR);
  // silenceDetected — без змін, для Remain
  if (currentMax<(silenceThreshold*10)){
    if(!silenceDetected){silenceStart=curM;silenceDetected=true;}
    if(curM-silenceStart>SILENCE_HYSTERESIS) blinkMute=(curM%1000<500); else blinkMute=false;
  } else {silenceDetected=false;silenceStart=curM;blinkMute=false;}

  // oledMute — через smoothRed з порогом з меню (OL XX)
  oledMute = (smoothRed < (float)oledThreshold);

  // Автовихід з меню через 40с бездіяльності
  if (currentState != WORK && lastMenuAction > 0 && (curM - lastMenuAction > 40000)) {
    saveSettings(); currentState=WORK; inGroupSelect=false; currentGroup=GRP_NONE;
    if(biipMenu) buzzMenuExit();
    lastMenuAction = 0;
  }
  bool btnPressedNow=(digitalRead(PIN_RESET)==LOW);

  // Пробудження зі standby при будь-якій дії енкодера
  if (standbyActive && (btnPressedNow || encCounter != 0)) {
    standbyActive = false;
    standbyTimer = curM;
    applyBrightness();
    display->displayOn();
  }
  if (curM-lastLedUpdate>=LED_UPDATE_INTERVAL){
    FastLED.setBrightness(ledBright);
    fill_solid(leds,LED_COUNT,CHSV(isRec?ledColor2:ledColor1,255,255));
    FastLED.show(); lastLedUpdate=curM;
  }

  cached_REWIND_SENSE=pcf_REWIND_SENSE;
  if (tapeCounter!=lastTapeValue){lastMoveTime=curM;lastTapeValue=tapeCounter;}
  if (curM-timeTickMillis>=1000){timeTickMillis=curM;totalSeconds++;}

  int diff=encCounter/4;
  if (diff!=0){
    encCounter -= diff * 4;
    if (currentState != WORK) lastMenuAction = curM; // скидаємо таймер меню
    if (currentState==WORK){
      if(btnPressedNow){
        bool newMpx = (diff<0);
        if(newMpx != mpxFilter) {
          mpxFilter = newMpx;
          if(biipWeb) { if(mpxFilter) buzzMenuEnter(); else buzzMenuExit(); }
        }
        actionPerformedWhilePressed=true;
      }
      else if(isRec){
        bool newSrc = (diff<0);
        if(newSrc != sourceMonitorActive) {
          sourceMonitorActive = newSrc;
          if(biipWeb) { if(sourceMonitorActive) buzzMenuEnter(); else buzzMenuExit(); }
        }
      }
      else{
        dolbyMode=(dolbyMode+diff+4)%4;
        if(biipOut) buzzClick(); // поворот поза меню — пі-біп
      }
    } else if (inGroupSelect) {
      int g = (int)currentGroup + diff;
      if (g < 1) g = 5;
      if (g > 5) g = 1;
      currentGroup = (MenuGroup)g;
      if(biipMenu) buzzGroup();
    } else if (currentState==MENU_WIFI_RESET){
      wifiResetCounter+=abs(diff);
      if (wifiResetCounter>=10&&!wifiResetBlinking){
        wifiResetBlinking=true;wifiResetBlinkTimer=curM;resetWifiSettings();
      }
    } else {
      if     (currentState==MENU_SCALE)      displayMode=(displayMode+diff+3)%3;
      else if(currentState==MENU_VIEWS)      viewMode=constrain(viewMode+diff,1,4);
      else if(currentState==MENU_PULSES)     pulsesSetting=constrain(pulsesSetting+diff,1,20);
      else if(currentState==MENU_ANIM)       animMode=constrain(animMode+diff,1,6);
      else if(currentState==MENU_PEAK_HOLD)  peakHoldTime=constrain(peakHoldTime+diff*200,0,4000);
      else if(currentState==MENU_RELEASE)   {releaseLevel=constrain(releaseLevel+diff,1,99);currentDecay=releaseLevel*0.005f;}
      else if(currentState==MENU_RED_RELEASE){redReleaseLevel=constrain(redReleaseLevel+diff,1,99);redDecay=redReleaseLevel*0.005f;}
      else if(currentState==MENU_BALANCE)    balanceVal=constrain(balanceVal+diff,-5,5);
      else if(currentState==MENU_GAIN)       gainLevel=constrain(gainLevel+diff,1,200);
      else if(currentState==MENU_BRIGHTNESS){brightnessLevel=constrain(brightnessLevel+diff,1,15);applyBrightness();}
      else if(currentState==MENU_LED_C1)    ledColor1+=diff*5;
      else if(currentState==MENU_LED_C2)    ledColor2+=diff*5;
      else if(currentState==MENU_LED_BRIGHT) ledBright=constrain(ledBright+diff*10,0,255);
      else if(currentState==MENU_SILENCE)    silenceThreshold=constrain(silenceThreshold+diff*5,5,400);
      else if(currentState==MENU_ARU)        aruLevel=constrain(aruLevel+diff,0,3);
      else if(currentState==MENU_CALIB && diff!=0) {
        if (!autoCalibActive) {
          autoCalibActive=true;
          aruMax=0.0f;
        } else {
          // Скасування калібрування
          autoCalibActive=false;
          aruMax=0.0f;
        }
      }
      else if(currentState==MENU_OLED_THR)   oledThreshold=constrain(oledThreshold+diff,0,16);
      else if(currentState==MENU_SPEC_GAIN)  specGain=constrain(specGain+diff*50,50,2000);
      else if(currentState==MENU_OLED_BRIGHT){
        oledBright=constrain(oledBright+diff,0,99);
        int contrast = map(oledBright, 0, 99, 0, 255);
        display->setContrast(contrast, contrast>10?241:5, contrast>10?64:0);
      }
      else if(currentState==MENU_SPEC_STYLE) specStyle=constrain(((specStyle+diff)%14+14)%14,0,13);
      else if(currentState==MENU_AUTO_TIME)  autoStyleTime=constrain(autoStyleTime+diff,2,10);
      else if(currentState==MENU_SPEC_PEAKS) specPeaks=constrain(((specPeaks+diff)%4+4)%4,0,3);
      else if(currentState==MENU_STANDBY)    standbyTime=constrain(standbyTime+diff,0,20);
      else if(currentState==MENU_OLED_TYPE){
        oledType=constrain(oledType+diff,0,1);
        display = (oledType==0) ? (OLEDDisplay*)&displaySSD : (OLEDDisplay*)&displaySH;
        display->init();
        if (oledRotate) display->flipScreenVertically();
        applyOledBrightness();
      }
      else if(currentState==MENU_OLED_ROT){
        oledRotate=constrain(oledRotate+diff,0,1);
        if (oledRotate) display->flipScreenVertically();
        else { display->init(); if (oledRotate==0) applyOledBrightness(); }
      }
      else if(currentState==MENU_BIIP_START) biipStart=constrain(biipStart+diff,0,1);
      else if(currentState==MENU_BIIP_OUT)   biipOut=constrain(biipOut+diff,0,1);
      else if(currentState==MENU_BIIP_MENU)  biipMenu=constrain(biipMenu+diff,0,1);
      else if(currentState==MENU_BIIP_WEB)   biipWeb=constrain(biipWeb+diff,0,1);
      if(biipMenu) buzzEncoderTurn();
    }
    // encCounter залишок вже збережено через -= diff*4 вище
  }

  // Допоміжні функції навігації
  auto firstInGroup = [](MenuGroup g) -> MenuState {
    if (g==GRP_VFD)  return MENU_SCALE;
    if (g==GRP_OLED) return MENU_SPEC_STYLE;
    if (g==GRP_LED)  return MENU_LED_C1;
    if (g==GRP_BIIP) return MENU_BIIP_START;
    return MENU_WIFI_DB;
  };
  auto lastInGroup = [](MenuGroup g) -> MenuState {
    if (g==GRP_VFD)  return MENU_CALIB;
    if (g==GRP_OLED) return MENU_STANDBY;
    if (g==GRP_LED)  return MENU_LED_BRIGHT;
    if (g==GRP_BIIP) return MENU_BIIP_WEB;
    return MENU_WIFI_RESET;
  };
  auto groupOf = [](MenuState s) -> MenuGroup {
    if (s>=MENU_SCALE      && s<=MENU_CALIB)      return GRP_VFD;
    if (s>=MENU_SPEC_STYLE && s<=MENU_STANDBY)  return GRP_OLED;
    if (s>=MENU_LED_C1     && s<=MENU_LED_BRIGHT) return GRP_LED;
    if (s>=MENU_BIIP_START && s<=MENU_BIIP_WEB)  return GRP_BIIP;
    return GRP_WIFI;
  };
  auto nextGroup = [](MenuGroup g) -> MenuGroup {
    if (g==GRP_VFD)  return GRP_OLED;
    if (g==GRP_OLED) return GRP_LED;
    if (g==GRP_LED)  return GRP_BIIP;
    if (g==GRP_BIIP) return GRP_WIFI;
    return GRP_VFD;
  };

  if (btnPressedNow){
    if(!btnPressed){
      btnPressed=true; btnTimer=curM; actionPerformedWhilePressed=false;
      if(currentState!=WORK) lastMenuAction=curM; // скидаємо таймер меню при кліку
    }
    if(!actionPerformedWhilePressed&&(curM-btnTimer>2000)){
      if(currentState==WORK){
        // Вхід в меню — показуємо першу групу
        inGroupSelect=true; currentGroup=GRP_VFD; currentState=MENU_SCALE;
        lastMenuAction=curM;
        if(biipMenu) buzzMenuEnter();
      } else {
        // Довге натискання в меню — зберегти і вийти
        saveSettings(); currentState=WORK; inGroupSelect=false; currentGroup=GRP_NONE;
        if(biipMenu) buzzMenuExit();
      }
      actionPerformedWhilePressed=true;
    }
  } else if (btnPressed){
    if(!actionPerformedWhilePressed){clickCount++;lastClickTime=curM;}
    btnPressed=false;
  }

  if (clickCount>0&&(curM-lastClickTime>DOUBLE_CLICK_GAP)){
    if (currentState==WORK){
      if(clickCount==1){
        if(!tapeReturnActive&&!remainModeActive){tapeCounter=0;lastMoveTime=curM;}
        if(biipOut) buzzEncoderTurn(); // клік поза меню — піп
      }
      else if(clickCount==2){
        if(!remainModeActive){
          if(!tapeReturnActive){
            tapeReturnActive=true;tapeCounter=0;zeroPulseRunning=false;zeroCrossBlocked=true;trBlinkCount=0;lastTapePos=tapeCounter;
            if(biipOut) buzzMenuEnter(); // tape return вмик
          } else {
            tapeReturnActive=false;
            if(biipOut) buzzMenuExit(); // tape return вимик
          }
        }
      }
      else if(clickCount==3){
        remainModeActive=!remainModeActive;
        if(remainModeActive){
          tapeReturnActive=false;tapeCounter=0;remainStep=WAIT_FOR_SOUND;soundStartTimer=0;glitchTimer=0;
          if(biipOut) buzzMenuEnter(); // remain вмик
        } else {
          digitalWrite(OUT_REWIND,LOW);digitalWrite(OUT_STOP,LOW);digitalWrite(OUT_PLAY,LOW);digitalWrite(OUT_FORWARD,LOW);activeSolenoidPin=-1;
          if(biipOut) buzzMenuExit(); // remain вимик
        }
      }
    } else if (inGroupSelect) {
      // Короткий клік — входимо в групу
      if (clickCount==1) {
        inGroupSelect=false;
        currentState=firstInGroup(currentGroup);
        if(biipMenu) buzzClick();
      } else if (clickCount==2) {
        // Подвійний клік — наступна група
        currentGroup=nextGroup(currentGroup);
        if(biipMenu) buzzGroup();
      }
    } else {
      // В підгрупі
      if (clickCount==1) {
        // Короткий клік — наступний пункт в групі
        if(currentState==MENU_WIFI_RESET){wifiResetCounter=0;wifiResetBlinking=false;}
        MenuState last = lastInGroup(groupOf(currentState));
        if(currentState==last) {
          currentState=firstInGroup(groupOf(currentState));
        } else {
          currentState=(MenuState)((int)currentState+1);
        }
        if(biipMenu) buzzClick();
      } else if (clickCount==2) {
        // Подвійний клік — виходимо з підгрупи, наступна група
        if(currentState==MENU_WIFI_RESET){wifiResetCounter=0;wifiResetBlinking=false;}
        currentGroup=nextGroup(groupOf(currentState));
        inGroupSelect=true;
        currentState=firstInGroup(currentGroup);
      } else if (clickCount==3 && currentState==MENU_CALIB) {
        // Потрійний клік в CA — скидання до дефолту з морганням
        autoCalibActive=false;
        aruMax=0.0f;
        gainLevel=25;
        prefs.putInt("gain",gainLevel);
        calibReset=true;
        calibResetTimer=curM;
      }
    }
    clickCount=0;
  }
  if(!inGroupSelect && currentState!=MENU_WIFI_RESET && wifiResetCounter>0) wifiResetCounter=0;

  for(int i=0;i<3;i++) chipBuffers[i]=0;

  // WiFi індикатор після скидання буферів
  setBit(1,28,lhState);

  // Tape Return
  if (tapeReturnActive){
    if (zeroPulseRunning){
      if(activeSolenoidPin!=OUT_STOP) tapeStop();
      if(curM-zeroPulseTimer>1000){
        if(curM-trBlinkTimer>500){trBlinkTimer=curM;trIndicatorState=!trIndicatorState;trBlinkCount++;
          if(trBlinkCount>=10){tapeReturnActive=false;zeroPulseRunning=false;}}
      }
      setBit(2,30,trIndicatorState);
    } else {
      bool trCrossed=(tapeCounter==0)||((lastTapePos>0&&tapeCounter<=0)||(lastTapePos<0&&tapeCounter>=0));
      if(trCrossed&&!zeroCrossBlocked){zeroPulseRunning=true;zeroPulseTimer=curM;trBlinkTimer=curM;trBlinkCount=0;}
      else{if(abs(tapeCounter)>5) zeroCrossBlocked=false;setBit(2,30,zeroCrossBlocked?(curM%600<300):true);}
    }
    lastTapePos=tapeCounter;
  }

  // Remain Mode
  if (remainModeActive){
    switch(remainStep){
      case WAIT_FOR_SOUND:
        setBit(1,29,(curM%200<100));
        if(!silenceDetected){if(soundStartTimer==0) soundStartTimer=curM;glitchTimer=0;if(curM-soundStartTimer>=3000){remainStep=WAIT_FOR_SILENCE;soundStartTimer=0;glitchTimer=curM;}}
        else{if(glitchTimer==0) glitchTimer=curM;if(curM-glitchTimer>400) soundStartTimer=0;}
        break;
      case WAIT_FOR_SILENCE:
        setBit(1,29,true);
        if(curM-glitchTimer<500) soundStartTimer=0;
        else{if(silenceDetected){if(soundStartTimer==0) soundStartTimer=curM;if(curM-soundStartTimer>=2500){tapeREW();remainSequenceTimer=curM;remainStep=REWINDING;soundStartTimer=0;lastTapePos=tapeCounter;}}else soundStartTimer=0;}
        break;
      case REWINDING:{setBit(1,29,(curM%1000<500));
        bool rc=(tapeCounter==0)||((lastTapePos>0&&tapeCounter<=0)||(lastTapePos<0&&tapeCounter>=0));
        if(rc){tapeStop();remainSequenceTimer=curM;remainStep=STOPPING;}lastTapePos=tapeCounter;break;}
      case STOPPING:
        setBit(1,29,(curM%1000<500));
        if(curM-remainSequenceTimer>REWIND_STOP_DELAY){tapePlay();remainSequenceTimer=curM;remainStep=START_PLAY;}
        break;
      case START_PLAY:
        setBit(1,29,(curM%1000<500));
        if(curM-remainSequenceTimer>1000){remainStep=WAIT_FOR_SOUND;soundStartTimer=0;glitchTimer=0;}
        break;
    }
  }

  setBit(3,2,blinkMute);

  // Аудіо
  float sigL=rawL*(gainLevel/10.0f),sigR=rawR*(gainLevel/10.0f);
  int tL=0,tR=0;
  if(displayMode==0){
    tL=constrain(map(constrain(sigL,0,4000),0,4000,0,16),0,16);
    tR=constrain(map(constrain(sigR,0,4000),0,4000,0,16),0,16);
  } else if(displayMode==1){
    // Шкала 1: -40,-35,-25,-20,-15,-10,-6,-4,-2,0,+2,+4,+6,+8,+10,+12 dB
    const float scale1[]={-40,-35,-25,-20,-15,-10,-6,-4,-2,0,2,4,6,8,10,12};
    float dbL=20.0f*log10f(max(sigL,1.0f)/1000.0f);
    float dbR=20.0f*log10f(max(sigR,1.0f)/1000.0f);
    tL=0; tR=0;
    for(int i=0;i<16;i++){if(dbL>=scale1[i]) tL=i+1;}
    for(int i=0;i<16;i++){if(dbR>=scale1[i]) tR=i+1;}
    // Відрізаємо шум — мінімум 5 сегментів щоб щось показати
    if(sigL < 150) tL=0;
    if(sigR < 150) tR=0;
    tL=constrain(tL,0,16); tR=constrain(tR,0,16);
  } else {
    // Шкала 2: -6,-4,-3,-2,-1,0,+1,+2,+3,+4,+6,+8,+10,+12,+14,+16 dB
    const float scale2[]={-6,-4,-3,-2,-1,0,1,2,3,4,6,8,10,12,14,16};
    float dbL=20.0f*log10f(max(sigL,1.0f)/1000.0f);
    float dbR=20.0f*log10f(max(sigR,1.0f)/1000.0f);
    tL=0; tR=0;
    for(int i=0;i<16;i++){if(dbL>=scale2[i]) tL=i+1;}
    for(int i=0;i<16;i++){if(dbR>=scale2[i]) tR=i+1;}
    tL=constrain(tL,0,16); tR=constrain(tR,0,16);
  }
  smoothL+=(tL-smoothL)*(tL>smoothL?0.6f:currentDecay);
  smoothR+=(tR-smoothR)*(tR>smoothR?0.6f:currentDecay);
  float mx=max(smoothL,smoothR);
  smoothRed+=(mx-smoothRed)*(mx>smoothRed?0.6f:redDecay);

  // АРУ — автоматичне регулювання підсилення
  // gainLevel зберігається як int, але для плавності тримаємо float акумулятор
  static float gainAccum = -1.0f; // -1 = не ініціалізовано
  if (gainAccum < 0) gainAccum = (float)gainLevel;

  if (aruLevel > 0 && !autoCalibActive) {
    // Швидкість: 1=повільно(~3с на крок), 2=середньо(~0.6с), 3=швидко(~0.15с)
    float aruSpeed = (aruLevel==1) ? 0.0003f : (aruLevel==2) ? 0.0015f : 0.006f;
    if      (smoothRed > 14.0f) gainAccum -= aruSpeed * gainAccum; // зменшуємо пропорційно
    else if (smoothRed <  6.0f) gainAccum += aruSpeed * gainAccum; // збільшуємо пропорційно
    gainAccum = constrain(gainAccum, 1.0f, 200.0f);
    gainLevel = (int)(gainAccum + 0.5f); // округлення, не обрізання
  } else {
    gainAccum = (float)gainLevel; // синхронізуємо при ручній зміні
  }

  // Автокалібрування — запам'ятовуємо максимальний rawL/rawR
  static unsigned long calibStart = 0;
  if (autoCalibActive) {
    int curRaw = max(rawL, rawR);
    if (curRaw > aruMax) aruMax = curRaw;
    if (calibStart == 0) calibStart = curM;
    if (curM - calibStart > 3000) {
      // Розраховуємо gainLevel щоб smoothRed досягав 14 при максимальному сигналі
      // sigL = rawL * (gainLevel/10.0f), tL = map(sigL, 0, 4000, 0, 16)
      // Хочемо: aruMax * (newGain/10.0) * (16/4000) = 14
      // newGain = 14 * 4000 / (aruMax * 16 / 10) = 14 * 4000 * 10 / (aruMax * 16)
      if (aruMax > 0) {
        int newGain = (int)(14.0f * 4000.0f * 10.0f / (aruMax * 16.0f));
        gainLevel = constrain(newGain, 1, 200);
        gainAccum = (float)gainLevel; // синхронізуємо акумулятор
      }
      markSettingsDirty(); // збережемо через debounce — без прямого prefs.putInt
      autoCalibActive = false;
      calibStart = 0;
      aruMax = 0;
    }
  } else {
    calibStart = 0;
  }
  renderBar((int)smoothL,peakPosL,peakVelL,peakT_L,0);
  renderBar((int)smoothR,peakPosR,peakVelR,peakT_R,1);

  int cL=(int)smoothRed;
  if(cL>=6) setBit(2,21,true);if(cL>=10) setBit(2,20,true);if(cL>=12) setBit(2,19,true);
  if(cL>=13) setBit(2,16,true);if(cL>=14) setBit(2,14,true);if(cL>=15) setBit(2,13,true);if(cL>=16) setBit(2,12,true);

  if     (viewMode==1){setBit(1,24,true); setBit(2,17,true); setBit(2,22,true);}
  else if(viewMode==2){setBit(1,24,true); setBit(2,17,false);setBit(2,22,false);}
  else if(viewMode==3){setBit(1,24,false);setBit(2,17,false);setBit(2,22,false);}
  else if(viewMode==4){setBit(1,24,false);setBit(2,17,true); setBit(2,22,true);}

  setBit(1,25,(displayMode<2));setBit(2,18,(displayMode==0||displayMode==2));

  if     (dolbyMode==0){setBit(2,11,false);setBit(2,9,false);setBit(2,8,false);digitalWrite(G_DOLBY_B,LOW); digitalWrite(G_DOLBY_C,LOW);}
  else if(dolbyMode==1){setBit(2,11,true); setBit(2,9,true); setBit(2,8,false);digitalWrite(G_DOLBY_B,HIGH);digitalWrite(G_DOLBY_C,LOW);}
  else if(dolbyMode==2){setBit(2,11,true); setBit(2,9,false);setBit(2,8,true); digitalWrite(G_DOLBY_B,LOW); digitalWrite(G_DOLBY_C,HIGH);}
  else if(dolbyMode==3){setBit(2,11,true); setBit(2,9,true); setBit(2,8,true); digitalWrite(G_DOLBY_B,HIGH);digitalWrite(G_DOLBY_C,HIGH);}

  setBit(2,34,pcf_PLAY);setBit(2,36,pcf_PAUSE);
  setBit(2,35,pcf_FORWARD);setBit(2,33,pcf_REWIND_SENSE);

  // Оновлюємо стан для WebSocket іконок — реальний стан з PCF індикаторів
  // При зміні — одразу публікуємо в MQTT (не чекаємо 5с)
  bool prev_PLAY    = ws_pcf_PLAY;
  bool prev_PAUSE   = ws_pcf_PAUSE;
  bool prev_FORWARD = ws_pcf_FORWARD;
  bool prev_REWIND  = ws_pcf_REWIND;
  ws_pcf_PLAY    = pcf_PLAY;
  ws_pcf_PAUSE   = pcf_PAUSE;
  ws_pcf_FORWARD = pcf_FORWARD;
  ws_pcf_REWIND  = pcf_REWIND_SENSE;
  ws_pcf_REC     = isRec;

  if (mqttEnabled && mqttClient.connected()) {
    bool changed = (ws_pcf_PLAY    != prev_PLAY   ||
                    ws_pcf_PAUSE   != prev_PAUSE  ||
                    ws_pcf_FORWARD != prev_FORWARD ||
                    ws_pcf_REWIND  != prev_REWIND);
    if (changed) {
      mqttClient.publish((String(MQTT_PREFIX)+"vfd_rew/state").c_str(), ws_pcf_REWIND  ? "ON" : "OFF");
      mqttClient.publish((String(MQTT_PREFIX)+"vfd_stp/state").c_str(),
        (!ws_pcf_PLAY && !ws_pcf_FORWARD && !ws_pcf_REWIND) ? "ON" : "OFF");
      mqttClient.publish((String(MQTT_PREFIX)+"vfd_ply/state").c_str(), ws_pcf_PLAY    ? "ON" : "OFF");
      mqttClient.publish((String(MQTT_PREFIX)+"vfd_pau/state").c_str(), ws_pcf_PAUSE   ? "ON" : "OFF");
      mqttClient.publish((String(MQTT_PREFIX)+"vfd_fwd/state").c_str(), ws_pcf_FORWARD ? "ON" : "OFF");
    }
  }

  setBit(2,6,mpxFilter);digitalWrite(G_FILTER,mpxFilter);
  if(isRec&&sourceMonitorActive){
    if(curM-lastBlinkMillis>=500){lastBlinkMillis=curM;arrowState=!arrowState;}
    setBit(2,10,arrowState);digitalWrite(G_SOURCE,HIGH);
  } else digitalWrite(G_SOURCE,LOW);
  setBit(3,4,(isRec&&sourceMonitorActive));setBit(3,3,!(isRec&&sourceMonitorActive));
  setBit(1,26,hxPro); // HX Pro — PCF P4
  if(!remainModeActive) setBit(1,29,false);

  bool mn=pcf_NORM_MET,cr=pcf_CR02;
  if     (!mn&&!cr){setBit(2,5,true); setBit(2,4,false);setBit(2,3,false);}
  else if( mn&&!cr){setBit(2,5,false);setBit(2,4,false);setBit(2,3,true);}
  else if(!mn&& cr){setBit(2,5,false);setBit(2,4,true); setBit(2,3,false);}
  else if( mn&& cr){setBit(2,5,false);setBit(2,4,true); setBit(2,3,true);}

  setBit(3,1,isRec);

  // Дисплей
  if (currentState==WORK){
    bool tm=(curM-lastMoveTime>TIMER_TIMEOUT);setBit(1,27,tm);
    int val=tm?totalSeconds:abs(tapeCounter),d[4];
    if(tm){
      if(val >= 3600) {
        // Більше години — показуємо HH:MM з моргаючою крапкою
        int h=val/3600, m=(val%3600)/60;
        d[0]=h/10; d[1]=h%10; d[2]=m/10; d[3]=m%10;
        setBit(2,29,(curM%1000)<500); // моргає 1 раз на секунду
      } else {
        // До години — показуємо MM:SS
        int m=(val/60)%60, s=val%60;
        d[0]=m/10; d[1]=m%10; d[2]=s/10; d[3]=s%10;
        setBit(2,29,true); // крапка постійна
      }
    }
    else{
      d[0]=(val/1000)%10;d[1]=(val/100)%10;d[2]=(val/10)%10;d[3]=val%10;
      // При від'ємному лічильнику — старший розряд моргає між цифрою і мінусом
      if(tapeCounter < 0) {
        if((curM%1000)<500) d[0] = 11; // 11 = символ мінус
      }
    }
    const byte seg_minus = 0x40; // тільки сегмент G = мінус
    for(int i=0;i<4;i++) {
      byte seg = (d[i]==11) ? seg_minus : numbers[d[i]];
      for(int s=0;s<7;s++) if(seg&(1<<s)) setBit(1,digitSegments[i][s],true);
    }

  } else if (inGroupSelect) {
    // Відображення назви групи
    // VFd = V F d _
    // OLEd = O L E d
    // LEd_ = L E d _
    // uuFi = WiFi (u u F i)
    byte g0=0,g1=0,g2=0,g3=0;
    if      (currentGroup==GRP_VFD) { g0=letter_V; g1=letter_F; g2=letter_d; }
    else if (currentGroup==GRP_OLED){ g0=letter_O; g1=letter_L; g2=letter_E; g3=letter_d; }
    else if (currentGroup==GRP_LED) { g0=letter_L; g1=letter_E; g2=letter_d; }
    else if (currentGroup==GRP_BIIP){ g0=letter_b; g1=letter_i; g2=letter_i; g3=letter_P; }
    else if (currentGroup==GRP_WIFI){ g0=0x3C; g1=0x30; g2=letter_F; g3=letter_i; } // uuFi = WiFi
    for(int s=0;s<7;s++){
      if(g0&(1<<s)) setBit(1,digitSegments[0][s],true);
      if(g1&(1<<s)) setBit(1,digitSegments[1][s],true);
      if(g2&(1<<s)) setBit(1,digitSegments[2][s],true);
      if(g3&(1<<s)) setBit(1,digitSegments[3][s],true);
    }

  } else if (currentState==MENU_WIFI_DB){
    int db=(wifiStatus==WIFI_CONNECTED)?abs(WiFi.RSSI()):0;
    int v=constrain(db,0,99);
    for(int s=0;s<7;s++){
      if(letter_d&(1<<s))      setBit(1,digitSegments[0][s],true);
      if(letter_b&(1<<s))      setBit(1,digitSegments[1][s],true);
      if(numbers[v/10]&(1<<s)) setBit(1,digitSegments[2][s],true);
      if(numbers[v%10]&(1<<s)) setBit(1,digitSegments[3][s],true);
    }
  } else if (currentState==MENU_WIFI_IP){
    IPAddress ip=(wifiStatus==WIFI_CONNECTED)?WiFi.localIP():WiFi.softAPIP();
    int l=ip[3];
    for(int s=0;s<7;s++){
      if(letter_A&(1<<s))           setBit(1,digitSegments[0][s],true);
      if(numbers[l/100]&(1<<s))     setBit(1,digitSegments[1][s],true);
      if(numbers[(l/10)%10]&(1<<s)) setBit(1,digitSegments[2][s],true);
      if(numbers[l%10]&(1<<s))      setBit(1,digitSegments[3][s],true);
    }
  } else if (currentState==MENU_WIFI_RESET){
    bool show=!wifiResetBlinking||(curM%400<200);
    if(wifiResetBlinking&&curM-wifiResetBlinkTimer>3000){
      wifiResetBlinking=false;wifiResetCounter=0;
      analogWrite(PIN_BRIGHTNESS_PWM,0);delay(300);ESP.restart();
    }
    if(show){
      for(int s=0;s<7;s++){
        if(letter_r&(1<<s)) setBit(1,digitSegments[0][s],true);
        if(letter_S&(1<<s)) setBit(1,digitSegments[1][s],true);
        if(letter_i&(1<<s)) setBit(1,digitSegments[2][s],true);
        if(letter_P&(1<<s)) setBit(1,digitSegments[3][s],true);
      }
    }
  } else {
    byte l1=0,l2=0;int valV=0;
    if     (currentState==MENU_SCALE)      {l1=letter_S;l2=letter_C;valV=displayMode;}
    else if(currentState==MENU_VIEWS)      {l1=letter_V;l2=letter_i;valV=viewMode;}
    else if(currentState==MENU_PULSES)     {l1=letter_P;l2=letter_u;valV=pulsesSetting;}
    else if(currentState==MENU_ANIM) {
      if (animMode==6) {
        // An 0d — демо режим, малюємо і виходимо
        for(int s=0;s<7;s++){
          if(letter_A&(1<<s))   setBit(1,digitSegments[0][s],true);
          if(letter_n&(1<<s))   setBit(1,digitSegments[1][s],true);
          if(numbers[0]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(letter_d&(1<<s))   setBit(1,digitSegments[3][s],true);
        }
        vfdSend();
      } else {
        l1=letter_A; l2=letter_n; valV=animMode;
        for(int s=0;s<7;s++){
          if(l1&(1<<s))               setBit(1,digitSegments[0][s],true);
          if(l2&(1<<s))               setBit(1,digitSegments[1][s],true);
          if(numbers[valV/10]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(numbers[valV%10]&(1<<s)) setBit(1,digitSegments[3][s],true);
        }
        vfdSend();
      }
    }
    else if(currentState==MENU_PEAK_HOLD)  {l1=letter_P;l2=letter_H;valV=peakHoldTime/100;}
    else if(currentState==MENU_RELEASE)    {l1=letter_r;l2=letter_S;valV=releaseLevel;}
    else if(currentState==MENU_RED_RELEASE){l1=letter_r;l2=letter_E;valV=redReleaseLevel;}
    else if(currentState==MENU_BALANCE) {
      // bL L1..L5 або R1..R5 або 00
      for(int s=0;s<7;s++){
        if(letter_b&(1<<s)) setBit(1,digitSegments[0][s],true);
        if(letter_L&(1<<s)) setBit(1,digitSegments[1][s],true);
        if(balanceVal == 0) {
          if(numbers[0]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(numbers[0]&(1<<s)) setBit(1,digitSegments[3][s],true);
        } else if(balanceVal < 0) {
          // L + цифра (приглушення лівого)
          if(letter_L&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(numbers[abs(balanceVal)]&(1<<s)) setBit(1,digitSegments[3][s],true);
        } else {
          // R + цифра (приглушення правого)
          if(letter_r&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(numbers[balanceVal]&(1<<s)) setBit(1,digitSegments[3][s],true);
        }
      }
    }
    else if(currentState==MENU_GAIN)       {l1=letter_G;l2=letter_n;valV=gainLevel;}
    else if(currentState==MENU_BRIGHTNESS) {l1=letter_b;l2=letter_r;valV=brightnessLevel;}
    else if(currentState==MENU_LED_C1)     {l1=letter_C;l2=numbers[1];valV=(int)(ledColor1/2.55);}
    else if(currentState==MENU_LED_C2)     {l1=letter_C;l2=numbers[2];valV=(int)(ledColor2/2.55);}
    else if(currentState==MENU_LED_BRIGHT) {l1=letter_L;l2=letter_b;valV=map(ledBright,0,255,0,99);}
    else if(currentState==MENU_BIIP_START) {l1=letter_b;l2=letter_S;valV=biipStart;}
    else if(currentState==MENU_BIIP_OUT)   {l1=letter_b;l2=letter_P;valV=biipOut;}
    else if(currentState==MENU_BIIP_MENU)  {l1=letter_b;l2=letter_n;valV=biipMenu;}
    else if(currentState==MENU_BIIP_WEB)   {l1=letter_b;l2=letter_u;valV=biipWeb;}
    else if(currentState==MENU_SILENCE)    {l1=letter_S;l2=letter_L;valV=silenceThreshold/10;}
    else if(currentState==MENU_ARU)        {l1=letter_A;l2=letter_r;valV=aruLevel;}
    else if(currentState==MENU_CALIB) {
      if (calibReset) {
        // Скидання до дефолту — CAde моргає
        if (curM - calibResetTimer > 2000) calibReset=false;
        if (curM%400 < 200) {
          for(int s=0;s<7;s++){
            if(letter_C&(1<<s)) setBit(1,digitSegments[0][s],true);
            if(letter_A&(1<<s)) setBit(1,digitSegments[1][s],true);
            if(letter_d&(1<<s)) setBit(1,digitSegments[2][s],true);
            if(letter_E&(1<<s)) setBit(1,digitSegments[3][s],true);
          }
        }
      } else {
        bool show = !autoCalibActive || (curM%500<250);
        if (show) {
          for(int s=0;s<7;s++){
            if(letter_C&(1<<s)) setBit(1,digitSegments[0][s],true);
            if(letter_A&(1<<s)) setBit(1,digitSegments[1][s],true);
            if(autoCalibActive) {
              // Калібрування активне — CA-3 моргає
              if(0x40&(1<<s))       setBit(1,digitSegments[2][s],true); // мінус
              if(numbers[3]&(1<<s)) setBit(1,digitSegments[3][s],true); // 3
            } else if(gainLevel==25) {
              // Дефолтне підсилення — CA-0
              if(0x40&(1<<s))       setBit(1,digitSegments[2][s],true); // мінус
              if(numbers[0]&(1<<s)) setBit(1,digitSegments[3][s],true); // 0
            } else {
              // Очікування — показуємо CA--
              if(0x40&(1<<s)) setBit(1,digitSegments[2][s],true); // мінус (сегмент g)
              if(0x40&(1<<s)) setBit(1,digitSegments[3][s],true); // мінус (сегмент g)
            }
          }
        }
      }
    }
    else if(currentState==MENU_OLED_THR)   {l1=letter_O;l2=letter_L;valV=oledThreshold;}
    else if(currentState==MENU_SPEC_GAIN)  {l1=letter_S;l2=letter_G;valV=specGain/20;}
    else if(currentState==MENU_OLED_BRIGHT){l1=letter_O;l2=letter_b;valV=oledBright;}
    else if(currentState==MENU_OLED_TYPE){
      // Ssd1 або SH11
      if (oledType==0) {
        for(int s=0;s<7;s++){
          if(letter_S&(1<<s))  setBit(1,digitSegments[0][s],true);
          if(letter_S&(1<<s))  setBit(1,digitSegments[1][s],true);
          if(letter_d&(1<<s))  setBit(1,digitSegments[2][s],true);
          if(numbers[1]&(1<<s))setBit(1,digitSegments[3][s],true);
        }
      } else {
        for(int s=0;s<7;s++){
          if(letter_S&(1<<s))  setBit(1,digitSegments[0][s],true);
          if(letter_H&(1<<s))  setBit(1,digitSegments[1][s],true);
          if(numbers[1]&(1<<s))setBit(1,digitSegments[2][s],true);
          if(numbers[1]&(1<<s))setBit(1,digitSegments[3][s],true);
        }
      }
    }
    else if(currentState==MENU_SPEC_STYLE) {
      // Завжди: Sd + 0А(авто) або 01-12(стилі 0-11)
      for(int s=0;s<7;s++){
        if(letter_S&(1<<s)) setBit(1,digitSegments[0][s],true);
        if(letter_d&(1<<s)) setBit(1,digitSegments[1][s],true);
      }
      if (specStyle == 12) {
        for(int s=0;s<7;s++){
          if(numbers[0]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(letter_A&(1<<s))   setBit(1,digitSegments[3][s],true);
        }
      } else if (specStyle == 13) {
        for(int s=0;s<7;s++){
          if(numbers[2]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(letter_A&(1<<s))   setBit(1,digitSegments[3][s],true);
        }
      } else {
        int dv = specStyle + 1; // 01..12
        for(int s=0;s<7;s++){
          if(numbers[dv/10]&(1<<s)) setBit(1,digitSegments[2][s],true);
          if(numbers[dv%10]&(1<<s)) setBit(1,digitSegments[3][s],true);
        }
      }
    }
    else if(currentState==MENU_AUTO_TIME)  {l1=letter_A;l2=letter_t;valV=autoStyleTime;}
    else if(currentState==MENU_SPEC_PEAKS) {l1=letter_P;l2=letter_b;valV=specPeaks;}
    else if(currentState==MENU_STANDBY)    {l1=letter_S;l2=letter_b;valV=standbyTime;}
    else if(currentState==MENU_OLED_ROT)   {l1=letter_O;l2=letter_r;valV=oledRotate;}
    // Загальний цикл — пропускаємо для станів що малюють самостійно
    bool skipGeneral = (currentState==MENU_SPEC_STYLE) ||
                       (currentState==MENU_ANIM && animMode==6) ||
                       (currentState==MENU_OLED_TYPE) ||
                       (currentState==MENU_CALIB) ||
                       (currentState==MENU_BALANCE);
    if(!skipGeneral) {
      for(int s=0;s<7;s++){
        if(l1&(1<<s))               setBit(1,digitSegments[0][s],true);
        if(l2&(1<<s))               setBit(1,digitSegments[1][s],true);
        if(numbers[valV/10]&(1<<s)) setBit(1,digitSegments[2][s],true);
        if(numbers[valV%10]&(1<<s)) setBit(1,digitSegments[3][s],true);
      }
    }
  }
  vfdSend();

  // OLED — три режими
  // OLED логіка
  static bool muteActive    = false;
  static unsigned long muteTimer      = 0;
  static unsigned long exitReelsTimer = 0;
  static OledMode waitingMode         = OLED_OFF;
  static unsigned long waitTimer      = 0;
  static bool oledTimersInited        = false;
  if (!oledTimersInited) { muteTimer=millis(); oledTimersInited=true; }

  static bool prevMuteActive = false;
  muteActive = oledMute;
  if (muteActive && !prevMuteActive) muteTimer = millis();
  prevMuteActive = muteActive;

  bool isPulsing = (millis()-lastPulseTime < 500);
  // Для стилю 2A (13) — бобіни не перехоплюють екран від імпульсів
  bool reelsFromPulse = isPulsing && (specStyle != 13);

  OledMode targetMode;
  bool inStyleMenu = (currentState == MENU_SPEC_STYLE || currentState == MENU_SPEC_PEAKS);
  if      (reelsFromPulse)              targetMode = OLED_REELS;
  else if (muteActive && !inStyleMenu)  targetMode = OLED_EYES;
  else                                  targetMode = OLED_SPECTRUM;

  if (targetMode == OLED_REELS) {
    oledMode       = OLED_REELS;
    exitReelsTimer = 0;
    waitingMode    = OLED_OFF;
  } else if (oledMode == OLED_REELS) {
    if (exitReelsTimer == 0) exitReelsTimer = millis();
    if (millis()-exitReelsTimer >= 2000) {
      oledMode=targetMode; exitReelsTimer=0; waitingMode=OLED_OFF;
    }
  } else if (targetMode != oledMode) {
    if (targetMode != waitingMode) { waitingMode=targetMode; waitTimer=millis(); }
    else if (millis()-waitTimer >= 3000) { oledMode=targetMode; waitingMode=OLED_OFF; }
  } else {
    waitingMode = OLED_OFF;
  }

  // Обертання бобін — обидві в одну сторону
  // При rewind (cached_REWIND_SENSE=true) — проти годинникової
  if (reelPulseCount > 0) {
    float step = reelPulseCount * 0.15f;
    if (cached_REWIND_SENSE) {
      reelAngleL += step; // реверс — за годинниковою
      reelAngleR += step;
    } else {
      reelAngleL -= step; // відтворення — проти годинникової
      reelAngleR -= step;
    }
    if (reelAngleL > 2*M_PI) reelAngleL -= 2*M_PI;
    if (reelAngleL < 0)      reelAngleL += 2*M_PI;
    if (reelAngleR > 2*M_PI) reelAngleR -= 2*M_PI;
    if (reelAngleR < 0)      reelAngleR += 2*M_PI;
    reelPulseCount = 0;
  }
  if (millis()-lastOledUpdate >= 80) {
    lastOledUpdate = millis();
    updateOLED();
  }

  // Standby — перевіряємо ПІСЛЯ оновлення oledMode
  if (!standbyTimerInited) { standbyTimer = millis(); standbyTimerInited = true; }

  if (standbyTime > 0 && currentState == WORK) {
    if (oledMode != OLED_EYES) {
      standbyTimer = millis();
      if (standbyActive) {
        standbyActive = false;
        applyBrightness();
        display->displayOn();
      }
    } else {
      if (!standbyActive && (millis() - standbyTimer >= (unsigned long)standbyTime * 60000UL)) {
        standbyActive = true;
        analogWrite(PIN_BRIGHTNESS_PWM, 0);
        display->displayOff();
      }
    }
  } else {
    standbyTimer = millis();
    if (standbyActive) {
      standbyActive = false;
      applyBrightness();
      display->displayOn();
    }
  }

  // Fix 7: Debounce збереження налаштувань — пишемо у Flash тільки через 2с після останньої зміни
  if (settingsDirty && millis() - settingsDirtyTime > 2000) {
    settingsDirty = false;
    saveSettings();
  }
}
