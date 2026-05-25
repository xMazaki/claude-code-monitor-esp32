/*
 * Claude Code Usage Monitor pour ESP32 tactile.
 * Voir README.md pour les details d'installation et le portail web.
 * Seul WIFI_SSID / WIFI_PASS est a renseigner ici, le reste se configure
 * a chaud via http://<ip-de-la-carte>/.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
// FS.h doit etre inclus avant WebServer.h sur ESP32 core 3.x sinon le header
// ne compile pas (la classe fs::FS n'est pas dans le scope global).
#include <FS.h>
using fs::FS;
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "claude_logo.h"

const char* WIFI_SSID = "VOTRE_SSID";
const char* WIFI_PASS = "VOTRE_PASSWORD_WIFI";

// Auth HTTP Basic du portail web. WEB_PASSWORD vide = pas d'auth.
const char* WEB_USERNAME = "admin";
const char* WEB_PASSWORD = "";

// Defaut utilise seulement si la NVS est vide. L'URL du proxy se configure
// ensuite depuis le portail web.
const char* CLAUDE_API_BASE_DEFAULT = "";

// Reglages charges depuis la NVS au boot et modifiables via le portail web.
unsigned long refreshIntervalMs = 5UL * 60UL * 1000UL;
String sessionKey = "";
String orgUuid    = "";   // vide = auto-discovery via /api/organizations
String claudeApiBase = "";
int thresholdSession5hWarn  = 70;
int thresholdSession5hAlert = 90;
int thresholdWeekly7dWarn   = 70;
int thresholdWeekly7dAlert  = 90;
String defaultBootView = "clock";
String tzName = "CET-1CEST,M3.5.0,M10.5.0/3";

// Luminosite du backlight quand l'ecran est "allume" (mode normal).
int   brightPercent   = 100;            // 0-100%

// Mode veille auto : dim du backlight apres un delai sans interaction.
bool  sleepEnabled    = false;          // active/desactive le mode veille
int   sleepDelayMin   = 5;              // delai avant dim (minutes)
int   sleepDimPercent = 10;             // luminosite dimmed (0-100%)
// Etat runtime
bool  isDimmed = false;
unsigned long lastActivityMs = 0;
#define BACKLIGHT_PIN 27

// Wake on alert : quand un seuil est franchi alors que l'ecran est dim, on
// rallume temporairement pendant ce delai puis on retombe en veille.
#define ALERT_WAKE_MS 30000UL
unsigned long alertWakeUntilMs = 0;

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
bool timeReady = false;

TFT_eSPI tft = TFT_eSPI();
// Un sprite plein ecran (320*240*16bit = 150 Ko) ne tient pas en RAM avec
// WiFi + TLS actifs. On dessine donc directement sur `tft` et on utilise des
// petits sprites uniquement pour les zones qui changent souvent (anti-flicker).
TFT_eSprite sprGaugeL = TFT_eSprite(&tft);
TFT_eSprite sprGaugeR = TFT_eSprite(&tft);
TFT_eSprite sprStatus = TFT_eSprite(&tft);
TFT_eSprite sprTime   = TFT_eSprite(&tft);
#define SCREEN_W 320
#define SCREEN_H 240

// Touch CST820 sur I2C
#define TOUCH_SDA 33
#define TOUCH_SCL 32
#define TOUCH_RST 25
#define TOUCH_INT 21
#define TOUCH_ADDR 0x15

// Palette Claude en RGB565
#define COL_BG        0x1041
#define COL_BG2       0x2104
#define COL_CARD      0x2945
#define COL_TEXT      0xFFFE
#define COL_DIM       0x8C71
#define COL_DIM2      0x52AA

// Orange "sparkle" de Claude : hsl(14.8, 63.1%, 59.6%) soit #D97757
#define COL_CLAUDE    0xDBAE
#define COL_CLAUDE_D  0xA28B

#define COL_OK        0x4E89
#define COL_WARN      0xFBC0
#define COL_DANGER    0xE186

enum ViewMode { VIEW_GAUGES, VIEW_CLOCK, VIEW_SETTINGS, VIEW_ERROR };
ViewMode currentView = VIEW_CLOCK;  // ecrase par setup() apres loadPrefs()

struct UsageData {
  float fiveHourPct = -1;
  float sevenDayPct = -1;
  String fiveHourResetIso = "";
  String sevenDayResetIso = "";
  float extraUsedEur = 0;
  bool extraEnabled = false;
  bool valid = false;
  String lastError = "";
  unsigned long lastFetchMs = 0;
};
UsageData usage;

bool wifiOk = false;
String lastStatusMsg = "Demarrage...";
uint16_t lastStatusColor = COL_DIM;
unsigned long bootTime = 0;

unsigned long touchStartMs = 0;
bool touchHeld = false;
int touchStartX = 0, touchStartY = 0;
unsigned long lastTouchEndMs = 0;
#define LONG_PRESS_MS 800
#define TOUCH_DEBOUNCE_MS 300

int touchMaxDx = 0;
#define SWIPE_THRESHOLD 80

float animFiveHour = 0;
float animSevenDay = 0;
unsigned long lastAnimMs = 0;
unsigned long lastRedrawMs = 0;
#define ANIM_FRAME_MS 16

// Etat du flash one-shot lorsqu'un seuil est franchi a la hausse.
bool flash5hActive = false;
bool flash7dActive = false;
unsigned long flash5hStartMs = 0;
unsigned long flash7dStartMs = 0;
bool prev5hOverWarn = false;
bool prev5hOverAlert = false;
bool prev7dOverWarn = false;
bool prev7dOverAlert = false;
#define FLASH_DURATION_MS 1500
#define FLASH_BLINKS 3

// Anim du logo en train de pulser pendant un fetch.
volatile bool fetchInProgress = false;
unsigned long fetchPulseStartMs = 0;
#define FETCH_PULSE_PERIOD_MS 600

// On distingue le dernier essai du dernier succes : si l'API echoue, on
// applique FAILURE_BACKOFF_MS au lieu de re-essayer en boucle (sinon l'UI
// se fige sur le timeout du handshake TLS).
unsigned long lastFetchAttemptMs = 0;
int consecutiveFetchFailures = 0;
#define FAILURE_BACKOFF_MS 60000UL

WebServer webServer(80);
Preferences prefs;
bool otaActive = false;
unsigned long pendingRebootAt = 0;

// Caches pour ne redessiner que ce qui a vraiment change entre deux ticks.
String lastTimeStr = "";
String lastDateStr = "";
float  lastBar5h   = -1;
float  lastBar7d   = -1;
bool   lastFlashL  = false;
bool   lastFlashR  = false;

// Declarations forward
void connectWiFi();
bool fetchOrgUuid();
bool fetchUsage();
void drawMain();
void drawGauges();
void drawClock();
void drawSettings();
void drawError(const String& msg);
void drawHeader();
void redrawHeaderLogo();
void drawStatusBar();
void updateGaugesAnim();
void updateClock();
void drawGaugeInto(TFT_eSprite& s, int px, int py, float pct, const char* label, const char* sub, uint16_t color, bool flashOn);
void handleTouch(int x, int y);
void handleSwipe(int dx);
void handleLongPress(int x, int y);
bool touchRead(int* x, int* y);
void touchInit();
String formatResetCountdown(const String& iso, bool withDay);
String formatResetIn(const String& iso);
uint16_t pctColor(float pct, int warn, int alert);
void setStatus(const String& msg, uint16_t color);
void checkThresholdFlashes();
void loadPrefs();
void savePrefs();
void setupWebServer();
void setupOTA();
void initNTP();
void rebootSoon();
void setBacklight(uint8_t level);
uint8_t brightPwm();
uint8_t dimPwm();
void fadeBacklight(uint8_t from, uint8_t to, int durationMs);
void wakeFromDim();
void checkSleepTimeout();

void setup() {
  // Desactive le brownout detector : sur les Guition JC2432W328 alimentees par
  // un port USB de PC, le pic de courant a l'activation WiFi (jusqu'a 500 mA)
  // peut faire chuter la tension sous le seuil du BOD, ce qui declenche un
  // reset en boucle. La carte a assez de capacite decouplee pour encaisser
  // ces pics, on peut donc desactiver la securite sans dommage.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.println("\n[BOOT] Claude Code Monitor v1.2");

  bootTime = millis();
  loadPrefs();

  tft.init();
  tft.setRotation(1);

  // Backlight pilote en PWM (LEDC). Demarre eteint pour faire un fade-in apres
  // avoir dessine le splash. API ESP32 core 3.x : ledcAttach(pin, freq, res).
  ledcAttach(BACKLIGHT_PIN, 5000, 8);
  setBacklight(0);
  tft.fillScreen(COL_BG);

  // Splash
  tft.setTextDatum(MC_DATUM);
  tft.drawBitmap((SCREEN_W - CLAUDE_LOGO_W) / 2, 50, claude_logo,
                 CLAUDE_LOGO_W, CLAUDE_LOGO_H, COL_CLAUDE);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Claude Code Monitor", SCREEN_W / 2, 145, 4);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Connexion WiFi...", SCREEN_W / 2, 180, 2);

  // Fade-in du backlight (0 -> luminosite configuree sur ~300 ms).
  fadeBacklight(0, brightPwm(), 300);
  lastActivityMs = millis();

  sprGaugeL.setColorDepth(16);
  sprGaugeR.setColorDepth(16);
  sprStatus.setColorDepth(16);
  sprTime.setColorDepth(16);
  bool sLok = sprGaugeL.createSprite(140, 150);
  bool sRok = sprGaugeR.createSprite(140, 150);
  bool sStok = sprStatus.createSprite(SCREEN_W, 18);
  bool sTok = sprTime.createSprite(220, 56);
  Serial.printf("[SPR] gaugeL=%d gaugeR=%d status=%d time=%d\n", sLok, sRok, sStok, sTok);

  touchInit();
  connectWiFi();

  if (wifiOk) {
    initNTP();
    setupWebServer();
    setupOTA();
  }

  // Le premier fetch est differe a la loop pour que l'UI s'affiche tout de
  // suite et que le portail web reste joignable meme si l'API est bloquee.
  if (sessionKey.length() == 0) {
    currentView = VIEW_ERROR;
    drawError(wifiOk
      ? ("Configurer via http://" + WiFi.localIP().toString() + "/")
      : "WiFi indisponible");
  } else {
    currentView = (defaultBootView == "gauges") ? VIEW_GAUGES : VIEW_CLOCK;
    drawMain();
  }
}

void loop() {
  unsigned long now = millis();
  int tx, ty;

  // Yield explicite : evite le watchdog Task et permet aux taches systeme
  // (WiFi, lwIP) de tourner. Sans ca, une iteration de loop trop chargee
  // peut declencher un reboot apres ~3 s.
  yield();

  if (wifiOk) {
    webServer.handleClient();
    ArduinoOTA.handle();
  }

  // Log diagnostic toutes les 30 s pour pouvoir reperer un drift heap.
  static unsigned long lastDiagMs = 0;
  if (now - lastDiagMs > 30000) {
    lastDiagMs = now;
    Serial.printf("[DIAG] up=%lus heap=%u/%u (largest=%u) rssi=%d view=%d valid=%d fails=%d\n",
                  (now - bootTime) / 1000,
                  ESP.getFreeHeap(), ESP.getHeapSize(),
                  ESP.getMaxAllocHeap(),
                  wifiOk ? WiFi.RSSI() : 0,
                  currentView, usage.valid,
                  consecutiveFetchFailures);
    // Garde-fou : si la heap libre tombe sous 20 Ko, on previent au log
    // pour que tu puisses reperer dans quel scenario ca arrive.
    if (ESP.getFreeHeap() < 20000) {
      Serial.println("[DIAG] WARNING heap critique");
    }
  }

  if (pendingRebootAt > 0 && now >= pendingRebootAt) {
    Serial.println("[REBOOT]");
    delay(100);
    ESP.restart();
  }

  // Pendant un OTA, l'ecran est prit par les callbacks ArduinoOTA.
  if (otaActive) return;

  // Watchdog WiFi : si on perd la connexion en cours de route (router qui
  // reboot, mise en veille longue, etc.), on retente proprement toutes les
  // 30 s. Evite la boucle "connexion WiFi..." qui ne sortait que par un
  // debranchement / rebranchement physique.
  static unsigned long lastWifiCheckMs = 0;
  if (now - lastWifiCheckMs > 30000) {
    lastWifiCheckMs = now;
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (wifiOk && !connected) {
      Serial.println("[WIFI] connection lost, reconnecting...");
      wifiOk = false;
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else if (!wifiOk && connected) {
      wifiOk = true;
      Serial.printf("[WIFI] reconnected: %s\n", WiFi.localIP().toString().c_str());
      // Reseau de retour : on retente immediatement les services.
      if (!timeReady) initNTP();
      lastFetchAttemptMs = 0;
      consecutiveFetchFailures = 0;
    } else if (!wifiOk && !connected) {
      // Toujours pas connecte : on relance proprement WiFi.begin().
      Serial.println("[WIFI] still down, retrying...");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  // Watchdog NTP : si la 1re synchro a echoue (timeReady reste a false),
  // on retente tant qu'on a du WiFi pour qu'on n'ait plus jamais a rebooter
  // pour voir l'heure s'afficher correctement.
  static unsigned long lastNtpCheckMs = 0;
  if (!timeReady && wifiOk && now - lastNtpCheckMs > 10000) {
    lastNtpCheckMs = now;
    struct tm t;
    if (getLocalTime(&t, 200)) {
      timeReady = true;
      Serial.printf("[NTP] late sync OK: %02d:%02d:%02d\n",
                    t.tm_hour, t.tm_min, t.tm_sec);
      // Force le repush du sprite time pour effacer le "--:--"
      lastTimeStr = "";
    } else {
      Serial.println("[NTP] still waiting...");
    }
  }

  bool touchPresent = touchRead(&tx, &ty);
  if (touchPresent) {
    // Toute interaction tactile reveille l'ecran si on etait en mode veille.
    // Si on etait dim, on consomme ce premier touch juste pour reveiller
    // (pas d'action declenchee) pour eviter qu'un toucher accidentel
    // declenche un refresh / changement de vue.
    if (isDimmed) {
      wakeFromDim();
      touchStartMs = 0;
      lastTouchEndMs = now;
      return;
    }
    lastActivityMs = now;
    if (touchStartMs == 0) {
      touchStartMs = now;
      touchStartX = tx;
      touchStartY = ty;
      touchMaxDx = 0;
      touchHeld = false;
    } else {
      int dx = tx - touchStartX;
      if (abs(dx) > abs(touchMaxDx)) touchMaxDx = dx;
      if (!touchHeld && abs(touchMaxDx) < SWIPE_THRESHOLD &&
          (now - touchStartMs > LONG_PRESS_MS)) {
        touchHeld = true;
        handleLongPress(touchStartX, touchStartY);
      }
    }
  } else if (touchStartMs > 0) {
    // Relachement : on distingue tap court, long press, et swipe.
    if (!touchHeld && (now - lastTouchEndMs > TOUCH_DEBOUNCE_MS)) {
      if (abs(touchMaxDx) >= SWIPE_THRESHOLD) {
        handleSwipe(touchMaxDx);
      } else if (now - touchStartMs < LONG_PRESS_MS) {
        handleTouch(touchStartX, touchStartY);
      }
    }
    touchStartMs = 0;
    touchHeld = false;
    touchMaxDx = 0;
    lastTouchEndMs = now;
  }

  // Refresh periodique. Apres N echecs consecutifs on attend FAILURE_BACKOFF_MS
  // entre chaque essai pour ne pas figer l'UI sur les timeouts TLS.
  if (wifiOk && sessionKey.length() > 0) {
    unsigned long sinceLast = now - lastFetchAttemptMs;
    unsigned long sinceSuccess = now - usage.lastFetchMs;
    bool dueByInterval = (usage.lastFetchMs == 0 && lastFetchAttemptMs == 0) ||
                         sinceSuccess > refreshIntervalMs;
    bool backoffOk = (lastFetchAttemptMs == 0) ||
                     (consecutiveFetchFailures == 0 ? sinceLast > 1000
                                                    : sinceLast > FAILURE_BACKOFF_MS);
    if (dueByInterval && backoffOk) {
      lastFetchAttemptMs = now;
      // Auto-discovery de l'org UUID si pas encore connue.
      if (orgUuid.length() == 0) {
        if (fetchOrgUuid()) savePrefs();
      }
      if (orgUuid.length() > 0) {
        fetchUsage();
      } else {
        consecutiveFetchFailures++;
      }
      // claudeGet() s'occupe deja du repaint cible (pas de fillScreen ici).
    }
  }

  // Tick d'animation (jauges, flash, pulse logo).
  if (now - lastAnimMs > ANIM_FRAME_MS) {
    lastAnimMs = now;

    bool animMoved = false;
    if (usage.valid) {
      // Ease-out doux a 60 FPS : convergence en ~600ms.
      const float dt = 0.12f;
      float diff5 = usage.fiveHourPct - animFiveHour;
      float diff7 = usage.sevenDayPct - animSevenDay;
      if (fabs(diff5) > 0.05 || fabs(diff7) > 0.05) {
        animFiveHour += diff5 * dt;
        animSevenDay += diff7 * dt;
        animMoved = true;
      } else if (animFiveHour != usage.fiveHourPct || animSevenDay != usage.sevenDayPct) {
        animFiveHour = usage.fiveHourPct;
        animSevenDay = usage.sevenDayPct;
        animMoved = true;
      }
    }

    if (flash5hActive && now - flash5hStartMs > FLASH_DURATION_MS) flash5hActive = false;
    if (flash7dActive && now - flash7dStartMs > FLASH_DURATION_MS) flash7dActive = false;

    bool needHighFrame =
      animMoved || flash5hActive || flash7dActive || fetchInProgress;

    if (currentView == VIEW_GAUGES && needHighFrame) {
      updateGaugesAnim();
    }
    // Sur la vue horloge, on ne redraw les barres que pendant l'anim ; l'heure
    // est mise a jour par le tick 1 Hz plus bas.
    else if (currentView == VIEW_CLOCK && (animMoved || flash5hActive || flash7dActive)) {
      updateClock();
    }

    if (fetchInProgress &&
        (currentView == VIEW_GAUGES || currentView == VIEW_CLOCK)) {
      redrawHeaderLogo();
    }
  }

  // Tick 1 Hz : seulement pour rafraichir l'heure/date sur la vue horloge.
  static unsigned long lastClockTick = 0;
  if (currentView == VIEW_CLOCK && now - lastClockTick > 1000) {
    lastClockTick = now;
    updateClock();
  }

  // Tick 10 s pour la status bar (independant de l'anim et de la vue).
  if ((currentView == VIEW_GAUGES || currentView == VIEW_CLOCK) &&
      now - lastRedrawMs > 10000) {
    lastRedrawMs = now;
    drawStatusBar();
  }

  // Mode veille auto : verifie si le delai d'inactivite est ecoule.
  checkSleepTimeout();
}

void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s...\n", WIFI_SSID);
  // Sequence robuste : on coupe proprement avant de se reconnecter pour
  // eviter les boucles "connexion..." infinies apres une longue mise en veille.
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // bug connu : le power-save WiFi de l'ESP32 cause
                         // des deconnexions silencieuses sur certains AP
  WiFi.setAutoReconnect(true);
  // Reduit la puissance d'emission de 20 dBm a 17 dBm pour diminuer le pic
  // de courant a la connexion (-> moins de risque de brownout sur USB faible).
  WiFi.setTxPower(WIFI_POWER_17dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.printf("[WIFI] OK, IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    wifiOk = false;
    Serial.println("[WIFI] Connection FAILED");
    setStatus("WiFi echec", COL_DANGER);
  }
}

String claudeBase() {
  if (claudeApiBase.length() > 0) return claudeApiBase;
  return "https://claude.ai";
}

int claudeGet(const String& path, String& bodyOut) {
  Serial.printf("[HEAP] free=%u kB / largest block=%u kB before fetch\n",
                ESP.getFreeHeap() / 1024,
                ESP.getMaxAllocHeap() / 1024);

  // Le handshake TLS reclame environ 30 Ko de heap contigu. Les sprites 16 bit
  // (85 Ko cumules) fragmentent la heap au point qu'il n'y a plus de bloc
  // assez gros : on les libere pendant le fetch puis on les recree apres.
  bool hadGaugeL = sprGaugeL.created();
  bool hadGaugeR = sprGaugeR.created();
  bool hadStatus = sprStatus.created();
  bool hadTime   = sprTime.created();
  if (hadGaugeL) sprGaugeL.deleteSprite();
  if (hadGaugeR) sprGaugeR.deleteSprite();
  if (hadStatus) sprStatus.deleteSprite();
  if (hadTime)   sprTime.deleteSprite();

  Serial.printf("[HEAP] after sprite free: free=%u kB / largest=%u kB\n",
                ESP.getFreeHeap() / 1024,
                ESP.getMaxAllocHeap() / 1024);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(4000);
  http.setTimeout(6000);

  String base = claudeBase();
  String url = base + path;
  bool viaProxy = (base != "https://claude.ai");
  Serial.printf("[HTTP] GET %s (proxy=%d)\n", url.c_str(), viaProxy);

  if (!http.begin(client, url)) {
    Serial.println("[HTTP] begin() failed");
    return -1;
  }

  if (viaProxy) {
    // Le proxy attend la sessionKey dans X-Session-Key et reinjecte le cookie.
    http.addHeader("X-Session-Key", sessionKey);
    http.addHeader("Accept", "application/json");
  } else {
    // Mode direct : on imite un navigateur (souvent insuffisant pour passer
    // le filtrage Cloudflare, d'ou la recommandation d'utiliser le proxy).
    http.addHeader("Cookie", String("sessionKey=") + sessionKey);
    http.addHeader("User-Agent",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/131.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    http.addHeader("anthropic-client-platform", "web_claude_ai");
    http.addHeader("anthropic-client-version", "1.0.0");
    http.addHeader("Origin", "https://claude.ai");
    http.addHeader("Referer", "https://claude.ai/settings/usage");
    http.addHeader("Sec-Fetch-Dest", "empty");
    http.addHeader("Sec-Fetch-Mode", "cors");
    http.addHeader("Sec-Fetch-Site", "same-origin");
  }

  int code = http.GET();
  if (code > 0) {
    bodyOut = http.getString();
  } else {
    Serial.printf("[HTTP] error: %s\n", http.errorToString(code).c_str());
    char tlsBuf[160] = "?";
    int tlsErr = client.lastError(tlsBuf, sizeof(tlsBuf));
    Serial.printf("[TLS]  lastError=%d (%s)\n", tlsErr, tlsBuf);
  }
  http.end();

  Serial.printf("[HTTP] code=%d, body length=%d, heap after=%u kB\n",
                code, bodyOut.length(), ESP.getFreeHeap() / 1024);

  if (hadGaugeL) sprGaugeL.createSprite(140, 150);
  if (hadGaugeR) sprGaugeR.createSprite(140, 150);
  if (hadStatus) sprStatus.createSprite(SCREEN_W, 18);
  if (hadTime)   sprTime.createSprite(220, 56);

  // Apres recreation des sprites, on invalide les caches pour que le prochain
  // tick d'anim redessine les zones utiles SANS full fillScreen (sinon l'ecran
  // saute visuellement a chaque fetch).
  lastBar5h = -1;
  lastBar7d = -1;
  if (currentView == VIEW_CLOCK) {
    // On force un repush du sprite time pour que la zone ne reste pas noire
    // entre la liberation des sprites et le prochain tick 1 Hz.
    String t, d;
    clockGetStrings(t, d);
    pushTimeSprite(t);
    lastTimeStr = t;
    updateClock();
    drawStatusBar();
  } else if (currentView == VIEW_GAUGES) {
    updateGaugesAnim();
  }

  return code;
}

bool fetchOrgUuid() {
  String body;
  int code = claudeGet("/api/organizations", body);
  if (code != 200) {
    usage.lastError = "Org fetch HTTP " + String(code);
    return false;
  }

  // On filtre le JSON aux seuls champs utiles pour tenir dans 4 Ko de heap.
  JsonDocument filter;
  filter[0]["uuid"] = true;
  filter[0]["capabilities"] = true;
  filter[0]["name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    usage.lastError = String("Org JSON: ") + err.c_str();
    Serial.printf("[CLAUDE] org parse error: %s\n", err.c_str());
    return false;
  }

  // On prend la premiere org avec capability claude_max ou claude_pro,
  // sinon fallback sur la premiere de la liste.
  JsonArray orgs = doc.as<JsonArray>();
  const char* firstUuid = nullptr;
  for (JsonObject org : orgs) {
    const char* u = org["uuid"];
    if (!u) continue;
    if (!firstUuid) firstUuid = u;
    JsonArray caps = org["capabilities"].as<JsonArray>();
    for (JsonVariant c : caps) {
      const char* s = c.as<const char*>();
      if (s && (strcmp(s, "claude_max") == 0 || strcmp(s, "claude_pro") == 0)) {
        orgUuid = u;
        const char* n = org["name"];
        Serial.printf("[CLAUDE] Selected org: %s (%s)\n", n ? n : "?", orgUuid.c_str());
        return true;
      }
    }
  }
  if (firstUuid) {
    orgUuid = firstUuid;
    Serial.printf("[CLAUDE] Fallback first org: %s\n", orgUuid.c_str());
    return true;
  }
  return false;
}

bool fetchUsage() {
  fetchInProgress = true;
  fetchPulseStartMs = millis();
  String body;
  int code = claudeGet("/api/organizations/" + orgUuid + "/usage", body);
  if (code != 200) {
    consecutiveFetchFailures++;
    usage.lastError = "Usage HTTP " + String(code);
    setStatus("Erreur API: " + String(code), COL_DANGER);
    fetchInProgress = false;
    return false;
  }
  consecutiveFetchFailures = 0;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    consecutiveFetchFailures++;
    usage.lastError = String("Usage JSON: ") + err.c_str();
    setStatus("JSON invalide", COL_DANGER);
    fetchInProgress = false;
    return false;
  }

  usage.fiveHourPct = doc["five_hour"]["utilization"] | 0.0f;
  usage.sevenDayPct = doc["seven_day"]["utilization"] | 0.0f;
  const char* r5 = doc["five_hour"]["resets_at"];
  const char* r7 = doc["seven_day"]["resets_at"];
  usage.fiveHourResetIso = r5 ? String(r5) : String("");
  usage.sevenDayResetIso = r7 ? String(r7) : String("");

  JsonObject extra = doc["extra_usage"];
  if (!extra.isNull()) {
    usage.extraEnabled = extra["is_enabled"] | false;
    usage.extraUsedEur = extra["used_credits"] | 0.0f;
  }

  usage.valid = true;
  usage.lastFetchMs = millis();
  usage.lastError = "";
  setStatus("OK", COL_OK);

  Serial.printf("[CLAUDE] 5h=%.1f%% 7d=%.1f%% extra=%.2f EUR\n",
                usage.fiveHourPct, usage.sevenDayPct, usage.extraUsedEur);
  fetchInProgress = false;
  return true;
}

uint16_t pctColor(float pct, int warn, int alert) {
  if (pct >= alert) return COL_DANGER;
  if (pct >= warn) return COL_WARN;
  return COL_OK;
}

void setStatus(const String& msg, uint16_t color) {
  lastStatusMsg = msg;
  lastStatusColor = color;
}

// Logo qui pulse pendant un fetch (alterne entre teinte normale et teinte sombre).
uint16_t headerLogoColor() {
  if (!fetchInProgress) return COL_CLAUDE;
  unsigned long t = millis() - fetchPulseStartMs;
  float phase = (t % FETCH_PULSE_PERIOD_MS) / (float)FETCH_PULSE_PERIOD_MS;
  float a = phase < 0.5f ? (phase * 2.0f) : (2.0f - phase * 2.0f);
  return (a > 0.5f) ? COL_CLAUDE : COL_CLAUDE_D;
}

void drawHeader() {
  tft.fillRect(0, 0, SCREEN_W, 42, COL_BG);

  tft.drawBitmap(10, 7, claude_logo_sm, CLAUDE_LOGO_SM_W, CLAUDE_LOGO_SM_H, headerLogoColor());

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Claude Code", 48, 14, 4);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("USAGE MONITOR", 48, 30, 1);

  tft.setTextDatum(MR_DATUM);
  if (wifiOk) {
    tft.setTextColor(COL_OK, COL_BG);
    tft.drawString("WiFi", SCREEN_W - 8, 14, 1);
    tft.setTextColor(COL_DIM, COL_BG);
    int rssi = WiFi.RSSI();
    tft.drawString(String(rssi) + "dBm", SCREEN_W - 8, 26, 1);
  } else {
    tft.setTextColor(COL_DANGER, COL_BG);
    tft.drawString("No WiFi", SCREEN_W - 8, 14, 1);
  }

  tft.drawFastHLine(8, 41, SCREEN_W - 16, COL_CARD);
}

// Repeint uniquement le logo du header (utilise pour la pulse pendant un fetch).
void redrawHeaderLogo() {
  tft.fillRect(10, 7, CLAUDE_LOGO_SM_W, CLAUDE_LOGO_SM_H, COL_BG);
  tft.drawBitmap(10, 7, claude_logo_sm, CLAUDE_LOGO_SM_W, CLAUDE_LOGO_SM_H, headerLogoColor());
}

// Dessine une jauge circulaire dans le sprite passe puis le pousse en (px, py).
// flashOn = effet inverse video pour signaler le franchissement d'un seuil.
void drawGaugeInto(TFT_eSprite& s, int px, int py, float pct,
                   const char* label, const char* sub, uint16_t color, bool flashOn) {
  const int sw = 140;
  const int sh = 150;
  const int cx = sw / 2;
  const int cy = 56;
  const int r = 52;

  uint16_t bg = flashOn ? color : COL_BG;
  uint16_t ringFg = flashOn ? COL_TEXT : color;
  uint16_t ringBg = flashOn ? color : COL_CARD;

  s.fillSprite(bg);

  s.drawSmoothArc(cx, cy, r, r - 9, 30, 330, ringBg, bg);

  if (pct >= 1.0) {
    int sweep = 300;
    int endAngle = 30 + (int)(sweep * (pct / 100.0));
    if (endAngle > 330) endAngle = 330;
    s.drawSmoothArc(cx, cy, r, r - 9, 30, endAngle, ringFg, bg);
  }

  char valBuf[8];
  snprintf(valBuf, sizeof(valBuf), "%d", (int)round(pct));
  s.setTextColor(flashOn ? COL_BG : COL_TEXT, bg);
  s.setTextDatum(MC_DATUM);
  s.drawString(valBuf, cx - 1, cy + 4, 4);

  // approxW : largeur approximative du nombre pour positionner le "%" a cote.
  int approxW = 14 * strlen(valBuf);
  s.setTextColor(flashOn ? COL_BG : COL_DIM, bg);
  s.setTextDatum(BL_DATUM);
  s.drawString("%", cx + approxW / 2 + 1, cy + 10, 2);

  s.setTextColor(flashOn ? COL_BG : color, bg);
  s.setTextDatum(MC_DATUM);
  s.drawString(label, cx, cy + r + 12, 2);

  // Sub-text (reset time)
  s.setTextColor(flashOn ? COL_BG : COL_DIM, bg);
  s.drawString(sub, cx, cy + r + 28, 1);

  s.pushSprite(px, py);
}

// Congruence de Zeller : renvoie le jour de la semaine (0=Dim ... 6=Sam).
int dayOfWeek(int y, int m, int d) {
  if (m < 3) { m += 12; y -= 1; }
  int K = y % 100;
  int J = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  // Zeller renvoie 0=Sam : on remappe vers 0=Dim.
  return (h + 6) % 7;
}

String formatResetCountdown(const String& iso, bool withDay) {
  // Si NTP est dispo, on prefere le countdown relatif "dans 2h15" qui est
  // bien plus parlant qu'une heure UTC absolue.
  if (timeReady) {
    String rel = formatResetIn(iso);
    if (rel.length() > 0) return rel;
  }

  // Fallback : afficher l'heure UTC brute issue de l'API.
  if (iso.length() < 16) return "--:--";
  int tIdx = iso.indexOf('T');
  if (tIdx < 0) return "--:--";
  String hhmm = iso.substring(tIdx + 1, tIdx + 6);

  if (!withDay) {
    return "reset " + hhmm + " UTC";
  }

  int y = iso.substring(0, 4).toInt();
  int mo = iso.substring(5, 7).toInt();
  int d = iso.substring(8, 10).toInt();
  if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31) {
    return "reset " + hhmm + " UTC";
  }
  static const char* days[] = {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"};
  String day = days[dayOfWeek(y, mo, d)];
  return "reset " + day + " " + hhmm + " UTC";
}

// Calcule un time_t depuis un struct tm interprete comme UTC.
// On ne touche PAS a la variable d'environnement TZ pour eviter les bugs de
// re-entrance qui faisaient afficher l'heure UTC sur la vue horloge.
static time_t timegmManual(int y, int mo, int d, int h, int mi, int s) {
  // Jours cumules en debut de chaque mois (annee non bissextile).
  static const int daysBefore[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  long days = (y - 1970) * 365L + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
  days += daysBefore[mo - 1] + (d - 1);
  bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
  if (leap && mo > 2) days += 1;
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + s);
}

// Countdown relatif vers un reset, calcule avec l'heure NTP locale.
// Renvoie "dans 2h15", "dans 45min" ou "imminent".
String formatResetIn(const String& iso) {
  if (!timeReady || iso.length() < 19) return "";
  int y  = iso.substring(0, 4).toInt();
  int mo = iso.substring(5, 7).toInt();
  int d  = iso.substring(8, 10).toInt();
  int h  = iso.substring(11, 13).toInt();
  int mi = iso.substring(14, 16).toInt();
  int s  = iso.substring(17, 19).toInt();
  if (y < 2000) return "";

  time_t resetT = timegmManual(y, mo, d, h, mi, s);
  time_t nowT;
  time(&nowT);
  long diff = (long)(resetT - nowT);
  if (diff <= 0) return "imminent";
  if (diff < 60) return "dans " + String(diff) + "s";
  long mins = diff / 60;
  if (mins < 60) return "dans " + String(mins) + "min";
  long hours = mins / 60;
  long rmin = mins % 60;
  return "dans " + String(hours) + "h" + (rmin < 10 ? "0" : "") + String(rmin);
}

void drawStatusBar() {
  sprStatus.fillSprite(COL_CARD);

  sprStatus.setTextColor(COL_DIM, COL_CARD);
  sprStatus.setTextDatum(ML_DATUM);
  if (usage.lastFetchMs > 0) {
    unsigned long elapsed = (millis() - usage.lastFetchMs) / 1000;
    char buf[40];
    if (elapsed < 60) snprintf(buf, sizeof(buf), "Maj il y a %lus", elapsed);
    else snprintf(buf, sizeof(buf), "Maj il y a %lum", elapsed / 60);
    sprStatus.drawString(buf, 8, 9, 1);
  } else {
    sprStatus.drawString("Jamais mis a jour", 8, 9, 1);
  }

  sprStatus.setTextColor(lastStatusColor, COL_CARD);
  sprStatus.setTextDatum(MR_DATUM);
  sprStatus.drawString(lastStatusMsg, SCREEN_W - 8, 9, 1);

  sprStatus.pushSprite(0, SCREEN_H - 18);
}

// Dispatch vers la fonction de redessin complete selon la vue courante.
void drawMain() {
  if (currentView == VIEW_GAUGES) drawGauges();
  else if (currentView == VIEW_CLOCK) drawClock();
  else if (currentView == VIEW_SETTINGS) drawSettings();
}

void drawGauges() {
  currentView = VIEW_GAUGES;
  checkThresholdFlashes();

  tft.fillScreen(COL_BG);
  drawHeader();

  if (!usage.valid) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Recuperation des donnees...", SCREEN_W / 2, SCREEN_H / 2 - 10, 2);
    if (usage.lastError.length()) {
      tft.setTextColor(COL_DANGER, COL_BG);
      tft.drawString(usage.lastError, SCREEN_W / 2, SCREEN_H / 2 + 12, 1);
    }
    drawStatusBar();
    return;
  }

  updateGaugesAnim();

  if (usage.extraEnabled && usage.extraUsedEur > 0) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    char buf[64];
    snprintf(buf, sizeof(buf), "Extra usage: %.2f EUR", usage.extraUsedEur);
    tft.drawString(buf, SCREEN_W / 2, SCREEN_H - 30, 1);
  }
}

// Retourne true si la phase "visible" du clignotement est active maintenant.
bool flashPhaseOn(unsigned long startMs) {
  unsigned long elapsed = millis() - startMs;
  unsigned long period = FLASH_DURATION_MS / (FLASH_BLINKS * 2);
  if (period == 0) return false;
  return (elapsed / period) % 2 == 0;
}

// Refresh only the two gauges + status bar (called every animation frame)
void updateGaugesAnim() {
  if (currentView != VIEW_GAUGES || !usage.valid) return;

  const int gaugeTop = 46;
  const int leftPx = (SCREEN_W / 2 - 140) / 2;
  const int rightPx = SCREEN_W / 2 + (SCREEN_W / 2 - 140) / 2;

  bool flashL = flash5hActive && flashPhaseOn(flash5hStartMs);
  bool flashR = flash7dActive && flashPhaseOn(flash7dStartMs);

  drawGaugeInto(sprGaugeL, leftPx, gaugeTop,
                animFiveHour,
                "SESSION 5H",
                formatResetCountdown(usage.fiveHourResetIso, false).c_str(),
                pctColor(usage.fiveHourPct, thresholdSession5hWarn, thresholdSession5hAlert),
                flashL);

  drawGaugeInto(sprGaugeR, rightPx, gaugeTop,
                animSevenDay,
                "SEMAINE 7J",
                formatResetCountdown(usage.sevenDayResetIso, true).c_str(),
                pctColor(usage.sevenDayPct, thresholdWeekly7dWarn, thresholdWeekly7dAlert),
                flashR);

  drawStatusBar();
}

void clockGetStrings(String& timeOut, String& dateOut) {
  timeOut = "--:--";
  dateOut = "";
  if (!timeReady) return;
  time_t now;
  struct tm tmLocal;
  time(&now);
  localtime_r(&now, &tmLocal);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmLocal.tm_hour, tmLocal.tm_min);
  timeOut = buf;
  static const char* daysFr[] = {"Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam"};
  static const char* monthsFr[] = {"jan", "fev", "mar", "avr", "mai", "jun",
                                   "jul", "aou", "sep", "oct", "nov", "dec"};
  snprintf(buf, sizeof(buf), "%s %d %s",
           daysFr[tmLocal.tm_wday], tmLocal.tm_mday, monthsFr[tmLocal.tm_mon]);
  dateOut = buf;
}

// Positions verticales du layout de la vue horloge.
#define CLOCK_TIME_TOP    50
#define CLOCK_TIME_H      56
#define CLOCK_DATE_Y      120
#define CLOCK_BAR_Y       160
#define CLOCK_BAR_GAP     24

// Pousse le sprite "heure" : appele a chaque changement de minute.
void pushTimeSprite(const String& timeStr) {
  sprTime.fillSprite(COL_BG);
  sprTime.setTextColor(timeReady ? COL_TEXT : COL_DIM, COL_BG);
  sprTime.setTextDatum(MC_DATUM);
  sprTime.drawString(timeStr, 110, 28, 7);
  sprTime.pushSprite((SCREEN_W - 220) / 2, CLOCK_TIME_TOP);
}

// Dessine une barre 5H/7J. Le pourcentage est aligne a droite a une position
// fixe et la barre s'arrete plus tot pour laisser un espace visuel clair.
void drawClockBar(int y, const char* label, float pct, uint16_t color, bool flash) {
  const int barH = 14;
  const int barX = 78;
  const int pctRightX = SCREEN_W - 8;   // bord droit du texte "%"
  const int pctWidth  = 50;             // largeur reservee a "100%"
  const int barRight  = pctRightX - pctWidth;
  const int barW = barRight - barX;

  tft.fillRect(0, y - 2, SCREEN_W, barH + 8, COL_BG);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(label, 10, y + barH / 2, 2);

  tft.fillRoundRect(barX, y, barW, barH, 4, COL_CARD);
  int fillW = (int)((pct / 100.0f) * (barW - 4));
  if (fillW > 0) {
    uint16_t c = flash ? COL_TEXT : color;
    tft.fillRoundRect(barX + 2, y + 2, fillW, barH - 4, 3, c);
  }
  char pctBuf[10];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)round(pct));
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(pctBuf, pctRightX, y + barH / 2, 2);
}

// Redessin complet : appele quand on entre dans la vue (swipe, boot, etc).
void drawClock() {
  currentView = VIEW_CLOCK;
  checkThresholdFlashes();

  tft.fillScreen(COL_BG);
  drawHeader();

  String timeStr, dateStr;
  clockGetStrings(timeStr, dateStr);
  pushTimeSprite(timeStr);
  lastTimeStr = timeStr;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(dateStr, SCREEN_W / 2, CLOCK_DATE_Y, 2);
  lastDateStr = dateStr;

  if (usage.valid) {
    bool flashL = flash5hActive && flashPhaseOn(flash5hStartMs);
    bool flashR = flash7dActive && flashPhaseOn(flash7dStartMs);
    drawClockBar(CLOCK_BAR_Y, "5H", animFiveHour,
                 pctColor(usage.fiveHourPct, thresholdSession5hWarn, thresholdSession5hAlert),
                 flashL);
    drawClockBar(CLOCK_BAR_Y + CLOCK_BAR_GAP, "7J", animSevenDay,
                 pctColor(usage.sevenDayPct, thresholdWeekly7dWarn, thresholdWeekly7dAlert),
                 flashR);
    lastBar5h = animFiveHour;
    lastBar7d = animSevenDay;
    lastFlashL = flashL;
    lastFlashR = flashR;
  } else {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Donnees indisponibles", SCREEN_W / 2, CLOCK_BAR_Y + 8, 1);
  }

  drawStatusBar();
}

// Tick leger : appele frequemment, ne redessine que les zones qui ont change.
// Heure : a chaque changement de minute. Date : 1 fois par jour. Barres :
// uniquement quand la valeur bouge (anim) ou que l'etat de flash change.
void updateClock() {
  if (currentView != VIEW_CLOCK) return;

  String timeStr, dateStr;
  clockGetStrings(timeStr, dateStr);
  if (timeStr != lastTimeStr) {
    pushTimeSprite(timeStr);
    lastTimeStr = timeStr;
  }
  if (dateStr != lastDateStr) {
    tft.fillRect(0, CLOCK_DATE_Y - 10, SCREEN_W, 22, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(dateStr, SCREEN_W / 2, CLOCK_DATE_Y, 2);
    lastDateStr = dateStr;
  }

  if (usage.valid) {
    bool flashL = flash5hActive && flashPhaseOn(flash5hStartMs);
    bool flashR = flash7dActive && flashPhaseOn(flash7dStartMs);
    if (fabs(animFiveHour - lastBar5h) > 0.5 || flashL != lastFlashL) {
      drawClockBar(CLOCK_BAR_Y, "5H", animFiveHour,
                   pctColor(usage.fiveHourPct, thresholdSession5hWarn, thresholdSession5hAlert),
                   flashL);
      lastBar5h = animFiveHour;
      lastFlashL = flashL;
    }
    if (fabs(animSevenDay - lastBar7d) > 0.5 || flashR != lastFlashR) {
      drawClockBar(CLOCK_BAR_Y + CLOCK_BAR_GAP, "7J", animSevenDay,
                   pctColor(usage.sevenDayPct, thresholdWeekly7dWarn, thresholdWeekly7dAlert),
                   flashR);
      lastBar7d = animSevenDay;
      lastFlashR = flashR;
    }
  }
}

// =============================================================
//  THRESHOLD FLASH DETECTION
// =============================================================
// Declenche le flash uniquement sur un franchissement a la hausse (et pas
// quand la valeur retombe a 0 apres un reset).
void checkThresholdFlashes() {
  if (!usage.valid) return;
  bool now5W = usage.fiveHourPct >= thresholdSession5hWarn;
  bool now5A = usage.fiveHourPct >= thresholdSession5hAlert;
  bool now7W = usage.sevenDayPct >= thresholdWeekly7dWarn;
  bool now7A = usage.sevenDayPct >= thresholdWeekly7dAlert;

  bool crossed5 = (now5W && !prev5hOverWarn) || (now5A && !prev5hOverAlert);
  bool crossed7 = (now7W && !prev7dOverWarn) || (now7A && !prev7dOverAlert);

  if (crossed5) {
    flash5hActive = true;
    flash5hStartMs = millis();
  }
  if (crossed7) {
    flash7dActive = true;
    flash7dStartMs = millis();
  }

  // Wake on alert : si un seuil vient d'etre franchi a la hausse alors qu'on
  // est en veille, on rallume l'ecran pour ALERT_WAKE_MS puis on retombera
  // en dim via checkSleepTimeout().
  if ((crossed5 || crossed7) && isDimmed) {
    Serial.println("[ALERT] threshold crossed during sleep, waking up");
    wakeFromDim();
    alertWakeUntilMs = millis() + ALERT_WAKE_MS;
  }

  prev5hOverWarn = now5W;
  prev5hOverAlert = now5A;
  prev7dOverWarn = now7W;
  prev7dOverAlert = now7A;
}

// Position du bouton "RAFRAICHIR MAINTENANT" : doit coller avec handleTouch().
#define SETTINGS_REFRESH_BTN_Y 178
#define SETTINGS_REFRESH_BTN_H 36

void drawSettings() {
  currentView = VIEW_SETTINGS;

  tft.fillScreen(COL_BG);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Reglages", 12, 20, 4);
  tft.drawFastHLine(8, 41, SCREEN_W - 16, COL_CARD);

  tft.fillRoundRect(SCREEN_W - 78, 6, 70, 26, 6, COL_CARD);
  tft.setTextColor(COL_TEXT, COL_CARD);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("RETOUR", SCREEN_W - 43, 19, 2);

  tft.setTextDatum(TL_DATUM);
  int y = 54;

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("WiFi:", 16, y, 2);
  tft.setTextColor(wifiOk ? COL_OK : COL_DANGER, COL_BG);
  tft.drawString(wifiOk ? WIFI_SSID : "deconnecte", 90, y, 2);
  y += 20;

  if (wifiOk) {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("IP:", 16, y, 2);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(WiFi.localIP().toString(), 90, y, 2);
    y += 20;
  }

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Org UUID:", 16, y, 2);
  tft.setTextColor(COL_DIM, COL_BG);
  String shortUuid = orgUuid.length() > 8 ? orgUuid.substring(0, 8) + "..." : orgUuid;
  tft.drawString(shortUuid, 110, y, 2);
  y += 20;

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Refresh:", 16, y, 2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(String(refreshIntervalMs / 1000 / 60) + " min", 110, y, 2);
  y += 20;

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Uptime:", 16, y, 2);
  tft.setTextColor(COL_DIM, COL_BG);
  unsigned long up = (millis() - bootTime) / 1000;
  char ubuf[32];
  snprintf(ubuf, sizeof(ubuf), "%luh %lum %lus", up / 3600, (up / 60) % 60, up % 60);
  tft.drawString(ubuf, 110, y, 2);

  tft.fillRoundRect(20, SETTINGS_REFRESH_BTN_Y, SCREEN_W - 40,
                    SETTINGS_REFRESH_BTN_H, 8, COL_CLAUDE_D);
  tft.setTextColor(COL_TEXT, COL_CLAUDE_D);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("RAFRAICHIR MAINTENANT", SCREEN_W / 2,
                 SETTINGS_REFRESH_BTN_Y + SETTINGS_REFRESH_BTN_H / 2, 2);
}

void drawError(const String& msg) {
  currentView = VIEW_ERROR;
  tft.fillScreen(COL_BG);

  tft.drawBitmap((SCREEN_W - CLAUDE_LOGO_W) / 2, 24, claude_logo,
                 CLAUDE_LOGO_W, CLAUDE_LOGO_H, COL_CLAUDE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Configuration requise", SCREEN_W / 2, 110, 4);

  tft.setTextColor(COL_CLAUDE, COL_BG);
  tft.drawString(msg, SCREEN_W / 2, 150, 2);

  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Renseigner la sessionKey sur le portail web", SCREEN_W / 2, 200, 1);
  tft.drawString("Puis 'Reboot' depuis la page", SCREEN_W / 2, 215, 1);
}

void touchInit() {
  Wire1.begin(TOUCH_SDA, TOUCH_SCL);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(50);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);
  Serial.println("[TOUCH] CST820 init OK");
}

bool touchRead(int* x, int* y) {
  Wire1.beginTransmission(TOUCH_ADDR);
  Wire1.write(0x02);
  if (Wire1.endTransmission() != 0) return false;

  Wire1.requestFrom(TOUCH_ADDR, 5);
  if (Wire1.available() < 5) return false;

  uint8_t numPoints = Wire1.read();
  uint8_t xHigh     = Wire1.read();
  uint8_t xLow      = Wire1.read();
  uint8_t yHigh     = Wire1.read();
  uint8_t yLow      = Wire1.read();

  if (numPoints == 0 || numPoints > 5) return false;

  int rawX = ((xHigh & 0x0F) << 8) | xLow;
  int rawY = ((yHigh & 0x0F) << 8) | yLow;

  // Mapping pour rotation 1 (paysage).
  *x = rawY;
  *y = 239 - rawX;

  if (*x < 0) *x = 0;
  if (*x >= SCREEN_W) *x = SCREEN_W - 1;
  if (*y < 0) *y = 0;
  if (*y >= SCREEN_H) *y = SCREEN_H - 1;
  return true;
}

bool inRect(int tx, int ty, int rx, int ry, int rw, int rh) {
  return (tx >= rx && tx <= rx + rw && ty >= ry && ty <= ry + rh);
}

void handleTouch(int x, int y) {
  Serial.printf("[TAP] x=%d y=%d view=%d\n", x, y, currentView);

  if (currentView == VIEW_SETTINGS) {
    if (inRect(x, y, SCREEN_W - 78, 6, 70, 26)) {
      currentView = (defaultBootView == "gauges") ? VIEW_GAUGES : VIEW_CLOCK;
      drawMain();
      return;
    }
    if (inRect(x, y, 20, SETTINGS_REFRESH_BTN_Y, SCREEN_W - 40, SETTINGS_REFRESH_BTN_H)) {
      setStatus("Rafraichissement...", COL_CLAUDE);
      drawSettings();
      fetchUsage();
      drawSettings();
      return;
    }
    return;
  }

  if (currentView == VIEW_GAUGES || currentView == VIEW_CLOCK) {
    // Tap n'importe ou = refresh manuel, ignore le backoff.
    setStatus("Rafraichissement...", COL_CLAUDE);
    consecutiveFetchFailures = 0;
    lastFetchAttemptMs = millis();
    fetchUsage();  // gere lui-meme le repaint cible
  }
}

void handleLongPress(int x, int y) {
  Serial.printf("[LONG] x=%d y=%d\n", x, y);
  if (currentView == VIEW_GAUGES || currentView == VIEW_CLOCK) {
    drawSettings();
  } else if (currentView == VIEW_SETTINGS) {
    currentView = (defaultBootView == "gauges") ? VIEW_GAUGES : VIEW_CLOCK;
    drawMain();
  }
}

void handleSwipe(int dx) {
  Serial.printf("[SWIPE] dx=%d view=%d\n", dx, currentView);
  if (currentView == VIEW_SETTINGS) return;
  currentView = (currentView == VIEW_CLOCK) ? VIEW_GAUGES : VIEW_CLOCK;
  drawMain();
}

void loadPrefs() {
  prefs.begin("claude", true);
  sessionKey              = prefs.getString("sk",      "");
  orgUuid                 = prefs.getString("org",     "");
  claudeApiBase           = prefs.getString("apibase", CLAUDE_API_BASE_DEFAULT);
  refreshIntervalMs       = (unsigned long)prefs.getUInt("refresh", 5 * 60 * 1000);
  thresholdSession5hWarn  = prefs.getInt("th5w",       70);
  thresholdSession5hAlert = prefs.getInt("th5a",       90);
  thresholdWeekly7dWarn   = prefs.getInt("th7w",       70);
  thresholdWeekly7dAlert  = prefs.getInt("th7a",       90);
  defaultBootView         = prefs.getString("boot",    "clock");
  tzName                  = prefs.getString("tz",      "CET-1CEST,M3.5.0,M10.5.0/3");
  sleepEnabled            = prefs.getBool("slpEn",     false);
  sleepDelayMin           = prefs.getInt("slpDel",     5);
  sleepDimPercent         = prefs.getInt("slpDim",     10);
  brightPercent           = prefs.getInt("bright",     100);
  prefs.end();
  Serial.printf("[PREFS] sk=%d chars, org=%s, base=%s, refresh=%lums, boot=%s\n",
                sessionKey.length(),
                orgUuid.length() ? orgUuid.c_str() : "(auto)",
                claudeApiBase.length() ? claudeApiBase.c_str() : "(direct)",
                refreshIntervalMs, defaultBootView.c_str());
}

void savePrefs() {
  prefs.begin("claude", false);
  prefs.putString("sk",      sessionKey);
  prefs.putString("org",     orgUuid);
  prefs.putString("apibase", claudeApiBase);
  prefs.putUInt("refresh",   (uint32_t)refreshIntervalMs);
  prefs.putInt("th5w",       thresholdSession5hWarn);
  prefs.putInt("th5a",       thresholdSession5hAlert);
  prefs.putInt("th7w",       thresholdWeekly7dWarn);
  prefs.putInt("th7a",       thresholdWeekly7dAlert);
  prefs.putString("boot",    defaultBootView);
  prefs.putString("tz",      tzName);
  prefs.putBool("slpEn",     sleepEnabled);
  prefs.putInt("slpDel",     sleepDelayMin);
  prefs.putInt("slpDim",     sleepDimPercent);
  prefs.putInt("bright",     brightPercent);
  prefs.end();
  Serial.println("[PREFS] saved");
}

void initNTP() {
  Serial.printf("[NTP] Starting with TZ=%s\n", tzName.c_str());
  // Set TZ avant configTzTime() pour que localtime_r() retourne l'heure locale.
  setenv("TZ", tzName.c_str(), 1);
  tzset();
  configTzTime(tzName.c_str(), NTP_SERVER_1, NTP_SERVER_2);
  // Synchro non-bloquante : le watchdog NTP de la loop() s'occupera de
  // detecter quand la 1re sync est dispo (gain ~3-5s sur le boot).
}

// Reboot differe : laisse le temps au HTTP response du portail d'etre envoyee.
void rebootSoon() { pendingRebootAt = millis() + 1500; }

// Ecrit la luminosite (0-255) sur le backlight PWM.
void setBacklight(uint8_t level) {
  ledcWrite(BACKLIGHT_PIN, level);
}

// Renvoie la valeur PWM correspondant a la luminosite "allume" configuree.
uint8_t brightPwm() {
  return (uint8_t)map(brightPercent, 0, 100, 0, 255);
}

// Renvoie la valeur PWM correspondant a la luminosite "veille" configuree.
uint8_t dimPwm() {
  return (uint8_t)map(sleepDimPercent, 0, 100, 0, 255);
}

// Fait un fade progressif de la luminosite actuelle vers une cible.
void fadeBacklight(uint8_t from, uint8_t to, int durationMs) {
  if (from == to) { setBacklight(to); return; }
  int steps = 20;
  int delayPerStep = durationMs / steps;
  if (delayPerStep < 1) delayPerStep = 1;
  for (int i = 1; i <= steps; i++) {
    int v = from + ((int)(to - from) * i) / steps;
    setBacklight((uint8_t)v);
    delay(delayPerStep);
  }
}

// Reveille l'ecran si on etait en mode dim. Doit etre appele a chaque
// interaction (touch, web portal action, etc.).
void wakeFromDim() {
  lastActivityMs = millis();
  // Toute interaction utilisateur annule la fenetre de wake forcee
  // (alerte de seuil) : on repart sur le delai standard.
  alertWakeUntilMs = 0;
  if (isDimmed) {
    isDimmed = false;
    fadeBacklight(dimPwm(), brightPwm(), 200);
  }
}

// Verifie si on doit entrer en veille. Appele depuis la loop.
void checkSleepTimeout() {
  if (!sleepEnabled || isDimmed) return;
  unsigned long now = millis();
  // Si on est en wake forcee suite a une alerte, on attend la fin du delai.
  if (alertWakeUntilMs > 0 && now < alertWakeUntilMs) return;
  unsigned long elapsedMs = now - lastActivityMs;
  if (elapsedMs > (unsigned long)sleepDelayMin * 60UL * 1000UL) {
    isDimmed = true;
    alertWakeUntilMs = 0;
    Serial.printf("[SLEEP] dim to %d%%\n", sleepDimPercent);
    fadeBacklight(brightPwm(), dimPwm(), 600);
  }
}


// Returns true if the request is authorized (or if auth is disabled).
// If unauthorized, sends a 401 response and returns false — caller must `return`.
bool webAuthOk() {
  if (strlen(WEB_PASSWORD) == 0) return true;  // auth disabled
  if (!webServer.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
    webServer.requestAuthentication(BASIC_AUTH, "Claude Monitor",
                                    "Authentification requise");
    return false;
  }
  return true;
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (char c : s) {
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void handleRoot() {
  if (!webAuthOk()) return;
  // Refus si la heap est trop basse : un fetch en cours + un client web qui
  // demande la page (10 Ko de string) peut crasher.
  if (ESP.getFreeHeap() < 30000) {
    webServer.send(503, "text/plain", "Busy, retry in a moment");
    return;
  }
  String html;
  html.reserve(8000);
  html += F("<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Claude Monitor</title>"
           "<style>"
           "*{box-sizing:border-box;font-family:-apple-system,system-ui,sans-serif}"
           "body{background:#18141B;color:#FAF8F5;margin:0;padding:20px;max-width:560px;margin:auto}"
           "h1{color:#D97757;font-weight:600;margin:0 0 8px}"
           "h2{color:#D97757;font-size:14px;text-transform:uppercase;letter-spacing:1px;margin:32px 0 8px;border-bottom:1px solid #2a2329;padding-bottom:6px}"
           ".sub{color:#8C7A78;font-size:13px;margin-bottom:24px}"
           ".card{background:#221C20;border-radius:10px;padding:16px;margin:8px 0}"
           ".stat{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #2a2329}"
           ".stat:last-child{border:0}"
           ".stat .k{color:#8C7A78}"
           "label{display:block;margin:14px 0 4px;font-size:13px;color:#D97757}"
           "input,select{width:100%;padding:10px 12px;background:#15101A;color:#FAF8F5;border:1px solid #2a2329;border-radius:6px;font-size:14px}"
           "input:focus,select:focus{outline:none;border-color:#D97757}"
           "button{background:#D97757;color:#18141B;border:0;padding:12px 20px;border-radius:8px;font-weight:600;font-size:14px;cursor:pointer;margin:6px 6px 6px 0}"
           "button.sec{background:#2a2329;color:#FAF8F5}"
           "button.danger{background:#A23B1F;color:#FAF8F5}"
           "button:hover{opacity:.85}"
           ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
           "</style></head><body>");

  html += F("<h1>Claude Monitor</h1>");
  html += "<div class='sub'>" + WiFi.localIP().toString() + " &middot; uptime " +
          String((millis() - bootTime) / 1000) + "s</div>";

  html += F("<h2>Statut</h2><div class='card'>");
  html += "<div class='stat'><span class='k'>WiFi</span><span>" +
          String(wifiOk ? WIFI_SSID : "deconnecte") + " (" + String(WiFi.RSSI()) + " dBm)</span></div>";
  html += "<div class='stat'><span class='k'>NTP</span><span>" +
          String(timeReady ? "synchronise" : "en attente") + "</span></div>";
  html += "<div class='stat'><span class='k'>Org UUID</span><span>" + htmlEscape(orgUuid) + "</span></div>";
  html += "<div class='stat'><span class='k'>API base</span><span>" +
          String(claudeApiBase.length() ? htmlEscape(claudeApiBase) : "claude.ai (direct)") + "</span></div>";
  if (usage.valid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f %% (reset %s)",
             usage.fiveHourPct, usage.fiveHourResetIso.c_str());
    html += "<div class='stat'><span class='k'>Session 5h</span><span>" + String(buf) + "</span></div>";
    snprintf(buf, sizeof(buf), "%.1f %% (reset %s)",
             usage.sevenDayPct, usage.sevenDayResetIso.c_str());
    html += "<div class='stat'><span class='k'>Semaine 7j</span><span>" + String(buf) + "</span></div>";
    snprintf(buf, sizeof(buf), "%.2f EUR", usage.extraUsedEur);
    html += "<div class='stat'><span class='k'>Extra usage</span><span>" + String(buf) + "</span></div>";
  } else {
    html += "<div class='stat'><span class='k'>Usage</span><span>" + htmlEscape(usage.lastError) + "</span></div>";
  }
  html += F("</div>");

  html += F("<form method='POST' action='/save'>"
           "<h2>Reglages</h2><div class='card'>");

  html += F("<label>SessionKey (cookie sk-ant-... depuis claude.ai)</label>");
  html += "<input type='text' name='sk' placeholder='sk-ant-sid02-...' value='" +
          htmlEscape(sessionKey) + "'>";

  html += F("<label>Organisation UUID <span style='color:#8C7A78'>(laisser vide = auto-detect)</span></label>");
  html += "<input type='text' name='org' placeholder='auto' value='" +
          htmlEscape(orgUuid) + "'>";

  html += F("<label>URL du proxy <span style='color:#8C7A78'>(ex: https://xxx.vercel.app, laisser vide = direct claude.ai)</span></label>");
  html += "<input type='text' name='apibase' placeholder='https://xxx.vercel.app' value='" +
          htmlEscape(claudeApiBase) + "'>";

  html += F("<label>Intervalle de refresh (minutes)</label>");
  html += "<input type='number' name='refresh' min='1' max='60' value='" +
          String(refreshIntervalMs / 60000) + "'>";

  html += F("<label>Vue par defaut au boot</label>"
           "<select name='boot'>");
  html += "<option value='clock'"  + String(defaultBootView == "clock"  ? " selected" : "") + ">Horloge (grande heure + barres)</option>";
  html += "<option value='gauges'" + String(defaultBootView == "gauges" ? " selected" : "") + ">Jauges (cercles 5h / 7j)</option>";
  html += F("</select>");

  // Fuseau horaire : dropdown de presets + champ libre POSIX TZ pour les autres.
  struct TzPreset { const char* label; const char* tz; };
  static const TzPreset presets[] = {
    {"Europe/Paris, Berlin, Madrid (CET/CEST)",  "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London, Lisbon (GMT/BST)",          "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Moscow (no DST)",                   "MSK-3"},
    {"America/New_York (EST/EDT)",               "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago (CST/CDT)",                "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver (MST/MDT)",                 "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles (PST/PDT)",            "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Sao_Paulo (no DST)",               "BRT3"},
    {"Asia/Tokyo (JST)",                         "JST-9"},
    {"Asia/Shanghai, Singapore (CST)",           "CST-8"},
    {"Asia/Kolkata (IST)",                       "IST-5:30"},
    {"Asia/Dubai (GST)",                         "GST-4"},
    {"Australia/Sydney (AEST/AEDT)",             "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland (NZST/NZDT)",             "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"UTC",                                      "UTC0"},
  };
  html += F("<label>Fuseau horaire</label>"
           "<select name='tz_preset' onchange=\""
           "var v=this.value; if(v!=='custom') document.getElementsByName('tz')[0].value=v;"
           "document.getElementsByName('tz')[0].style.display=(v==='custom')?'block':'none';"
           "\">");
  bool matched = false;
  for (auto& p : presets) {
    bool sel = (tzName == p.tz);
    if (sel) matched = true;
    html += "<option value='" + String(p.tz) + "'" + (sel ? " selected" : "") +
            ">" + String(p.label) + "</option>";
  }
  html += "<option value='custom'" + String(matched ? "" : " selected") + ">Personnalise...</option>";
  html += F("</select>");

  html += F("<label style='font-size:11px;color:#8C7A78'>TZ POSIX brut (avance)</label>");
  html += "<input type='text' name='tz' value='" + htmlEscape(tzName) + "'";
  if (matched) html += " style='display:none'";
  html += ">";

  html += F("<h2>Luminosite</h2><div class='card'>");
  html += F("<label>Luminosite ecran allume (%)</label>");
  html += "<input type='number' name='bright' min='5' max='100' value='" +
          String(brightPercent) + "'>";
  html += F("</div>");

  html += F("<h2>Mode veille</h2><div class='card'>");
  html += F("<label><input type='checkbox' name='slpEn' value='1' style='width:auto;margin-right:8px'");
  if (sleepEnabled) html += F(" checked");
  html += F(">Activer le mode veille auto</label>");
  html += F("<label>Delai avant veille (minutes)</label>");
  html += "<input type='number' name='slpDel' min='1' max='60' value='" +
          String(sleepDelayMin) + "'>";
  html += F("<label>Luminosite en veille (%)</label>");
  html += "<input type='number' name='slpDim' min='0' max='100' value='" +
          String(sleepDimPercent) + "'>";
  html += F("</div>");

  html += F("<h2>Seuils d'alerte</h2>"
           "<div class='grid2'>"
           "<div><label>Session 5h orange (%)</label>");
  html += "<input type='number' name='th5w' min='0' max='100' value='" + String(thresholdSession5hWarn) + "'></div>";
  html += F("<div><label>Session 5h rouge (%)</label>");
  html += "<input type='number' name='th5a' min='0' max='100' value='" + String(thresholdSession5hAlert) + "'></div>";
  html += F("<div><label>Semaine 7j orange (%)</label>");
  html += "<input type='number' name='th7w' min='0' max='100' value='" + String(thresholdWeekly7dWarn) + "'></div>";
  html += F("<div><label>Semaine 7j rouge (%)</label>");
  html += "<input type='number' name='th7a' min='0' max='100' value='" + String(thresholdWeekly7dAlert) + "'></div>";
  html += F("</div></div>");

  html += F("<button type='submit'>Enregistrer</button>"
           "<button type='button' class='sec' onclick='fetch(\"/refresh\",{method:\"POST\"}).then(()=>location.reload())'>Forcer un fetch</button>"
           "<button type='button' class='danger' onclick='if(confirm(\"Reboot ?\"))fetch(\"/reboot\",{method:\"POST\"})'>Reboot</button>"
           "</form>");

  html += F("<h2>Donnees brutes</h2><div class='card'>"
           "<a style='color:#D97757' href='/usage.json'>/usage.json</a>"
           "</div></body></html>");

  webServer.send(200, "text/html", html);
}

void handleSave() {
  if (!webAuthOk()) return;
  if (webServer.hasArg("sk")) {
    String k = webServer.arg("sk");
    k.trim();
    // Vide = reset. Sinon on exige une longueur minimale pour eviter les typos.
    if (k.length() == 0 || k.length() > 10) sessionKey = k;
  }
  if (webServer.hasArg("org")) {
    String o = webServer.arg("org");
    o.trim();
    orgUuid = o;  // vide = redeclenche l'auto-discovery au prochain fetch
  }
  if (webServer.hasArg("apibase")) {
    String b = webServer.arg("apibase");
    b.trim();
    // Strip du slash final pour ne pas avoir de "//" dans les URLs.
    while (b.endsWith("/")) b.remove(b.length() - 1);
    claudeApiBase = b;
  }
  if (webServer.hasArg("refresh")) {
    int m = webServer.arg("refresh").toInt();
    if (m >= 1 && m <= 60) refreshIntervalMs = (unsigned long)m * 60UL * 1000UL;
  }
  if (webServer.hasArg("boot")) {
    String b = webServer.arg("boot");
    if (b == "clock" || b == "gauges") defaultBootView = b;
  }
  // Le preset prend la priorite sauf si "custom" est selectionne (auquel cas
  // on prend la valeur du champ libre tz).
  String chosenTz;
  if (webServer.hasArg("tz_preset")) {
    String p = webServer.arg("tz_preset");
    if (p != "custom" && p.length() > 0) chosenTz = p;
  }
  if (chosenTz.length() == 0 && webServer.hasArg("tz")) {
    chosenTz = webServer.arg("tz");
  }
  if (chosenTz.length() > 0) {
    tzName = chosenTz;
    setenv("TZ", tzName.c_str(), 1);
    tzset();
  }
  auto getPct = [&](const char* k, int& out) {
    if (webServer.hasArg(k)) {
      int v = webServer.arg(k).toInt();
      if (v >= 0 && v <= 100) out = v;
    }
  };
  getPct("th5w", thresholdSession5hWarn);
  getPct("th5a", thresholdSession5hAlert);
  getPct("th7w", thresholdWeekly7dWarn);
  getPct("th7a", thresholdWeekly7dAlert);

  // Luminosite ecran allume : 5-100% (en dessous de 5% l'ecran est quasi noir).
  if (webServer.hasArg("bright")) {
    int v = webServer.arg("bright").toInt();
    if (v >= 5 && v <= 100) brightPercent = v;
  }

  // Mode veille : la checkbox HTML n'envoie l'arg que si elle est cochee.
  sleepEnabled = webServer.hasArg("slpEn");
  if (webServer.hasArg("slpDel")) {
    int v = webServer.arg("slpDel").toInt();
    if (v >= 1 && v <= 60) sleepDelayMin = v;
  }
  getPct("slpDim", sleepDimPercent);
  // Si on vient de modifier les reglages : reset timer + sortir du dim +
  // appliquer immediatement la nouvelle luminosite "allume".
  wakeFromDim();
  setBacklight(brightPwm());

  savePrefs();
  // Reset du backoff : la prochaine iteration retente immediatement avec
  // les nouveaux reglages.
  consecutiveFetchFailures = 0;
  lastFetchAttemptMs = 0;
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "Saved");
}

void handleUsageJson() {
  if (!webAuthOk()) return;
  JsonDocument doc;
  doc["valid"] = usage.valid;
  doc["five_hour"]["pct"]      = usage.fiveHourPct;
  doc["five_hour"]["resets_at"] = usage.fiveHourResetIso;
  doc["seven_day"]["pct"]      = usage.sevenDayPct;
  doc["seven_day"]["resets_at"] = usage.sevenDayResetIso;
  doc["extra_eur"]             = usage.extraUsedEur;
  doc["last_fetch_ago_ms"]     = (long)(millis() - usage.lastFetchMs);
  doc["last_error"]            = usage.lastError;
  doc["uptime_ms"]             = (long)(millis() - bootTime);
  String out;
  serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

void handleRefresh() {
  if (!webAuthOk()) return;
  // Refresh manuel : ignore le backoff.
  consecutiveFetchFailures = 0;
  lastFetchAttemptMs = millis();
  if (sessionKey.length() > 0 && orgUuid.length() == 0) {
    if (fetchOrgUuid()) savePrefs();
  }
  bool ok = false;
  if (sessionKey.length() > 0 && orgUuid.length() > 0) ok = fetchUsage();

  // Si on etait coince sur l'ecran "configuration requise" et qu'on a maintenant
  // des donnees, on bascule sur la vue par defaut.
  if (ok && currentView == VIEW_ERROR) {
    currentView = (defaultBootView == "gauges") ? VIEW_GAUGES : VIEW_CLOCK;
    drawMain();
  }
  // Pour VIEW_GAUGES / VIEW_CLOCK, fetchUsage() a deja fait le repaint cible.
  webServer.send(200, "text/plain", ok ? "OK" : "FAILED");
}

void handleReboot() {
  if (!webAuthOk()) return;
  webServer.send(200, "text/plain", "Rebooting...");
  rebootSoon();
}

void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/usage.json", handleUsageJson);
  webServer.on("/refresh", HTTP_POST, handleRefresh);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  Serial.printf("[WEB] Listening on http://%s/\n", WiFi.localIP().toString().c_str());
}

void setupOTA() {
  ArduinoOTA.setHostname("claude-monitor");
  // Decommenter pour proteger l'OTA par mot de passe.
  // ArduinoOTA.setPassword("claude");

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start");
    otaActive = true;
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_CLAUDE, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("OTA Update", SCREEN_W / 2, SCREEN_H / 2 - 20, 4);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("0 %", SCREEN_W / 2, SCREEN_H / 2 + 20, 4);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End");
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_OK, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("OTA OK", SCREEN_W / 2, SCREEN_H / 2, 4);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = (progress * 100) / total;
    Serial.printf("[OTA] %u%%\n", pct);
    tft.fillRect(SCREEN_W / 2 - 60, SCREEN_H / 2 + 10, 120, 24, COL_BG);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(String(pct) + " %", SCREEN_W / 2, SCREEN_H / 2 + 20, 4);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[OTA] Error %u\n", err);
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_DANGER, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("OTA Error " + String(err), SCREEN_W / 2, SCREEN_H / 2, 2);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready (hostname=claude-monitor)");
}

