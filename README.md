# Icebox Monitor (ESP32)

Monitors a fridge/icebox's fridge and crisper compartment temperatures
(DS18B20 probes) and compressor duty cycle (sensed via a PC817
optocoupler on the 12V solid-state-relay output that drives the
compressor), and serves a live dashboard with 24-hour history graphs at
**http://192.168.8.67/**.

It also pushes its live readings over UDP to a separate Victron BLE
Monitor dashboard board on the same boat network, so everything shows up
in one place — see **How this connects to the Victron dashboard** below.
This board's own page keeps working independently either way.

Wi-Fi credentials go in `include/secrets.h` (gitignored, kept out of
version control) — see step 2 below.

## 1. Install PlatformIO

Easiest path: install the **PlatformIO IDE extension** in VS Code
(Extensions -> search "PlatformIO IDE" -> Install). It will handle the
ESP32 toolchain and libraries automatically based on `platformio.ini`
(`ESPAsyncWebServer`/`AsyncTCP`, `OneWire`, `DallasTemperature`,
`ArduinoJson`).

## 2. Set up your credentials

Wi-Fi credentials live in `include/secrets.h`, which is gitignored so
they never end up committed anywhere. Copy the template and fill it in:

```bash
cp include/secrets.h.example include/secrets.h
```

Then open `include/secrets.h` and fill in:

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

## 3. Check your network range

The static IP is set to `192.168.8.67` with gateway `192.168.8.1` and
subnet `255.255.255.0`. If your router's network isn't `192.168.8.x`,
update these lines near the top of `src/main.cpp`:

```cpp
IPAddress local_IP(192, 168, 8, 67);
IPAddress gateway (192, 168, 8, 1);
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns1    (192, 168, 8, 1);
IPAddress dns2    (8, 8, 8, 8);
```

Make sure `192.168.8.67` isn't already used by another device and isn't
inside your router's DHCP lease range. Also set `GMT_OFFSET_SEC` nearby
to your timezone's offset in seconds, so the history graph's timestamps
are in local time rather than UTC.

## 4. Build & upload

With the project folder open in VS Code / PlatformIO:

- Click the checkmark (Build) icon, then the right-arrow (Upload) icon,
  with the ESP32 plugged in via USB.
- Or from a terminal in this folder: `pio run -t upload`
- Open the Serial Monitor (115200 baud) to watch it connect to Wi-Fi,
  find its temperature probes, and start its telemetry push.

## 5. View the dashboard

Once connected, open:

**http://192.168.8.67/**

It shows one card, auto-refreshing every 5 seconds:

- **Fridge** / **Crisper**: current temperature from each DS18B20 probe.
- **Compressor**: ON/OFF, debounced against sensor chatter.
- **Duty Cycle**: how hard the compressor's working, as a trailing
  4-hour average (see **Duty cycle** below for why not a raw snapshot).
- **Cycles**: how many times the compressor has switched on since boot.
- **Wi-Fi signal**: RSSI in dBm — useful for telling a real problem apart
  from "this board just has weak signal at this spot" if data ever goes
  missing (fridges/coolers are often a rough RF environment, lots of
  metal near the antenna).

Below that are two 24-hour history graphs (temperature, and duty cycle),
and a footer showing uptime and heap health — see **Reliability** below
for what those numbers mean.

## Wiring

### Temperature probes

Two separate DS18B20 probes, each on its **own** 1-Wire bus (not shared,
unlike the Victron dashboard's 3-probes-on-one-bus setup) — one wire
each is simpler here since there's only ever one sensor per bus to worry
about identifying:

| Probe    | ESP32 pin | Notes |
|----------|-----------|-------|
| Fridge   | GPIO4     | `FRIDGE_ONEWIRE_PIN` |
| Crisper  | GPIO16    | `CRISPER_ONEWIRE_PIN` |

Each probe needs a pull-up resistor (~4.7kΩ) from its DATA line to
3.3V, same as any 1-Wire bus. Standard 3-wire hookup: VCC to 3.3V, GND
to GND, DATA to its GPIO (+ the pull-up).

**If you're using a WROVER module** (has PSRAM): move these off
GPIO16/17, since WROVER uses those pins for PSRAM — pick e.g. GPIO25/26
instead, and update `FRIDGE_ONEWIRE_PIN`/`CRISPER_ONEWIRE_PIN`
accordingly. GPIO pins here were otherwise chosen to avoid the ESP32's
strapping pins (0/2/5/12/15).

### Compressor sensing

A PC817 optocoupler isolates the ESP32 from the 12V solid-state relay
output that drives the compressor — LED side across the 12V SSR output
(through a current-limiting resistor sized for 12V), phototransistor
collector to `COMPRESSOR_SENSE_PIN` (GPIO17), emitter to GND. The
ESP32's internal pull-up is enabled in software (`INPUT_PULLUP`), so no
external pull-up resistor is needed on the sense line itself.

**Polarity**: the code currently treats **HIGH** on this pin as
"compressor on" (`raw == HIGH` in `updateCompressorSensing()`). This was
determined empirically against the actual build rather than assumed from
a textbook PC817 circuit — the "obvious" wiring often reads the opposite
way in practice depending on exact component orientation and pull-up
placement. If you rebuild this circuit and the compressor/duty readings
come out inverted (ON when it's actually off), flip that comparison to
`raw == LOW` instead.

A 100ms debounce (`DEBOUNCE_MS`) filters out any switching noise from the
sense line before counting a state change.

## Duty cycle

There are actually three different "duty cycle" numbers in this
project, deliberately different from each other for different purposes:

1. **Raw per-minute value** (`history[].dutyPercent`): computed once a
   minute, this is the literal percentage of that specific minute the
   compressor was on. Stored for all 24 hours of history at full
   resolution - this is what `/api/history` returns.
2. **Live figure** (`dutyPercentLastWindow()` - the dashboard card,
   `/api/live`, and the UDP telemetry push all use this): a trailing
   **4-hour** average of the per-minute values above. A single minute is
   noisy (door opens, ice maker, ambient swings) and not very
   representative of "how hard is this thing working right now" - 4
   hours smooths that out into something meaningful.
3. **Graphed value** (the duty history chart only): a trailing
   **15-minute** average, computed client-side in JavaScript over the
   same raw per-minute data. This one exists because a fridge compressor
   typically cycles over 10-30+ minutes, so at 1-minute resolution
   almost every point lands near 0% or 100% (it's rarely exactly
   mid-cycle when a given minute closes) - which looks like a plain
   on/off square wave rather than a meaningful trend. 15 minutes is
   enough to span a real chunk of a cycle without smoothing away all the
   detail. This constant (`DUTY_SMOOTHING_WINDOW` in the page's
   JavaScript) is easy to retune if 15 minutes ends up too smooth or too
   jumpy once you're looking at real data.

None of this affects what's actually stored on the board - only #2 and
#3 are display/reporting choices layered on top of the same raw #1 data.

## How this connects to the Victron dashboard

This board pushes its live readings to a separate Victron BLE Monitor
dashboard board over UDP unicast every 10 seconds - fire-and-forget, no
TCP connection or socket lifecycle, sent straight to that board's IP on
port **2000** (`DASHBOARD_IP`/`TELEMETRY_UDP_PORT` near the top of
`src/main.cpp`). It shares that port with a diesel heater board that
also pushes telemetry there; the dashboard board tells the two apart by
sender IP.

This is one-directional and this board doesn't care whether the
dashboard board is even running - if it's down or unreachable, sending
UDP packets into the void costs nothing and this board's own dashboard
keeps working normally regardless.

If you ever change this board's own static IP, that's independent of
this - but if the *dashboard* board's static IP changes, `DASHBOARD_IP`
here needs updating to match, or the telemetry has nowhere to go.

## Reliability

This board runs unattended, so it's set up to recover from the most
common failure modes on its own:

- **Task watchdog**: if a single pass through the main loop ever takes
  longer than 20 seconds, the watchdog reboots the board automatically.
  Normal operation never comes close to this.
- **Wi-Fi connect timeout**: at boot, if Wi-Fi hasn't connected within
  60 seconds, the board reboots and tries the whole boot sequence again
  rather than sitting there forever with nothing running.
- **Active reconnect monitoring**: if Wi-Fi drops during normal
  operation, this retries a reconnect every 15 seconds, and does a full
  reboot if it's been continuously down for 3 minutes straight - rather
  than silently sitting disconnected until someone notices the data's
  gone missing.

**Reading the heap footer**: same idea as the Victron dashboard's -
free heap at boot, free heap right now, and the largest allocatable
block. A steadily dropping "now" over many hours with no leveling off
points to a real leak; free heap much bigger than the largest block
means fragmentation, not a leak; both low but stable and close together
is just a tight-but-normal operating baseline.

`/api/history`'s response streams directly rather than being built as
one large in-memory string first, specifically so that endpoint doesn't
need a single large contiguous allocation regardless of how full the
24-hour history buffer gets - that used to be the thing most likely to
fail first as heap got tighter over many hours of uptime, which is what
was causing the history graphs to silently stop appearing without any
other symptom.

## Troubleshooting

- **A temperature reads as "no probe" / stays frozen**: double-check the
  pull-up resistor is actually there for that probe's bus (a very common
  miss - 1-Wire won't work at all without it) and that VCC/GND/DATA are
  wired to the right pins for that specific probe (fridge and crisper
  are on separate buses, so a miswire only affects one of them).
- **Compressor/duty readings look inverted** (shows ON when it's
  actually off, or vice versa): see **Polarity** under Wiring above -
  flip `raw == HIGH` to `raw == LOW` (or back) in
  `updateCompressorSensing()`.
- **Duty cycle looks like it's stuck near 0% or 100%, not a real
  percentage**: expected behavior for the *live* 4-hour figure if the
  board has been up for less than 4 hours (it averages over however much
  history exists so far) - give it more time. If the *graph* looks
  binary/square-wave even after hours of uptime, see **Duty cycle**
  above.
- **History graphs don't appear at all**: check the small error line
  above the card (`#err`) - fetch/parse failures now show up there
  instead of failing silently. If it's empty and the graphs still don't
  render, check the Serial Monitor for anything unusual and confirm
  `/api/history` loads directly in a browser. Also worth knowing: the
  charting library (Chart.js) loads from a CDN
  (`cdn.jsdelivr.net`) rather than being bundled - if the device viewing
  the dashboard is on the boat's local Wi-Fi with no internet uplink,
  the charts won't be able to load at all, even though the card above
  them (fridge/crisper/compressor/duty) will keep working fine, since
  that part doesn't depend on any external library.
- **No data reaching the Victron dashboard**: check this board's own
  Serial Monitor for a "Telemetry UDP -> ..." line at boot (confirms
  it's at least trying), confirm `DASHBOARD_IP` matches that board's
  actual current static IP, and check whether your router has a
  "client isolation"/"AP isolation" setting enabled (blocks
  device-to-device traffic, common on guest networks).
- **Board keeps rebooting**: check the Serial Monitor - a watchdog
  reboot logs a distinctive block before restarting (search for
  `task_wdt`); repeated Wi-Fi-down restarts print "WiFi down for 3+
  minutes" first, which points at a Wi-Fi/RF problem rather than a
  software one.

## Project structure

```
icebox-monitor/
├── platformio.ini      # Board + library config
├── .gitignore           # Keeps include/secrets.h out of version control
├── include/
│   ├── secrets.h.example  # Template - copy to secrets.h and fill in
│   └── secrets.h           # Your real Wi-Fi credentials (gitignored)
├── src/
│   └── main.cpp         # WiFi + temp probes + compressor sensing + web dashboard + history graphs + UDP telemetry push
└── README.md
```
