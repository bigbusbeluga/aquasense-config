# AquaSense ESP32 and ThingSpeak

The ESP32 reads its sensors locally and uploads the latest readings to ThingSpeak
over verified HTTPS every 20 seconds while connected to the internet. No phone or
laptop is needed after the firmware has been configured and uploaded.

## Configure

1. Open this folder in Antigravity with PlatformIO.
2. Copy `include/secrets.example.h` to `include/secrets.h` if needed. In the local
   `secrets.h`, fill at least one row of `WIFI_NETWORKS` with your 2.4 GHz
   SSID and password. Networks are tried in list order at startup. Empty SSIDs
   are skipped. After connecting, the device can reconnect to that selected network.
3. Sign in to [ThingSpeak](https://thingspeak.mathworks.com/) and create a channel
   under **Channels > My Channels > New Channel**. Enable and name fields 1–6
   exactly as shown below, then save the channel.
4. Open that channel's **API Keys** tab. Copy its **Write API Key** into
   `THINGSPEAK_WRITE_API_KEY` in `include/secrets.h`. This local credentials file
   is ignored by Git; the repository contains only the empty example. A channel ID is not needed for
   this REST endpoint: the Write API Key selects the destination channel.
5. Build and upload with PlatformIO. Open the serial monitor at **115200 baud**.
   A successful request prints `ThingSpeak: uploaded entry ...`.
6. Open the channel's **Private View** to see its charts.

| ThingSpeak field | Label | Unit |
| --- | --- | --- |
| 1 | pH | pH |
| 2 | TDS | ppm |
| 3 | Turbidity | NTU |
| 4 | Water temperature | degrees C |
| 5 | Estimated DO saturation | mg/L |
| 6 | Derived EC | microSiemens/cm |

DO saturation is calculated from temperature and TDS, and EC is calculated from
TDS. DO saturation is the equilibrium oxygen estimate, not a measurement of the
water's actual dissolved oxygen. Both fields are derived values.
If the temperature sensor fails, fields 4 and 5 are left blank for that entry
and the entry status reports `temperature_sensor_error`.

## Upload behavior

- Local sensor readings and LCD updates continue between uploads. An HTTPS
  request can briefly delay the next reading while it completes or times out.
- All six values are written together as one ThingSpeak message. Upload attempts
  are spaced at least 20 seconds apart, including after a failed request.
- The device synchronizes its clock using NTP for TLS certificate validation.
  If synchronization is unavailable, uploads wait while local readings continue.
- With an empty Write API Key, uploads are disabled. With no connected network,
  the device continues taking readings offline.
- Offline readings are not buffered or uploaded later. Each upload uses the latest
  readings. A subsequent upload is attempted after a failure; an uncertain network
  response can mean that the server accepted an entry without confirming it.
- `include/thingspeak_ca.h` contains the trusted DigiCert Global Root G2 certificate
  used to validate `api.thingspeak.com`. Refresh this certificate if ThingSpeak
  changes its root certificate chain.

## Free option

ThingSpeak offers a free option for noncommercial small projects, with up to
4 channels, 3 million messages per year, and a minimum 15-second update interval.
A message can contain up to 8 fields; this project's six fields fit in one message.
This project's 20-second upload interval is within that rate limit.

Sources: [ThingSpeak licensing FAQ](https://thingspeak.mathworks.com/pages/license_faq)
and [REST write API](https://www.mathworks.com/help/thingspeak/writedata.html).
