#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MAX_QUERIES          10
#define PH_QUERY_FETCH_COUNT 80
#define MAX_TOP_BLOCKED      10

// ---------------------------------------------------------------------------
// Live query data (Mode 0)
// ---------------------------------------------------------------------------
struct PiQuery {
  char domain[64];
  char client[16];
  char client_name[48];
  bool allowed;
  bool valid;
};

static PiQuery ph_queries[MAX_QUERIES];
static int     ph_query_count = 0;

// ---------------------------------------------------------------------------
// Summary stats (Mode 1)
// ---------------------------------------------------------------------------
struct PiStats {
  long  queries_today;
  long  blocked_today;
  float percent_blocked;
  long  domains_on_blocklist;
  int   active_clients;
  bool  valid;
};

static PiStats ph_stats = {0, 0, 0.0f, 0, 0, false};

// ---------------------------------------------------------------------------
// Top blocked domains (Mode 2)
// ---------------------------------------------------------------------------
struct PiBlockEntry {
  char domain[64];
  long count;
  bool valid;
};

static PiBlockEntry ph_top_blocked[MAX_TOP_BLOCKED];
static int          ph_top_blocked_count = 0;

// ---------------------------------------------------------------------------
// Watched hits and sniffer state
// ---------------------------------------------------------------------------
#define PH_MAX_WATCH_HITS      10
#define PH_MAX_SNIFFER_ALERTS   8

struct PhWatchHit {
  char label[32];
  char client[16];
  char domain[64];
  bool blocked;
  bool doh;
  bool valid;
};

struct PhWatchDeviceSummary {
  char ip[16];
  char client_name[48];
  char label[32];
  int  blocked_hits;
  int  slipped_hits;
  int  doh_hits;
  bool active;
  bool valid;
};

struct PhSnifferAlert {
  char label[32];
  char detail[72];
  uint8_t severity;  // 0 = info/protected, 1 = warning
  bool valid;
};

static PhWatchHit           ph_watch_hits[PH_MAX_WATCH_HITS];
static int                  ph_watch_hit_count = 0;
static PhWatchDeviceSummary ph_watch_summaries[PH_MAX_WATCH_DEVICES];
static int                  ph_watch_summary_count = 0;
static PhSnifferAlert       ph_sniffer_alerts[PH_MAX_SNIFFER_ALERTS];
static int                  ph_sniffer_alert_count = 0;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static int  ph_last_http_code = 0;
static char ph_last_error[48] = "";
static char ph_sid[65]        = "";
static bool ph_auth_done      = false;

static const char *PH_DOH_PROVIDERS[] = {
  "dns.google",
  "dns64.dns.google",
  "cloudflare-dns.com",
  "1dot1dot1dot1.cloudflare.com",
  "mozilla.cloudflare-dns.com",
  "dns.quad9.net",
  "dns9.quad9.net",
  "dns11.quad9.net",
  "dns.nextdns.io",
  "dns.adguard.com",
  "dns.adguard-dns.com",
  "doh.opendns.com",
  "doh.dns.apple.com"
};

static unsigned long ph_last_seen_refresh_ms = 0;
static const unsigned long PH_SEEN_REFRESH_MS = 5UL * 60UL * 1000UL;
static unsigned long ph_last_blocklist_refresh_ms = 0;
static const unsigned long PH_BLOCKLIST_REFRESH_MS = 30UL * 60UL * 1000UL;
static const size_t PH_BLOCKLIST_MAX_BYTES = 96UL * 1024UL;
static int ph_blocklist_domain_count = 0;
static PhWatchedDomain ph_blocklist_domains_cache[PH_MAX_WATCH_DOMAINS];
static int ph_blocklist_domain_cache_count = 0;
static char ph_active_blocklist_url[256] = "";

static int phGet(const char *path, String &out);

// ---------------------------------------------------------------------------
// Status classification — Pi-hole v6 string values
// Allowed: FORWARDED, CACHE, CACHE_STALE, RETRIED, RETRIED_DNSSEC, IN_PROGRESS
// ---------------------------------------------------------------------------
static bool phStatusAllowed(const char *status) {
  return (strncmp(status, "FORWARDED",  9) == 0 ||
          strncmp(status, "CACHE",      5) == 0 ||
          strncmp(status, "RETRIED",    7) == 0 ||
          strcmp (status, "IN_PROGRESS")   == 0);
}

static char phAsciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool phEqualsIgnoreCase(const char *lhs, const char *rhs) {
  if (!lhs || !rhs) return false;
  while (*lhs && *rhs) {
    if (phAsciiLower(*lhs) != phAsciiLower(*rhs)) return false;
    lhs++;
    rhs++;
  }
  return *lhs == '\0' && *rhs == '\0';
}

static void phLowerCopy(const char *src, char *dst, size_t dstLen) {
  if (dstLen == 0) return;
  size_t i = 0;
  for (; src[i] != '\0' && i + 1 < dstLen; i++) dst[i] = phAsciiLower(src[i]);
  dst[i] = '\0';
}

static bool phDomainMatchesRule(const char *domain, const char *rule) {
  size_t domainLen = strlen(domain);
  size_t ruleLen = strlen(rule);
  if (ruleLen == 0 || domainLen < ruleLen) return false;
  if (strcmp(domain, rule) == 0) return true;
  return (domainLen > ruleLen &&
          strcmp(domain + domainLen - ruleLen, rule) == 0 &&
          domain[domainLen - ruleLen - 1] == '.');
}

static bool phDomainMatchesWatched(const char *domain) {
  for (int i = 0; i < ph_watch_domain_count; i++) {
    if (ph_watch_domains[i].valid && phDomainMatchesRule(domain, ph_watch_domains[i].domain)) {
      return true;
    }
  }
  return false;
}

static bool phDomainIsDohProvider(const char *domain) {
  for (size_t i = 0; i < sizeof(PH_DOH_PROVIDERS) / sizeof(PH_DOH_PROVIDERS[0]); i++) {
    if (phDomainMatchesRule(domain, PH_DOH_PROVIDERS[i])) return true;
  }
  return false;
}

static const char *phWatchLabelForDevice(const PhWatchedDevice &device) {
  if (device.label[0] != '\0') return device.label;
  if (device.client_name[0] != '\0') return device.client_name;
  if (device.mac[0] != '\0') return device.mac;
  return device.ip;
}

static const PhSeenDevice *phFindSeenDeviceByIp(const char *ip) {
  for (int i = 0; i < ph_seen_device_count; i++) {
    if (ph_seen_devices[i].valid && strcmp(ph_seen_devices[i].ip, ip) == 0) {
      return &ph_seen_devices[i];
    }
  }
  return nullptr;
}

static void phResetWatchState() {
  ph_watch_hit_count = 0;
  for (int i = 0; i < PH_MAX_WATCH_HITS; i++) {
    ph_watch_hits[i].label[0] = '\0';
    ph_watch_hits[i].client[0] = '\0';
    ph_watch_hits[i].domain[0] = '\0';
    ph_watch_hits[i].blocked = false;
    ph_watch_hits[i].doh = false;
    ph_watch_hits[i].valid = false;
  }

  ph_watch_summary_count = ph_watched_device_count;
  for (int i = 0; i < PH_MAX_WATCH_DEVICES; i++) {
    ph_watch_summaries[i].ip[0] = '\0';
    ph_watch_summaries[i].client_name[0] = '\0';
    ph_watch_summaries[i].label[0] = '\0';
    ph_watch_summaries[i].blocked_hits = 0;
    ph_watch_summaries[i].slipped_hits = 0;
    ph_watch_summaries[i].doh_hits = 0;
    ph_watch_summaries[i].active = false;
    ph_watch_summaries[i].valid = false;
  }

  for (int i = 0; i < ph_watched_device_count && i < PH_MAX_WATCH_DEVICES; i++) {
    PhWatchDeviceSummary &summary = ph_watch_summaries[i];
    const PhWatchedDevice &device = ph_watched_devices[i];
    strncpy(summary.ip, device.ip, sizeof(summary.ip) - 1);
    summary.ip[sizeof(summary.ip) - 1] = '\0';
    strncpy(summary.client_name, device.client_name, sizeof(summary.client_name) - 1);
    summary.client_name[sizeof(summary.client_name) - 1] = '\0';
    strncpy(summary.label, phWatchLabelForDevice(device), sizeof(summary.label) - 1);
    summary.label[sizeof(summary.label) - 1] = '\0';
    summary.valid = device.valid;
  }

  ph_sniffer_alert_count = 0;
  for (int i = 0; i < PH_MAX_SNIFFER_ALERTS; i++) {
    ph_sniffer_alerts[i].label[0] = '\0';
    ph_sniffer_alerts[i].detail[0] = '\0';
    ph_sniffer_alerts[i].severity = 0;
    ph_sniffer_alerts[i].valid = false;
  }
}

static int phFindWatchedDeviceIndex(const char *ip, const char *clientName) {
  const PhSeenDevice *seen = phFindSeenDeviceByIp(ip);
  const char *mac = seen ? seen->mac : "";
  for (int i = 0; i < ph_watched_device_count; i++) {
    if (!ph_watched_devices[i].valid) continue;
    bool ipMatch = (ph_watched_devices[i].ip[0] != '\0' &&
                    strcmp(ph_watched_devices[i].ip, ip) == 0);
    bool nameMatch = (ph_watched_devices[i].client_name[0] != '\0' &&
                      clientName[0] != '\0' &&
                      phEqualsIgnoreCase(ph_watched_devices[i].client_name, clientName));
    bool macMatch = (ph_watched_devices[i].mac[0] != '\0' &&
                     mac[0] != '\0' &&
                     phEqualsIgnoreCase(ph_watched_devices[i].mac, mac));
    if (ipMatch || nameMatch || macMatch) return i;
  }
  return -1;
}

static void phResolveWatchLabel(char *dst, size_t dstLen, int watchedIndex,
                                const char *ip, const char *clientName) {
  if (dstLen == 0) return;
  if (watchedIndex >= 0 && watchedIndex < ph_watch_summary_count && ph_watch_summaries[watchedIndex].label[0] != '\0') {
    strncpy(dst, ph_watch_summaries[watchedIndex].label, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return;
  }
  if (clientName[0] != '\0') {
    strncpy(dst, clientName, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return;
  }
  strncpy(dst, ip, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static void phAddWatchHit(const char *label, const char *ip, const char *domain,
                          bool blocked, bool doh) {
  if (ph_watch_hit_count >= PH_MAX_WATCH_HITS) return;
  PhWatchHit &hit = ph_watch_hits[ph_watch_hit_count++];
  strncpy(hit.label, label, sizeof(hit.label) - 1);
  hit.label[sizeof(hit.label) - 1] = '\0';
  strncpy(hit.client, ip, sizeof(hit.client) - 1);
  hit.client[sizeof(hit.client) - 1] = '\0';
  strncpy(hit.domain, domain, sizeof(hit.domain) - 1);
  hit.domain[sizeof(hit.domain) - 1] = '\0';
  hit.blocked = blocked;
  hit.doh = doh;
  hit.valid = true;
}

static void phAddSnifferAlert(const char *label, const char *detail, uint8_t severity) {
  if (ph_sniffer_alert_count >= PH_MAX_SNIFFER_ALERTS) return;
  PhSnifferAlert &alert = ph_sniffer_alerts[ph_sniffer_alert_count++];
  strncpy(alert.label, label, sizeof(alert.label) - 1);
  alert.label[sizeof(alert.label) - 1] = '\0';
  strncpy(alert.detail, detail, sizeof(alert.detail) - 1);
  alert.detail[sizeof(alert.detail) - 1] = '\0';
  alert.severity = severity;
  alert.valid = true;
}

static void phBuildSnifferAlerts() {
  for (int i = 0; i < ph_watch_summary_count && i < PH_MAX_WATCH_DEVICES; i++) {
    PhWatchDeviceSummary &summary = ph_watch_summaries[i];
    if (!summary.valid) continue;

    if (summary.slipped_hits >= ph_watch_threshold) {
      char detail[72];
      snprintf(detail, sizeof(detail), "%d allowed watched hits", summary.slipped_hits);
      phAddSnifferAlert(summary.label, detail, 1);
    }
    if (summary.doh_hits > 0) {
      char detail[72];
      snprintf(detail, sizeof(detail), "DoH/provider hits: %d", summary.doh_hits);
      phAddSnifferAlert(summary.label, detail, 1);
    }
    if (summary.blocked_hits > 0 && summary.slipped_hits == 0 && summary.doh_hits == 0) {
      char detail[72];
      snprintf(detail, sizeof(detail), "%d watched hits blocked", summary.blocked_hits);
      phAddSnifferAlert(summary.label, detail, 0);
    }
  }
}

static bool phLooksLikeHttpUrl(const char *url) {
  if (!url) return false;
  return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool phLooksLikeDomainName(const String &value) {
  if (value.length() == 0 || value.length() >= 64) return false;
  bool hasDot = false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '.') {
      if (c == '.') hasDot = true;
      continue;
    }
    return false;
  }
  return hasDot;
}

static void phClearBlocklistDomainCache() {
  for (int i = 0; i < PH_MAX_WATCH_DOMAINS; i++) {
    ph_blocklist_domains_cache[i].domain[0] = '\0';
    ph_blocklist_domains_cache[i].valid = false;
  }
  ph_blocklist_domain_cache_count = 0;
}

static bool phBlocklistCacheContains(const char *domain) {
  for (int i = 0; i < ph_blocklist_domain_cache_count; i++) {
    if (ph_blocklist_domains_cache[i].valid &&
        strcmp(ph_blocklist_domains_cache[i].domain, domain) == 0) {
      return true;
    }
  }
  return false;
}

static bool phCacheBlocklistDomain(const char *domain) {
  if (!domain || domain[0] == '\0') return false;
  if (phBlocklistCacheContains(domain)) return true;
  if (ph_blocklist_domain_cache_count >= PH_MAX_WATCH_DOMAINS) return false;
  PhWatchedDomain &entry = ph_blocklist_domains_cache[ph_blocklist_domain_cache_count++];
  strncpy(entry.domain, domain, sizeof(entry.domain) - 1);
  entry.domain[sizeof(entry.domain) - 1] = '\0';
  entry.valid = true;
  return true;
}

static void phRebuildMergedWatchDomains() {
  phParseWatchDomains();
  for (int i = 0; i < ph_blocklist_domain_cache_count; i++) {
    if (ph_blocklist_domains_cache[i].valid) {
      phAddWatchDomainRule(ph_blocklist_domains_cache[i].domain);
    }
  }
  ph_blocklist_domain_count = ph_blocklist_domain_cache_count;
}

static bool phAddBlocklistLine(const String &rawLine) {
  String line = rawLine;
  int hashAt = line.indexOf('#');
  if (hashAt >= 0) line = line.substring(0, hashAt);
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return false;

  String candidate = "";
  int firstSpace = line.indexOf(' ');
  int firstTab = line.indexOf('\t');
  int splitAt = -1;
  if (firstSpace >= 0 && firstTab >= 0) splitAt = min(firstSpace, firstTab);
  else if (firstSpace >= 0) splitAt = firstSpace;
  else splitAt = firstTab;

  if (splitAt >= 0) {
    String first = line.substring(0, splitAt);
    String rest = line.substring(splitAt + 1);
    rest.trim();
    int secondSpace = rest.indexOf(' ');
    int secondTab = rest.indexOf('\t');
    int nextSplit = -1;
    if (secondSpace >= 0 && secondTab >= 0) nextSplit = min(secondSpace, secondTab);
    else if (secondSpace >= 0) nextSplit = secondSpace;
    else nextSplit = secondTab;
    if (nextSplit >= 0) rest = rest.substring(0, nextSplit);

    if (first == "0.0.0.0" || first == "127.0.0.1" || first == "::1") {
      candidate = rest;
    } else {
      candidate = first;
    }
  } else {
    candidate = line;
  }

  candidate.trim();
  if (!phLooksLikeDomainName(candidate)) return false;

  char domainBuf[64];
  candidate.toCharArray(domainBuf, sizeof(domainBuf));
  return phCacheBlocklistDomain(domainBuf);
}

static bool phRefreshBlocklistDomains(bool force = false) {
  if (strcmp(ph_active_blocklist_url, ph_blocklist_url) != 0) {
    force = true;
  }

  if (ph_blocklist_url[0] == '\0') {
    phClearBlocklistDomainCache();
    phRebuildMergedWatchDomains();
    strncpy(ph_active_blocklist_url, ph_blocklist_url, sizeof(ph_active_blocklist_url) - 1);
    ph_active_blocklist_url[sizeof(ph_active_blocklist_url) - 1] = '\0';
    ph_last_blocklist_refresh_ms = millis();
    return true;
  }
  if (!phLooksLikeHttpUrl(ph_blocklist_url)) {
    phClearBlocklistDomainCache();
    phRebuildMergedWatchDomains();
    snprintf(ph_last_error, sizeof(ph_last_error), "Blocklist URL invalid");
    strncpy(ph_active_blocklist_url, ph_blocklist_url, sizeof(ph_active_blocklist_url) - 1);
    ph_active_blocklist_url[sizeof(ph_active_blocklist_url) - 1] = '\0';
    ph_last_blocklist_refresh_ms = millis();
    return false;
  }
  phRebuildMergedWatchDomains();
  if (WiFi.status() != WL_CONNECTED) return false;

  unsigned long now = millis();
  if (!force && ph_last_blocklist_refresh_ms != 0 &&
      (now - ph_last_blocklist_refresh_ms) < PH_BLOCKLIST_REFRESH_MS) {
    return true;
  }

  strncpy(ph_active_blocklist_url, ph_blocklist_url, sizeof(ph_active_blocklist_url) - 1);
  ph_active_blocklist_url[sizeof(ph_active_blocklist_url) - 1] = '\0';

  bool isHttps = (strncmp(ph_blocklist_url, "https://", 8) == 0);
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setTimeout(12000);
  if (isHttps) secureClient.setInsecure();
  bool beginOk = isHttps
                 ? http.begin(secureClient, ph_blocklist_url)
                 : http.begin(plainClient, ph_blocklist_url);
  if (!beginOk) {
    snprintf(ph_last_error, sizeof(ph_last_error), "Blocklist begin failed");
    ph_last_blocklist_refresh_ms = now;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(ph_last_error, sizeof(ph_last_error), "Blocklist HTTP %d", code);
    http.end();
    ph_last_blocklist_refresh_ms = now;
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t totalBytes = 0;
  unsigned long lastDataAt = millis();
  phClearBlocklistDomainCache();
  while ((http.connected() || stream->available()) &&
         totalBytes < PH_BLOCKLIST_MAX_BYTES &&
         ph_blocklist_domain_cache_count < PH_MAX_WATCH_DOMAINS) {
    if (!stream->available()) {
      if ((millis() - lastDataAt) > 1500) break;
      delay(10);
      continue;
    }
    String line = stream->readStringUntil('\n');
    totalBytes += line.length() + 1;
    lastDataAt = millis();
    phAddBlocklistLine(line);
  }
  http.end();

  ph_last_blocklist_refresh_ms = now;
  phRebuildMergedWatchDomains();
  Serial.printf("[PiHole] Blocklist merged: %d imported, %d total watched domains\n",
                ph_blocklist_domain_count, ph_watch_domain_count);
  return true;
}

static bool phFetchNetworkDevices(bool force = false) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') return false;

  unsigned long now = millis();
  if (!force && ph_last_seen_refresh_ms != 0 &&
      (now - ph_last_seen_refresh_ms) < PH_SEEN_REFRESH_MS) {
    return true;
  }

  String payload;
  int code = phGet("/api/network/devices", payload);
  if (code != HTTP_CODE_OK) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return false;
  }

  JsonArray devices = doc["devices"].as<JsonArray>();
  if (devices.isNull()) {
    return false;
  }

  String serialized;
  int added = 0;
  for (JsonObject dev : devices) {
    if (added >= PH_MAX_WATCH_DEVICES) break;

    const char *name = dev["name"] | dev["hostname"] | dev["host"] | "";
    const char *mac = dev["hwaddr"] | dev["mac"] | "";

    String ipValue = "";
    JsonVariant ipVariant = dev["ip"];
    if (ipVariant.is<const char*>()) {
      ipValue = String(ipVariant.as<const char*>());
    } else if (ipVariant.is<JsonArray>()) {
      JsonArray ipArray = ipVariant.as<JsonArray>();
      for (JsonVariant entry : ipArray) {
        const char *candidate = entry.as<const char*>();
        if (candidate && candidate[0] != '\0') {
          ipValue = String(candidate);
          break;
        }
      }
    }

    ipValue.trim();
    if (ipValue.length() == 0 && (!name || name[0] == '\0') && (!mac || mac[0] == '\0')) continue;
    if (serialized.length() > 0) serialized += '\n';
    serialized += ipValue;
    serialized += '|';
    serialized += String(name);
    serialized += '|';
    serialized += String(mac);
    added++;
  }

  phSaveSeenDevicesCache(serialized.c_str());
  ph_last_seen_refresh_ms = now;
  Serial.printf("[PiHole] Seen devices cached: %d\n", ph_seen_device_count);
  return true;
}

// ---------------------------------------------------------------------------
// Auth — POST empty (or set) password to /api/auth to get session ID
// ---------------------------------------------------------------------------
static bool phAuthenticate() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  char authUrl[80];
  snprintf(authUrl, sizeof(authUrl), "http://%s:%u/api/auth", ph_pihole_host, ph_pihole_port);

  if (!http.begin(client, authUrl)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "auth begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  char body[80];
  snprintf(body, sizeof(body), "{\"password\":\"%s\"}", ph_pihole_pass);
  int code = http.POST(body);
  ph_last_http_code = code;

  if (code != HTTP_CODE_OK) {
    Serial.printf("[PiHole] Auth HTTP error: %d\n", code);
    snprintf(ph_last_error, sizeof(ph_last_error), "Auth HTTP %d", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "Auth JSON error");
    return false;
  }

  const char *sid = doc["session"]["sid"] | "";
  strncpy(ph_sid, sid, sizeof(ph_sid) - 1);
  ph_sid[sizeof(ph_sid) - 1] = '\0';
  ph_auth_done = true;

  if (ph_sid[0] != '\0') {
    Serial.printf("[PiHole] Auth OK, SID: %.8s...\n", ph_sid);
  } else {
    Serial.println("[PiHole] Auth OK, passwordless — no SID needed");
  }
  return true;
}

// ---------------------------------------------------------------------------
// URL builder — appends ?sid= or &sid= as needed
// path may already contain query params, e.g. "/api/queries?max=10"
// ---------------------------------------------------------------------------
static void phBuildUrl(char *url, size_t urlLen, const char *path) {
  if (ph_sid[0] != '\0') {
    const char *sep = strchr(path, '?') ? "&" : "?";
    snprintf(url, urlLen, "http://%s:%u%s%ssid=%s", ph_pihole_host, ph_pihole_port, path, sep, ph_sid);
  } else {
    snprintf(url, urlLen, "http://%s:%u%s", ph_pihole_host, ph_pihole_port, path);
  }
}

// ---------------------------------------------------------------------------
// Generic authenticated GET — handles auth on first call and 401 retry.
// Retries once after 600 ms on begin() failure (transient socket exhaustion).
// Returns HTTP status code, populates out on 200. Returns -1 on begin() fail.
// ---------------------------------------------------------------------------
static int phGet(const char *path, String &out) {
  if (!ph_auth_done && !phAuthenticate()) return -1;

  char url[200];
  phBuildUrl(url, sizeof(url), path);

  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      Serial.printf("[PiHole] begin() failed, retry %d...\n", attempt);
      delay(attempt * 800);  // 800ms, then 1600ms
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(8000);

    if (!http.begin(client, url)) continue;  // try again with fresh socket

    int code = http.GET();
    ph_last_http_code = code;

    if (code == 401) {
      http.end();
      ph_sid[0]    = '\0';
      ph_auth_done = false;
      if (!phAuthenticate()) return 401;
      phBuildUrl(url, sizeof(url), path);
      WiFiClient client2;
      HTTPClient http2;
      http2.setTimeout(8000);
      if (!http2.begin(client2, url)) continue;
      code = http2.GET();
      ph_last_http_code = code;
      if (code == HTTP_CODE_OK) out = http2.getString();
      http2.end();
      return code;
    }

    if (code == HTTP_CODE_OK) out = http.getString();
    http.end();
    return code;
  }

  return -1;  // both attempts failed
}

// ---------------------------------------------------------------------------
// Fetch last MAX_QUERIES DNS queries (Mode 0)
// ---------------------------------------------------------------------------
static bool phFetch() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  char path[48];
  snprintf(path, sizeof(path), "/api/queries?max=%d", PH_QUERY_FETCH_COUNT);

  String payload;
  int code = phGet(path, payload);
  if (code != HTTP_CODE_OK) {
    if (code == -1) snprintf(ph_last_error, sizeof(ph_last_error), "begin() failed");
    else            snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    snprintf(ph_last_error, sizeof(ph_last_error), "JSON: %s", err.c_str());
    return false;
  }

  JsonArray queries = doc["queries"].as<JsonArray>();
  if (queries.isNull()) {
    snprintf(ph_last_error, sizeof(ph_last_error), "No queries array");
    return false;
  }

  ph_query_count = 0;
  phResetWatchState();
  for (JsonObject row : queries) {
    const char *domain = row["domain"]       | "";
    const char *ip     = row["client"]["ip"] | "";
    const char *clientName = row["client"]["name"] | "";
    const char *status = row["status"]       | "";
    char domainLower[64];
    phLowerCopy(domain, domainLower, sizeof(domainLower));
    bool allowed = phStatusAllowed(status);
    int watchedIndex = phFindWatchedDeviceIndex(ip, clientName);
    bool watchDomainMatch = (ph_watch_domain_count > 0) && phDomainMatchesWatched(domainLower);
    bool watchDeviceMatch = (watchedIndex >= 0);
    bool hasWatchedDevices = (ph_watched_device_count > 0);
    bool eligibleWatchedHit = watchDomainMatch && (watchDeviceMatch || !hasWatchedDevices);
    bool dohHit = allowed && (phDomainIsDohProvider(domainLower)) && (watchDeviceMatch || !hasWatchedDevices);

    if (ph_query_count < MAX_QUERIES) {
      PiQuery &q = ph_queries[ph_query_count++];
      strncpy(q.domain, domain, sizeof(q.domain) - 1); q.domain[sizeof(q.domain)-1] = '\0';
      strncpy(q.client, ip, sizeof(q.client) - 1); q.client[sizeof(q.client)-1] = '\0';
      strncpy(q.client_name, clientName, sizeof(q.client_name) - 1); q.client_name[sizeof(q.client_name)-1] = '\0';
      q.allowed = allowed;
      q.valid   = true;
    }

    if (watchedIndex >= 0 && watchedIndex < ph_watch_summary_count) {
      ph_watch_summaries[watchedIndex].active = true;
    }

    if (eligibleWatchedHit) {
      char label[32];
      phResolveWatchLabel(label, sizeof(label), watchedIndex, ip, clientName);
      phAddWatchHit(label, ip, domain, !allowed, false);
      if (watchedIndex >= 0 && watchedIndex < ph_watch_summary_count) {
        if (allowed) ph_watch_summaries[watchedIndex].slipped_hits++;
        else         ph_watch_summaries[watchedIndex].blocked_hits++;
      }
    }

    if (dohHit) {
      if (watchedIndex >= 0 && watchedIndex < ph_watch_summary_count) {
        ph_watch_summaries[watchedIndex].doh_hits++;
      }
    }
  }

  phBuildSnifferAlerts();
  Serial.printf("[PiHole] Fetched %d queries\n", ph_query_count);
  return true;
}

// ---------------------------------------------------------------------------
// Fetch summary stats (Mode 1) — /api/stats/summary
// ---------------------------------------------------------------------------
static bool phFetchStats() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  String payload;
  int code = phGet("/api/stats/summary", payload);
  if (code != HTTP_CODE_OK) {
    if (code == -1) snprintf(ph_last_error, sizeof(ph_last_error), "begin() failed");
    else            snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "Stats JSON error");
    return false;
  }

  ph_stats.queries_today        = doc["queries"]["total"]                 | 0L;
  ph_stats.blocked_today        = doc["queries"]["blocked"]               | 0L;
  ph_stats.percent_blocked      = doc["queries"]["percent_blocked"]       | 0.0f;
  ph_stats.domains_on_blocklist = doc["gravity"]["domains_being_blocked"] | 0L;
  ph_stats.active_clients       = doc["clients"]["active"]                | 0;
  ph_stats.valid                = true;

  Serial.printf("[PiHole] Stats: %ld total, %ld blocked (%.1f%%)\n",
                ph_stats.queries_today, ph_stats.blocked_today, ph_stats.percent_blocked);
  return true;
}

// ---------------------------------------------------------------------------
// Fetch top blocked domains (Mode 2) — /api/stats/top_blocked
// ---------------------------------------------------------------------------
static bool phFetchTopBlocked() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  char path[64];
  snprintf(path, sizeof(path), "/api/stats/top_domains?blocked=true&count=%d", MAX_TOP_BLOCKED);

  String payload;
  int code = phGet(path, payload);
  if (code != HTTP_CODE_OK) {
    if (code == -1) snprintf(ph_last_error, sizeof(ph_last_error), "begin() failed");
    else            snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "TopBlk JSON error");
    return false;
  }

  JsonArray domains = doc["domains"].as<JsonArray>();
  if (domains.isNull()) {
    snprintf(ph_last_error, sizeof(ph_last_error), "No domains in response");
    return false;
  }

  ph_top_blocked_count = 0;
  for (JsonObject row : domains) {
    if (ph_top_blocked_count >= MAX_TOP_BLOCKED) break;
    PiBlockEntry &e = ph_top_blocked[ph_top_blocked_count++];
    strncpy(e.domain, row["domain"] | "", sizeof(e.domain) - 1);
    e.domain[sizeof(e.domain) - 1] = '\0';
    e.count = row["count"] | 0L;
    e.valid = true;
  }

  Serial.printf("[PiHole] Top blocked: %d entries\n", ph_top_blocked_count);
  return true;
}

// ---------------------------------------------------------------------------
// Top clients (Mode 3) — /api/stats/top_clients
// ---------------------------------------------------------------------------
#define MAX_TOP_CLIENTS 10

struct PiClientEntry {
  char name[48];
  long count;
  bool valid;
};

static PiClientEntry ph_top_clients[MAX_TOP_CLIENTS];
static int           ph_top_clients_count = 0;

static bool phFetchTopClients() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  char path[64];
  snprintf(path, sizeof(path), "/api/stats/top_clients?count=%d", MAX_TOP_CLIENTS);

  String payload;
  int code = phGet(path, payload);
  if (code != HTTP_CODE_OK) {
    if (code == -1) snprintf(ph_last_error, sizeof(ph_last_error), "begin() failed");
    else            snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "TopClt JSON error");
    return false;
  }

  JsonArray clients = doc["clients"].as<JsonArray>();
  if (clients.isNull()) {
    snprintf(ph_last_error, sizeof(ph_last_error), "No clients in response");
    return false;
  }

  ph_top_clients_count = 0;
  for (JsonObject row : clients) {
    if (ph_top_clients_count >= MAX_TOP_CLIENTS) break;
    PiClientEntry &e = ph_top_clients[ph_top_clients_count++];
    const char *name = row["name"] | "";
    const char *ip   = row["ip"]   | "";
    const char *display = (name[0] != '\0') ? name : ip;
    strncpy(e.name, display, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.count = row["count"] | 0L;
    e.valid = true;
  }

  Serial.printf("[PiHole] Top clients: %d entries\n", ph_top_clients_count);
  return true;
}

// ---------------------------------------------------------------------------
// Activity history (Mode 4) — /api/history, 10-min buckets over 24h (144 pts)
// ---------------------------------------------------------------------------
#define MAX_HISTORY 144

struct PiHistoryPoint {
  int total;
  int blocked;
};

static PiHistoryPoint ph_history[MAX_HISTORY];
static int            ph_history_count = 0;

static bool phFetchHistory() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  String payload;
  int code = phGet("/api/history", payload);
  if (code != HTTP_CODE_OK) {
    if (code == -1) snprintf(ph_last_error, sizeof(ph_last_error), "begin() failed");
    else            snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "History JSON error");
    return false;
  }

  JsonArray hist = doc["history"].as<JsonArray>();
  if (hist.isNull()) {
    snprintf(ph_last_error, sizeof(ph_last_error), "No history in response");
    return false;
  }

  ph_history_count = 0;
  for (JsonObject pt : hist) {
    if (ph_history_count >= MAX_HISTORY) break;
    ph_history[ph_history_count].total   = pt["total"]   | 0;
    ph_history[ph_history_count].blocked = pt["blocked"] | 0;
    ph_history_count++;
  }

  Serial.printf("[PiHole] History: %d points\n", ph_history_count);
  return true;
}

// ---------------------------------------------------------------------------
// Returns just the last octet of an IP string ("192.168.0.5" -> "5")
// ---------------------------------------------------------------------------
static void phLastOctet(const char *ip, char *buf, size_t bufLen) {
  const char *last = strrchr(ip, '.');
  if (last && *(last + 1) != '\0') {
    strncpy(buf, last + 1, bufLen - 1);
    buf[bufLen - 1] = '\0';
  } else {
    strncpy(buf, ip, bufLen - 1);
    buf[bufLen - 1] = '\0';
  }
}
