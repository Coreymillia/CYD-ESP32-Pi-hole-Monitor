#pragma once

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MAX_QUERIES     10
#define MAX_TOP_BLOCKED 10

// ---------------------------------------------------------------------------
// Live query data (Mode 0)
// ---------------------------------------------------------------------------
struct PiQuery {
  char domain[64];
  char client[16];
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
// Shared state
// ---------------------------------------------------------------------------
static int  ph_last_http_code = 0;
static char ph_last_error[48] = "";
static char ph_sid[65]        = "";
static bool ph_auth_done      = false;

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

// ---------------------------------------------------------------------------
// Auth — POST empty (or set) password to /api/auth to get session ID
// ---------------------------------------------------------------------------
static bool phAuthenticate() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  char authUrl[80];
  snprintf(authUrl, sizeof(authUrl), "http://%s/api/auth", ph_pihole_host);

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
    snprintf(url, urlLen, "http://%s%s%ssid=%s", ph_pihole_host, path, sep, ph_sid);
  } else {
    snprintf(url, urlLen, "http://%s%s", ph_pihole_host, path);
  }
}

// ---------------------------------------------------------------------------
// Generic authenticated GET — handles auth on first call and 401 retry
// Returns HTTP status code, populates out on 200. Returns -1 on begin() fail.
// ---------------------------------------------------------------------------
static int phGet(const char *path, String &out) {
  if (!ph_auth_done && !phAuthenticate()) return -1;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  char url[200];
  phBuildUrl(url, sizeof(url), path);
  if (!http.begin(client, url)) return -1;

  int code = http.GET();
  ph_last_http_code = code;

  if (code == 401) {
    http.end();
    ph_sid[0]    = '\0';
    ph_auth_done = false;
    if (!phAuthenticate()) return 401;
    phBuildUrl(url, sizeof(url), path);
    if (!http.begin(client, url)) return -1;
    code = http.GET();
    ph_last_http_code = code;
  }

  if (code == HTTP_CODE_OK) out = http.getString();
  http.end();
  return code;
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
  snprintf(path, sizeof(path), "/api/queries?max=%d", MAX_QUERIES);

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
  for (JsonObject row : queries) {
    if (ph_query_count >= MAX_QUERIES) break;
    const char *domain = row["domain"]       | "";
    const char *ip     = row["client"]["ip"] | "";
    const char *status = row["status"]       | "";

    PiQuery &q = ph_queries[ph_query_count++];
    strncpy(q.domain, domain, sizeof(q.domain) - 1); q.domain[sizeof(q.domain)-1] = '\0';
    strncpy(q.client, ip,     sizeof(q.client) - 1); q.client[sizeof(q.client)-1] = '\0';
    q.allowed = phStatusAllowed(status);
    q.valid   = true;
  }

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
