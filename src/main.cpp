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
// Display modes
// ---------------------------------------------------------------------------
#define MODE_LIVE_FEED   0
#define MODE_STATS       1
#define MODE_TOP_BLOCKED 2
#define NUM_MODES        3

static int currentMode = MODE_LIVE_FEED;

static const char *modeTitle[] = {
  "Pi-Hole Monitor",
  "Pi-Hole Stats",
  "Top Blocked"
};

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
  gfx->print(modeTitle[currentMode]);
  gfx->setTextColor(COLOR_DIM);
  gfx->setCursor(104, 3);
  gfx->print(ph_pihole_host);

  // Column labels (vary by mode)
  gfx->fillRect(0, COLHDR_Y, gfx->width(), COLHDR_H, RGB565_BLACK);
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  if (currentMode == MODE_LIVE_FEED) {
    gfx->setCursor(COL_STATUS_X, COLHDR_Y + 1); gfx->print("ST");
    gfx->setCursor(COL_CLIENT_X, COLHDR_Y + 1); gfx->print(".CLT");
    gfx->setCursor(COL_DOMAIN_X, COLHDR_Y + 1); gfx->print("DOMAIN");
  } else if (currentMode == MODE_TOP_BLOCKED) {
    gfx->setCursor(2,  COLHDR_Y + 1); gfx->print("#");
    gfx->setCursor(16, COLHDR_Y + 1); gfx->print("COUNT");
    gfx->setCursor(56, COLHDR_Y + 1); gfx->print("DOMAIN");
  }
  // MODE_STATS: no column headers

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
// Number formatters
// ---------------------------------------------------------------------------
// Formats a large integer with comma separators, e.g. 1847203 -> "1,847,203"
static void formatNum(long n, char *buf, size_t bufLen) {
  if (n < 0)       { snprintf(buf, bufLen, "--");                                          return; }
  if (n < 1000)    { snprintf(buf, bufLen, "%ld", n);                                      return; }
  if (n < 1000000) { snprintf(buf, bufLen, "%ld,%03ld", n / 1000, n % 1000);              return; }
  snprintf(buf, bufLen, "%ld,%03ld,%03ld", n / 1000000, (n % 1000000) / 1000, n % 1000);
}

// Formats a block count compactly, e.g. 99999 -> "99999", 150000 -> "150K"
static void formatCount(long n, char *buf, size_t bufLen) {
  if (n < 100000)   { snprintf(buf, bufLen, "%ld",  n);              return; }
  if (n < 10000000) { snprintf(buf, bufLen, "%ldK", n / 1000);       return; }
  snprintf(buf, bufLen, "%.1fM", n / 1000000.0f);
}

// ---------------------------------------------------------------------------
// Render all query rows (Mode 0)
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
// Render stats dashboard (Mode 1)
// ---------------------------------------------------------------------------
void drawStats() {
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);
  if (!ph_stats.valid) return;

  char buf[16];

  // --- Row 1: Queries Today | Blocked Today ---
  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_DIM);
  gfx->setCursor(2,   ROWS_Y + 4);  gfx->print("QUERIES TODAY");
  gfx->setCursor(164, ROWS_Y + 4);  gfx->print("BLOCKED TODAY");

  gfx->setTextSize(2);
  formatNum(ph_stats.queries_today, buf, sizeof(buf));
  gfx->setTextColor(COLOR_TEXT);
  gfx->setCursor(2, ROWS_Y + 14);   gfx->print(buf);

  formatNum(ph_stats.blocked_today, buf, sizeof(buf));
  gfx->setTextColor(COLOR_BLOCKED);
  gfx->setCursor(164, ROWS_Y + 14); gfx->print(buf);

  // --- Divider ---
  gfx->drawFastHLine(0, ROWS_Y + 38, gfx->width(), COLOR_DIM);

  // --- Percent blocked (centered, yellow) ---
  snprintf(buf, sizeof(buf), "%.1f%% BLOCKED", ph_stats.percent_blocked);
  int pctW = strlen(buf) * 12;  // textSize 2 = 12px wide per char
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFE0);  // yellow
  gfx->setCursor((gfx->width() - pctW) / 2, ROWS_Y + 46);
  gfx->print(buf);

  // --- Divider ---
  gfx->drawFastHLine(0, ROWS_Y + 70, gfx->width(), COLOR_DIM);

  // --- Row 2: Blocklist Domains | Active Clients ---
  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_DIM);
  gfx->setCursor(2,   ROWS_Y + 78); gfx->print("BLOCKLIST DOMAINS");
  gfx->setCursor(210, ROWS_Y + 78); gfx->print("CLIENTS");

  gfx->setTextSize(2);
  formatNum(ph_stats.domains_on_blocklist, buf, sizeof(buf));
  gfx->setTextColor(COLOR_TEXT);
  gfx->setCursor(2,   ROWS_Y + 88); gfx->print(buf);

  formatNum(ph_stats.active_clients, buf, sizeof(buf));
  gfx->setTextColor(0x07FF);  // cyan
  gfx->setCursor(210, ROWS_Y + 88); gfx->print(buf);
}

// ---------------------------------------------------------------------------
// Render top blocked domains (Mode 2)
// ---------------------------------------------------------------------------
void drawTopBlocked() {
  const int domainChars = (gfx->width() - 56) / 6;  // ~44 chars

  for (int i = 0; i < MAX_TOP_BLOCKED; i++) {
    int y = ROWS_Y + i * ROW_H;
    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

    if (i >= ph_top_blocked_count || !ph_top_blocked[i].valid) continue;

    PiBlockEntry &e = ph_top_blocked[i];

    // Rank
    char rankBuf[4];
    snprintf(rankBuf, sizeof(rankBuf), "%d", i + 1);
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(2, y + 7);
    gfx->print(rankBuf);

    // Count
    char countBuf[10];
    formatCount(e.count, countBuf, sizeof(countBuf));
    gfx->setTextColor(COLOR_BLOCKED);
    gfx->setCursor(16, y + 7);
    gfx->print(countBuf);

    // Domain
    char domBuf[48];
    truncate(e.domain, domBuf, domainChars);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setCursor(56, y + 7);
    gfx->print(domBuf);
  }
}

// ---------------------------------------------------------------------------
// Fetch + redraw for the current mode
// ---------------------------------------------------------------------------
void refreshDisplay() {
  // Brief flash to show an update is in progress
  gfx->fillRect(0, HEADER_Y, gfx->width(), HEADER_H, 0x0010);
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print("Refreshing...");

  bool ok = false;
  if (currentMode == MODE_LIVE_FEED)   ok = phFetch();
  if (currentMode == MODE_STATS)       ok = phFetchStats();
  if (currentMode == MODE_TOP_BLOCKED) ok = phFetchTopBlocked();

  drawChrome();

  if (ok) {
    if (currentMode == MODE_LIVE_FEED)   drawQueries();
    if (currentMode == MODE_STATS)       drawStats();
    if (currentMode == MODE_TOP_BLOCKED) drawTopBlocked();
  } else {
    // Clear the full data area so stale content from a previous mode doesn't show through
    gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);
    char errMsg[48];
    snprintf(errMsg, sizeof(errMsg), "Fetch failed: %s", ph_last_error);
    gfx->setTextColor(COLOR_BLOCKED);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print(errMsg);
  }
}

// ---------------------------------------------------------------------------
// Button handler — short press cycles mode, 3-second hold restarts into setup
// ---------------------------------------------------------------------------
void checkButton() {
  if (digitalRead(0) != LOW) return;

  unsigned long pressStart = millis();
  while (digitalRead(0) == LOW) {
    if (millis() - pressStart >= 3000) {
      // Long press — set NVS flag and restart into portal
      showStatus("Restarting setup...");
      Preferences prefs;
      prefs.begin("cydpihole", false);
      prefs.putBool("forceportal", true);
      prefs.end();
      delay(1000);
      ESP.restart();
    }
    delay(20);
  }

  // Short press — advance to next mode and refresh immediately
  currentMode = (currentMode + 1) % NUM_MODES;
  refreshDisplay();
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

  bool showPortal = !ph_has_settings || ph_force_portal;

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
  refreshDisplay();
}

// ---------------------------------------------------------------------------
// Loop — refresh every 5 seconds
// ---------------------------------------------------------------------------
#define REFRESH_INTERVAL (5 * 1000UL)
unsigned long lastRefresh = 0;

void loop() {
  checkButton();
  if (lastRefresh == 0 || (millis() - lastRefresh) >= REFRESH_INTERVAL) {
    refreshDisplay();
    lastRefresh = millis();
  }
  delay(20);
}
