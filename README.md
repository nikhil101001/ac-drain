# AC Condensate Auto-Drain

ESP32 controller that empties an air-conditioner condensate tray on its own,
with a web dashboard and Telegram control.

Two reed float switches watch the tray. At **70%** the pump runs a **4 min**
cycle and repeats while the float stays wet. At **90%** the overflow handler
takes over: pump forced on, red LED, beeping buzzer, Telegram alert — and if a
full **5 min** run does not drop the level, the pump is stopped and flagged as
blocked, because at that point it clearly isn't draining and the pump is the
thing worth protecting.

The overflow run gets the longer cap on purpose. It is a deadline on one
uninterrupted run rather than a repeating cycle, there is more water to shift at
90%, and cutting it off at the normal 4 min would report a blockage that is
really just a bigger job.

Both times are **set from the dashboard**, not the source. They are stored on the
controller and survive a power cut; 4 min and 5 min are only what a freshly
flashed board starts with. See [Run times](#run-times).

The **manual switch is an enable switch, not a run switch.** In its OFF position
the pump stops immediately and automatic operation is suspended. The run timer is
*frozen, not reset* — turn the switch back on with a float still wet and the pump
finishes the remainder of the run rather than starting over. A 90% overflow
ignores the switch entirely; water damage outranks it.

Priority is strict: **overflow > manual switch OFF > web/Telegram manual > auto cycle.**

## Hardware

| Signal | GPIO | Notes |
| --- | --- | --- |
| Reed float, 70% | 33 | `INPUT_PULLUP`, closes to GND. Two wires only: GND and GPIO33. |
| Reed float, 90% | 25 | `INPUT_PULLUP`, closes to GND. Two wires only: GND and GPIO25. |
| Manual switch | 26 | `INPUT_PULLUP`, closes to GND — see `SWITCH_CLOSED_IS_ENABLED` |
| Pump relay IN | 23 | **Active-high on this build** — see `RELAY_ACTIVE_LOW`. **Also needs a 10k pull-down to GND.** |
| Green LED | 19 | Lit while the relay is energised |
| Red LED | 18 | Lit while the 90% float is wet |
| Buzzer (BC337 base via 1k) | 22 | Sounds during overflow, plus one short self-test beep at boot |
| White LED | — | Wired straight to 3V3 through a resistor |

**LED polarity:** anode (+) to the GPIO through the resistor, cathode (−) to GND.
Wired the other way round — anode to GND, cathode to the pin — they are
reverse-biased and never light, in any state. 220–470 Ω suits a 3.3 V pin; 1 kΩ
gives only ~1.5 mA and is very dim.

The white LED deliberately uses no GPIO. It is lit whenever the board has
power, so it cannot lie by staying on after a crash.

**The resistor on the relay input is not optional, and its direction depends on
your module.** ESP32 GPIOs are floating inputs from power-on until `setup()`
runs, and again through every reset and every flash. Nothing holds the relay line
at its safe level during that window, so a resistor has to. `setup()` parks the
pin at its idle level before calling `pinMode(OUTPUT)`, which covers everything
after boot; the resistor covers the boot itself.

| Module type | `RELAY_ACTIVE_LOW` | Boot resistor on IN |
| --- | --- | --- |
| Energises on `IN=HIGH` (**this build**) | `false` | 10k **pull-down to GND** |
| Energises on `IN=LOW` | `true` | 10k **pull-up to 3V3** |

Get this backwards and the resistor holds the relay *on* through every boot,
which is the exact fault it exists to prevent. Identifying the module is easy:
with the pump on the relay's **NO** terminal and the controller idle, the pump
should be off. If it runs continuously and ignores every stop command, the
polarity is wrong.

If your switch reads backwards, flip `SWITCH_CLOSED_IS_ENABLED`. Its default —
closed contacts mean enabled — puts a broken switch wire on the inhibited side,
where the 90% handler still protects the tray *and* still raises an alert, rather
than failing silently.

### Power wiring

- **Return the pump's negative directly to the SMPS −V**, not to the buck
  converter's output ground rail. Amps of pump current sharing the conductor the
  ESP32 uses as its 0 V reference causes ground bounce: random resets and
  phantom float reads.
- **The flyback diode goes across the pump**, cathode to +, anode to −. In
  series it either blocks the pump entirely or does nothing.
- Bulk capacitance on the 5 V rail (470–1000 µF) plus 100 nF at the ESP32. A
  relay coil energising on the same rail is a classic brownout-reset.
- Size the DC fuse to roughly 2× pump stall current. A 10 A fuse on a condensate
  pump will not blow before the wiring does.

## Setup

1. Install the **Universal Telegram Bot** (Brian Lough) and **ArduinoJson**
   libraries, plus the ESP32 board package.
2. Copy the credentials template and fill it in:
   ```sh
   cp sketch_jul24a/secrets.example.h sketch_jul24a/secrets.h
   ```
3. Flash it:
   ```sh
   ./flash main
   ```
4. The bot messages you the dashboard URL on boot. The sketch also reports on
   `Serial` at 115200 — boot, WiFi, switch and pump transitions — but it only
   prints on events, so a monitor opened on an idle controller looks dead until
   something happens. The dashboard and Telegram are the primary interfaces.

`secrets.h` is gitignored. What goes in it:

| Define | |
| --- | --- |
| `WIFI_SSID`, `WIFI_PASSWORD` | Required |
| `BOT_TOKEN`, `CHAT_ID` | Required — from @BotFather and @userinfobot |
| `WEB_USER`, `WEB_PASSWORD` | Optional on a trusted LAN. **Required** before exposing the dashboard beyond it |

## Run times

Both run caps are set from the **Run times** card on the dashboard, in seconds,
and stored in NVS on the controller — so retiming the pump needs no reflash and
survives a power cut.

This is the one thing on the board that is deliberately written to flash. The
event log refuses to persist for the opposite reason: it would write every few
minutes forever, whereas these are written only when someone moves the number.

- **Auto cycle** — every ordinary timed run: the 70% cycle and any manual run
  from the dashboard or Telegram.
- **Overflow cap** — the deadline on the single run at 90%. Past it, the pump is
  stopped and the level is reported as a blockage.

Anything from **10 s to 15 min** is accepted. The ceiling is not arbitrary: the
cap is the only thing that stops the pump running dry once the tray has emptied,
so there is no "no limit" option. Values outside the range are rejected whole
rather than half-applied, and a stored value that falls outside the range of the
firmware reading it — after a downgrade, say — is discarded for the default
rather than trusted.

A change takes effect immediately, including on a run already in progress:
shorten the cap below what has already elapsed and the pump stops on the next
pass. That is the behaviour you want while standing over the tray with a
stopwatch, which is the only reason to be on this card.

Changes are recorded in the activity log, so a pump that starts behaving
differently can be traced to the moment someone retimed it.

```
POST /api/config?run=240&overflow=300   # either argument, or both
POST /api/config?reset=1                # back to the firmware defaults
```

## Flashing

`./flash` wraps compile, upload and monitor. It finds `arduino-cli` inside the
Arduino IDE bundle, so a plain IDE install needs nothing extra, and it detects
the USB port and each sketch's baud rate on its own.

```sh
./flash              # menu of every sketch, then flash and monitor
./flash relay        # fuzzy match: "relay" finds relay_test
./flash main         # alias for sketch_jul24a
./flash relay -n     # flash without opening the monitor
./flash -m relay     # monitor only
./flash -r           # hard reset, restarting whatever is flashed
./flash -l           # sketches, ports and detected toolchain
```

Overrides when auto-detection is not what you want: `--port`, `--baud`,
`--fqbn`, or the `AC_PORT` and `AC_FQBN` environment variables.

**To watch a sketch boot, press EN on the board with the monitor already open.**
Opening the monitor does not reset the ESP32, and `-r` has to close the monitor
to reach the port, so boot output is gone before it reopens. Do not run a
monitor and a reset against the same port at once — they fight over DTR/RTS and
can leave the board silent and looking bricked until a clean upload clears it.

## Web

| Route | |
| --- | --- |
| `GET /` | Dashboard: tank level, live state, run countdown, run times, activity log, controls |
| `GET /api/status` | JSON status |
| `GET /api/log` | Event log; `?since=<seq>` for an incremental fetch |
| `GET /api/log.csv` | The same log as a CSV download |
| `POST /pump/on` | Manual run, capped at the auto-cycle length |
| `POST /pump/off` | Stop immediately |
| `POST /api/config` | Set the run caps — `?run=<sec>&overflow=<sec>`, or `?reset=1` |

The dashboard polls `/api/status` every 2s, pauses while the tab is hidden, and
disables its buttons whenever the overflow handler or the manual switch owns
the pump — the same checks the HTTP handlers enforce, so the UI never offers a
control that the controller would refuse. It states no duration of its own
either: every run length on the page comes from the device, so retiming the pump
cannot leave the UI confidently quoting the old number.

### Login

Setting `WEB_USER` and `WEB_PASSWORD` in `secrets.h` puts every route above
behind HTTP **digest** auth — digest rather than basic, so the password is not
sent in the clear on each of the ~1800 polls an hour this page makes.

That protects the credential, not the traffic. The page and its JSON are still
plain HTTP, so encryption has to come from whatever you put in front of it.

Leave both undefined and the dashboard is open, which is a defensible choice on a
LAN you trust. The boot log says so out loud every time, because it is an easy
decision to forget you made.

## Reaching it from outside the house

Telegram already works from anywhere and costs nothing to set up — it is an
outbound connection, so there is no port open and nothing to attack. `/status`,
`/log`, `/pumpon` and `/pumpoff` cover everything except the run-time card and
the CSV download. If that is enough, stop here.

For the dashboard itself, put something in front of it. In order of preference:

**1. Tailscale (or any WireGuard VPN).** Best answer if you have anything
always-on at home — a Pi, a NAS, a mini PC, or a router with Tailscale support.
Nothing is exposed to the public internet, the traffic is encrypted end to end,
and the ESP32 needs no changes: it never knows it is happening.

Run it on that machine as a [subnet
router](https://tailscale.com/kb/1019/subnets) advertising your LAN:

```sh
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --advertise-routes=192.168.1.0/24 --accept-dns=false
```

Then approve the route in the Tailscale admin console (Machines → that node →
Edit route settings), install Tailscale on your phone, and the dashboard answers
at the controller's LAN address from anywhere.

Give the ESP32 a static DHCP lease on the router first, so the address it
answers on cannot change under you.

**2. Cloudflare Tunnel.** Also needs a small always-on machine to run
`cloudflared`, and gives you HTTPS on a real hostname plus [Cloudflare
Access](https://developers.cloudflare.com/cloudflare-one/policies/access/) in
front — email or SSO login before anyone reaches the pump. Choose this over
Tailscale if you want to hand access to someone without installing a VPN on
their phone. Still no inbound port on your router.

**3. Port-forwarding the ESP32 to the internet. Don't.** It is plain HTTP with a
mains relay on the end of it, on a device with no security updates and a TCP
stack sized for a microcontroller. Residential IPs are scanned continuously;
this would be found in hours. If you take nothing else from this section: not
this one.

Both real options need a second always-on device on the network, which is not a
limitation of this project so much as of what an ESP32 can be asked to do
safely. If you don't have one, Telegram is the honest answer.

## Event log

The controller records the events that matter and nothing else: boot, pump start
(with what triggered it — auto cycle, manual, the manual switch, or overflow),
pump stop with run duration, overflow began and cleared, blockage detected,
manual switch turned off/on, WiFi lost/restored, firmware updated, and run times
changed. No per-poll or per-debounce noise.

It lives in a **fixed 256-entry ring buffer in RAM** — 2 KB, statically
allocated, oldest entry overwritten. Nothing is written to flash: this device
switches a relay every few minutes and persisting each event would burn flash
endurance for no benefit.

The device is the single source of truth, so every phone and laptop that opens
the dashboard sees the same history rather than its own partial copy in
`localStorage`. Clients fetch incrementally with `?since=<seq>`, so the
2-second poll costs about 70 bytes once caught up.

The tradeoff of staying out of flash is that the log starts empty after a power
cycle. `GET /api/log.csv` (the **Download CSV** link on the dashboard) exists to
archive it off-device before that happens. At roughly 20 events a day, 256
entries is about two weeks of history; raise `LOG_CAPACITY` if you want more —
keep it a power of two, since the ring index is a mask rather than a modulo.

Timestamps come from SNTP at boot. The device stores only uptime seconds per
event plus a single boot epoch, and deliberately keeps that epoch in **UTC** with
no timezone applied — the browser localises it, so there is no offset to
misconfigure on the device. Until the clock syncs (usually ~5 s), the dashboard
labels entries relative to boot.

## Telegram

`/status` `/log` `/pumpon` `/pumpoff` `/uptime` — attached as a tap keyboard, so
nothing needs typing. Messages from any chat other than `CHAT_ID` are ignored.

```
🟢 Auto cycle
Pump     on
Level    70% draining
Switch   on (auto)
Run      2m 48s left
Runs     12, 0 overflow
Total    2h 38m
Uptime   6h 05m
```

`/log` returns the last 8 events with relative ages, which is why the chat
replies need no timezone handling:

```
📋 Recent activity
2m       pump off, 4m 0s run
8m       pump on (auto)
1h 04m   powered on
```

## Notes

- All timing is `millis()`-based; a slow Telegram poll cannot stretch a pump run.
- The page is served from `PROGMEM` and the JSON is built with `snprintf` into a
  fixed buffer, so the 2-second poll loop does not fragment the heap over months
  of uptime.
- WiFi reconnects on its own. If it never comes back, the reed and pump logic
  keeps working — the network is only for monitoring and overrides.
- Nothing in the firmware states a run length of its own. Both caps are rendered
  to a string once, wherever they change, and every serial line, Telegram reply,
  HTTP response and dashboard label quotes that — so a retimed pump cannot leave
  something behind confidently claiming the old figure.
