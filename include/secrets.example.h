#pragma once
// =====================================================================
// secrets.example.h — TEMPLATE. Copy this file to "secrets.h" and fill
// in your real values. secrets.h is listed in .gitignore and will
// never be committed, so your Wi-Fi credentials and Google Script URL
// stay out of version control.
//
//   cp include/secrets.example.h include/secrets.h
//
// The firmware checks these at boot and prints a [CONFIG] warning (and
// shows "CHECK secrets.h" on the OLED) if any placeholder is left in.
// =====================================================================

// The network the REAL board joins - usually your phone's hotspot. It
// must be 2.4 GHz and WPA2-Personal: the ESP32 can't join 5 GHz-only,
// WPA2-Enterprise or captive-portal (login page) networks. The name is
// case-sensitive.
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// Your deployed Google Apps Script Web App URL (Deploy > New deployment
// > Web app > "Anyone" access). Ends in /exec.
#define GOOGLE_SCRIPT_URL "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec"

// A shared secret sent with every request so random strangers who guess
// your script URL can't write junk rows into your attendance sheet.
// This MUST match the API_KEY constant in the Apps Script backend
// (see backend/AppsScript.gs).
#define API_KEY "change-this-shared-secret"
