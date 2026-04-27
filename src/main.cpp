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

#define DEVICE_NAME      "CYDPiHole"
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
#define MODE_WATCH_HITS  5
#define MODE_TARGETS     6
#define MODE_SNIFFER     7
#define MODE_SEEN_DEVICES 8
#define NUM_MODES         9

static int  currentMode          = MODE_LIVE_FEED;
static bool modeHasData[NUM_MODES] = {false};
static int  seenSelectedIndex    = 0;
static int  seenPendingIndex     = -1;
static bool seenPendingAdd       = true;
static int  seenPageIndex        = 0;

static const char *modeTitle[] = {
  "Pi-Hole Monitor",
  "Pi-Hole Stats",
  "Top Blocked",
  "Top Clients",
  "24h Activity",
  "Watched Hits",
  "Target Devices",
  "DNS Sniffer",
  "Seen Devices"
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
#define COLOR_WATCH_SAFE    0x039F   // blue
#define COLOR_WATCH_SLIPPED 0xF800   // red

// ---------------------------------------------------------------------------
// Status display
// ---------------------------------------------------------------------------
void refreshDisplay();

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
  } else if (currentMode == MODE_WATCH_HITS) {
    gfx->setCursor(2, COLHDR_Y + 1);  gfx->print("ST");
    gfx->setCursor(26, COLHDR_Y + 1); gfx->print("DEV");
    gfx->setCursor(98, COLHDR_Y + 1); gfx->print("DOMAIN");
  } else if (currentMode == MODE_TARGETS) {
    gfx->setCursor(2, COLHDR_Y + 1);   gfx->print("DEVICE");
    gfx->setCursor(170, COLHDR_Y + 1); gfx->print("S/B");
    gfx->setCursor(230, COLHDR_Y + 1); gfx->print("DOH");
  } else if (currentMode == MODE_SNIFFER) {
    gfx->setCursor(2, COLHDR_Y + 1); gfx->print("ALERTS");
  } else if (currentMode == MODE_SEEN_DEVICES) {
    gfx->setCursor(2, COLHDR_Y + 1);   gfx->print("SEEN");
    gfx->setCursor(150, COLHDR_Y + 1); gfx->print("IP / MAC");
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
// Render watched hits (Mode 5)
// ---------------------------------------------------------------------------
void drawWatchedHits() {
  const int labelChars = 11;
  const int domainChars = 36;
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);

  if (ph_watch_domain_count == 0) {
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print("Add watched domains in the AP portal.");
    gfx->setCursor(4, ROWS_Y + 20);
    gfx->print("Hold BOOT 3s to re-enter setup.");
    return;
  }

  if (ph_watch_hit_count == 0) {
    gfx->setTextColor(COLOR_WATCH_SAFE);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print("No recent watched-domain hits.");
    return;
  }

  for (int i = 0; i < PH_MAX_WATCH_HITS; i++) {
    int y = ROWS_Y + i * ROW_H;
    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);
    if (i >= ph_watch_hit_count || !ph_watch_hits[i].valid) continue;

    PhWatchHit &hit = ph_watch_hits[i];
    uint16_t rowColor = hit.blocked ? COLOR_WATCH_SAFE : COLOR_WATCH_SLIPPED;

    gfx->setTextColor(rowColor);
    gfx->setTextSize(1);
    gfx->setCursor(2, y + 7);
    gfx->print(hit.blocked ? "BLK" : "SLP");

    char labelBuf[16];
    truncate(hit.label, labelBuf, labelChars);
    gfx->setCursor(26, y + 7);
    gfx->print(labelBuf);

    char domainBuf[40];
    truncate(hit.domain, domainBuf, domainChars);
    gfx->setCursor(98, y + 7);
    gfx->print(domainBuf);
  }
}

// ---------------------------------------------------------------------------
// Render watched target summaries (Mode 6)
// ---------------------------------------------------------------------------
void drawTargetDevices() {
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);

  if (ph_watched_device_count == 0) {
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print("Add watched devices in the AP portal.");
    gfx->setCursor(4, ROWS_Y + 20);
    gfx->print("Format: IP | Client | MAC | Label");
    return;
  }

  for (int i = 0; i < ph_watch_summary_count && i < PH_MAX_WATCH_DEVICES; i++) {
    int y = ROWS_Y + i * 16;
    if (y > gfx->height() - 18) break;

    PhWatchDeviceSummary &summary = ph_watch_summaries[i];
    char labelBuf[20];
    truncate(summary.label, labelBuf, 18);

    gfx->setTextSize(1);
    gfx->setTextColor(summary.slipped_hits > 0 ? COLOR_WATCH_SLIPPED :
                      summary.blocked_hits > 0 ? COLOR_WATCH_SAFE : COLOR_TEXT);
    gfx->setCursor(2, y);
    gfx->print(labelBuf);

    char countsBuf[12];
    snprintf(countsBuf, sizeof(countsBuf), "%d/%d", summary.slipped_hits, summary.blocked_hits);
    gfx->setTextColor(summary.slipped_hits > 0 ? COLOR_WATCH_SLIPPED : COLOR_WATCH_SAFE);
    gfx->setCursor(182, y);
    gfx->print(countsBuf);

    char dohBuf[4];
    snprintf(dohBuf, sizeof(dohBuf), "%d", summary.doh_hits);
    gfx->setTextColor(summary.doh_hits > 0 ? COLOR_WATCH_SLIPPED : COLOR_DIM);
    gfx->setCursor(246, y);
    gfx->print(dohBuf);

    gfx->setTextColor(COLOR_DIM);
    gfx->setCursor(2, y + 8);
    if (summary.ip[0] != '\0') gfx->print(summary.ip);
    else if (summary.client_name[0] != '\0') gfx->print(summary.client_name);
    else gfx->print("No match key");
  }
}

// ---------------------------------------------------------------------------
// Render sniffer alerts (Mode 7)
// ---------------------------------------------------------------------------
void drawSniffer() {
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);

  if (ph_watched_device_count == 0) {
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print("Sniffer mode uses watched devices.");
    gfx->setCursor(4, ROWS_Y + 20);
    gfx->print("Add them in the AP portal first.");
    return;
  }

  if (ph_sniffer_alert_count == 0) {
    gfx->setTextColor(COLOR_WATCH_SAFE);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 7);
    gfx->print("No sniffer alerts right now.");
    gfx->setCursor(4, ROWS_Y + 20);
    gfx->print("Blocked watched hits stay blue.");
    return;
  }

  int y = ROWS_Y + 2;
  for (int i = 0; i < ph_sniffer_alert_count && i < PH_MAX_SNIFFER_ALERTS; i++) {
    if (y > gfx->height() - 18) break;
    PhSnifferAlert &alert = ph_sniffer_alerts[i];
    uint16_t col = alert.severity ? COLOR_WATCH_SLIPPED : COLOR_WATCH_SAFE;
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(2, y);
    gfx->print(alert.label);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setCursor(2, y + 9);
    gfx->print(alert.detail);
    y += 22;
  }
}

// ---------------------------------------------------------------------------
// Render seen devices picker (Mode 8)
// ---------------------------------------------------------------------------
void drawSeenDevices() {
  const int rowH = 18;
  const int footerH = 34;
  const int listBottom = gfx->height() - footerH;
  const int visibleRows = (listBottom - ROWS_Y) / rowH;
  const int totalPages = max(1, (ph_seen_device_count + visibleRows - 1) / visibleRows);
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);

  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_DIM);
  gfx->setCursor(2, ROWS_Y - 10);
  gfx->print("<");
  gfx->setCursor(gfx->width() - 6, ROWS_Y - 10);
  gfx->print(">");

  if (ph_seen_device_count == 0) {
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 8);
    gfx->print("No seen devices cached yet.");
    gfx->setCursor(4, ROWS_Y + 20);
    gfx->print("Leave the CYD on WiFi a bit longer.");
    return;
  }

  if (seenSelectedIndex >= ph_seen_device_count) seenSelectedIndex = ph_seen_device_count - 1;
  if (seenSelectedIndex < 0) seenSelectedIndex = 0;
  if (seenPageIndex < 0) seenPageIndex = 0;
  if (seenPageIndex >= totalPages) seenPageIndex = totalPages - 1;

  int start = seenPageIndex * visibleRows;

  for (int row = 0; row < visibleRows; row++) {
    int index = start + row;
    int y = ROWS_Y + row * rowH;
    gfx->fillRect(0, y, gfx->width(), rowH, RGB565_BLACK);
    if (index >= ph_seen_device_count || !ph_seen_devices[index].valid) continue;

    bool selected = (index == seenSelectedIndex);
    bool watched = phSeenDeviceIsWatched(index);
    if (selected) gfx->fillRect(0, y, gfx->width(), rowH, 0x1082);

    PhSeenDevice &dev = ph_seen_devices[index];
    char nameBuf[22];
    truncate(dev.name[0] ? dev.name : (dev.ip[0] ? dev.ip : dev.mac), nameBuf, 20);

    gfx->setTextSize(1);
    gfx->setTextColor(watched ? COLOR_WATCH_SAFE : COLOR_TEXT);
    gfx->setCursor(2, y + 2);
    gfx->print(watched ? "[ON]" : "[ADD]");
    gfx->setCursor(34, y + 2);
    gfx->print(nameBuf);

    gfx->setTextColor(COLOR_DIM);
    gfx->setCursor(34, y + 10);
    if (dev.ip[0] != '\0') gfx->print(dev.ip);
    else if (dev.mac[0] != '\0') gfx->print(dev.mac);
    if (dev.mac[0] != '\0' && dev.ip[0] != '\0') {
      gfx->setCursor(128, y + 10);
      gfx->print(dev.mac);
    }
  }

  int footerY = gfx->height() - footerH;
  gfx->drawFastHLine(0, footerY, gfx->width(), COLOR_DIM);
  if (seenPendingIndex >= 0 && seenPendingIndex < ph_seen_device_count) {
    PhSeenDevice &dev = ph_seen_devices[seenPendingIndex];
    char labelBuf[22];
    truncate(dev.name[0] ? dev.name : (dev.ip[0] ? dev.ip : dev.mac), labelBuf, 20);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setTextSize(1);
    gfx->setCursor(4, footerY + 4);
    gfx->print(seenPendingAdd ? "Add " : "Remove ");
    gfx->print(labelBuf);
    gfx->fillRect(0, footerY + 14, gfx->width() / 2, footerH - 14, 0x2945);
    gfx->fillRect(gfx->width() / 2, footerY + 14, gfx->width() / 2, footerH - 14, seenPendingAdd ? COLOR_WATCH_SAFE : COLOR_WATCH_SLIPPED);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setCursor(18, footerY + 22);
    gfx->print("Cancel");
    gfx->setCursor(gfx->width() / 2 + 18, footerY + 22);
    gfx->print(seenPendingAdd ? "Confirm Add" : "Confirm Remove");
  } else {
    gfx->setTextColor(COLOR_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, footerY + 4);
    gfx->print("< Page");
    char pageBuf[20];
    snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", seenPageIndex + 1, totalPages);
    int pageX = (gfx->width() - (int)strlen(pageBuf) * 6) / 2;
    gfx->setCursor(pageX, footerY + 4);
    gfx->print(pageBuf);
    gfx->setCursor(gfx->width() - 38, footerY + 4);
    gfx->print("Page >");
    gfx->setCursor(4, footerY + 18);
    gfx->print("Tap row to add/remove. Top corners change mode.");
  }
}

static bool handleSeenDevicesTouch(int tx, int ty) {
  const int footerH = 34;
  const int rowH = 18;
  const int footerY = gfx->height() - footerH;
  const int visibleRows = (footerY - ROWS_Y) / rowH;
  const int totalPages = max(1, (ph_seen_device_count + visibleRows - 1) / visibleRows);

  if (ty < HEADER_H) {
    if (tx < 48) {
      seenPendingIndex = -1;
      currentMode = (currentMode + NUM_MODES - 1) % NUM_MODES;
      refreshDisplay();
      return true;
    }
    if (tx > gfx->width() - 48) {
      seenPendingIndex = -1;
      currentMode = (currentMode + 1) % NUM_MODES;
      refreshDisplay();
      return true;
    }
  }

  if (ph_seen_device_count == 0) return true;
  if (seenPageIndex < 0) seenPageIndex = 0;
  if (seenPageIndex >= totalPages) seenPageIndex = totalPages - 1;

  int start = seenPageIndex * visibleRows;

  if (seenPendingIndex >= 0 && ty >= footerY + 14) {
    if (tx < gfx->width() / 2) {
      seenPendingIndex = -1;
      drawChrome();
      drawSeenDevices();
      return true;
    }
    bool changed = seenPendingAdd ? phAddSeenDeviceToWatched(seenPendingIndex)
                                  : phRemoveSeenDeviceFromWatched(seenPendingIndex);
    seenPendingIndex = -1;
    if (changed) {
      refreshDisplay();
    } else {
      drawChrome();
      drawSeenDevices();
    }
    return true;
  }

  if (seenPendingIndex < 0 && ty >= footerY) {
    if (tx < gfx->width() / 3) {
      if (seenPageIndex > 0) {
        seenPageIndex--;
        drawChrome();
        drawSeenDevices();
      }
      return true;
    }
    if (tx > (gfx->width() * 2) / 3) {
      if (seenPageIndex + 1 < totalPages) {
        seenPageIndex++;
        drawChrome();
        drawSeenDevices();
      }
      return true;
    }
    return true;
  }

  if (ty < ROWS_Y || ty >= footerY) {
    return true;
  }

  int tappedRow = (ty - ROWS_Y) / rowH;
  int seenIndex = start + tappedRow;
  if (seenIndex < 0 || seenIndex >= ph_seen_device_count || !ph_seen_devices[seenIndex].valid) {
    return true;
  }

  seenSelectedIndex = seenIndex;
  seenPendingIndex = seenIndex;
  seenPendingAdd = !phSeenDeviceIsWatched(seenIndex);
  drawChrome();
  drawSeenDevices();
  return true;
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
  if (currentMode == MODE_WATCH_HITS)  ok = phFetch();
  if (currentMode == MODE_TARGETS)     ok = phFetch();
  if (currentMode == MODE_SNIFFER)     ok = phFetch();
  if (currentMode == MODE_SEEN_DEVICES) ok = true;

  if (ok) {
    modeHasData[currentMode] = true;
    drawChrome();
    if (currentMode == MODE_LIVE_FEED)   drawQueries();
    if (currentMode == MODE_STATS)       drawStats();
    if (currentMode == MODE_TOP_BLOCKED) drawTopBlocked();
    if (currentMode == MODE_TOP_CLIENTS) drawTopClients();
    if (currentMode == MODE_ACTIVITY)    drawActivityGraph();
    if (currentMode == MODE_WATCH_HITS)  drawWatchedHits();
    if (currentMode == MODE_TARGETS)     drawTargetDevices();
    if (currentMode == MODE_SNIFFER)     drawSniffer();
    if (currentMode == MODE_SEEN_DEVICES) drawSeenDevices();
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
  phRefreshBlocklistDomains(true);
  phFetchNetworkDevices(true);

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
  phRefreshBlocklistDomains(false);
  phFetchNetworkDevices(false);
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
      int ty = map(p.y, 240, 3800, 0, gfx->height());
      tx = constrain(tx, 0, gfx->width() - 1);
      ty = constrain(ty, 0, gfx->height() - 1);
      if (currentMode == MODE_SEEN_DEVICES) {
        handleSeenDevicesTouch(tx, ty);
      } else if (tx < gfx->width() / 2)
        currentMode = (currentMode + NUM_MODES - 1) % NUM_MODES;  // left → prev
      else
        currentMode = (currentMode + 1) % NUM_MODES;              // right → next
      if (currentMode != MODE_SEEN_DEVICES || seenPendingIndex < 0) refreshDisplay();
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
