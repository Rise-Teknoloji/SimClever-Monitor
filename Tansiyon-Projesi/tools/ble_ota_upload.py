#!/usr/bin/env python3
"""
C-press BLE OTA Upload Script
================================
ESP32'ye BLE uzerinden firmware guncellemesi gonderir.

Gereksinimler:
    pip install bleak tqdm

Kullanim:
    python ble_ota_upload.py firmware.bin
    python ble_ota_upload.py firmware.bin --device "C-press_v1.0"
    python ble_ota_upload.py firmware.bin --address "AA:BB:CC:DD:EE:FF"

Not:
    Arduino IDE'den .bin dosyasi almak icin:
    Sketch > Export compiled Binary
    Dosya proje klasorunde olusur.
"""

import asyncio
import argparse
import sys
import os

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError:
    print("HATA: 'bleak' kutuphanesi gerekli!")
    print("Kurulum: pip install bleak")
    sys.exit(1)

try:
    from tqdm import tqdm
except ImportError:
    tqdm = None
    print("UYARI: 'tqdm' yok, ilerleme cubugu gosterilmeyecek. (pip install tqdm)")

# --- BLE UUID'ler (ble_ota.h ile eslesmeli) ---
OTA_SERVICE_UUID      = "fb1e4001-54ae-4a28-9f74-dfccb248601d"
OTA_CTRL_CHAR_UUID    = "fb1e4002-54ae-4a28-9f74-dfccb248601d"
OTA_DATA_CHAR_UUID    = "fb1e4003-54ae-4a28-9f74-dfccb248601d"
OTA_VERSION_CHAR_UUID = "fb1e4004-54ae-4a28-9f74-dfccb248601d"

CHUNK_SIZE = 240  # BLE MTU'ya uygun kucuk chunk boyutu
DEFAULT_DEVICE_NAME = "C-press_v1.0"


class BleOtaUploader:
    def __init__(self):
        self.client = None
        self.ctrl_response = asyncio.Event()
        self.last_response = ""

    def _notification_handler(self, sender, data):
        """OTA_CTRL notify callback."""
        msg = data.decode("utf-8", errors="replace")
        self.last_response = msg
        self.ctrl_response.set()

        if msg.startswith("PROG:"):
            pass
        elif msg.startswith("ERR:"):
            print(f"\n[HATA] ESP32: {msg}")
        elif msg.startswith("OK:"):
            print(f"  [OK] ESP32: {msg}")

    async def scan_device(self, device_name, timeout=10):
        """Cihazi ismine gore tara."""
        print(f"[SCAN] '{device_name}' cihazi araniyor ({timeout}s)...")

        device = await BleakScanner.find_device_by_name(
            device_name, timeout=timeout
        )

        if device is None:
            print(f"[HATA] '{device_name}' bulunamadi!")
            return None

        print(f"  [OK] Bulundu: {device.name} ({device.address})")
        return device.address

    async def connect(self, address):
        """BLE cihazina baglan."""
        print(f"[CONNECT] Baglaniliyor: {address}...")
        self.client = BleakClient(address)
        await self.client.connect()

        if not self.client.is_connected:
            raise BleakError("Baglanti kurulamadi!")

        print("  [OK] Baglanti kuruldu!")

        # Notify'i ac
        await self.client.start_notify(OTA_CTRL_CHAR_UUID, self._notification_handler)

    async def read_version(self):
        """Mevcut firmware versiyonunu oku."""
        try:
            data = await self.client.read_gatt_char(OTA_VERSION_CHAR_UUID)
            version = data.decode("utf-8", errors="replace")
            print(f"  [INFO] Mevcut FW Versiyon: {version}")
            return version
        except Exception as e:
            print(f"  [UYARI] Versiyon okunamadi: {e}")
            return None

    async def send_ctrl(self, command, timeout=10):
        """OTA kontrol komutu gonder ve yanit bekle."""
        self.ctrl_response.clear()
        await self.client.write_gatt_char(
            OTA_CTRL_CHAR_UUID,
            command.encode("utf-8"),
            response=True
        )
        try:
            await asyncio.wait_for(self.ctrl_response.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            print(f"  [UYARI] '{command}' komutu icin yanit zaman asimi!")
            return False

        return self.last_response.startswith("OK:")

    async def upload_firmware(self, firmware_data):
        """Firmware verisini chunk'lar halinde gonder."""
        total = len(firmware_data)
        sent = 0

        if tqdm:
            pbar = tqdm(total=total, unit="B", unit_scale=True, desc="Yukleniyor")
        else:
            pbar = None

        while sent < total:
            end = min(sent + CHUNK_SIZE, total)
            chunk = firmware_data[sent:end]

            await self.client.write_gatt_char(
                OTA_DATA_CHAR_UUID,
                chunk,
                response=True  # ACK bekle - guvenli gonderim
            )

            sent = end

            if pbar:
                pbar.update(len(chunk))
            else:
                pct = int(sent * 100 / total)
                print(f"\r  Yukleniyor: {pct}% ({sent}/{total} byte)", end="")

            # BLE stack'in rahatlamasi icin bekleme
            await asyncio.sleep(0.05)

        if pbar:
            pbar.close()
        else:
            print()

        print(f"  [OK] Toplam {sent} byte gonderildi.")

    async def disconnect(self):
        """Baglantiyi kes."""
        if self.client and self.client.is_connected:
            await self.client.stop_notify(OTA_CTRL_CHAR_UUID)
            await self.client.disconnect()
            print("  [OK] Baglanti kesildi.")


def find_default_firmware():
    """Build klasorundeki .bin dosyasini otomatik bul."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)  # tools/ -> proje klasoru
    build_dir = os.path.join(project_dir, "build")

    if not os.path.isdir(build_dir):
        return None

    # build/ altindaki ilk board klasorunde .ino.bin dosyasini ara
    for root, dirs, files in os.walk(build_dir):
        for f in files:
            if f.endswith(".ino.bin") and not f.endswith(".merged.bin"):
                return os.path.join(root, f)
    return None


async def main():
    parser = argparse.ArgumentParser(
        description="C-press BLE OTA Upload Tool"
    )
    parser.add_argument("firmware", nargs="?", default=None,
                        help=".bin firmware dosya yolu (bos birakilirsa build/ klasorunden otomatik bulur)")
    parser.add_argument("--device", default=DEFAULT_DEVICE_NAME,
                        help=f"Cihaz adi (varsayilan: {DEFAULT_DEVICE_NAME})")
    parser.add_argument("--address", default=None,
                        help="BLE MAC adresi (scan atlamak icin)")
    args = parser.parse_args()

    # Firmware dosyasini bul
    fw_path = args.firmware
    if fw_path is None:
        fw_path = find_default_firmware()
        if fw_path is None:
            print("[HATA] .bin dosyasi bulunamadi!")
            print("  Cozum: Arduino IDE > Sketch > Export compiled Binary")
            print("  Veya: python ble_ota_upload.py <dosya.bin>")
            sys.exit(1)
        print(f"[AUTO] Firmware bulundu: {fw_path}")

    if not os.path.isfile(fw_path):
        print(f"[HATA] Dosya bulunamadi: {fw_path}")
        sys.exit(1)

    with open(fw_path, "rb") as f:
        firmware_data = f.read()

    fw_size = len(firmware_data)
    print(f"[FW] Firmware: {fw_path} ({fw_size:,} byte)")

    uploader = BleOtaUploader()

    try:
        # 1. Cihazi bul veya dogrudan baglan
        address = args.address
        if address is None:
            address = await uploader.scan_device(args.device)
            if address is None:
                sys.exit(1)

        # 2. Baglan
        await uploader.connect(address)

        # 3. Mevcut versiyonu oku
        await uploader.read_version()

        # 4. OTA baslat
        print(f"\n[OTA] Guncelleme baslatiliyor ({fw_size:,} byte)...")
        success = await uploader.send_ctrl(f"BEGIN:{fw_size}")
        if not success:
            print("[HATA] OTA baslatilamadi!")
            await uploader.disconnect()
            sys.exit(1)

        # 5. Firmware gonder
        await uploader.upload_firmware(firmware_data)

        # 6. OTA sonlandir
        print("\n[OTA] Sonlandiriliyor...")
        success = await uploader.send_ctrl("END", timeout=30)
        if success:
            print("\n[BASARILI] OTA guncelleme tamamlandi! Cihaz yeniden baslatiliyor...")
        else:
            print("\n[HATA] OTA sonlandirma basarisiz!")
            await uploader.send_ctrl("ABORT")

    except BleakError as e:
        print(f"\n[HATA] BLE Hatasi: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\n[IPTAL] Iptal edildi! ABORT gonderiliyor...")
        try:
            await uploader.send_ctrl("ABORT", timeout=5)
        except Exception:
            pass
    finally:
        await uploader.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
