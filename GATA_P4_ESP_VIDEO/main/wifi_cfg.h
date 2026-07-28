/**
 * @file wifi_cfg.h
 * @brief Wi-Fi AP credentials and network config.
 *
 * Kept separate from capture code so network settings can be changed independently.
 * In AP mode, the C6 creates the network and the iPhone connects to it.
 */

#pragma once

#define WIFI_AP_SSID "GATA_P4_VIDEO"
#define WIFI_AP_PASSWORD "gatap4cam"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONN 4
#define WIFI_AP_IP "192.168.4.1"

/** UDP video port announced via mDNS and used by the streaming socket. */
#define VIDEO_UDP_PORT 3334

/** UDP port for AI bounding box metadata (sent alongside video). */
#define BBOX_UDP_PORT 3335
