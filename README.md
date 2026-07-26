# AC Condensate Auto-Drain

ESP32 controller that empties an air-conditioner condensate tray on its own,
with a web dashboard and Telegram control.

Two reed float switches watch the tray. At **70%** the pump runs a 6-minute
cycle and repeats while the float stays wet. At **90%** the overflow handler
takes over: pump forced on, red LED, buzzer, Telegram alert — and if a full
6-minute run does not drop the level, the pump is stopped and flagged as
blocked, because at that point it clearly isn't draining and the pump is the
thing worth protecting. A manual rocker switch runs the pump for exactly as
long as it is held closed, no timer.

Priority is strict: **overflow > manual switch > web/Telegram manual > auto cycle.**

## Hardware

| Signal | GPIO | Notes |
| --- | --- | --- |
| Reed float, 70% | 33 | `INPUT_PULLUP`, closes to GND |
| Reed float, 90% | 25 | `INPUT_PULLUP`, closes to GND |
| Manual rocker switch | 26 | `INPUT_PULLUP`, closes to GND |
| Pump relay IN | 23 | Active-low by default — see `RELAY_ACTIVE_LOW` |
| Green LED | 19 | Lit while the relay is energised |
| Red LED | 18 | Lit while the 90% float is wet |
| Buzzer (BC337 base via 1k) | 22 | Sounds during overflow |
| White LED | — | Wired straight to 3V3 through a resistor |

The white LED deliberately uses no GPIO. It is lit whenever the board has
power, so it cannot lie by staying on after a crash.

If your relay module energises on `IN=HIGH`, flip `RELAY_ACTIVE_LOW` to `false`.

## Setup

1. Install the **Universal Telegram Bot** (Brian Lough) and **ArduinoJson**
   libraries, plus the ESP32 board package.
2. Copy the credentials template and fill it in:
   ```sh
   cp sketch_jul24a/secrets.example.h sketch_jul24a/secrets.h
   ```
3. Open `sketch_jul24a/sketch_jul24a.ino`, select an ESP32 board, upload.
4. The serial monitor (115200) prints the dashboard URL. The bot also messages
   it to you on boot.

`secrets.h` is gitignored.

## Web

| Route | |
| --- | --- |
| `GET /` | Dashboard: tank level, live state, run countdown, controls |
| `GET /api/status` | JSON status |
| `POST /pump/on` | Manual run, capped at 6 minutes |
| `POST /pump/off` | Stop immediately |

The dashboard polls `/api/status` every 2s, pauses while the tab is hidden, and
disables its buttons whenever the overflow handler or the manual switch owns
the pump — the same checks the HTTP handlers enforce, so the UI never offers a
control that the controller would refuse.

## Telegram

`/status` `/pumpon` `/pumpoff` `/uptime` — attached as a tap keyboard, so
nothing needs typing. Messages from any chat other than `CHAT_ID` are ignored.

```
🟢 Auto cycle
Pump     on
Level    70% draining
Switch   open
Run      4m 12s left
Total    2h 38m
Uptime   6h 05m
```

## Notes

- All timing is `millis()`-based; a slow Telegram poll cannot stretch a pump run.
- The page is served from `PROGMEM` and the JSON is built with `snprintf` into a
  fixed buffer, so the 2-second poll loop does not fragment the heap over months
  of uptime.
- WiFi reconnects on its own. If it never comes back, the reed and pump logic
  keeps working — the network is only for monitoring and overrides.
