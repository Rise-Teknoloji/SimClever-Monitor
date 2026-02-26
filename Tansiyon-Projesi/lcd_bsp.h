#ifndef LCD_BSP_H
#define LCD_BSP_H

#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "lvgl.h"
#include "esp_check.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif 

/* --- DIŞARIDAN ÇAĞIRILACAK FONKSİYONLAR --- */

// Ekranı başlatan fonksiyon
void lcd_lvgl_Init(void);

// Basıncı güncelleyen fonksiyon
void basinc_guncelle(int deger);

// Pili güncelleyen fonksiyon 
void pil_guncelle(int yuzde);

// Şarj durumunu gösteren fonksiyon
void sarj_durumu_goster(bool sarj_oluyor);

// Kilit mekanizmaları
bool example_lvgl_lock(int timeout_ms);
void example_lvgl_unlock(void);

// Yardımcılar
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area);

#ifdef __cplusplus
}
#endif

#endif /* LCD_BSP_H */