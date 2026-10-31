# York AC Remote: phone app with an ESP32 Wi-Fi to IR bridge

Your phone opens a remote-control app over home Wi-Fi. An ESP32 (about RM20–30) sits in the room and
sends the infrared codes to the York aircond. This works with any phone (iPhone or Android)
and doesn't need an IR blaster in the phone.

```
Phone (app) --Wi-Fi--> ESP32 bridge --infrared--> York aircond
                         ^ IR receiver (optional): learns/follows your original remote
```

## 1. Parts

| Part | Notes |
|---|---|
| ESP32 dev board (ESP32-WROOM DevKit) | any ESP32 works |
| 940 nm IR LED ×1–2 | 5 mm |
| NPN transistor 2N2222 / S8050 | drives the LED harder for range |
| Resistors: 1 kΩ (base), 33–47 Ω (LED) | |
| IR receiver VS1838B or TSOP38238 (optional, recommended) | for auto-detect and syncing with the physical remote |
| USB 5 V adapter + cable | power |

## 2. Wiring

```
ESP32 GPIO 4 ──[1kΩ]── Base (2N2222)
                        Collector ──[IR LED cathode(-)]  IR LED anode(+) ──[33Ω]── 5V (VIN)
                        Emitter ── GND

IR receiver (VS1838B, looking at the dome):  OUT ── GPIO 14,  GND ── GND,  VCC ── 3.3V
```
Point the IR LED at the aircond's indoor unit (it has the small receiver window). Line of sight up to about 5–8 m.

## 3. Flash the firmware

1. Install **Arduino IDE 2**. Under *Boards Manager*, install **esp32 by Espressif**.
2. Under *Library Manager*, install **IRremoteESP8266** (version **2.8.5 or newer**).
3. Open `york_ac_remote/york_ac_remote.ino` (keep `web_ui.h` in the same folder).
4. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top. Your router must be on **2.4 GHz** (ESP32 can't use 5 GHz).
5. Select board **ESP32 Dev Module**, then click Upload. Open Serial Monitor at 115200 to see the address.

If Wi-Fi fails, the ESP32 starts its own hotspot **York-AC-Setup** (password `yorkac123`), and the app is at `http://192.168.4.1`.

## 4. Install the app on your phone

1. On home Wi-Fi, open **http://york-ac.local** (or the IP from Serial Monitor, e.g. `http://192.168.1.50`).
   Some older Android phones don't support `.local`. If yours doesn't, use the IP and set a DHCP reservation for it in your router.
2. Add it to your home screen:
   - **iPhone (Safari):** Share → *Add to Home Screen*
   - **Android (Chrome):** ⋮ → *Add to Home screen*

It then opens full-screen like a normal app.

## 5. Match your York model (important)

York uses different remote protocols on different models. In Malaysia, many York units come from the Daikin/OYL
family and use **Daikin-style** codes (remote ECGS01-i). Older units use the **York GRYLH2A** protocol.

- **Easiest:** In the app, tap ⚙ → **Detect from my York remote**. Then point your original remote at the IR receiver and press power. The bridge identifies the protocol and saves it.
- **Manual:** In ⚙, pick a protocol and tap **Send test signal**. If the aircond beeps, keep that one. If not, try the next (YORK → DAIKIN → DAIKIN2 → DAIKIN176/152/128/64 → COOLIX/MIDEA → GREE).

## Features

- Power, 16–30 °C, Cool / Dry / Fan / Auto / Heat, fan Auto/Low/Med/High
- Swing, Turbo, Eco, Sleep (only sent if your unit's protocol supports them)
- Off timer (30 min to 8 h), run by the ESP32, so it works with your phone off
- Follows the physical remote: if someone uses the original remote, the app updates within about 5 s (needs the IR receiver)
- Remembers the last settings and protocol after a power cut
- Demo mode: if you open `index.html` directly, you can try the UI without hardware

## Notes and limits

- **York GRYLH2A protocol:** Temperature range is 18–32 °C. It uses a *power toggle*, so the bridge tracks on/off itself. If the app and aircond ever get out of sync (e.g. after a power cut), tap power once more.
- The app only works on your home network. Remote access from outside the house isn't included (it would need a VPN such as Tailscale, or an MQTT/Home Assistant setup).
- To change the app's design, edit `index.html`, then run `python3 make_web_ui.py` to rebuild `web_ui.h` and upload again.

## API (for Home Assistant / automation)

```
GET  /api/state                     -> current state JSON
POST /api/state   power=1&mode=cool&temp=24&fan=auto&swing=0&turbo=0&econo=0&sleep=0&timer=60
POST /api/config  protocol=DAIKIN&name=Bedroom
POST /api/learn   (start 20 s listen)    GET /api/learn -> detection result
```
