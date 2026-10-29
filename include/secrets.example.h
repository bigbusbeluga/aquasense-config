#pragma once

// Copy this file to include/secrets.h and enter your device credentials there.
// Networks are tried in order. Empty SSIDs are skipped.
const WiFiNetwork WIFI_NETWORKS[] = {
  {"", ""}, // First 2.4 GHz Wi-Fi network: {"SSID", "password"}
  {"", ""}, // Second network
  {"", ""}, // Third network
};

// Channel Write API Key, from ThingSpeak > Channels > My Channels > API Keys.
const char* THINGSPEAK_WRITE_API_KEY = "";
