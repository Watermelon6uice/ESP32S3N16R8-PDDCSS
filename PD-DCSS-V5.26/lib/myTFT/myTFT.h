#ifndef MY_TFT_H
#define MY_TFT_H

#include <lvgl.h>
#include <TFT_eSPI.h>
#include "gui_guider.h"
#include "events_init.h"
#include "custom.h"

// 初始化TFT显示屏
void tft_init();

// 设置LVGL
void lvgl_setup();

// 初始化GUI界面
void init_gui(lv_ui *ui);

// 处理LVGL任务
void handle_lvgl_tasks();

extern TFT_eSPI tft;
extern lv_ui guider_ui;

#endif // MY_TFT_H