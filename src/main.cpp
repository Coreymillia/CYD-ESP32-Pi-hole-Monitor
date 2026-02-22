// CYDPiHole - Pi-hole DNS Query Monitor for CYD (Cheap Yellow Display)
// Displays the last 10 Pi-hole DNS queries (domain, status, client) on an
// ILI9341 320x240 TFT. Updates every 5 seconds.
// Setup: First boot opens CYDPiHole_Setup AP. Hold BOOT button at startup
//        to re-enter setup on subsequent boots.

#include <Arduino.h>
#include <WiFi.h>

/*******************************************************************************
 * Display setup - CYD (Cheap Yellow Display) proven working config
 * ILI9341 320x240 landscape via hardware SPI
 ******************************************************************************/
#include <Arduino_GFX_Library.h>

#define GFX_BL 21  // CYD backlight pin

Arduino_DataBus *bus = new Arduino_HWSPI(
    2  /* DC */,
    15 /* CS */,
    14 /* SCK */,
    13 /* MOSI */,
    12 /* MISO */);

Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation: landscape 320x240 */);
/*******************************************************************************
 * End of display setup
 ******************************************************************************/

#include "Portal.h"
#include "PiHole.h"

// ---------------------------------------------------------------------------
// Layout constants (textSize 1 = 6x8px per character)
// ---------------------------------------------------------------------------
#define COL_STATUS_X   2    // "OK " or "BLK" — 3 chars = 18px
#define COL_CLIENT_X   26   // ".octet" — up to 4 chars = 24px
#define COL_DOMAIN_X   58   // domain — remaining ~260px = ~43 chars

#define HEADER_Y       0
#define HEADER_H       14
#define COLHDR_Y       15
#define COLHDR_H       10
#define DIVIDER_Y      26
#define ROWS_Y         28   // first data row top
#define ROW_H          21   // height per query row

#define COLOR_ALLOWED  0x07E0   // green
#define COLOR_BLOCKED  0xF800   // red
#define COLOR_UNKNOWN  0x8410   // grey
#define COLOR_HEADER   0x001F   // dark blue background for header
#define COLOR_TEXT     RGB565_WHITE
#define COLOR_DIM      0x8410   // grey for column labels

// ---------------------------------------------------------------------------
// Status display
// ---------------------------------------------------------------------------
void showStatus(const char *msg) {
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, COLOR_HEADER);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print(msg);
  Serial.println(msg);
}

// ---------------------------------------------------------------------------
// Draw the static chrome (header bar + column labels + divider)
// ---------------------------------------------------------------------------
void drawChrome() {
  // Header bar
  gfx->fillRect(0, HEADER_Y, gfx->width(), HEADER_H, COLOR_HEADER);
  gfx->setTextColor(0x07FF);  // cyan
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print("Pi-Hole Monitor");
  gfx->setTextColor(COLOR_DIM);
  gfx->setCursor(104, 3);
  gfx->print(ph_pihole_host);

  // Column labels
  gfx->fillRect(0, COLHDR_Y, gfx->width(), COLHDR_H, RGB565_BLACK);
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(COL_STATUS_X, COLHDR_Y + 1);
  gfx->print("ST");
  gfx->setCursor(COL_CLIENT_X, COLHDR_Y + 1);
  gfx->print(".CLT");
  gfx->setCursor(COL_DOMAIN_X, COLHDR_Y + 1);
  gfx->print("DOMAIN");

  // Divider line
  gfx->drawFastHLine(0, DIVIDER_Y, gfx->width(), COLOR_DIM);
}

// ---------------------------------------------------------------------------
// Truncate a string to fit maxChars, appending ".." if cut
// ---------------------------------------------------------------------------
void truncate(const char *src, char *dst, int maxChars) {
  int len = strlen(src);
  if (len <= maxChars) {
    strcpy(dst, src);
  } else {
    strncpy(dst, src, maxChars - 2);
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
  }
}

// ---------------------------------------------------------------------------
// Render all query rows
// ---------------------------------------------------------------------------
void drawQueries() {
  // Domain column width in chars: (320 - COL_DOMAIN_X) / 6px = ~43 chars
  const int domainChars = (gfx->width() - COL_DOMAIN_X) / 6;

  for (int i = 0; i < MAX_QUERIES; i++) {
    int y = ROWS_Y + i * ROW_H;

    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

    if (i >= ph_query_count || !ph_queries[i].valid) continue;

    PiQuery &q = ph_queries[i];

    // Status
    uint16_t statusColor = q.allowed ? COLOR_ALLOWED : COLOR_BLOCKED;
    gfx->setTextColor(statusColor);
    gfx->setTextSize(1);
    gfx->setCursor(COL_STATUS_X, y + 7);
    gfx->print(q.allowed ? "OK " : "BLK");

    // Client (last octet)
    char octet[6];
    phLastOctet(q.client, octet, sizeof(octet));
    char clientBuf[6];
    snprintf(clientBuf, sizeof(clientBuf), ".%s", octet);
    gfx->setTextColor(COLOR_DIM);
    gfx->setCursor(COL_CLIENT_X, y + 7);
    gfx->print(clientBuf);

    // Domain
    char domBuf[48];
    truncate(q.domain, domBuf, domainChars);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setCursor(COL_DOMAIN_X, y + 7);
    gfx->print(domBuf);
  }
}

// ---------------------------------------------------------------------------
// Fetch + redraw with status indication
// ---------------------------------------------------------------------------
void refreshQueries() {
  // Flash the header to show an update is in progress
  gfx->fillRect(0, HEADER_Y, gfx->width(), HEADER_H, 0x0010);
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print("Refreshing...");

  if (phFetch()) {
    drawChrome();
    drawQueries();
  } else {
    drawChrome();
    char errMsg[48];
    snprintf(errMsg, sizeof(errMsg), "Fetch failed: %s", ph_last_error);
    gfx->setTextColor(COLOR_BLOCKED);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print(errMsg);
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("CYDPiHole - Pi-hole DNS Query Monitor");

  // Init display
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);

  // Enable backlight
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // Boot button is GPIO 0 (active LOW)
  pinMode(0, INPUT_PULLUP);

  // Load saved WiFi settings from flash
  phLoadSettings();

  bool showPortal = !ph_has_settings;  // always open portal on first boot

  if (!showPortal) {
    // Settings exist — give a 3-second window to hold BOOT button to re-enter setup
    showStatus("Hold BOOT to change settings...");
    for (int i = 0; i < 30 && !showPortal; i++) {
      if (digitalRead(0) == LOW) showPortal = true;
      delay(100);
    }
  }

  if (showPortal) {
    phInitPortal();
    while (!portalDone) {
      phRunPortal();
      delay(5);
    }
    phClosePortal();
  }

  // Connect to WiFi
  gfx->fillScreen(RGB565_BLACK);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ph_wifi_ssid, ph_wifi_pass);

  int dots = 0;
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 30000) {
      char errMsg[60];
      snprintf(errMsg, sizeof(errMsg), "WiFi failed: \"%s\"", ph_wifi_ssid);
      showStatus(errMsg);
      while (true) delay(1000);
    }
    delay(500);
    char msg[48];
    snprintf(msg, sizeof(msg), "Connecting to WiFi%.*s", (dots % 4) + 1, "....");
    showStatus(msg);
    dots++;
  }

  showStatus("WiFi connected!");
  delay(400);

  drawChrome();
  refreshQueries();
}

// ---------------------------------------------------------------------------
// Loop — refresh every 5 seconds
// ---------------------------------------------------------------------------
#define REFRESH_INTERVAL (5 * 1000UL)
unsigned long lastRefresh = 0;

void loop() {
  if (lastRefresh == 0 || (millis() - lastRefresh) >= REFRESH_INTERVAL) {
    refreshQueries();
    lastRefresh = millis();
  }
  delay(500);
}
