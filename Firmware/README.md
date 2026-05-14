## MeteoNode Firmware

This is the firmware for the MeteoNode. The core idea is to wake up, read the sensors, pull a regional forecast from Open-Meteo, adjust the rain probability using local pressure and humidity trends, and go back to sleep. Everything runs on the ESP32 locally, no external processing.

---

## Libraries

Install these through PlatformIO or the Arduino Library Manager. If you're using the platformio.ini it will handle this automatically.

- Adafruit BMP280
- Adafruit Unified Sensor
- HTU21D (enjoyneering's version)
- ArduinoJson v7

---

## Setup

At the top of `Firmware.ino` there are defines for WiFi credentials and the node's coordinates. These need to be set before flashing.

```cpp
#define WIFI_SSID        "YOUR_SSID"
#define WIFI_PASSWORD    "YOUR_PASSWORD"

#define LATITUDE         "Node's Latitude"
#define LONGITUDE        "Node's Longitude"
```

The coordinates are passed directly to the Open-Meteo API, so they should be as accurate as possible for the forecast to be useful. `SLEEP_SECONDS` is 15 minutes by default. `LATCH_HOLD_PIN` is GPIO5, change it if the 2N7000 gate is wired differently.

---

## Wiring

**I2C**  Both sensors share the same bus.
- SDA  GPIO 21
- SCL  GPIO 22
- BMP280 default address is 0x76, code tries 0x77 as a fallback
- HTU21D is always 0x40

**Battery ADC**  100K-100K voltage divider into GPIO34.
```
BAT+  100K  GPIO34  100K  GND
```

**Soft latch**  GPIO5 held HIGH keeps the system on. When battery voltage drops below the critical threshold the firmware pulls it LOW, the MOSFET latch opens, and power is cut completely. After that it requires a manual restart.

---

## Battery Voltage and the ADC Problem

Since the ESP32 is powered directly from the LiFePO4 cell there is no stable 3.3V reference for the ADC. The supply voltage shifts between roughly 3.0V and 3.6V depending on charge state, which would make a fixed-reference ADC reading meaningless.

To fix this the firmware reads the ESP32's internal 1.1V bandgap reference on every wake cycle and uses that to figure out what VREF actually is at that moment. The battery voltage is then calculated using the calibrated VREF rather than assuming a fixed value. It's not laboratory accurate but it's consistent and close enough to catch low voltage events reliably.

Thresholds:
- Below 2.90V  cuts power via latch
- Below 3.00V  skips WiFi, reads sensors and sleeps
- Normal otherwise

---

## Wake Cycle

Every time the ESP32 wakes from deep sleep it runs through this sequence:

1. Hold latch pin HIGH immediately so the system stays on
2. Read battery voltage, cut power or skip WiFi if needed
3. Read BMP280 and HTU21D over I2C
4. Push readings into the RTC ring buffer
5. Connect to WiFi and fetch Open-Meteo forecast if battery allows
6. Calculate adjusted local rain probability
7. Print report to serial
8. Sleep for `SLEEP_SECONDS`

The RTC ring buffer stores the last 4 pressure, humidity, and temperature readings across sleep cycles. It needs at least 2 samples before trend analysis activates. At 15 minute intervals that means one hour of history to work with.

---

## Forecast Adjustment

Open-Meteo provides a regional rain probability. The local sensors can sometimes pick up changes faster, pressure drops in particular tend to precede incoming weather before the regional model updates. The firmware calculates a slope across the RTC history for pressure, humidity, and temperature and uses that to shift the Open-Meteo probability up or down.

The adjustment runs in logit space rather than raw percentage so the result stays naturally bounded between 0 and 100 without any clamping.

The next 3 hourly values from Open-Meteo are averaged to smooth out short-term noise before the adjustment is applied.

---

## Serial Output

Baud rate is 115200. A typical cycle looks like this:
 
```
boot 4
bat: 3.31V
P=1008.42 hPa T=27.3 C H=81.2% Bat=3.31V
wifi connecting.... ok
forecast: rain=42.0% temp=28.1C hum=79.0%
--- meteonode ---
boot #4
battery: 3.31V
pressure: 1008.42 hPa
temp: 27.3C
humidity: 81.2%
open-meteo rain: 42.0%
local adjusted: 61.3%
condition: Likely Rain
----------------
sleeping 900s
```
 
Disconnect serial before deploying outside. Keeping UART active draws extra current through the whole sleep cycle.
 
---
 
## Known Issues
 
- The bandgap VREF calibration works well at room temperature but I haven't tested it under direct sun heat. It might drift more than expected.
- WiFi retries every wake cycle even after repeated failures. A backoff or failure counter would be better but I haven't added it yet.
- No OTA support, firmware updates require a physical connection.
- ArduinoJson v7 deprecates `DynamicJsonDocument` in favour of `JsonDocument`. It still compiles and works fine for now but should be updated at some point.
