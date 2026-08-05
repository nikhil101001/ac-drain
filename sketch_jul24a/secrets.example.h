#pragma once

/*
 * Copy this file to secrets.h and fill in your own values.
 * secrets.h is gitignored - real credentials never get committed.
 */
#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
#define BOT_TOKEN     "000000000:XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"   // from @BotFather
#define CHAT_ID       "000000000"                                        // from @userinfobot

// Over-the-air updates. OTA_PASSWORD is required - the sketch will not compile
// without it, because an open OTA port hands the pump, the relay and the WiFi
// credentials to anyone on the network. Pick something long; you type it once,
// and ./flash --ota reads it from here.
#define OTA_PASSWORD  "pick-a-long-random-one"
#define OTA_HOSTNAME  "ac-drain"    // dashboard also answers at ac-drain.local

// Dashboard login. Optional, and optional only because a LAN-only controller
// can reasonably go without. REQUIRED before you expose it beyond the LAN by
// any route - every control on that page switches a mains relay. Comment both
// lines out to go back to an open dashboard.
#define WEB_USER      "admin"
#define WEB_PASSWORD  "pick-another-long-one"
