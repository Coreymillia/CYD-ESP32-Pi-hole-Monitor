#pragma once

#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// gfx is defined in main.cpp
extern Arduino_GFX *gfx;

// ---------------------------------------------------------------------------
// Persisted settings
// ---------------------------------------------------------------------------
static char ph_wifi_ssid[64]    = "";
static char ph_wifi_pass[64]    = "";
static char ph_pihole_host[64]  = "";    // Pi-hole IP or hostname
static uint16_t ph_pihole_port  = 80;   // Pi-hole web UI port (default 80, use 8080 if Pi-Alert shares the Pi)
static char ph_pihole_pass[64]  = "";   // Pi-hole admin password (empty = passwordless)
static bool ph_has_settings     = false;  // true if SSID + Pi-hole host are saved
static bool ph_force_portal     = false;  // set by long-press to force setup on next boot

#define PH_WATCH_TEXT_MAX     1024
#define PH_MAX_WATCH_DOMAINS  256
#define PH_MAX_WATCH_DEVICES  12

struct PhWatchedDomain {
  char domain[64];
  bool valid;
};

struct PhWatchedDevice {
  char ip[16];
  char client_name[48];
  char mac[18];
  char label[32];
  bool valid;
};

struct PhSeenDevice {
  char ip[16];
  char name[48];
  char mac[18];
  bool valid;
};

static char ph_watch_domains_text[PH_WATCH_TEXT_MAX] = "";
static char ph_watch_devices_text[PH_WATCH_TEXT_MAX] = "";
static char ph_seen_devices_text[PH_WATCH_TEXT_MAX] = "";
static char ph_blocklist_url[256] = "";
static uint8_t ph_watch_threshold = 3;

static PhWatchedDomain ph_watch_domains[PH_MAX_WATCH_DOMAINS];
static int             ph_watch_domain_count = 0;
static PhWatchedDevice ph_watched_devices[PH_MAX_WATCH_DEVICES];
static int             ph_watched_device_count = 0;
static PhSeenDevice    ph_seen_devices[PH_MAX_WATCH_DEVICES];
static int             ph_seen_device_count = 0;

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

// ---------------------------------------------------------------------------
// Watch config helpers
// ---------------------------------------------------------------------------
static String phTrimCopy(const String &value) {
  int start = 0;
  int end = value.length();
  while (start < end && isspace((unsigned char)value[start])) start++;
  while (end > start && isspace((unsigned char)value[end - 1])) end--;
  return value.substring(start, end);
}

static String phHtmlEscape(const char *raw) {
  String out;
  for (size_t i = 0; raw[i] != '\0'; i++) {
    char c = raw[i];
    if (c == '&')      out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else               out += c;
  }
  return out;
}

static String phJsEscape(const char *raw) {
  String out;
  for (size_t i = 0; raw[i] != '\0'; i++) {
    char c = raw[i];
    if (c == '\\' || c == '\'') {
      out += '\\';
      out += c;
    } else if (c == '\n' || c == '\r') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

static String phNextPipeField(const String &line, int &startAt) {
  if (startAt < 0 || startAt >= line.length()) {
    startAt = line.length();
    return "";
  }
  int sep = line.indexOf('|', startAt);
  if (sep < 0) {
    String field = phTrimCopy(line.substring(startAt));
    startAt = line.length();
    return field;
  }
  String field = phTrimCopy(line.substring(startAt, sep));
  startAt = sep + 1;
  return field;
}

static bool phWatchDomainExists(const char *domain) {
  for (int i = 0; i < ph_watch_domain_count; i++) {
    if (ph_watch_domains[i].valid && strcmp(ph_watch_domains[i].domain, domain) == 0) {
      return true;
    }
  }
  return false;
}

static bool phAddWatchDomainRule(const char *domain) {
  if (!domain || domain[0] == '\0') return false;
  if (phWatchDomainExists(domain)) return true;
  if (ph_watch_domain_count >= PH_MAX_WATCH_DOMAINS) return false;
  PhWatchedDomain &entry = ph_watch_domains[ph_watch_domain_count++];
  strncpy(entry.domain, domain, sizeof(entry.domain) - 1);
  entry.domain[sizeof(entry.domain) - 1] = '\0';
  entry.valid = true;
  return true;
}

static void phParseWatchDomains() {
  for (int i = 0; i < PH_MAX_WATCH_DOMAINS; i++) {
    ph_watch_domains[i].domain[0] = '\0';
    ph_watch_domains[i].valid = false;
  }
  ph_watch_domain_count = 0;

  String raw = String(ph_watch_domains_text);
  raw.replace("\r", "");
  int start = 0;
  while (start <= raw.length() && ph_watch_domain_count < PH_MAX_WATCH_DOMAINS) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    String line = phTrimCopy(raw.substring(start, end));
    line.toLowerCase();
    if (line.length() > 0) {
      char domainBuf[64];
      line.toCharArray(domainBuf, sizeof(domainBuf));
      phAddWatchDomainRule(domainBuf);
    }
    start = end + 1;
  }
}

static void phParseWatchedDevices() {
  for (int i = 0; i < PH_MAX_WATCH_DEVICES; i++) {
    ph_watched_devices[i].ip[0] = '\0';
    ph_watched_devices[i].client_name[0] = '\0';
    ph_watched_devices[i].mac[0] = '\0';
    ph_watched_devices[i].label[0] = '\0';
    ph_watched_devices[i].valid = false;
  }
  ph_watched_device_count = 0;

  String raw = String(ph_watch_devices_text);
  raw.replace("\r", "");
  int start = 0;
  while (start <= raw.length() && ph_watched_device_count < PH_MAX_WATCH_DEVICES) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    String line = phTrimCopy(raw.substring(start, end));
    start = end + 1;
    if (line.length() == 0) continue;

    int pos = 0;
    String ip = phNextPipeField(line, pos);
    String client = phNextPipeField(line, pos);
    String field3 = phNextPipeField(line, pos);
    String field4 = phNextPipeField(line, pos);
    String mac = "";
    String label = "";
    if (field4.length() > 0) {
      mac = field3;
      label = field4;
    } else if (field3.indexOf(':') >= 0) {
      mac = field3;
    } else {
      label = field3;
    }
    if (ip.length() == 0 && client.length() == 0) continue;
    if (label.length() == 0) label = client.length() ? client : ip;

    PhWatchedDevice &entry = ph_watched_devices[ph_watched_device_count++];
    ip.toCharArray(entry.ip, sizeof(entry.ip));
    client.toCharArray(entry.client_name, sizeof(entry.client_name));
    mac.toCharArray(entry.mac, sizeof(entry.mac));
    label.toCharArray(entry.label, sizeof(entry.label));
    entry.valid = true;
  }
}

static void phParseSeenDevices() {
  for (int i = 0; i < PH_MAX_WATCH_DEVICES; i++) {
    ph_seen_devices[i].ip[0] = '\0';
    ph_seen_devices[i].name[0] = '\0';
    ph_seen_devices[i].mac[0] = '\0';
    ph_seen_devices[i].valid = false;
  }
  ph_seen_device_count = 0;

  String raw = String(ph_seen_devices_text);
  raw.replace("\r", "");
  int start = 0;
  while (start <= raw.length() && ph_seen_device_count < PH_MAX_WATCH_DEVICES) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    String line = phTrimCopy(raw.substring(start, end));
    start = end + 1;
    if (line.length() == 0) continue;

    int pos = 0;
    String ip = phNextPipeField(line, pos);
    String name = phNextPipeField(line, pos);
    String mac = phNextPipeField(line, pos);
    if (ip.length() == 0 && name.length() == 0 && mac.length() == 0) continue;

    PhSeenDevice &entry = ph_seen_devices[ph_seen_device_count++];
    ip.toCharArray(entry.ip, sizeof(entry.ip));
    name.toCharArray(entry.name, sizeof(entry.name));
    mac.toCharArray(entry.mac, sizeof(entry.mac));
    entry.valid = true;
  }
}

static void phRefreshWatchConfig() {
  if (ph_watch_threshold < 1) ph_watch_threshold = 1;
  if (ph_watch_threshold > 20) ph_watch_threshold = 20;
  phParseWatchDomains();
  phParseWatchedDevices();
  phParseSeenDevices();
}

static void phSaveSeenDevicesCache(const char *seenDevices) {
  Preferences prefs;
  prefs.begin("cydpihole", false);
  prefs.putString("seendev", seenDevices);
  prefs.end();

  strncpy(ph_seen_devices_text, seenDevices, sizeof(ph_seen_devices_text) - 1);
  ph_seen_devices_text[sizeof(ph_seen_devices_text) - 1] = '\0';
  phParseSeenDevices();
}

static void phPersistDomainSettings(const char *watchDomains,
                                    const char *blocklistUrl,
                                    uint8_t watchThreshold) {
  Preferences prefs;
  prefs.begin("cydpihole", false);
  prefs.putString("watchdom", watchDomains);
  prefs.putString("blkurl", blocklistUrl);
  prefs.putUChar("watchthr", watchThreshold);
  prefs.end();

  strncpy(ph_watch_domains_text, watchDomains, sizeof(ph_watch_domains_text) - 1);
  ph_watch_domains_text[sizeof(ph_watch_domains_text) - 1] = '\0';
  strncpy(ph_blocklist_url, blocklistUrl, sizeof(ph_blocklist_url) - 1);
  ph_blocklist_url[sizeof(ph_blocklist_url) - 1] = '\0';
  ph_watch_threshold = watchThreshold;
  phRefreshWatchConfig();
}

static void phPersistWatchedDevices() {
  String serialized;
  for (int i = 0; i < ph_watched_device_count; i++) {
    if (!ph_watched_devices[i].valid) continue;
    if (serialized.length() > 0) serialized += '\n';
    serialized += String(ph_watched_devices[i].ip);
    serialized += " | ";
    serialized += String(ph_watched_devices[i].client_name);
    serialized += " | ";
    serialized += String(ph_watched_devices[i].mac);
    serialized += " | ";
    serialized += String(ph_watched_devices[i].label);
  }

  Preferences prefs;
  prefs.begin("cydpihole", false);
  prefs.putString("watchdev", serialized);
  prefs.end();

  serialized.toCharArray(ph_watch_devices_text, sizeof(ph_watch_devices_text));
  ph_watch_devices_text[sizeof(ph_watch_devices_text) - 1] = '\0';
  phRefreshWatchConfig();
}

static int phFindWatchedDeviceBySeenIndex(int seenIndex) {
  if (seenIndex < 0 || seenIndex >= ph_seen_device_count || !ph_seen_devices[seenIndex].valid) {
    return -1;
  }
  const PhSeenDevice &seen = ph_seen_devices[seenIndex];
  for (int i = 0; i < ph_watched_device_count; i++) {
    if (!ph_watched_devices[i].valid) continue;
    bool ipMatch = (seen.ip[0] != '\0' &&
                    ph_watched_devices[i].ip[0] != '\0' &&
                    strcmp(ph_watched_devices[i].ip, seen.ip) == 0);
    bool nameMatch = (seen.name[0] != '\0' &&
                      ph_watched_devices[i].client_name[0] != '\0' &&
                      phTrimCopy(String(ph_watched_devices[i].client_name)).equalsIgnoreCase(phTrimCopy(String(seen.name))));
    bool macMatch = (seen.mac[0] != '\0' &&
                     ph_watched_devices[i].mac[0] != '\0' &&
                     phTrimCopy(String(ph_watched_devices[i].mac)).equalsIgnoreCase(phTrimCopy(String(seen.mac))));
    if (ipMatch || nameMatch || macMatch) return i;
  }
  return -1;
}

static bool phSeenDeviceIsWatched(int seenIndex) {
  return phFindWatchedDeviceBySeenIndex(seenIndex) >= 0;
}

static bool phAddSeenDeviceToWatched(int seenIndex) {
  if (seenIndex < 0 || seenIndex >= ph_seen_device_count || !ph_seen_devices[seenIndex].valid) return false;
  if (phSeenDeviceIsWatched(seenIndex)) return true;
  if (ph_watched_device_count >= PH_MAX_WATCH_DEVICES) return false;

  const PhSeenDevice &seen = ph_seen_devices[seenIndex];
  PhWatchedDevice &entry = ph_watched_devices[ph_watched_device_count++];
  strncpy(entry.ip, seen.ip, sizeof(entry.ip) - 1);
  entry.ip[sizeof(entry.ip) - 1] = '\0';
  strncpy(entry.client_name, seen.name, sizeof(entry.client_name) - 1);
  entry.client_name[sizeof(entry.client_name) - 1] = '\0';
  strncpy(entry.mac, seen.mac, sizeof(entry.mac) - 1);
  entry.mac[sizeof(entry.mac) - 1] = '\0';
  const char *label = seen.name[0] ? seen.name : (seen.ip[0] ? seen.ip : seen.mac);
  strncpy(entry.label, label, sizeof(entry.label) - 1);
  entry.label[sizeof(entry.label) - 1] = '\0';
  entry.valid = true;
  phPersistWatchedDevices();
  return true;
}

static bool phRemoveSeenDeviceFromWatched(int seenIndex) {
  int watchedIndex = phFindWatchedDeviceBySeenIndex(seenIndex);
  if (watchedIndex < 0) return false;

  for (int i = watchedIndex; i + 1 < ph_watched_device_count; i++) {
    ph_watched_devices[i] = ph_watched_devices[i + 1];
  }
  if (ph_watched_device_count > 0) {
    ph_watched_device_count--;
    ph_watched_devices[ph_watched_device_count].ip[0] = '\0';
    ph_watched_devices[ph_watched_device_count].client_name[0] = '\0';
    ph_watched_devices[ph_watched_device_count].mac[0] = '\0';
    ph_watched_devices[ph_watched_device_count].label[0] = '\0';
    ph_watched_devices[ph_watched_device_count].valid = false;
  }
  phPersistWatchedDevices();
  return true;
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static void phLoadSettings() {
  Preferences prefs;
  prefs.begin("cydpihole", true);
  String ssid   = prefs.getString("ssid",   "");
  String pass   = prefs.getString("pass",   "");
  String pihost = prefs.getString("pihost", "");
  String pipass = prefs.getString("pipass", "");
  String watchDomains = prefs.getString("watchdom", "");
  String watchDevices = prefs.getString("watchdev", "");
  String seenDevices = prefs.getString("seendev", "");
  String blocklistUrl = prefs.getString("blkurl", "");
  uint16_t piport = (uint16_t)prefs.getUInt("piport", 80);
  uint8_t watchThreshold = (uint8_t)prefs.getUChar("watchthr", 3);
  bool   force  = prefs.getBool("forceportal", false);
  prefs.end();

  // Clear the flag immediately so a crash won't loop us in setup
  if (force) {
    Preferences rw;
    rw.begin("cydpihole", false);
    rw.putBool("forceportal", false);
    rw.end();
  }

  ssid.toCharArray(ph_wifi_ssid,    sizeof(ph_wifi_ssid));
  pass.toCharArray(ph_wifi_pass,    sizeof(ph_wifi_pass));
  pihost.toCharArray(ph_pihole_host, sizeof(ph_pihole_host));
  pipass.toCharArray(ph_pihole_pass, sizeof(ph_pihole_pass));
  watchDomains.toCharArray(ph_watch_domains_text, sizeof(ph_watch_domains_text));
  watchDevices.toCharArray(ph_watch_devices_text, sizeof(ph_watch_devices_text));
  seenDevices.toCharArray(ph_seen_devices_text, sizeof(ph_seen_devices_text));
  blocklistUrl.toCharArray(ph_blocklist_url, sizeof(ph_blocklist_url));
  ph_pihole_port  = piport;
  ph_watch_threshold = watchThreshold;
  ph_has_settings = (ssid.length() > 0 && pihost.length() > 0);
  ph_force_portal = force;
  phRefreshWatchConfig();
}

static void phSaveSettings(const char *ssid, const char *pass,
                           const char *pihost, const char *pipass, uint16_t piport,
                           const char *watchDomains, const char *watchDevices,
                           uint8_t watchThreshold) {
  Preferences prefs;
  prefs.begin("cydpihole", false);
  prefs.putString("ssid",   ssid);
  prefs.putString("pass",   pass);
  prefs.putString("pihost", pihost);
  prefs.putString("pipass", pipass);
  prefs.putUInt("piport",   piport);
  prefs.putString("watchdom", watchDomains);
  prefs.putString("watchdev", watchDevices);
  prefs.putUChar("watchthr", watchThreshold);
  prefs.end();

  strncpy(ph_wifi_ssid,   ssid,   sizeof(ph_wifi_ssid)   - 1);
  strncpy(ph_wifi_pass,   pass,   sizeof(ph_wifi_pass)   - 1);
  strncpy(ph_pihole_host, pihost, sizeof(ph_pihole_host) - 1);
  strncpy(ph_pihole_pass, pipass, sizeof(ph_pihole_pass) - 1);
  strncpy(ph_watch_domains_text, watchDomains, sizeof(ph_watch_domains_text) - 1);
  ph_watch_domains_text[sizeof(ph_watch_domains_text) - 1] = '\0';
  strncpy(ph_watch_devices_text, watchDevices, sizeof(ph_watch_devices_text) - 1);
  ph_watch_devices_text[sizeof(ph_watch_devices_text) - 1] = '\0';
  ph_pihole_port  = piport;
  ph_watch_threshold = watchThreshold;
  ph_has_settings = true;
  phRefreshWatchConfig();
}

// ---------------------------------------------------------------------------
// On-screen setup instructions (320x240 landscape)
// ---------------------------------------------------------------------------
static void phShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0x07FF);  // cyan
  gfx->setTextSize(2);
  gfx->setCursor(20, 5);
  gfx->print("CYDPiHole Setup");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(60, 26);
  gfx->print("Pi-hole Query Monitor");

  gfx->setTextColor(0xFFE0);  // yellow
  gfx->setCursor(4, 46);
  gfx->print("1. Connect your phone/PC to WiFi:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(14, 58);
  gfx->print("CYDPiHole_Setup");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 82);
  gfx->print("2. Open your browser and go to:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(50, 94);
  gfx->print("192.168.4.1");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 118);
  gfx->print("3. Enter your WiFi and Pi-hole");
  gfx->setCursor(4, 130);
  gfx->print("   details, then tap Save.");
  gfx->setCursor(4, 146);
  gfx->print("4. Watch lists are optional in");
  gfx->setCursor(4, 158);
  gfx->print("   the portal setup form.");

  if (ph_has_settings) {
    gfx->setTextColor(0x07E0);  // green
    gfx->setCursor(4, 182);
    gfx->print("Existing settings found. Tap");
    gfx->setCursor(4, 194);
    gfx->print("'No Changes' to keep them.");
  }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static void phHandleRoot() {
  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CYDPiHole Setup</title>"
    "<style>"
    "body{background:#0d0d1a;color:#00ccff;font-family:Arial,sans-serif;"
         "text-align:center;padding:20px;max-width:480px;margin:auto;}"
    "h1{color:#00ffff;font-size:1.6em;margin-bottom:4px;}"
    "p{color:#88aacc;font-size:0.9em;}"
    "label{display:block;text-align:left;margin:14px 0 4px;color:#88ddff;font-weight:bold;}"
    "input{width:100%;box-sizing:border-box;background:#001a33;color:#00ccff;"
          "border:2px solid #0066aa;border-radius:6px;padding:10px;font-size:1em;}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
         "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
    ".btn-save{background:#004488;color:#00ffff;border:2px solid #0099dd;}"
    ".btn-save:hover{background:#0066bb;}"
     ".btn-skip{background:#1a1a2e;color:#667788;border:2px solid #334455;}"
     ".btn-skip:hover{background:#223344;color:#aabbcc;}"
     ".note{color:#445566;font-size:0.82em;margin-top:16px;}"
     ".device-list{background:#071320;border:1px solid #113355;border-radius:8px;padding:10px;text-align:left;}"
     ".seen-row{border-bottom:1px solid #113355;padding:8px 0;}"
     ".seen-row:last-child{border-bottom:none;}"
     ".seen-meta{color:#88aacc;font-size:0.84em;display:block;margin-top:4px;}"
     ".btn-add{background:#113355;color:#00ffff;border:1px solid #0099dd;padding:7px 10px;border-radius:6px;cursor:pointer;margin-top:6px;}"
      "textarea{width:100%;min-height:104px;box-sizing:border-box;background:#001a33;color:#00ccff;"
           "border:2px solid #0066aa;border-radius:6px;padding:10px;font-size:0.95em;font-family:monospace;}"
      "hr{border:1px solid #113355;margin:20px 0;}"
      "</style>"
      "<script>"
      "function addSeenDevice(ip,name,mac){"
      "const ta=document.querySelector('textarea[name=\"watchdevices\"]');"
      "if(!ta)return;"
      "const label=(name&&name.length)?name:(ip&&ip.length?ip:(mac&&mac.length?mac:'device'));"
      "const line=[ip||'',name||'',mac||'',label].join(' | ');"
      "const cur=ta.value.replace(/\\s+$/,'');"
      "ta.value=cur.length?cur+'\\n'+line:line;"
      "ta.focus();"
      "}"
      "</script></head><body>"
      "<h1>&#128273; CYDPiHole Setup</h1>"
     "<p>Enter your WiFi, Pi-hole, and optional watch settings.</p>"
      "<form method='post' action='/save'>"
    "<label>WiFi Network Name (SSID):</label>"
    "<input type='text' name='ssid' value='";
  html += String(ph_wifi_ssid);
  html += "' placeholder='Your 2.4 GHz WiFi name' maxlength='63' required>"
    "<label>WiFi Password:</label>"
    "<input type='password' name='pass' value='";
  html += String(ph_wifi_pass);
  html += "' placeholder='Leave blank if open network' maxlength='63'>"
    "<hr>"
    "<label>Pi-hole IP / Hostname:</label>"
    "<input type='text' name='pihost' value='";
  html += String(ph_pihole_host);
  html += "' placeholder='e.g. 192.168.0.105' maxlength='63' required>"
    "<label>Pi-hole Port:</label>"
    "<input type='number' name='piport' value='";
  html += String(ph_pihole_port);
  html += "' placeholder='80' min='1' max='65535' required>"
    "<label>Pi-hole Password"
    " <span style='color:#445566;font-weight:normal'>(leave blank if none)</span>:</label>"
     "<input type='password' name='pipass' value='";
  html += String(ph_pihole_pass);
  html += "' placeholder='Leave blank if passwordless' maxlength='63'>"
    "<hr>"
    "<label>Watched Devices <span style='color:#445566;font-weight:normal'>(IP | Client Name | MAC | Label)</span>:</label>"
    "<textarea name='watchdevices' placeholder='192.168.0.42 | kid-ipad | aa:bb:cc:dd:ee:ff | Owen iPad&#10;192.168.0.55 | school-chromebook | 11:22:33:44:55:66 | School Chromebook'>";
  html += phHtmlEscape(ph_watch_devices_text);
  html += "</textarea>"
    "<p class='note'>Use watched devices here. Domain and blocklist settings have their own save section below.</p>"
     "<br><button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
      "</form>";
  html += "<hr><h2 style='color:#00ffff'>Domains and Blocklist</h2>"
          "<form method='post' action='/save_domains'>"
          "<label>Watched Domains <span style='color:#445566;font-weight:normal'>(one per line)</span>:</label>"
          "<textarea name='watchdomains' placeholder='youtube.com&#10;discord.com&#10;netflix.com'>";
  html += phHtmlEscape(ph_watch_domains_text);
  html += "</textarea>"
          "<label>Blocklist URL <span style='color:#445566;font-weight:normal'>(one list only)</span>:</label>"
          "<input type='text' name='blocklisturl' value='";
  html += phHtmlEscape(ph_blocklist_url);
  html += "' placeholder='https://example.com/blocklist.txt' maxlength='255'>"
          "<label>Allowed-Hit Warning Threshold:</label>"
          "<input type='number' name='watchthreshold' value='";
  html += String(ph_watch_threshold);
  html += "' min='1' max='20' required>"
          "<p class='note'>Manual watched domains and the single blocklist URL are merged at runtime. Use this save button when you only want to update domains or the blocklist.</p>"
          "<button class='btn btn-save' type='submit'>&#128190; Save Domains / Blocklist</button>"
          "</form>";
  if (ph_seen_device_count > 0) {
    html += "<hr><h2 style='color:#00ffff'>Recently Seen Devices</h2>"
            "<p class='note'>These come from Pi-hole after the CYD has connected before. Tap Add to append a real device to the watched list.</p>"
            "<div class='device-list'>";
    for (int i = 0; i < ph_seen_device_count; i++) {
      if (!ph_seen_devices[i].valid) continue;
      html += "<div class='seen-row'><strong>";
      html += phHtmlEscape(ph_seen_devices[i].name[0] ? ph_seen_devices[i].name : ph_seen_devices[i].ip);
      html += "</strong><span class='seen-meta'>IP: ";
      html += phHtmlEscape(ph_seen_devices[i].ip);
      html += " &middot; MAC: ";
      html += phHtmlEscape(ph_seen_devices[i].mac[0] ? ph_seen_devices[i].mac : "--");
      html += "</span><button class='btn-add' type='button' onclick=\"addSeenDevice('";
      html += phJsEscape(ph_seen_devices[i].ip);
      html += "','";
      html += phJsEscape(ph_seen_devices[i].name);
      html += "','";
      html += phJsEscape(ph_seen_devices[i].mac);
      html += "')\">Add to watched devices</button></div>";
    }
    html += "</div>";
  }
  if (ph_has_settings) {
    html += "<hr>"
      "<form method='post' action='/nochange'>"
      "<button class='btn btn-skip' type='submit'>&#10006; No Changes &mdash; Use Current Settings</button>"
      "</form>";
  }
  html += "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only.</p>"
    "</body></html>";

  portalServer->send(200, "text/html", html);
}

static void phHandleSave() {
  String ssid   = portalServer->hasArg("ssid")   ? portalServer->arg("ssid")   : "";
  String pass   = portalServer->hasArg("pass")   ? portalServer->arg("pass")   : "";
  String pihost = portalServer->hasArg("pihost") ? portalServer->arg("pihost") : "";
  String pipass = portalServer->hasArg("pipass") ? portalServer->arg("pipass") : "";
  String watchDevices = portalServer->hasArg("watchdevices") ? portalServer->arg("watchdevices") : "";
  uint16_t piport = portalServer->hasArg("piport") ? (uint16_t)portalServer->arg("piport").toInt() : 80;
  if (piport == 0) piport = 80;

  if (ssid.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#0d0d1a;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; SSID cannot be empty!</h2>"
      "<a href='/' style='color:#00ccff'>&#8592; Go Back</a></body></html>");
    return;
  }

  if (pihost.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#0d0d1a;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; Pi-hole IP cannot be empty!</h2>"
      "<a href='/' style='color:#00ccff'>&#8592; Go Back</a></body></html>");
    return;
  }

  phSaveSettings(
    ssid.c_str(),
    pass.c_str(),
    pihost.c_str(),
    pipass.c_str(),
    piport,
    ph_watch_domains_text,
    watchDevices.c_str(),
    ph_watch_threshold
  );

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}"
    "p{color:#88aacc;}</style></head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>WiFi: <b>" + ssid + "</b></p>"
    "<p>Pi-hole: <b>" + pihost + "</b></p>"
    "<p>Watched domains: <b>" + String(ph_watch_domain_count) + "</b></p>"
    "<p>Watched devices: <b>" + String(ph_watched_device_count) + "</b></p>"
    "<p>You can close this page and disconnect from <b>CYDPiHole_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

static void phHandleSaveDomains() {
  String watchDomains = portalServer->hasArg("watchdomains") ? portalServer->arg("watchdomains") : "";
  String blocklistUrl = portalServer->hasArg("blocklisturl") ? portalServer->arg("blocklisturl") : "";
  uint8_t watchThreshold = portalServer->hasArg("watchthreshold")
                           ? (uint8_t)portalServer->arg("watchthreshold").toInt()
                           : ph_watch_threshold;
  if (watchThreshold < 1) watchThreshold = 1;
  if (watchThreshold > 20) watchThreshold = 20;

  phPersistDomainSettings(watchDomains.c_str(), blocklistUrl.c_str(), watchThreshold);

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}p{color:#88aacc;}</style></head><body>"
    "<h2>&#9989; Domains Saved!</h2>"
    "<p>Manual watched domains: <b>" + String(ph_watch_domain_count) + "</b></p>"
    "<p>Blocklist URL: <b>" + String(ph_blocklist_url[0] ? ph_blocklist_url : "(none)") + "</b></p>"
    "<p>You can close this page or go back to save other settings.</p>"
    "</body></html>");
}

static void phHandleNoChange() {
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}"
    "p{color:#88aacc;}</style></head><body>"
    "<h2>&#128077; No Changes</h2>"
    "<p>Using saved settings. Connecting now.</p>"
    "<p>You can close this page and disconnect from <b>CYDPiHole_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void phInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CYDPiHole_Setup", "");
  delay(500);

  portalDNS    = new DNSServer();
  portalServer = new WebServer(80);

  portalDNS->start(53, "*", WiFi.softAPIP());

  portalServer->on("/",         phHandleRoot);
  portalServer->on("/save",     HTTP_POST, phHandleSave);
  portalServer->on("/save_domains", HTTP_POST, phHandleSaveDomains);
  portalServer->on("/nochange", HTTP_POST, phHandleNoChange);
  portalServer->onNotFound(phHandleRoot);
  portalServer->begin();

  portalDone = false;

  phShowPortalScreen();

  Serial.printf("[Portal] AP up — connect to CYDPiHole_Setup, open %s\n",
                WiFi.softAPIP().toString().c_str());
}

static void phRunPortal() {
  portalDNS->processNextRequest();
  portalServer->handleClient();
}

static void phClosePortal() {
  portalServer->stop();
  portalDNS->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);

  delete portalServer; portalServer = nullptr;
  delete portalDNS;    portalDNS    = nullptr;
}
