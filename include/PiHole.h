#pragma once

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MAX_QUERIES   10

struct PiQuery {
  char domain[64];
  char client[16];  // full IP string
  bool allowed;     // true = allowed, false = blocked
  bool valid;       // false = slot unused
};

static PiQuery ph_queries[MAX_QUERIES];
static int     ph_query_count = 0;
static int     ph_last_http_code = 0;  // set on each fetch attempt
static char    ph_last_error[48] = "";
static char    ph_sid[65] = "";        // Pi-hole v6 session ID (empty = no auth needed)
static bool    ph_auth_done = false;   // true once we've completed the auth handshake

// Returns true if a Pi-hole v6 status string represents an allowed query.
// Allowed: FORWARDED, CACHE, CACHE_STALE, RETRIED, RETRIED_DNSSEC, IN_PROGRESS
static bool phStatusAllowed(const char *status) {
  return (strncmp(status, "FORWARDED",  9) == 0 ||
          strncmp(status, "CACHE",      5) == 0 ||
          strncmp(status, "RETRIED",    7) == 0 ||
          strcmp (status, "IN_PROGRESS")   == 0);
}

// POST /api/auth with an empty password to obtain a session ID (Pi-hole v6).
// Returns true and populates ph_sid on success.
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

// Fetch the last MAX_QUERIES queries from Pi-hole v6.
// Populates ph_queries[] and ph_query_count.
// Returns true on success.
static bool phFetch() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (ph_pihole_host[0] == '\0') {
    snprintf(ph_last_error, sizeof(ph_last_error), "No Pi-hole host set");
    return false;
  }

  // Authenticate if we haven't done the auth handshake yet
  if (!ph_auth_done) {
    if (!phAuthenticate()) return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  char url[128];
  if (ph_sid[0] != '\0') {
    snprintf(url, sizeof(url),
             "http://%s/api/queries?max=%d&sid=%s",
             ph_pihole_host, MAX_QUERIES, ph_sid);
  } else {
    snprintf(url, sizeof(url),
             "http://%s/api/queries?max=%d",
             ph_pihole_host, MAX_QUERIES);
  }

  if (!http.begin(client, url)) {
    snprintf(ph_last_error, sizeof(ph_last_error), "http.begin() failed");
    return false;
  }

  int code = http.GET();
  ph_last_http_code = code;

  // Session expired — re-authenticate once and retry
  if (code == 401) {
    http.end();
    ph_sid[0] = '\0';
    ph_auth_done = false;
    if (!phAuthenticate()) return false;
    if (ph_sid[0] != '\0') {
      snprintf(url, sizeof(url),
               "http://%s/api/queries?max=%d&sid=%s",
               ph_pihole_host, MAX_QUERIES, ph_sid);
    } else {
      snprintf(url, sizeof(url),
               "http://%s/api/queries?max=%d",
               ph_pihole_host, MAX_QUERIES);
    }
    if (!http.begin(client, url)) {
      snprintf(ph_last_error, sizeof(ph_last_error), "http.begin() failed");
      return false;
    }
    code = http.GET();
    ph_last_http_code = code;
  }

  if (code != HTTP_CODE_OK) {
    Serial.printf("[PiHole] HTTP error: %d\n", code);
    snprintf(ph_last_error, sizeof(ph_last_error), "HTTP %d", code);
    http.end();
    return false;
  }

  // Read full payload before parsing — more reliable than streaming on ESP32
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[PiHole] JSON parse error: %s\n", err.c_str());
    snprintf(ph_last_error, sizeof(ph_last_error), "JSON: %s", err.c_str());
    return false;
  }

  JsonArray queries = doc["queries"].as<JsonArray>();
  if (queries.isNull()) {
    Serial.println("[PiHole] No 'queries' array in response");
    snprintf(ph_last_error, sizeof(ph_last_error), "No queries array");
    return false;
  }

  ph_query_count = 0;
  for (JsonObject row : queries) {
    if (ph_query_count >= MAX_QUERIES) break;

    // v6 row format: {domain, client:{ip, name}, status, type, ...}
    const char *domain = row["domain"]      | "";
    const char *ip     = row["client"]["ip"] | "";
    const char *status = row["status"]      | "";

    PiQuery &q = ph_queries[ph_query_count++];
    strncpy(q.domain, domain, sizeof(q.domain) - 1);
    q.domain[sizeof(q.domain) - 1] = '\0';
    strncpy(q.client, ip, sizeof(q.client) - 1);
    q.client[sizeof(q.client) - 1] = '\0';
    q.allowed = phStatusAllowed(status);
    q.valid   = true;
  }

  Serial.printf("[PiHole] Fetched %d queries\n", ph_query_count);
  return true;
}

// Returns just the last octet of an IP string, e.g. "192.168.0.5" -> "5"
// Result is written into buf (must be at least 4 bytes).
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
