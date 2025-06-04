#include <Arduino.h>
#include "myTFT.h"

// 用于演示更新的变量
float voltage = 0.0;
float current = 2.13;
unsigned long previousMillis = 0;
const long interval = 100;  

void setup()
{
    Serial.begin(115200); /* prepare for possible serial debug 为可能的串行调试做准备*/
    tft_init();
    lvgl_setup();
    
    /*Create a GUI-Guider app */
    init_gui(&guider_ui);

    Serial.println("Setup done");
}

void loop()
{
    // 检查是否应该更新显示
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;
        
        // 更新电压值（每次加0.1V，当达到5.0V时重置）
        voltage += 0.1;
        if (voltage > 15.0) {
            voltage = 0.0;
        }
        
        // 使用char缓冲区而不是直接使用lv_label_set_text_fmt
        char voltage_buf[16];
        char current_buf[16];
        char power_buf[16];
        
        // 先格式化字符串
        sprintf(voltage_buf, "%.2f", voltage);
        sprintf(current_buf, "%.2f", current);
        sprintf(power_buf, "%.2f", voltage * current);
        
        // 使用lv_label_set_text设置文本
        lv_label_set_text(guider_ui.screen_Uout, voltage_buf);
        lv_label_set_text(guider_ui.screen_Iout, current_buf);
        lv_label_set_text(guider_ui.screen_Pout, power_buf);
        
        // 打印到串口调试
        Serial.print("电压: ");
        Serial.print(voltage_buf);
        Serial.print(" 电流: ");
        Serial.print(current_buf);
        Serial.print(" 功率: ");
        Serial.println(power_buf);
    }
    
    // 处理LVGL任务
    handle_lvgl_tasks();
}