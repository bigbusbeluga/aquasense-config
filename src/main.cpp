/*****************************************************************
 * AQUASENSE: 6-parameter firmware with 20x4 I2C LCD
 *
 * Physical : DS18B20 (GPIO4), SEN0244 TDS (GPIO35), SEN0161 pH (GPIO34),
 *            SEN0189 turbidity (GPIO32 via 10k/20k divider)
 * Derived  : EC (Eq. 3.1), DO (Eq. 3.2)
 * LCD      : 20x4 I2C (SDA GPIO21, SCL GPIO22) - powered from 5 V!
 *
 * LCD layout:
 *   pH:7.2  TDS:245
 *   Turb:2.1 Temp:25.4C
 *   DO:8.2  EC:487
 *   PASSED:5/6  SAFE
 *
 * platformio.ini lib_deps:
 *   paulstoffregen/OneWire
 *   milesburton/DallasTemperature
 *   duinowitchery/hd44780
 *****************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>   // auto-detects address and pin mapping
#include <math.h>
#include <time.h>
#include "thingspeak_ca.h"

// --- WI-FI ---
// Configure 2.4 GHz networks in include/secrets.h, in priority order.
// Empty SSIDs are skipped. Add or remove rows as needed.
struct WiFiNetwork {
  const char* ssid;
  const char* password;
};
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h" // Empty defaults for a fresh checkout.
#endif
// Allow up to 15 seconds for each listed network.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// --- THINGSPEAK ---
// Paste the channel's Write API Key (not the account key or Read API Key).
// The REST write endpoint identifies the channel from this key; no channel ID is needed.
// THINGSPEAK_WRITE_API_KEY is configured in include/secrets.h.
const unsigned long THINGSPEAK_UPLOAD_INTERVAL_MS = 20000;
static_assert(THINGSPEAK_UPLOAD_INTERVAL_MS >= 15000,
              "ThingSpeak free channels require at least 15 seconds between writes.");

// --- PINS ---
#define ONEWIRE_BUS 4
#define TDS_PIN     35
#define PH_PIN      34
#define TURBID_PIN  32
#define I2C_SDA     21
#define I2C_SCL     22

// --- FILTERING ---
const int   SAMPLE_COUNT = 30;       // odd number
const float EMA_ALPHA    = 0.3f;
const float TURBID_VOLTAGE_DIVIDER_RATIO = 1.5f;   // 10k over 20k
const float KE_FACTOR = 0.64f;       // TDS -> EC factor (Eq. 3.1)

// --- CALIBRATION ---
float TDS_CAL   = 1.0f;              // known_ppm / displayed_ppm
// pH: PH_SLOPE  = (4.0 - 7.0) / (V_at_pH4 - V_at_pH7)
//     PH_OFFSET = 7.0 - PH_SLOPE * V_at_pH7      (voltages in volts)
float PH_SLOPE  = -5.70f;
float PH_OFFSET = 21.34f;

// --- WHO LIMITS ---
const float PH_MIN = 6.5f,  PH_MAX = 8.5f;
const float TURB_MAX = 5.0f;
const float TDS_MAX  = 500.0f;
const float EC_MIN = 200.0f, EC_MAX = 1000.0f;   // use 800 for the stricter limit
const float DO_MIN = 5.0f;
const float TEMP_MIN = 20.0f, TEMP_MAX = 30.0f;

OneWire oneWire(ONEWIRE_BUS);
DallasTemperature tempSensor(&oneWire);
hd44780_I2Cexp lcd;

float waterTempC  = 25.0f;
bool  tempOk      = false;
float smoothedTds = -1.0f, smoothedPh = -1.0f, smoothedNtu = -1.0f;
float smoothedEc  = -1.0f, smoothedDo = -1.0f;

uint32_t medianMilliVolts(uint8_t pin) {
  uint32_t buf[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    buf[i] = analogReadMilliVolts(pin);
    delay(5);
  }
  for (int i = 1; i < SAMPLE_COUNT; i++) {
    uint32_t k = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = k;
  }
  return buf[SAMPLE_COUNT / 2];
}

float ema(float prev, float x) { return (prev < 0) ? x : prev + EMA_ALPHA * (x - prev); }

// Eq. 3.1
float calculateEC(float tds_ppm) {
  if (tds_ppm < 0.0f) tds_ppm = 0.0f;
  return tds_ppm / KE_FACTOR;
}

// Eq. 3.2 (Benson-Krause freshwater saturation + salinity correction)
float calculateDO(float temp_c, float tds_ppm) {
  double TK = temp_c + 273.15;
  double lnDO = -139.34411 + 1.575701e5 / TK - 6.642308e7 / (TK * TK)
                + 1.243800e10 / (TK * TK * TK) - 8.621949e11 / (TK * TK * TK * TK);
  double sal = tds_ppm / 1000.0;
  lnDO -= sal * (0.017674 - 10.754 / TK + 2140.7 / (TK * TK));
  double d = exp(lnDO);
  return d < 0 ? 0.0f : (float)d;
}

// Write one full 20-character row (pads with spaces so old text is overwritten)
void lcdRow(uint8_t row, const char* text) {
  char buf[21];
  snprintf(buf, sizeof buf, "%-20.20s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void connectToKnownWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  bool hasConfiguredNetwork = false;

  for (const WiFiNetwork& network : WIFI_NETWORKS) {
    if (network.ssid[0] == '\0') continue;
    hasConfiguredNetwork = true;

    WiFi.disconnect();
    Serial.printf("Connecting to Wi-Fi: %s", network.ssid);
    WiFi.begin(network.ssid, network.password);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setAutoReconnect(true); // Reconnect only to this successfully selected network.
      Serial.print("Wi-Fi connected. IP address: ");
      Serial.println(WiFi.localIP());
      return;
    }

    WiFi.disconnect();
    Serial.println("Connection failed. Trying the next listed network.");
  }

  WiFi.mode(WIFI_OFF);
  Serial.println(hasConfiguredNetwork
    ? "No listed Wi-Fi network connected. Continuing sensor readings offline."
    : "Wi-Fi list is empty. Continuing sensor readings offline.");
}

void uploadToThingSpeak() {
  if (THINGSPEAK_WRITE_API_KEY[0] == '\0' || WiFi.status() != WL_CONNECTED) return;

  static bool hasAttemptedUpload = false;
  static unsigned long lastAttemptMs = 0;
  unsigned long now = millis();
  if (hasAttemptedUpload && now - lastAttemptMs < THINGSPEAK_UPLOAD_INTERVAL_MS) return;
  hasAttemptedUpload = true;
  lastAttemptMs = now;

  // Certificate verification needs a valid clock. NTP sync runs in the background.
  if (time(nullptr) < 1704067200) { // 2024-01-01 UTC
    Serial.println("ThingSpeak: waiting for internet time synchronization.");
    return;
  }

  // Field order: pH, TDS (ppm), turbidity (NTU), temperature (C),
  // estimated DO saturation (mg/L), derived EC (uS/cm).
  String payload;
  payload.reserve(256);
  payload = "field1=" + String(smoothedPh, 2);
  payload += "&field2=" + String(smoothedTds, 2);
  payload += "&field3=" + String(smoothedNtu, 2);
  if (tempOk) {
    payload += "&field4=" + String(waterTempC, 2);
    payload += "&field5=" + String(smoothedDo, 2);
    payload += "&status=temperature_sensor_ok";
  } else {
    // Leave temperature and DO blank rather than upload the compensation fallback.
    payload += "&status=temperature_sensor_error";
  }
  payload += "&field6=" + String(smoothedEc, 2);

  WiFiClientSecure client;
  client.setCACert(THINGSPEAK_ROOT_CA);
  client.setHandshakeTimeout(5);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, "https://api.thingspeak.com/update")) {
    Serial.println("ThingSpeak: could not initialize HTTPS request.");
    return;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("THINGSPEAKAPIKEY", THINGSPEAK_WRITE_API_KEY);
  int statusCode = http.POST(payload);
  String entryId = statusCode == HTTP_CODE_OK ? http.getString() : String();
  http.end();
  // Wait a full interval after completion, including failed/uncertain writes.
  lastAttemptMs = millis();

  if (statusCode == HTTP_CODE_OK && entryId.toInt() > 0) {
    Serial.printf("ThingSpeak: uploaded entry %ld.\n", entryId.toInt());
  } else {
    Serial.printf("ThingSpeak: upload failed (HTTP/transport code %d). Check Wi-Fi, API key and channel settings.\n",
                  statusCode);
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  delay(1000);

  connectToKnownWiFi();
  if (THINGSPEAK_WRITE_API_KEY[0] != '\0') {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println("ThingSpeak uploads disabled: set THINGSPEAK_WRITE_API_KEY in main.cpp.");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);               // 100 kHz is the safest speed for LCD backpacks
  delay(500);                          // give the LCD time to power up

  int status = lcd.begin(20, 4);       // 0 = OK
  if (status) {
    Serial.printf("LCD init FAILED (status %d). Check 5V power, SDA/SCL wiring.\n", status);
  } else {
    Serial.println("LCD init OK");
    lcd.backlight();
    lcd.clear();
    lcdRow(0, "AQUASENSE");
    lcdRow(1, "Water potability");
    lcdRow(2, "screening device");
    lcdRow(3, "Initializing...");
  }

  tempSensor.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN,    ADC_11db);
  analogSetPinAttenuation(PH_PIN,     ADC_11db);
  analogSetPinAttenuation(TURBID_PIN, ADC_11db);

  delay(1500);
  lcd.clear();
}

void loop() {
  // 1. Temperature
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  tempOk = (t > -55.0f && t < 125.0f && t != 85.0f);
  waterTempC = tempOk ? t : 25.0f;     // 25 C only used for compensation if sensor fails

  // 2. TDS
  float tds_v = medianMilliVolts(TDS_PIN) / 1000.0f;
  float vc = tds_v / (1.0f + 0.02f * (waterTempC - 25.0f));
  float tds = (133.42f * vc * vc * vc - 255.86f * vc * vc + 857.39f * vc) * 0.5f * TDS_CAL;
  if (tds < 0) tds = 0;
  smoothedTds = ema(smoothedTds, tds);

  // 3. pH
  float ph_v = medianMilliVolts(PH_PIN) / 1000.0f;
  float rawPh = constrain(PH_SLOPE * ph_v + PH_OFFSET, 0.0f, 14.0f);
  smoothedPh = ema(smoothedPh, rawPh);

  // 4. Turbidity
  float sensor_v = (medianMilliVolts(TURBID_PIN) / 1000.0f) * TURBID_VOLTAGE_DIVIDER_RATIO;
  float ntu;
  if (sensor_v >= 4.2f)      ntu = 0.0f;
  else if (sensor_v >= 2.5f) ntu = max(0.0f, -1120.4f * sensor_v * sensor_v + 5742.3f * sensor_v - 4352.9f);
  else                       ntu = 3000.0f;
  smoothedNtu = ema(smoothedNtu, ntu);

  // 5. Soft-sensed EC and DO
  smoothedEc = ema(smoothedEc, calculateEC(smoothedTds));
  smoothedDo = ema(smoothedDo, calculateDO(waterTempC, smoothedTds));

  // 6. WHO evaluation (a missing temperature sensor fails Temp and DO)
  bool phPass   = smoothedPh >= PH_MIN && smoothedPh <= PH_MAX;
  bool tdsPass  = smoothedTds <= TDS_MAX;
  bool turbPass = smoothedNtu < TURB_MAX;
  bool tempPass = tempOk && waterTempC >= TEMP_MIN && waterTempC <= TEMP_MAX;
  bool doPass   = tempOk && smoothedDo >= DO_MIN;
  bool ecPass   = smoothedEc >= EC_MIN && smoothedEc <= EC_MAX;

  int passedCount = phPass + tdsPass + turbPass + tempPass + doPass + ecPass;
  bool isSafe = (passedCount >= 5);

  // 7. LCD
  char line[40], tb[10], tp[10], dv[10];
  snprintf(tb, sizeof tb, smoothedNtu < 100.0f ? "%.1f" : "%.0f", smoothedNtu);
  if (tempOk) { snprintf(tp, sizeof tp, "%.1fC", waterTempC); snprintf(dv, sizeof dv, "%.1f", smoothedDo); }
  else        { snprintf(tp, sizeof tp, "ERR");               snprintf(dv, sizeof dv, "ERR"); }

  snprintf(line, sizeof line, "pH:%.1f  TDS:%.0f", smoothedPh, smoothedTds);   lcdRow(0, line);
  snprintf(line, sizeof line, "Turb:%s Temp:%s", tb, tp);                       lcdRow(1, line);
  snprintf(line, sizeof line, "DO:%s  EC:%.0f", dv, smoothedEc);                lcdRow(2, line);
  snprintf(line, sizeof line, "PASSED:%d/6  %s", passedCount, isSafe ? "SAFE" : "UNSAFE"); lcdRow(3, line);

  Serial.printf("pH:%.1f | TDS:%.0f | Turb:%.1f | Temp:%s | DO:%s | EC:%.0f | Passed:%d/6 -> %s\n",
                smoothedPh, smoothedTds, smoothedNtu, tp, dv, smoothedEc, passedCount,
                isSafe ? "SAFE FOR DRINKING" : "UNSAFE");

  uploadToThingSpeak();

  delay(500);
}
