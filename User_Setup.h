// =============================================================
//  User_Setup.h for Guition JC2432W328
//  (ESP32-WROOM-32 + ST7789 240x320 + CST820 capacitive touch)
// =============================================================
//  This file must REPLACE the default User_Setup.h
//  in your TFT_eSPI library folder.
//
//  Location:
//    [Arduino Sketchbook]/libraries/TFT_eSPI/User_Setup.h
// =============================================================

#define USER_SETUP_LOADED

// ─── DISPLAY DRIVER ─────────────────────────────────────────
#define ST7789_DRIVER

// ─── DISPLAY DIMENSIONS ─────────────────────────────────────
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ─── COLOR ORDER ────────────────────────────────────────────
#define TFT_RGB_ORDER TFT_BGR

// ─── INVERSION ──────────────────────────────────────────────
#define TFT_INVERSION_OFF

// ─── SPI PINS (HSPI) ───────────────────────────────────────
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1  // Connected to EN/RST
#define TFT_BL    27  // Backlight control
#define TFT_BACKLIGHT_ON HIGH

// ─── SPI FREQUENCY ──────────────────────────────────────────
// 80 MHz : max supporté par le ST7789 du Guition, ~30% de gain FPS sur les
// pushSprite vs 55 MHz. Si tu as des glitchs d'affichage, repasser à 55 MHz.
#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ─── USE HSPI BUS ───────────────────────────────────────────
#define USE_HSPI_PORT

// ─── FONTS ──────────────────────────────────────────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ─── MISC ───────────────────────────────────────────────────
#define SUPPORT_TRANSACTIONS
