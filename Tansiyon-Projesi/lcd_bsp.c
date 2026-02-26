#include "lcd_bsp.h"
#include "esp_lcd_sh8601.h"
#include "lcd_config.h"
#include "FT3168.h"
#include "read_lcd_id_bsp.h"

static SemaphoreHandle_t lvgl_mux = NULL; 
#define LCD_HOST    SPI2_HOST

#define SH8601_ID 0x86
#define CO5300_ID 0xff
static uint8_t READ_LCD_ID = 0x00; 

/* --- FONKSİYON PROTOTİPLERİ --- */
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void example_increase_lvgl_tick(void *arg);
static void example_lvgl_port_task(void *arg);
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area);

/* PROTOTİPLER */
void pil_guncelle(int yuzde);
void basinc_guncelle(int deger);
void sarj_durumu_goster(bool sarj_oluyor);

/* --- TANSİYON ALETİ DEĞİŞKENLERİ --- */
static lv_obj_t * meter;
static lv_meter_indicator_t * indic;
static lv_obj_t * label_basinc; 
static lv_obj_t * bar_pil;        
static lv_obj_t * label_pil_text;
static lv_obj_t * pil_basi;
static lv_obj_t * sarj_ikonu;

/* --- EKRAN SÜRÜCÜ KOMUTLARI --- */
static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 120},
  {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  {0x35, (uint8_t []){0x00}, 1, 0},
  {0x53, (uint8_t []){0x20}, 1, 10},
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
};
static const sh8601_lcd_init_cmd_t co5300_lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 80},   
  {0xC4, (uint8_t []){0x80}, 1, 0},
  {0x53, (uint8_t []){0x20}, 1, 1},
  {0x63, (uint8_t []){0xFF}, 1, 1},
  {0x51, (uint8_t []){0x00}, 1, 1},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
};

/* --- GÜNCELLEME FONKSİYONLARI --- */

void pil_guncelle(int yuzde)
{
    if(bar_pil == NULL || label_pil_text == NULL) return;
    
    // Barı güncelle
    lv_bar_set_value(bar_pil, yuzde, LV_ANIM_ON);

    // Renk Ayarı
    lv_color_t renk;
    if(yuzde >= 60) {
        renk = lv_color_hex(0x009000); // Yeşil
    } else if(yuzde >= 20) {
        renk = lv_color_hex(0x1E69D2); // Turuncu
    } else {
        renk = lv_color_hex(0x0024FF); // Kırmızı
    }
    
    lv_obj_set_style_bg_color(bar_pil, renk, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(bar_pil, renk, LV_PART_INDICATOR);

    // Yazıyı güncelle
    lv_label_set_text_fmt(label_pil_text, "%d%%", yuzde);
}

void sarj_durumu_goster(bool sarj_oluyor)
{
    if(sarj_ikonu == NULL) return;
    
    if(sarj_oluyor) {
        lv_obj_clear_flag(sarj_ikonu, LV_OBJ_FLAG_HIDDEN);
        
        // Pulse animasyonu
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, sarj_ikonu);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_playback_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_obj_add_flag(sarj_ikonu, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(sarj_ikonu, NULL);
    }
}

void basinc_guncelle(int deger)
{
    if(meter == NULL || label_basinc == NULL) return;
    lv_meter_set_indicator_value(meter, indic, deger);
    lv_label_set_text_fmt(label_basinc, "%d", deger);
}

/* --- KADRAN TASARIMI --- */
void tansiyon_arayuzu_yap(void)
{
    /* 1. KADRANI OLUŞTUR */
    meter = lv_meter_create(lv_scr_act());
    lv_obj_set_style_bg_color(meter, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(meter, LV_OPA_COVER, LV_PART_MAIN); 
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);
    lv_obj_set_size(meter, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_center(meter);

    /* --- ÜST LOGO (SimClever) - FONT 40 --- */
    lv_obj_t * label_top = lv_label_create(meter);
    lv_label_set_recolor(label_top, true); 
    lv_label_set_text(label_top, "#009000 Sim##000000 Clever#");
    
    #if LV_FONT_MONTSERRAT_40
        lv_obj_set_style_text_font(label_top, &lv_font_montserrat_40, 0);
    #else
        lv_obj_set_style_text_font(label_top, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_align(label_top, LV_ALIGN_CENTER, 0, -60); 

    /* --- ALT LOGO (C-press) - FONT 36 --- */
    lv_obj_t * label_bottom = lv_label_create(meter);
    lv_label_set_recolor(label_bottom, true); 
    lv_label_set_text(label_bottom, "#009000 C##000000 -press#");

    #if LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(label_bottom, &lv_font_montserrat_36, 0);
    #else
        lv_obj_set_style_text_font(label_bottom, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_align(label_bottom, LV_ALIGN_CENTER, 0, 95); 

    /* --- PİL GÖSTERGESİ (BAR ŞEKLİNDE) --- */
    bar_pil = lv_bar_create(meter);
    lv_obj_set_size(bar_pil, 70, 30);
    lv_bar_set_range(bar_pil, 0, 100);
    lv_bar_set_value(bar_pil, 0, LV_ANIM_OFF); 
    
    // C-press yazısının altına hizala (165px)
    lv_obj_align(bar_pil, LV_ALIGN_CENTER, 0, 165);

    // 1. ANA KISIM - Sadece arka plan
    lv_obj_set_style_bg_opa(bar_pil, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_pil, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_pil, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_pil, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_pil, 3, LV_PART_MAIN);

    // 2. DOLGU
    lv_obj_set_style_bg_opa(bar_pil, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_pil, lv_color_hex(0x009000), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_pil, 4, LV_PART_INDICATOR);

    // Hafif gölge efekti
    lv_obj_set_style_shadow_width(bar_pil, 6, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(bar_pil, lv_color_hex(0x009000), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(bar_pil, LV_OPA_20, LV_PART_INDICATOR);

    // 3. PİL BAŞLIĞI
    pil_basi = lv_obj_create(meter);
    lv_obj_set_size(pil_basi, 4, 18);
    lv_obj_align_to(pil_basi, bar_pil, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_set_style_bg_color(pil_basi, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_opa(pil_basi, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pil_basi, 0, 0);
    lv_obj_set_style_radius(pil_basi, 2, 0);
    lv_obj_set_style_pad_all(pil_basi, 0, 0);

    // 4. ŞARJ İKONU (başta gizli)
    sarj_ikonu = lv_label_create(bar_pil);
    lv_label_set_text(sarj_ikonu, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(sarj_ikonu, lv_color_white(), 0);
    lv_obj_align(sarj_ikonu, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_add_flag(sarj_ikonu, LV_OBJ_FLAG_HIDDEN);

    // 5. YÜZDE YAZISI
label_pil_text = lv_label_create(bar_pil);
lv_label_set_text(label_pil_text, "--%");
lv_obj_set_style_text_color(label_pil_text, lv_color_black(), 0);

#if LV_FONT_MONTSERRAT_18
lv_obj_set_style_text_font(label_pil_text, &lv_font_montserrat_18, 0);
#endif

lv_obj_center(label_pil_text);

    /* --- KADRAN SAYILARI - FONT 26 --- */
    static lv_style_t style_ticks;
    lv_style_init(&style_ticks);
    #if LV_FONT_MONTSERRAT_26
        lv_style_set_text_font(&style_ticks, &lv_font_montserrat_26);
    #else
        lv_style_set_text_font(&style_ticks, &lv_font_montserrat_14);
    #endif
    lv_style_set_text_color(&style_ticks, lv_color_black());
    lv_obj_add_style(meter, &style_ticks, LV_PART_TICKS);

    /* 2. ÖLÇEK (SCALE) */
    lv_meter_scale_t * scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, 61, 2, 10, lv_color_white());
    lv_meter_set_scale_major_ticks(meter, scale, 4, 4, 20, lv_color_white(), 20); 
    lv_meter_set_scale_range(meter, scale, 0, 300, 270, 135);

    /* 3. RENKLİ BÖLGELER */
    
    // 0 - 80: SİYAH 
    lv_meter_indicator_t * indic_low = lv_meter_add_arc(meter, scale, 12, lv_color_black(), 0);
    lv_meter_set_indicator_start_value(meter, indic_low, 0);
    lv_meter_set_indicator_end_value(meter, indic_low, 80);

    // 80 - 160: KIRMIZI 
    lv_meter_indicator_t * indic_mid = lv_meter_add_arc(meter, scale, 12, lv_color_hex(0x0024FF), 0);
    lv_meter_set_indicator_start_value(meter, indic_mid, 80);
    lv_meter_set_indicator_end_value(meter, indic_mid, 160);

    // 160 - 300: SİYAH
    lv_meter_indicator_t * indic_high = lv_meter_add_arc(meter, scale, 12, lv_color_black(), 0);
    lv_meter_set_indicator_start_value(meter, indic_high, 160);
    lv_meter_set_indicator_end_value(meter, indic_high, 300);

    /* 4. İBRE */
    indic = lv_meter_add_needle_line(meter, scale, 5, lv_palette_darken(LV_PALETTE_GREY, 3), -15);

    /* 5. DİJİTAL GÖSTERGE */
    lv_obj_t * label_unit = lv_label_create(meter);
    lv_label_set_text(label_unit, "mmHg");
    lv_obj_align(label_unit, LV_ALIGN_CENTER, 0, 35); 
    lv_obj_set_style_text_color(label_unit, lv_palette_darken(LV_PALETTE_GREY, 2), 0);

    label_basinc = lv_label_create(meter);
    lv_label_set_text(label_basinc, "0"); 
    #if LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(label_basinc, &lv_font_montserrat_48, 0); 
    #endif
    lv_obj_set_style_text_color(label_basinc, lv_color_black(), 0);
    lv_obj_align(label_basinc, LV_ALIGN_CENTER, 0, -10); 
}

/* --- BAŞLATMA AYARLARI --- */
/* --- BAŞLATMA AYARLARI --- */
void lcd_lvgl_Init(void)
{
  READ_LCD_ID = read_lcd_id();
  static lv_disp_draw_buf_t disp_buf; 
  static lv_disp_drv_t disp_drv;      

  const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                               EXAMPLE_PIN_NUM_LCD_DATA0,
                                                               EXAMPLE_PIN_NUM_LCD_DATA1,
                                                               EXAMPLE_PIN_NUM_LCD_DATA2,
                                                               EXAMPLE_PIN_NUM_LCD_DATA3,
                                                               EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_handle_t io_handle = NULL;

  const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                              example_notify_lvgl_flush_ready,
                                                                              &disp_drv);

  sh8601_vendor_config_t vendor_config = 
  {
    .flags = 
    {
      .use_qspi_interface = 1,
    },
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  esp_lcd_panel_handle_t panel_handle = NULL;
  
  const esp_lcd_panel_dev_config_t panel_config = 
  {
    .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
    .bits_per_pixel = LCD_BIT_PER_PIXEL,
    .vendor_config = &vendor_config,
  };
  vendor_config.init_cmds = (READ_LCD_ID == SH8601_ID) ? sh8601_lcd_init_cmds : co5300_lcd_init_cmds;
  vendor_config.init_cmds_size = (READ_LCD_ID == SH8601_ID) ? sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]) : sizeof(co5300_lcd_init_cmds) / sizeof(co5300_lcd_init_cmds[0]);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));

  lv_init();
  lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1);
  // BLE ile birlikte çift buffer'a yeterli DMA bellek yok → Tek buffer modu
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;

  // DÖNDÜRME AYARLARI
  disp_drv.sw_rotate = 1;
  disp_drv.rotated = LV_DISP_ROT_270;  

  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;    
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.disp = disp;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t lvgl_tick_timer_args = 
  {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  lvgl_mux = xSemaphoreCreateMutex(); 
  assert(lvgl_mux);
  xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
  
  if (example_lvgl_lock(-1)) 
  {   
    tansiyon_arayuzu_yap(); 
    example_lvgl_unlock();
  }
}

bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}
static void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1))
    {
      task_delay_ms = lv_timer_handler();
      
      example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    }
    else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_driver);
  return false;
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int offsetx1 = (READ_LCD_ID == SH8601_ID) ? area->x1 : area->x1 + 0x06;
  const int offsetx2 = (READ_LCD_ID == SH8601_ID) ? area->x2 : area->x2 + 0x06;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint16_t tp_x,tp_y;
  uint8_t win = getTouch(&tp_x,&tp_y);
  if(win)
  {
    data->point.x = tp_x;
    data->point.y = tp_y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}