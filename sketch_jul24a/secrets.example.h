#pragma once

/*
 * Copy this file to secrets.h and fill in your own values.
 * secrets.h is gitignored - real credentials never get committed.
 */
#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
#define BOT_TOKEN     "000000000:XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"   // from @BotFather
#define CHAT_ID       "000000000"                                        // from @userinfobot

// Dashboard login. Optional, and optional only because a LAN-only controller
// can reasonably go without. REQUIRED before you expose it beyond the LAN by
// any route - every control on that page switches a mains relay. Comment both
// lines out to go back to an open dashboard.
#define WEB_USER      "admin"
#define WEB_PASSWORD  "pick-another-long-one"
