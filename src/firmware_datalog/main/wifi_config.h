/*
 * wifi_config.h - WiFi credentials for the FallSena data-logging firmware.
 *
 * =====================================================================
 *  SET THESE before building. The ESP32-C6 joins this network as a STA
 *  (station/client) and then runs a TCP SERVER on FALLSENA_TCP_PORT so
 *  the host PC (on the same network) can connect and pull trial data.
 *
 *  After boot, the ESP prints its assigned IP address over the USB
 *  serial log, e.g.  "WiFi connected, IP: 192.168.1.42".  Point your
 *  host collector at that IP and port FALLSENA_TCP_PORT (default 3333).
 * =====================================================================
 */

#pragma once

/* <<< SET THESE >>> Your 2.4 GHz WiFi network credentials. */
#define WIFI_SSID   "Sweet-Home"
#define WIFI_PASS   "home-1234-!"

/* TCP port the ESP listens on for the host collector to connect. */
#define FALLSENA_TCP_PORT   3333

/* How many times to retry joining the AP before giving up (then it keeps
 * retrying in the background; recording still works, flush just warns). */
#define FALLSENA_WIFI_MAX_RETRY   10
