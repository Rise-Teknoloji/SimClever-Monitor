#ifndef BLE_OTA_H
#define BLE_OTA_H

#include "Arduino.h"
#include <BLEDevice.h>
#include <BLEServer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- FIRMWARE VERSION --- */
#define FW_VERSION "1.0.0"

/* --- OTA BLE UUID'LER --- */
// OTA için ayrı bir servis
#define OTA_SERVICE_UUID      "fb1e4001-54ae-4a28-9f74-dfccb248601d"
#define OTA_CTRL_CHAR_UUID    "fb1e4002-54ae-4a28-9f74-dfccb248601d"  // Kontrol: BEGIN/END/ABORT
#define OTA_DATA_CHAR_UUID    "fb1e4003-54ae-4a28-9f74-dfccb248601d"  // Firmware data chunks
#define OTA_VERSION_CHAR_UUID "fb1e4004-54ae-4a28-9f74-dfccb248601d"  // FW version (READ)

/* --- OTA Durumları --- */
typedef enum {
  OTA_IDLE = 0,
  OTA_IN_PROGRESS,
  OTA_COMPLETE,
  OTA_ERROR,
  OTA_REBOOT_PENDING
} ota_state_t;

/* --- Fonksiyon Prototipleri --- */

/**
 * BLE OTA servisini mevcut BLE Server'a ekler.
 * setup() içinde, BLE Server oluşturulduktan sonra çağrılmalı.
 *
 * @param server Mevcut BLEServer pointer'ı
 */
void ble_ota_setup(BLEServer* server);

/**
 * OTA durumunu kontrol eder. loop() içinde çağrılmalı.
 * OTA tamamlandıysa ESP32'yi yeniden başlatır.
 */
void ble_ota_loop(void);

/**
 * Mevcut OTA durumunu döner.
 */
ota_state_t ble_ota_get_state(void);

/**
 * OTA ilerleme yüzdesini döner (0-100).
 */
uint8_t ble_ota_get_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_OTA_H */
