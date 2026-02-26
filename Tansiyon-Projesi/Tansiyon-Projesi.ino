#include "lcd_bsp.h" 

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
  
  // Basınç Eğim Hesabı
  slope = pressureMaxRef_mmHg / (voltageMaxRef - voltageZero);
  
  // Ekranı Başlat
  lcd_lvgl_Init();
  
  // Pin Modları
  pinMode(sensorPin, INPUT);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(bataryaPin, INPUT); // Pil okuma pini
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
  // 1.5  LED BLINK (Basınç Aralığına Göre Hız)
  // ==========================================
  int blinkInterval = 0; // 0 = LED kapalı

  if      (pressure_mmHg >= 160) blinkInterval = 60;   // Çok hızlı
  else if (pressure_mmHg >= 120) blinkInterval = 120;  // Hızlı
  else if (pressure_mmHg >=  80) blinkInterval = 250;  // Orta
  else if (pressure_mmHg >=  40) blinkInterval = 500;  // Yavaş

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
    basinc_guncelle((int)pressure_mmHg);
    example_lvgl_unlock();
  }

  delay(5); // Çok kısa bekleme (Akıcılık için)
}