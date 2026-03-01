#include "ble_ota.h"
#include <Update.h>
#include <BLE2902.h>

/* --- Dahili Değişkenler --- */
static ota_state_t  otaState      = OTA_IDLE;
static uint8_t      otaProgress   = 0;
static uint32_t     otaTotalSize  = 0;
static uint32_t     otaReceived   = 0;
static unsigned long rebootTimer  = 0;

static BLECharacteristic* pOtaCtrlChar = nullptr;
static BLECharacteristic* pOtaDataChar = nullptr;
static BLECharacteristic* pOtaVerChar  = nullptr;

/* --- Yardımcı: Client'a durum bildirimi gönder --- */
static void notifyStatus(const char* status) {
  if (pOtaCtrlChar) {
    pOtaCtrlChar->setValue((uint8_t*)status, strlen(status));
    pOtaCtrlChar->notify();
  }
}

/* =====================================================
 * OTA CONTROL CHARACTERISTIC CALLBACK
 * Komutlar:
 *   "BEGIN:123456"  → OTA başlat (firmware boyutu byte cinsinden)
 *   "END"           → OTA sonlandır ve reboot planla
 *   "ABORT"         → OTA iptal et
 *   "VERSION"       → Versiyon bilgisi gönder (opsiyonel, read de var)
 * ===================================================== */
class OtaCtrlCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue();
    if (val.length() == 0) return;

    Serial.print("BLE OTA Ctrl: ");
    Serial.println(val);

    // --- BEGIN:size ---
    if (val.startsWith("BEGIN:")) {
      if (otaState == OTA_IN_PROGRESS) {
        Serial.println("BLE OTA: Zaten devam ediyor, once ABORT gerekli!");
        notifyStatus("ERR:ALREADY_RUNNING");
        return;
      }

      String sizeStr = val.substring(6);
      otaTotalSize = sizeStr.toInt();

      if (otaTotalSize == 0) {
        Serial.println("BLE OTA: Gecersiz boyut!");
        notifyStatus("ERR:INVALID_SIZE");
        return;
      }

      Serial.printf("BLE OTA: Guncelleme basliyor, boyut: %u byte\n", otaTotalSize);

      if (!Update.begin(otaTotalSize)) {
        Serial.println("BLE OTA: Update.begin() basarisiz!");
        Update.printError(Serial);
        otaState = OTA_ERROR;
        notifyStatus("ERR:BEGIN_FAIL");
        return;
      }

      otaReceived = 0;
      otaProgress = 0;
      otaState    = OTA_IN_PROGRESS;
      notifyStatus("OK:BEGIN");
      Serial.println("BLE OTA: Flash'a yazim hazir.");
    }

    // --- END ---
    else if (val == "END") {
      if (otaState != OTA_IN_PROGRESS) {
        Serial.println("BLE OTA: END komutu geldi ama OTA aktif degil!");
        notifyStatus("ERR:NOT_RUNNING");
        return;
      }

      if (Update.end(true)) {  // true = boyut kontrolu yap
        Serial.println("BLE OTA: Guncelleme BASARILI!");
        Serial.printf("BLE OTA: Toplam %u byte yazildi.\n", otaReceived);
        otaState = OTA_REBOOT_PENDING;
        otaProgress = 100;
        notifyStatus("OK:END");
        rebootTimer = millis();  // 2 saniye sonra reboot
      } else {
        Serial.println("BLE OTA: Update.end() BASARISIZ!");
        Update.printError(Serial);
        otaState = OTA_ERROR;
        notifyStatus("ERR:END_FAIL");
      }
    }

    // --- ABORT ---
    else if (val == "ABORT") {
      if (otaState == OTA_IN_PROGRESS) {
        Update.abort();
        Serial.println("BLE OTA: Guncelleme IPTAL edildi!");
      }
      otaState     = OTA_IDLE;
      otaTotalSize = 0;
      otaReceived  = 0;
      otaProgress  = 0;
      notifyStatus("OK:ABORT");
    }

    else {
      Serial.print("BLE OTA: Bilinmeyen komut: ");
      Serial.println(val);
      notifyStatus("ERR:UNKNOWN_CMD");
    }
  }
};

/* =====================================================
 * OTA DATA CHARACTERISTIC CALLBACK
 * Binary firmware chunk'ları burada alınır ve flash'a yazılır.
 * Chunk boyutu: client tarafında max 512 byte önerilir.
 * ===================================================== */
class OtaDataCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (otaState != OTA_IN_PROGRESS) {
      // OTA başlatılmamış, veriyi reddet
      return;
    }

    String val = c->getValue();
    size_t len = val.length();

    if (len == 0) return;

    // Flash'a yaz
    size_t written = Update.write((uint8_t*)val.c_str(), len);
    if (written != len) {
      Serial.printf("BLE OTA: Yazim hatasi! Beklenen: %u, Yazilan: %u\n", len, written);
      Update.printError(Serial);
      otaState = OTA_ERROR;
      notifyStatus("ERR:WRITE_FAIL");
      return;
    }

    otaReceived += len;

    // İlerleme hesapla
    uint8_t newProgress = (uint8_t)((otaReceived * 100UL) / otaTotalSize);
    if (newProgress != otaProgress) {
      otaProgress = newProgress;

      // Her %5'te bir bildir (BLE trafiğini azaltmak için)
      if (otaProgress % 5 == 0 || otaProgress == 100) {
        char progressMsg[16];
        snprintf(progressMsg, sizeof(progressMsg), "PROG:%u", otaProgress);
        notifyStatus(progressMsg);
        Serial.printf("BLE OTA: %%%u (%u / %u byte)\n", otaProgress, otaReceived, otaTotalSize);
      }
    }
  }
};

/* =====================================================
 * PUBLIC API
 * ===================================================== */

void ble_ota_setup(BLEServer* server) {
  if (server == nullptr) {
    Serial.println("BLE OTA: Server null, atlandı!");
    return;
  }

  // OTA Servisi oluştur
  BLEService* pOtaService = server->createService(BLEUUID(OTA_SERVICE_UUID), 20);

  // --- Kontrol Karakteristiği (WRITE + NOTIFY) ---
  pOtaCtrlChar = pOtaService->createCharacteristic(
    OTA_CTRL_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pOtaCtrlChar->setCallbacks(new OtaCtrlCallback());
  pOtaCtrlChar->addDescriptor(new BLE2902());  // Notify için CCC descriptor

  // --- Veri Karakteristiği (WRITE = güvenli yazma, ACK ile) ---
  pOtaDataChar = pOtaService->createCharacteristic(
    OTA_DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pOtaDataChar->setCallbacks(new OtaDataCallback());

  // --- Versiyon Karakteristiği (READ only) ---
  pOtaVerChar = pOtaService->createCharacteristic(
    OTA_VERSION_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pOtaVerChar->setValue(FW_VERSION);

  pOtaService->start();

  // OTA servisini de advertising'e ekle
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(OTA_SERVICE_UUID);

  Serial.println("BLE OTA: Servis baslatildi");
  Serial.printf("BLE OTA: FW Version: %s\n", FW_VERSION);
}

void ble_ota_loop(void) {
  // OTA tamamlandıysa 2 saniye sonra reboot
  if (otaState == OTA_REBOOT_PENDING && rebootTimer > 0) {
    if (millis() - rebootTimer >= 2000) {
      Serial.println("BLE OTA: Sistem yeniden baslatiliyor...");
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }
}

ota_state_t ble_ota_get_state(void) {
  return otaState;
}

uint8_t ble_ota_get_progress(void) {
  return otaProgress;
}
