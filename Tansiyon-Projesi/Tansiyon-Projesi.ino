#include "lcd_bsp.h"
#include "ble_ota.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertisedDevice.h>

/* --- BLE SERVER AYARLARI --- */
#define SERVICE_UUID          "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Basınç alma
#define TANSIYON_CHAR_UUID    "a1b2c3d4-e5f6-7890-abcd-ef1234567890"  // SYS/DIA/BPM alma

/* --- BLE CLIENT AYARLARI (Stetoskop kontrol) --- */
#define STETHOSCOPE_SERVICE_UUID   "5908fab1-7ff3-43f2-b353-d10f0644857c"
#define STETHOSCOPE_CHAR_UUID      "3f39c63d-dc06-4268-b581-8a67d969b192"

/* --- TANSİYON DEĞERLERİ (Ana sistemden gelecek) --- */
int sys = 0;   // Sistolik basınç (mmHg) - ana sistem gönderecek
int dia = 0;   // Diyastolik basınç (mmHg) - ana sistem gönderecek
int bpm = 0;   // Nabız (BPM) - ana sistem gönderecek
static bool tansiyonGuncel = false; // Ana sistemden veri geldi mi?

/* --- BLE Server Değişkenleri --- */
BLEServer*         pServer         = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool  bleConnected = false;
int   bleBasinc    = -1;   // -1 = BLE'den veri yok, sensörü kullan

/* --- BLE Client Değişkenleri (Stetoskop) --- */
static BLEClient* pStetoskopClient = nullptr;
static BLERemoteCharacteristic* pRemoteChar = nullptr;
static bool stetoskopConnected = false;
static bool doConnect          = false;
static bool doScan             = false;
static BLEAdvertisedDevice* targetDevice = nullptr;
static bool sonSesDurumu       = false;  // Önceki ses durumu (değişim takibi)
static unsigned long sonStetoskopGonder = 0; // Gönderim throttle
static bool tryPublicAddr      = true;   // Adres tipi cycling

/* --- FreeRTOS Stetoskop Task (Non-blocking BLE Client) --- */
static QueueHandle_t stetoskopQueue = NULL;
#define STETOSKOP_QUEUE_LEN  2
#define STETOSKOP_TASK_STACK 4096
#define STETOSKOP_TASK_PRIO  1

/* --- BLE CALLBACK'LER --- */
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s)    { bleConnected = true;  Serial.println("BLE: Baglandi"); }
  void onDisconnect(BLEServer* s) {
    bleConnected = false;
    bleBasinc    = -1;
    Serial.println("BLE: Koptu, reklam yeniden basliyor...");
    s->startAdvertising();  // Tekrar keşfedilebilir ol
  }
};

class BasincCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String val = c->getValue();
    if (val.length() > 0) {
      bleBasinc = val.toInt();
      Serial.print("BLE Basinc: ");
      Serial.println(bleBasinc);
    }
  }
};

/* --- ANA SİSTEMDEN SYS/DIA/BPM ALMA CALLBACK --- */
// Format: "SYS,DIA,BPM"  örn: "120,80,75"
class TansiyonCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String val = c->getValue();
    if (val.length() > 0) {
      // Parse: "120,80,75"
      int comma1 = val.indexOf(',');
      int comma2 = val.indexOf(',', comma1 + 1);
      
      if (comma1 > 0 && comma2 > comma1) {
        sys = val.substring(0, comma1).toInt();
        dia = val.substring(comma1 + 1, comma2).toInt();
        bpm = val.substring(comma2 + 1).toInt();
        tansiyonGuncel = true;
        
        Serial.printf("Ana Sistem -> SYS: %d, DIA: %d, BPM: %d\n", sys, dia, bpm);
        // NOT: LVGL güncellemesi loop() içinde yapılıyor (BLE callback'te mutex kullanmıyoruz)
      } else {
        Serial.print("Tansiyon parse hatasi: ");
        Serial.println(val);
      }
    }
  }
};

/* --- STETOSKOP PAYLOAD OLUŞTURUCU --- */
String buildStetoskopPayload(int bpmVal) {
  // Protokole birebir uygun format (Python örneğinden)
  // NOT: SolAkciger'de ses adı yok (: ile ? arası boş), SagAkciger'de _ var
  return "Kalp:Tansiyon?" + String(bpmVal) +
         ";SagAkciger:_?" + String(bpmVal) +
         ";SolAkciger:?" + String(bpmVal) +
         ";Bagirsak:_";
}

/* --- STETOSKOP CLIENT CALLBACK (kopma takibi) --- */
class StetoskopClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) {
    Serial.println("Stetoskop: onConnect callback");
  }
  void onDisconnect(BLEClient* c) {
    stetoskopConnected = false;
    pRemoteChar = nullptr;
    doScan = true;
    Serial.println("Stetoskop: Baglanti koptu! Tekrar aranacak...");
  }
};

/* --- STETOSKOP BLE BAĞLANTI FONKSİYONU --- */
bool connectToStetoskop() {
  Serial.print("Stetoskopa baglaniliyor: ");
  Serial.println(targetDevice->getAddress().toString().c_str());

  // Scan'i tamamen temizle
  BLEDevice::getScan()->clearResults();
  delay(300);

  pStetoskopClient = BLEDevice::createClient();

  // Adres tipini belirle
  uint8_t addrType = tryPublicAddr ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
  Serial.printf("Adres tipi: %s\n", tryPublicAddr ? "PUBLIC" : "RANDOM");

  if (!pStetoskopClient->connect(targetDevice->getAddress(), addrType)) {
    Serial.println("Stetoskop: Baglanti basarisiz!");
    tryPublicAddr = !tryPublicAddr;
    return false;
  }
  Serial.println("Stetoskop: Baglandi!");
  return true;
}

/* --- STETOSKOP'A PAYLOAD GÖNDER (bağlan → yaz → çık) --- */
bool sendStetoskopPayload(int bpmVal) {
  if (targetDevice == nullptr) return false;

  // 1. Bağlan
  if (!connectToStetoskop()) return false;

  // 2. Servisi bul
  BLERemoteService* pRemoteService = pStetoskopClient->getService(BLEUUID(STETHOSCOPE_SERVICE_UUID));
  if (pRemoteService == nullptr) {
    Serial.println("Stetoskop: Servis bulunamadi!");
    pStetoskopClient->disconnect();
    return false;
  }

  // 3. Karakteristiği bul
  BLERemoteCharacteristic* pChar = pRemoteService->getCharacteristic(BLEUUID(STETHOSCOPE_CHAR_UUID));
  if (pChar == nullptr) {
    Serial.println("Stetoskop: Karakteristik bulunamadi!");
    pStetoskopClient->disconnect();
    return false;
  }

  // 4. Payload yaz
  String payload = buildStetoskopPayload(bpmVal);
  pChar->writeValue(payload.c_str(), payload.length());
  Serial.print("Stetoskop yazildi: ");
  Serial.println(payload);

  // 5. Hemen bağlantıyı kes
  delay(100);
  pStetoskopClient->disconnect();
  Serial.println("Stetoskop: Baglanti kesildi (serbest)");

  return true;
}

/* --- STETOSKOP FREERTOS TASK (Non-blocking BLE Client) --- */
static void stetoskop_task(void *arg) {
  int bpmToSend;
  for (;;) {
    // Queue'dan BPM değeri gelene kadar bekle (blocking, ama kendi task'ında)
    if (xQueueReceive(stetoskopQueue, &bpmToSend, portMAX_DELAY) == pdTRUE) {
      Serial.printf("Stetoskop Task: BPM=%d gonderiliyor...\n", bpmToSend);
      if (!sendStetoskopPayload(bpmToSend)) {
        Serial.println("Stetoskop Task: Gonderim basarisiz!");
      }
    }
  }
}

/* --- STETOSKOP SCAN CALLBACK --- */
class StetoskopScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Cihaz bulundu: ");
    Serial.println(advertisedDevice.toString().c_str());

    // Cihaz adı "RTCOUND" ile başlıyorsa stetoskop olarak tanı
    if (advertisedDevice.haveName() &&
        String(advertisedDevice.getName().c_str()).startsWith("RTCOUND20125")) {
      Serial.println(">>> Stetoskop cihazi bulundu! <<<");
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;
    }
  }
};

/* --- PIN TANIMLAMALARI --- */
const int sensorPin = 1;   // Basınç Sensörü (IO7)
const int bataryaPin = 4;  // Pil Voltajı (IO4 - Dahili Voltaj Bölücü)
const int ledPin    = 2;   // LED Çıkışı (IO2)

/* --- KALİBRASYON DEĞERLERİ --- */
float voltageZero = 0.205; // Senin kalibrasyon değerin
float voltageMaxRef = 1.725;
float pressureMaxRef_mmHg = 200.0;
float slope; 

/* --- ZAMANLAYICILAR --- */
unsigned long sonPilOkuma   = 0; // Pili sürekli okuyup işlemciyi yormayalım
unsigned long sonLedToggle  = 0; // LED blink zamanlayıcısı
bool ledDurum = false;           // LED'in anlık durumu (açık/kapalı)

/* --- BASINÇ GÜNCELLEME THROTTLE --- */
unsigned long sonBasincOkuma = 0;  // Son ekran güncelleme zamanı
int           sonBasincDeger = -1; // Önceki basınç değeri (değişim kontrolü)

void setup() {
  Serial.begin(115200);
  delay(500); // Serial Monitor'ün açılması için bekle
  
  // Basınç Eğim Hesabı
  slope = pressureMaxRef_mmHg / (voltageMaxRef - voltageZero);

  // Pin Modları
  pinMode(sensorPin, INPUT);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(bataryaPin, INPUT);

  Serial.printf("Heap (baslangic): %d byte\n", ESP.getFreeHeap());

  // --- BLE Peripheral Başlat (EKRANDAN ÖNCE!) ---
  BLEDevice::init("C-press_v1.0");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  // Karakteristik 1: Basınç alma
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  pCharacteristic->setCallbacks(new BasincCallback());

  // Karakteristik 2: SYS/DIA/BPM alma (Ana sistemden)
  BLECharacteristic* pTansiyonChar = pService->createCharacteristic(
                      TANSIYON_CHAR_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  pTansiyonChar->setCallbacks(new TansiyonCallback());

  pService->start();

  // --- BLE OTA Servisini Başlat ---
  ble_ota_setup(pServer);

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  Serial.println("BLE: C-press_v1.0 yayinda!");
  Serial.printf("Heap (BLE Server sonrasi): %d byte\n", ESP.getFreeHeap());

  // --- BLE Client Scan Başlat (Stetoskop aranıyor) ---
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new StetoskopScanCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);  // 5 saniye scan, non-blocking
  Serial.println("BLE Client: Stetoskop araniyor...");
  Serial.printf("Heap (BLE Client sonrasi): %d byte\n", ESP.getFreeHeap());

  // --- Stetoskop FreeRTOS Task ve Queue ---
  stetoskopQueue = xQueueCreate(STETOSKOP_QUEUE_LEN, sizeof(int));
  assert(stetoskopQueue);
  xTaskCreate(stetoskop_task, "STETOSKOP", STETOSKOP_TASK_STACK, NULL, STETOSKOP_TASK_PRIO, NULL);
  Serial.println("Stetoskop Task: Olusturuldu");

  // --- Sabit tansiyon değerleri (test) ---
  Serial.printf("Test Degerleri -> SYS: %d, DIA: %d, BPM: %d\n", sys, dia, bpm);

  // --- Ekranı Başlat (BLE'den SONRA) ---
  lcd_lvgl_Init();
  Serial.printf("Heap (LCD sonrasi): %d byte\n", ESP.getFreeHeap());
}

void loop() {
  // ==========================================
  // 0. BLE OTA KONTROL (Reboot vs.)
  // ==========================================
  ble_ota_loop();

  // ==========================================
  // 0. STETOSKOP SCAN YÖNETİMİ
  // ==========================================
  // Scan'da cihaz bulunduysa, adresini kaydet (bağlanmayı sonraya bırak)
  if (doConnect) {
    Serial.println("Stetoskop: Cihaz adresi kaydedildi, hazir.");
    doConnect = false;
    stetoskopConnected = true; // Adres hazır, gönderim yapabilir
  }

  // Cihaz bulunamadıysa tekrar scan
  if (!stetoskopConnected && doScan) {
    BLEDevice::getScan()->start(5, false);
    doScan = false;
    Serial.println("BLE Client: Tekrar scan baslatildi...");
  }

  // ==========================================
  // 1. BASINÇ SENSÖRÜNÜ OKUMA (Hızlı olmalı)
  // ==========================================
  long sumADC = 0;
  int samples = 20; 
  
  for(int i=0; i<samples; i++){
    sumADC += analogRead(sensorPin);
  }
  float rawADC = sumADC / (float)samples;

  float voltage = (rawADC / 4095.0) * 3.3;
  float pressure_mmHg = (voltage - voltageZero) * slope;

  // Negatif ve parazit değerleri temizle
  if(pressure_mmHg < 2.0) {
    pressure_mmHg = 0.0;
  }

  // ==========================================
  // 1.5  AKTİF BASINÇ DEĞERİ SEÇİMİ
  // ==========================================
  // BLE bağlıysa ve değer geldiyse → BLE değerini kullan
  // Değilse → sensör değerini kullan
  float aktifBasinc = (bleBasinc >= 0) ? (float)bleBasinc : pressure_mmHg;

  // ==========================================
  // 1.6  LED BLINK (Basınç Aralığına Göre Hız)
  // ==========================================
  int blinkInterval = 0; // 0 = LED kapalı

  if      (aktifBasinc >= 160) blinkInterval = 60;   // Çok hızlı
  else if (aktifBasinc >= 120) blinkInterval = 120;  // Hızlı
  else if (aktifBasinc >=  80) blinkInterval = 250;  // Orta
  else if (aktifBasinc >=  40) blinkInterval = 500;  // Yavaş

  if (blinkInterval > 0) {
    if (millis() - sonLedToggle >= (unsigned long)blinkInterval) {
      sonLedToggle = millis();
      ledDurum = !ledDurum;
      digitalWrite(ledPin, ledDurum ? HIGH : LOW);
    }
  } else {
    // Aralık dışı → LED kapalı
    digitalWrite(ledPin, LOW);
    ledDurum = false;
  }

  // ==========================================
  // 2. PİL DURUMUNU OKUMA (Saniyede 1 kez yeterli)
  // ==========================================
  if(millis() - sonPilOkuma > 15000) {
    sonPilOkuma = millis();
    
    // --- TİTREMEYİ ENGELLEMEK İÇİN ORTALAMA ALIYORUZ ---
    long batSum = 0;
    int batSamples = 20; // 20 kere oku, ortalamasını al
    for(int k=0; k < batSamples; k++) {
        batSum += analogRead(bataryaPin);
        delayMicroseconds(200); // 200us × 20 = 4ms (eski: 2ms × 20 = 40ms)
    }
    
    // Waveshare şemasına göre (1/3 Voltaj Bölücü)
    // Formül: (OrtalamaADC / 4095) * 3.3V * 3
    float batVolt = (batSum / (float)batSamples / 4095.0) * 3.3 * 3.0;
    
    // --- LIPO PİL YÜZDE HESABI ---
    // 4.20V = %100 (Tam Dolu)
    // 3.30V = %0   (ESP32 için güvenli alt sınır)
    // map fonksiyonu: (Değer, AltSınır, ÜstSınır, HedefAlt, HedefÜst)
    int yuzde = map(batVolt * 100, 330, 420, 0, 100);
    
    // Sınırlandırma (Yüzde 0-100 dışına çıkmasın)
    yuzde = constrain(yuzde, 0, 100);

    // Ekrana Gönder (10ms timeout — sonsuz bekleme önlendi)
    if (example_lvgl_lock(10)) {
      pil_guncelle(yuzde);
      example_lvgl_unlock();
    }
    
    // Debug için seri porta yazdıralım
    Serial.print("Pil Voltaj: ");
    Serial.print(batVolt);
    Serial.print("V | Yuzde: %");
    Serial.println(yuzde);
  }

  // ==========================================
  // 3. EKRANI GÜNCELLEME (Yırtılmayı önlemek için throttle)
  // ==========================================
  int yeniBasinc = (int)aktifBasinc;
  if (yeniBasinc != sonBasincDeger && (millis() - sonBasincOkuma >= 50)) {
    sonBasincOkuma = millis();
    sonBasincDeger = yeniBasinc;
    if (example_lvgl_lock(10)) {  // 10ms timeout (bloklamayı önle)
      basinc_guncelle(yeniBasinc);
      example_lvgl_unlock();
    }
  }

  // ==========================================
  // 3.5 TANSİYON BİLGİ GÜNCELLEME (BLE callback'ten flag ile)
  // ==========================================
  if (tansiyonGuncel) {
    if (example_lvgl_lock(10)) {
      tansiyon_bilgi_guncelle(sys, dia, bpm);
      example_lvgl_unlock();
    }
  }

  // ==========================================
  // 4. STETOSKOP SES KONTROLÜ (Korotkoff)
  //    Queue ile ayrı FreeRTOS task'a gönderim (NON-BLOCKING)
  //    Sadece ana sistemden SYS/DIA/BPM gelmişse aktif
  // ==========================================
  if (stetoskopConnected && targetDevice != nullptr && tansiyonGuncel) {
    // Basınç-tansiyon karşılaştırması
    bool sesDuyulmali = (aktifBasinc > dia) && (aktifBasinc < sys);
    bool durumDegisti = (sesDuyulmali != sonSesDurumu);

    if (durumDegisti) {
      sonSesDurumu = sesDuyulmali;

      if (!sesDuyulmali) {
        // SES KAPALI → BPM=0 gönder (stetoskopu sustur)
        Serial.print("Stetoskop SES KAPALI | Basinc: ");
        Serial.println((int)aktifBasinc);

        int muteVal = 0;
        if (xQueueSend(stetoskopQueue, &muteVal, 0) != pdTRUE) {
          Serial.println("Stetoskop: Queue dolu, mute gonderilemedi!");
        }
      } else {
        // SES ACIK → BPM gönder (stetoskopu aç)
        Serial.print("Stetoskop SES ACIK | Basinc: ");
        Serial.print((int)aktifBasinc);
        Serial.print(" | BPM: ");
        Serial.println(bpm);

        int bpmVal = bpm;
        if (xQueueSend(stetoskopQueue, &bpmVal, 0) != pdTRUE) {
          Serial.println("Stetoskop: Queue dolu, unmute gonderilemedi!");
        }
      }
    }
  }

  delay(5); // Çok kısa bekleme
}