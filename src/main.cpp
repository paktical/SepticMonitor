#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_ADS1X15.h>

#include "config.h"

// Verbose serial output is dev-only; release builds compile it away.
#if LOG_VERBOSE
#define LOGF(...) Serial.printf(__VA_ARGS__)
#define LOGLN(x)  Serial.println(x)
#else
#define LOGF(...) do {} while (0)
#define LOGLN(x)  do {} while (0)
#endif

Adafruit_ADS1115 ads;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastPublish = 0;


bool readChipTempC(float &outC)
{
    float c = temperatureRead();   // built into the Arduino-ESP32 core

    // 53.33 C is the classic "sensor not present / invalid" sentinel
    // (raw 128 converted). Treat NaN or that exact value as invalid.
    if (isnan(c) || fabsf(c - 53.33f) < 0.01f) return false;

    outC = c;
    return true;
}


int measureSpread()
{
    int16_t vmin = 32767, vmax = -32768;
    int count = 0;
    unsigned long start = millis();
    while (millis() - start < SAMPLE_MS)
    {
        int16_t raw = ads.readADC_SingleEnded(0);
        if (raw < vmin) vmin = raw;
        if (raw > vmax) vmax = raw;
        count++;
    }
    return vmax - vmin;
}

const char* wifiStatusName(wl_status_t s)
{
    switch (s)
    {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (network not seen -- wrong name, or 5 GHz only)";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED (usually a bad password)";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

const char* authName(wifi_auth_mode_t m)
{
    switch (m)
    {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3_PSK (transition mode)";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI_PSK";
        default:                        return "?";
    }
}

// Lists what the radio can actually see, so a band/security problem is obvious.
// Records the strongest matching AP so we can associate with it directly.
uint8_t bestBssid[6];
int32_t bestChannel = 0;
bool    haveBest    = false;

void scanNetworks()
{
    Serial.println("WiFi: scanning for visible 2.4 GHz networks...");
    int n = WiFi.scanNetworks();
    if (n <= 0)
    {
        Serial.println("WiFi: scan found NOTHING -- antenna or radio init problem.");
        return;
    }

    int32_t bestRssi = -1000;
    haveBest = false;

    for (int i = 0; i < n; i++)
    {
        Serial.printf("  [%2d] %-24s rssi=%4d ch=%2d auth=%s\n",
                      i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                      authName(WiFi.encryptionType(i)));

        if (WiFi.SSID(i) == WIFI_SSID && WiFi.RSSI(i) > bestRssi)
        {
            bestRssi    = WiFi.RSSI(i);
            bestChannel = WiFi.channel(i);
            memcpy(bestBssid, WiFi.BSSID(i), 6);
            haveBest = true;
        }
    }

    if (haveBest)
    {
        Serial.printf("WiFi: strongest \"%s\" is %02X:%02X:%02X:%02X:%02X:%02X "
                      "on ch %d (rssi %d)\n",
                      WIFI_SSID, bestBssid[0], bestBssid[1], bestBssid[2],
                      bestBssid[3], bestBssid[4], bestBssid[5],
                      (int)bestChannel, (int)bestRssi);
    }
    WiFi.scanDelete();
}

// The ESP-IDF disconnect reason is the only thing that distinguishes "wrong
// password" from "AP kicked us" -- Arduino reports all of them as status 6.
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        uint8_t r = info.wifi_sta_disconnected.reason;
        const char* meaning = "see esp_wifi_types.h";
        switch (r)
        {
            case 2:   meaning = "AUTH_EXPIRE"; break;
            case 4:   meaning = "ASSOC_EXPIRE"; break;
            case 15:  meaning = "4WAY_HANDSHAKE_TIMEOUT -- almost always a WRONG PASSWORD"; break;
            case 39:  meaning = "TIMEOUT -- AP stopped responding mid-association, NOT a password problem"; break;
            case 200: meaning = "BEACON_TIMEOUT"; break;
            case 201: meaning = "NO_AP_FOUND"; break;
            case 202: meaning = "AUTH_FAIL -- WRONG PASSWORD"; break;
            case 203: meaning = "ASSOC_FAIL"; break;
            case 204: meaning = "HANDSHAKE_TIMEOUT"; break;
            case 205: meaning = "CONNECTION_FAIL"; break;
        }
        Serial.printf("WiFi: disconnected, reason=%u (%s)\n", r, meaning);
    }
}

void connectWifi()
{
    Serial.printf("WiFi: connecting to \"%s\" (password %u chars)\n",
                  WIFI_SSID, (unsigned)strlen(WIFI_PASSWORD));

    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // power save can stall association on mesh APs

    // Print the MAC so this board can be identified in the router's client list.
    Serial.printf("WiFi: this board's MAC is %s\n", WiFi.macAddress().c_str());

    // Scan first so we can target the strongest AP by BSSID. With several APs
    // sharing this SSID, letting the driver pick can land us on a weak one that
    // times out mid-handshake (reason 39).
    scanNetworks();

    unsigned long attemptStart = millis();
    if (haveBest)
    {
        Serial.printf("WiFi: associating with strongest AP on ch %d\n", (int)bestChannel);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, bestChannel, bestBssid);
    }
    else
    {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);

        // Give up on this attempt and report the real reason, then retry.
        if (millis() - attemptStart >= WIFI_CONNECT_TIMEOUT_MS)
        {
            wl_status_t s = WiFi.status();
            Serial.printf("WiFi: attempt timed out after %lu ms. status=%d %s\n",
                          (unsigned long)WIFI_CONNECT_TIMEOUT_MS, (int)s, wifiStatusName(s));
            // Alternate between BSSID-locked and driver's-choice association so
            // the log shows which of the two actually works.
            static bool lockToBest = false;
            lockToBest = !lockToBest;

            // Back off between attempts. Hammering a mesh AP with association
            // requests can get the MAC temporarily rate-limited or blocked,
            // which turns a transient failure into a persistent one.
            static unsigned long backoffMs = 5000;
            scanNetworks();
            WiFi.disconnect();
            Serial.printf("WiFi: backing off %lu ms before next attempt\n", backoffMs);
            delay(backoffMs);
            if (backoffMs < 60000) backoffMs *= 2;

            attemptStart = millis();

            if (lockToBest && haveBest)
            {
                Serial.println("WiFi: retrying, locked to strongest AP...");
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD, bestChannel, bestBssid);
            }
            else
            {
                Serial.println("WiFi: retrying, driver's choice of AP...");
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            }
            continue;
        }
        LOGF(".");
    }

    Serial.printf("\nWiFi: connected. IP=%s  gateway=%s  rssi=%d  ch=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.channel());
}

void connectMqtt()
{
    while (!mqtt.connected())
    {
        LOGF("MQTT: connecting to %s:%d ...\n", MQTT_BROKER, MQTT_PORT);

        // Last Will: if we drop unexpectedly, broker publishes "offline" for us
        bool ok = mqtt.connect(
            DEVICE_ID,
            nullptr, nullptr,          // no username/password
            TOPIC_STATUS, 1, true,     // will topic, QoS1, retained
            "offline"                  // will message
        );

        if (ok)
        {
            LOGLN("MQTT: connected");
            // Announce we're online (retained so late subscribers see it)
            mqtt.publish(TOPIC_STATUS, "online", true);
        }
        else
        {
            LOGF("MQTT: failed, rc=%d  retrying in %lu ms\n",
                 mqtt.state(), (unsigned long)MQTT_RETRY_MS);
            delay(MQTT_RETRY_MS);
        }
    }
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(2000);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!ads.begin(ADS1115_I2C_ADDR))
    {
        Serial.println("ERROR: ADS1115 not found!");
        while (1) delay(10);
    }
    ads.setDataRate(RATE_ADS1115_860SPS);
    ads.setGain(GAIN_ONE);

    connectWifi();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    connectMqtt();

    LOGF("Setup complete. device=%s fw=%s topics=%s\n",
         DEVICE_ID, FW_VERSION, TOPIC_PREFIX);
}

void loop()
{
    // Keep connections alive
    if (WiFi.status() != WL_CONNECTED) connectWifi();
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();

    // Publish on interval
    if (millis() - lastPublish >= PUBLISH_MS)
    {
        lastPublish = millis();

        int spread = measureSpread();
        const char* pumpState = (spread >= SPREAD_THRESHOLD) ? "RUNNING" : "STOPPED";

        // Rough amps estimate (placeholder until real calibration)
        float currentRms = spread * 0.0025f;

        JsonDocument doc;
        doc["deviceId"]      = DEVICE_ID;
        doc["fwVersion"]     = FW_VERSION;
        doc["uptimeSeconds"] = millis() / 1000;
        doc["wifiRssi"]      = WiFi.RSSI();
        doc["spreadRaw"]     = spread;
        doc["currentRms"]    = currentRms;
        doc["pumpState"]     = pumpState;

        // Chip (die) temperature — only include if a valid reading exists
        float chipTempC;
        if (readChipTempC(chipTempC))
        {
            doc["chipTempC"] = chipTempC;
            doc["chipTempF"] = chipTempC * 9.0f / 5.0f + 32.0f;
        }

        char buffer[256];
        size_t n = serializeJson(doc, buffer);

        mqtt.publish(TOPIC_TELEMETRY, buffer, n);
        LOGF("Published: %s\n", buffer);
    }
}