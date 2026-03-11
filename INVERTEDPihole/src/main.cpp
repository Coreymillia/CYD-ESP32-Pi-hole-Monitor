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

#define DEVICE_NAME      "INVERTEDPihole"
#define FIRMWARE_VERSION "1.0.0"
#include "CYDIdentity.h"

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

#include <XPT2046_Touchscreen.h>

// ---------------------------------------------------------------------------
// Touch — XPT2046 on VSPI (same wiring as all CYD variants)
// ---------------------------------------------------------------------------
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33
#define TOUCH_DEBOUNCE 400

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static unsigned long lastTouchTime = 0;

#include "Portal.h"
#include "PiHole.h"

// ---------------------------------------------------------------------------
// Display modes
// ---------------------------------------------------------------------------
#define MODE_LIVE_FEED   0
#define MODE_STATS       1
#define MODE_TOP_BLOCKED 2
#define MODE_TOP_CLIENTS 3
#define MODE_ACTIVITY    4
#define NUM_MODES        5

static int  currentMode          = MODE_LIVE_FEED;
static bool modeHasData[NUM_MODES] = {false};

static const char *modeTitle[] = {
  "Pi-Hole Monitor",
  "Pi-Hole Stats",
  "Top Blocked",
  "Top Clients",
  "24h Activity"
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

#define COLOR_ALLOWED  0x67E0   // bright lime green
#define COLOR_BLOCKED  0xFD20   // orange
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
  } else if (currentMode == MODE_TOP_BLOCKED || currentMode == MODE_TOP_CLIENTS) {
    gfx->setCursor(2,  COLHDR_Y + 1); gfx->print("#");
    gfx->setCursor(16, COLHDR_Y + 1); gfx->print("COUNT");
    gfx->setCursor(56, COLHDR_Y + 1);
    gfx->print(currentMode == MODE_TOP_CLIENTS ? "CLIENT" : "DOMAIN");
  }
  // MODE_STATS, MODE_ACTIVITY: no column headers

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

  static const uint16_t allowedColors[] = { 0x07FF, 0xFFE0, 0x535F };  // cyan, yellow, blue

  for (int i = 0; i < MAX_QUERIES; i++) {
    int y = ROWS_Y + i * ROW_H;

    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

    if (i >= ph_query_count || !ph_queries[i].valid) continue;

    PiQuery &q = ph_queries[i];

    uint16_t rowColor = q.allowed ? allowedColors[i % 3] : COLOR_BLOCKED;

    // Status
    gfx->setTextColor(rowColor);
    gfx->setTextSize(1);
    gfx->setCursor(COL_STATUS_X, y + 7);
    gfx->print(q.allowed ? "OK " : "BLK");

    // Client (last octet)
    char octet[6];
    phLastOctet(q.client, octet, sizeof(octet));
    char clientBuf[6];
    snprintf(clientBuf, sizeof(clientBuf), ".%s", octet);
    gfx->setTextColor(0xC47F);  // lavender
    gfx->setCursor(COL_CLIENT_X, y + 7);
    gfx->print(clientBuf);

    // Domain
    char domBuf[48];
    truncate(q.domain, domBuf, domainChars);
    gfx->setTextColor(rowColor);
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
    gfx->setTextColor(COLOR_BLOCKED);  // orange — all entries here are blocked
    gfx->setCursor(56, y + 7);
    gfx->print(domBuf);
  }
}

// ---------------------------------------------------------------------------
// Render top clients (Mode 3)
// ---------------------------------------------------------------------------
void drawTopClients() {
  const int nameChars = (gfx->width() - 56) / 6;

  for (int i = 0; i < MAX_TOP_CLIENTS; i++) {
    int y = ROWS_Y + i * ROW_H;
    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

    if (i >= ph_top_clients_count || !ph_top_clients[i].valid) continue;

    PiClientEntry &e = ph_top_clients[i];

    char rankBuf[4];
    snprintf(rankBuf, sizeof(rankBuf), "%d", i + 1);
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(2, y + 7);
    gfx->print(rankBuf);

    char countBuf[10];
    formatCount(e.count, countBuf, sizeof(countBuf));
    gfx->setTextColor(0x07FF);  // cyan
    gfx->setCursor(16, y + 7);
    gfx->print(countBuf);

    char nameBuf[52];
    truncate(e.name, nameBuf, nameChars);
    gfx->setTextColor(0x07FF);  // cyan
    gfx->setCursor(56, y + 7);
    gfx->print(nameBuf);
  }
}

// ---------------------------------------------------------------------------
// Render 24h activity graph (Mode 4)
// Stacked bars: green = allowed, red = blocked; oldest left, newest right
// ---------------------------------------------------------------------------
void drawActivityGraph() {
  const int graphBottom = gfx->height() - 14;
  const int maxBarH     = graphBottom - ROWS_Y - 2;

  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);
  if (ph_history_count == 0) return;

  int maxTotal = 1;
  for (int i = 0; i < ph_history_count; i++)
    if (ph_history[i].total > maxTotal) maxTotal = ph_history[i].total;

  // 2px-wide bars, no gap; centre in display width
  const int barW    = 2;
  const int leftPad = (gfx->width() - ph_history_count * barW) / 2;

  for (int i = 0; i < ph_history_count; i++) {
    int x      = leftPad + i * barW;
    int totalH = (ph_history[i].total   * maxBarH) / maxTotal;
    int blockH = (ph_history[i].blocked * maxBarH) / maxTotal;
    int allowH = totalH - blockH;

    if (allowH > 0)
      gfx->fillRect(x, graphBottom - totalH,  barW, allowH, COLOR_ALLOWED);
    if (blockH > 0)
      gfx->fillRect(x, graphBottom - blockH,  barW, blockH, COLOR_BLOCKED);
  }

  // Labels
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(leftPad, graphBottom + 3);
  gfx->print("24H AGO");
  int nowX = leftPad + ph_history_count * barW - 18;
  gfx->setCursor(max(nowX, 0), graphBottom + 3);
  gfx->print("NOW");

  gfx->setTextColor(COLOR_ALLOWED);
  gfx->setCursor(130, graphBottom + 3); gfx->print("OK");
  gfx->setTextColor(COLOR_BLOCKED);
  gfx->setCursor(154, graphBottom + 3); gfx->print("BLK");
}

// ---------------------------------------------------------------------------
// Fetch + redraw for the current mode
// On transient fetch failure: keep stale data visible, flash error in header
// ---------------------------------------------------------------------------
void refreshDisplay() {
  gfx->fillRect(0, HEADER_Y, gfx->width(), HEADER_H, 0x0010);
  gfx->setTextColor(COLOR_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print("Refreshing...");

  bool ok = false;
  if (currentMode == MODE_LIVE_FEED)   ok = phFetch();
  if (currentMode == MODE_STATS)       ok = phFetchStats();
  if (currentMode == MODE_TOP_BLOCKED) ok = phFetchTopBlocked();
  if (currentMode == MODE_TOP_CLIENTS) ok = phFetchTopClients();
  if (currentMode == MODE_ACTIVITY)    ok = phFetchHistory();

  if (ok) {
    modeHasData[currentMode] = true;
    drawChrome();
    if (currentMode == MODE_LIVE_FEED)   drawQueries();
    if (currentMode == MODE_STATS)       drawStats();
    if (currentMode == MODE_TOP_BLOCKED) drawTopBlocked();
    if (currentMode == MODE_TOP_CLIENTS) drawTopClients();
    if (currentMode == MODE_ACTIVITY)    drawActivityGraph();
  } else if (modeHasData[currentMode]) {
    // Stale data is already on screen — just flash the error in the header
    gfx->fillRect(0, HEADER_Y, gfx->width(), HEADER_H, 0x3000);  // dark red
    char errMsg[52];
    snprintf(errMsg, sizeof(errMsg), "ERR: %s", ph_last_error);
    gfx->setTextColor(COLOR_BLOCKED);
    gfx->setTextSize(1);
    gfx->setCursor(4, 3);
    gfx->print(errMsg);
  } else {
    // No prior data — show error over blank screen
    drawChrome();
    gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);
    char errMsg[52];
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
  gfx->invertDisplay(true);
  gfx->fillScreen(RGB565_BLACK);

  // Enable backlight
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // Boot button is GPIO 0 (active LOW)
  pinMode(0, INPUT_PULLUP);

  // Touch controller
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

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
  identityBegin();

  drawChrome();
  refreshDisplay();
}

// ---------------------------------------------------------------------------
// Loop — refresh every 5 seconds
// ---------------------------------------------------------------------------
#define REFRESH_INTERVAL (5 * 1000UL)
unsigned long lastRefresh = 0;

void loop() {
  identityHandle();
  checkButton();
  if (lastRefresh == 0 || (millis() - lastRefresh) >= REFRESH_INTERVAL) {
    refreshDisplay();
    lastRefresh = millis();
  }

  // Touch: left half = previous mode, right half = next mode
  if (ts.tirqTouched() && ts.touched()) {
    unsigned long now = millis();
    if (now - lastTouchTime > TOUCH_DEBOUNCE) {
      lastTouchTime = now;
      TS_Point p = ts.getPoint();
      int tx = map(p.x, 200, 3900, 0, gfx->width());
      tx = constrain(tx, 0, gfx->width() - 1);
      if (tx < gfx->width() / 2)
        currentMode = (currentMode + NUM_MODES - 1) % NUM_MODES;  // left → prev
      else
        currentMode = (currentMode + 1) % NUM_MODES;              // right → next
      refreshDisplay();
      lastRefresh = millis();
    }
  }

  // Countdown bar: 1px at y=239, drains left→right over REFRESH_INTERVAL
  unsigned long elapsed = millis() - lastRefresh;
  int barW = (elapsed >= REFRESH_INTERVAL)
             ? 0
             : (int)((REFRESH_INTERVAL - elapsed) * (long)gfx->width() / REFRESH_INTERVAL);
  gfx->drawFastHLine(0,    gfx->height() - 1, barW,                RGB565_BLUE);
  gfx->drawFastHLine(barW, gfx->height() - 1, gfx->width() - barW, RGB565_BLACK);

  delay(20);
}
