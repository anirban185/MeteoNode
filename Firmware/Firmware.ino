#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_BMP280.h>
#include <HTU21D.h>
#include <esp_adc_cal.h>

// WiFi credentials

#define WIFI_SSID        "YOUR_SSID"
#define WIFI_PASSWORD    "YOUR_PASSWORD"

// Node coordinates

#define LATITUDE         "Node's Latitude"
#define LONGITUDE        "Node's Longitude"

// Deep sleep intervals

#define SLEEP_SECONDS    (15 * 60)

#define BAT_ADC_PIN      34
#define BAT_DIVIDER_RATIO 2.0f
#define ADC_MAX          4095.0f

// battery voltage thresholds

#define BAT_CRITICAL_V   2.90f
#define BAT_LOW_V        3.00f

// Soft-latch hold pin

#define LATCH_HOLD_PIN   5

// Trend history storing in RTC

#define HISTORY_LEN      4

// RTC Memory

RTC_DATA_ATTR float  rtc_pressure[HISTORY_LEN]    = {0};
RTC_DATA_ATTR float  rtc_humidity[HISTORY_LEN]    = {0};
RTC_DATA_ATTR float  rtc_temperature[HISTORY_LEN] = {0};
RTC_DATA_ATTR uint8_t rtc_head                    = 0;
RTC_DATA_ATTR uint8_t rtc_count                   = 0;
RTC_DATA_ATTR uint32_t rtc_boot_count             = 0;

// Globals

Adafruit_BMP280 bmp;
HTU21D          htu;

struct SensorData {
  float pressure_hpa;
  float temperature_c;
  float humidity_pct;
  float battery_v;
  bool  sensor_ok;
};

struct ForecastData {
  float rain_chance;
  float forecast_temp_c;
  float forecast_humidity;
  bool  fetch_ok;
};

struct LocalForecast {
  float adjusted_rain_chance;
  String condition_label;
};

// battery 

float readBatteryVoltage() {
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += adc1_get_raw(ADC1_CHANNEL_6);  // GPIO 34
    delay(2);
  }
  uint32_t raw_avg = sum / 16;

  uint32_t mv = esp_adc_cal_raw_to_voltage(raw_avg, &adc_chars);
  return (mv / 1000.0f) * BAT_DIVIDER_RATIO;
}

void cutPower() {
  Serial.println("battery critical, cutting power");
  Serial.flush();
  delay(100);
  digitalWrite(LATCH_HOLD_PIN, LOW);
  esp_sleep_enable_timer_wakeup(6ULL * 3600ULL * 1000000ULL);
  esp_deep_sleep_start();
}

SensorData readSensors() {
  SensorData d = {};
  
  // Sensors

  bool bmp_ok = bmp.begin(0x76);
  if (!bmp_ok) bmp_ok = bmp.begin(0x77);
  bool htu_ok = htu.begin();

  if (!bmp_ok || !htu_ok) {
    Serial.printf("sensor init fail: BMP=%d HTU=%d\n", bmp_ok, htu_ok);
    d.sensor_ok = false;
    return d;
  }

  bmp.setSampling(
    Adafruit_BMP280::MODE_FORCED,
    Adafruit_BMP280::SAMPLING_X2,
    Adafruit_BMP280::SAMPLING_X16,
    Adafruit_BMP280::FILTER_X4,
    Adafruit_BMP280::STANDBY_MS_1
  );

  delay(100);

  d.pressure_hpa   = bmp.readPressure() / 100.0f;
  d.temperature_c  = htu.readTemperature();
  d.humidity_pct   = htu.readHumidity();
  // battery_v is set in setup() to avoid redundant ADC reads
  d.sensor_ok      = true;

  Serial.printf("P=%.2f hPa T=%.2f C H=%.1f%% Bat=%.2fV\n",
    d.pressure_hpa, d.temperature_c, d.humidity_pct, d.battery_v);

  return d;
}

// Trend Analysis

void updateHistory(float pressure, float humidity, float temperature) {
  rtc_pressure[rtc_head]    = pressure;
  rtc_humidity[rtc_head]    = humidity;
  rtc_temperature[rtc_head] = temperature;
  rtc_head = (rtc_head + 1) % HISTORY_LEN;
  if (rtc_count < HISTORY_LEN) rtc_count++;
}

float calcTrend(float* buf, uint8_t count) {
  if (count < 2) return 0.0f;
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
  for (uint8_t i = 0; i < count; i++) {
    uint8_t idx = (rtc_head - count + i + HISTORY_LEN) % HISTORY_LEN;
    sum_x  += i;
    sum_y  += buf[idx];
    sum_xy += i * buf[idx];
    sum_x2 += i * i;
  }
  float denom = (count * sum_x2 - sum_x * sum_x);
  if (fabsf(denom) < 1e-6f) return 0.0f;
  return (count * sum_xy - sum_x * sum_y) / denom;
}

// Open-Meteo Fetch

ForecastData fetchForecast() {
  ForecastData f = {};

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
               LATITUDE + "&longitude=" + LONGITUDE +
               "&hourly=precipitation_probability,temperature_2m,relative_humidity_2m"
               "&forecast_hours=3&timezone=Asia%2FKolkata";

  WiFiClientSecure client;
  client.setInsecure();          
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("http fail: %d\n", code);
    http.end();
    f.fetch_ok = false;
    return f;
  }

  WiFiClient* stream = http.getStreamPtr();
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, *stream);
  http.end();

  if (err) {
    Serial.printf("json error: %s\n", err.c_str());
    f.fetch_ok = false;
    return f;
  }

  JsonArray rain_arr  = doc["hourly"]["precipitation_probability"];
  JsonArray temp_arr  = doc["hourly"]["temperature_2m"];
  JsonArray hum_arr   = doc["hourly"]["relative_humidity_2m"];

  float rain_sum = 0, temp_sum = 0, hum_sum = 0;
  int   n = min((int)rain_arr.size(), 3);
  if (n == 0) {
    Serial.println("empty forecast arrays");
    f.fetch_ok = false;
    return f;
  }
  for (int i = 0; i < n; i++) {
    rain_sum += rain_arr[i].as<float>();
    temp_sum += temp_arr[i].as<float>();
    hum_sum  += hum_arr[i].as<float>();
  }

  f.rain_chance      = rain_sum / n;
  f.forecast_temp_c  = temp_sum / n;
  f.forecast_humidity= hum_sum  / n;
  f.fetch_ok         = true;

  Serial.printf("forecast: rain=%.1f%% temp=%.1fC hum=%.1f%%\n",
    f.rain_chance, f.forecast_temp_c, f.forecast_humidity);

  return f;
}

// Bayesian-style Local Adjustment

static float toLogit(float p_pct) {
  float p = constrain(p_pct / 100.0f, 0.001f, 0.999f);
  return logf(p / (1.0f - p));
}

static float fromLogit(float logit) {
  return 100.0f / (1.0f + expf(-logit));
}

LocalForecast adjustForecast(const ForecastData& fc, uint8_t history_count) {
  LocalForecast lf = {};

  if (!fc.fetch_ok) {
    lf.adjusted_rain_chance = 50.0f;
    lf.condition_label      = "No Forecast";
    return lf;
  }

  float logit = toLogit(fc.rain_chance);

  if (history_count >= 2) {
    float dp = calcTrend(rtc_pressure,    history_count);
    float dh = calcTrend(rtc_humidity,    history_count);
    float dt = calcTrend(rtc_temperature, history_count);

// Pressure Trend
    if      (dp < -0.8f) logit += 1.5f;
    else if (dp < -0.3f) logit += 0.6f;
    else if (dp >  0.8f) logit -= 1.2f;
    else if (dp >  0.3f) logit -= 0.4f;

// Humidity Trend
    if      (dh >  5.0f) logit += 0.8f;
    else if (dh >  2.0f) logit += 0.3f;
    else if (dh < -5.0f) logit -= 0.6f;
    else if (dh < -2.0f) logit -= 0.2f;

    float cur_hum = rtc_humidity[(rtc_head - 1 + HISTORY_LEN) % HISTORY_LEN];
    if (dt < -0.5f && cur_hum > 75.0f) logit += 0.4f;
  }

  lf.adjusted_rain_chance = fromLogit(logit);

// Condition label
  float r = lf.adjusted_rain_chance;
  if      (r < 15.0f) lf.condition_label = "Clear";
  else if (r < 35.0f) lf.condition_label = "Partly Cloudy";
  else if (r < 55.0f) lf.condition_label = "Chance of Rain";
  else if (r < 75.0f) lf.condition_label = "Likely Rain";
  else                lf.condition_label = "Rain Expected";

  return lf;
}

// WiFi

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("wifi connecting");
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf(" ok\n");
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" fail");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
}

// Output

void printResult(const SensorData& s, const ForecastData& fc, const LocalForecast& lf) {
  Serial.println("--- meteonode ---");
  Serial.printf("boot #%u\n", rtc_boot_count);
  Serial.printf("battery: %.2fV\n", s.battery_v);
  if (s.sensor_ok) {
    Serial.printf("pressure: %.2f hPa\n", s.pressure_hpa);
    Serial.printf("temp: %.1fC\n", s.temperature_c);
    Serial.printf("humidity: %.1f%%\n", s.humidity_pct);
  }
  if (fc.fetch_ok) {
    Serial.printf("open-meteo rain: %.1f%%\n", fc.rain_chance);
  }
  Serial.printf("local adjusted: %.1f%%\n", lf.adjusted_rain_chance);
  Serial.printf("condition: %s\n", lf.condition_label.c_str());
  Serial.println("----------------");
}

// Setup

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LATCH_HOLD_PIN, OUTPUT);
  digitalWrite(LATCH_HOLD_PIN, HIGH);

  rtc_boot_count++;
  Serial.printf("\nboot %u\n", rtc_boot_count);

  float bat = readBatteryVoltage();
  Serial.printf("bat: %.2fV\n", bat);

  if (bat < BAT_CRITICAL_V) {
    cutPower();
  }

  bool skip_wifi = (bat < BAT_LOW_V);
  if (skip_wifi) {
    Serial.println("low battery, skipping wifi");
  }

  Wire.begin();
  SensorData sensors = readSensors();
  sensors.battery_v = bat;

  if (sensors.sensor_ok) {
    updateHistory(sensors.pressure_hpa, sensors.humidity_pct, sensors.temperature_c);
  }

  ForecastData forecast = {};
  if (!skip_wifi) {
    if (connectWiFi()) {
      forecast = fetchForecast();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }

  LocalForecast local = adjustForecast(forecast, rtc_count);
  printResult(sensors, forecast, local);

  Serial.printf("sleeping %ds\n", SLEEP_SECONDS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
}
