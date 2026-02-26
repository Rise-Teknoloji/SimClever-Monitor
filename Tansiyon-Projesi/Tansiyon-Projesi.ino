#include "lcd_bsp.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

/* --- BLE AYARLARI --- */
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer*         pServer         = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool  bleConnected = false;
int   bleBasinc    = -1;   // -1 = BLE'den veri yok, sensörü kullan

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

/* --- PIN TANIMLAMALARI --- */
const int sensorPin = 7;   // Basınç Sensörü (IO7)
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
  BLEDevice::init("SimClever");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  pCharacteristic->setCallbacks(new BasincCallback());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  Serial.println("BLE: SimClever yayinda!");
  Serial.printf("Heap (BLE sonrasi): %d byte\n", ESP.getFreeHeap());

  // --- Ekranı Başlat (BLE'den SONRA) ---
  lcd_lvgl_Init();
  Serial.printf("Heap (LCD sonrasi): %d byte\n", ESP.getFreeHeap());
}

void loop() {
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
  if(millis() - sonPilOkuma > 1000) {
    sonPilOkuma = millis();
    
    // --- TİTREMEYİ ENGELLEMEK İÇİN ORTALAMA ALIYORUZ ---
    long batSum = 0;
    int batSamples = 20; // 20 kere oku, ortalamasını al
    for(int k=0; k < batSamples; k++) {
        batSum += analogRead(bataryaPin);
        delay(2); // Okumalar arası minik bekleme
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

    // Ekrana Gönder (Kilit mekanizması ile)
    if (example_lvgl_lock(-1)) {
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
  // 3. EKRANI GÜNCELLEME (İbre Akıcılığı için)
  // ==========================================
  if (example_lvgl_lock(-1)) {
    basinc_guncelle((int)aktifBasinc);
    example_lvgl_unlock();
  }

  delay(5); // Çok kısa bekleme (Akıcılık için)
}