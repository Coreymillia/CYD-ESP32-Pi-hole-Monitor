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
static char ph_pihole_host[64]  = "";  // Pi-hole IP or hostname
static char ph_pihole_pass[64]  = "";  // Pi-hole admin password (empty = passwordless)
static bool ph_has_settings     = false;  // true if SSID + Pi-hole host are saved
static bool ph_force_portal     = false;  // set by long-press to force setup on next boot

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

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
  ph_has_settings = (ssid.length() > 0 && pihost.length() > 0);
  ph_force_portal = force;
}

static void phSaveSettings(const char *ssid, const char *pass,
                           const char *pihost, const char *pipass) {
  Preferences prefs;
  prefs.begin("cydpihole", false);
  prefs.putString("ssid",   ssid);
  prefs.putString("pass",   pass);
  prefs.putString("pihost", pihost);
  prefs.putString("pipass", pipass);
  prefs.end();

  strncpy(ph_wifi_ssid,   ssid,   sizeof(ph_wifi_ssid)   - 1);
  strncpy(ph_wifi_pass,   pass,   sizeof(ph_wifi_pass)   - 1);
  strncpy(ph_pihole_host, pihost, sizeof(ph_pihole_host) - 1);
  strncpy(ph_pihole_pass, pipass, sizeof(ph_pihole_pass) - 1);
  ph_has_settings = true;
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

  if (ph_has_settings) {
    gfx->setTextColor(0x07E0);  // green
    gfx->setCursor(4, 152);
    gfx->print("Existing settings found. Tap");
    gfx->setCursor(4, 164);
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
    "hr{border:1px solid #113355;margin:20px 0;}"
    "</style></head><body>"
    "<h1>&#128273; CYDPiHole Setup</h1>"
    "<p>Enter your WiFi credentials to connect to Pi-hole.</p>"
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
  html += "' placeholder='e.g. 192.168.0.103' maxlength='63' required>"
    "<label>Pi-hole Password"
    " <span style='color:#445566;font-weight:normal'>(leave blank if none)</span>:</label>"
    "<input type='password' name='pipass' value='";
  html += String(ph_pihole_pass);
  html += "' placeholder='Leave blank if passwordless' maxlength='63'>"
    "<br><button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
    "</form>";
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

  phSaveSettings(ssid.c_str(), pass.c_str(), pihost.c_str(), pipass.c_str());

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}"
    "p{color:#88aacc;}</style></head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>WiFi: <b>" + ssid + "</b></p>"
    "<p>Pi-hole: <b>" + pihost + "</b></p>"
    "<p>You can close this page and disconnect from <b>CYDPiHole_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
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
