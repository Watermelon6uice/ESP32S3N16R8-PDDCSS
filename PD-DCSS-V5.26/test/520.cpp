//导入库
#include <Arduino.h>
#include "myTFT.h"
#include "myStateButton.h" // 引入按钮状态管理库
#include "myEncoder.h" // 引入编码器库

// FreeRTOS相关头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

// 添加标准库头文件
#include <stdint.h> // 用于uint32_t类型

//函数声明
void updateDisplay();
void clearDisplay();
void updateButtonState(bool is_on);
void updateUSet(); // 更新U_SET值
void checkConfirmTimeout(); // 检查确认超时
void confirmUSet(); // 确认电压设置
void IRAM_ATTR confirmButtonISR(); // 确认按钮中断处理函数
void IRAM_ATTR stepSwitchISR(); // 步进切换按钮中断处理函数
void toggleStepSize(); // 切换步进值
void uiUpdateTask(void* parameter); // 合并的UI更新和LVGL刷新任务
void dataSamplingTask(void* parameter);
void encoderTask(void* parameter); // 专门处理编码器输入的任务
void initTaskControl(); // 初始化任务控制互斥量函数声明

// 定义GPIO引脚
#define BUTTON_STATE_PIN 19
#define ENCODER_PIN_A 16  // 编码器A相引脚
#define ENCODER_PIN_B 17  // 编码器B相引脚
#define CONFIRM_BUTTON_PIN 21  // 确认按钮引脚
#define STEP_SWITCH_PIN 45  // 步进切换按钮引脚，改在了GPIO45上

// 创建状态按钮对象
MyStateButton stateButton(BUTTON_STATE_PIN);

// 创建旋转编码器对象
myEncoder encoder(ENCODER_PIN_A, ENCODER_PIN_B);

// 用于演示更新的变量
float voltage = 0.0;
float current = 2.13;
unsigned long previousMillis = 0;
const long interval = 1000;  

// 电压设置值
float u_set = 5.00; // 初始化为5.00V
float temp_u_set = 5.00; // 临时电压设置值，用于存储未确认的调整
float orig_u_set = 5.00; // 原始电压设置值，用于在超时时恢复
const float u_set_min = 2.00; // 最小值
const float u_set_max = 15.00; // 最大值
const float u_set_step = 0.10; // 步进值
const float u_set_step_fine = 0.10; // 细调步进值
const float u_set_step_coarse = 1.00; // 粗调步进值
bool use_fine_step = true; // 默认使用细调步进
bool u_set_confirmed = true; // 电压设置是否已确认
unsigned long last_adjustment_time = 0; // 最后一次调整时间
const unsigned long confirm_timeout = 5000; // 确认超时时间（5秒）

// 任务句柄
TaskHandle_t uiTaskHandle = NULL;    // UI和LVGL合并任务的句柄
TaskHandle_t dataTaskHandle = NULL;  // 数据采样任务句柄
TaskHandle_t encoderTaskHandle = NULL; // 编码器处理任务句柄

// 互斥量用于保护数据访问
SemaphoreHandle_t dataMutex;

// 事件组用于任务同步
EventGroupHandle_t systemEvents;
#define UI_UPDATE_EVENT (1 << 0)
#define DATA_READY_EVENT (1 << 1)
#define CONFIRM_EVENT (1 << 2) // 确认事件
#define STEP_SWITCH_EVENT (1 << 3) // 步进切换事件

// 任务优先级 (在ESP-IDF FreeRTOS中，数值越小优先级越高)
#define UI_TASK_PRIORITY 2      // UI和LVGL刷新任务优先级
#define DATA_TASK_PRIORITY 3    // 数据采样任务优先级
#define ENCODER_TASK_PRIORITY 1 // 编码器任务优先级最高，确保实时响应

// 数据采样任务控制变量
static SemaphoreHandle_t taskControlMutex = NULL; // 用于保护任务控制变量的互斥量
static volatile bool g_dataTaskRunning = false; // 控制数据采样任务是否执行数据采集


void setup()
{    
    Serial.begin(115200); /* prepare for possible serial debug 为可能的串行调试做准备*/
    
    // 创建互斥量和事件组
    dataMutex = xSemaphoreCreateMutex();
    systemEvents = xEventGroupCreate();
    
    tft_init();
    lvgl_setup();
    
    /*Create a GUI-Guider app */
    init_gui(&guider_ui);
    
    // 初始化任务控制互斥量
    initTaskControl();
    
    // 初始化按钮状态库
    stateButton.begin();
    
    // 初始化编码器
    encoder.begin();
    
    // 初始化确认按钮（GPIO21）
    pinMode(CONFIRM_BUTTON_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(CONFIRM_BUTTON_PIN), confirmButtonISR, RISING);
    
    // 初始化步进切换按钮（GPIO48）
    pinMode(STEP_SWITCH_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(STEP_SWITCH_PIN), stepSwitchISR, RISING);
    
    // 设置按钮状态变化回调函数
    stateButton.setStateChangeCallback(updateButtonState);
    
    // 初始化UI显示状态
    lv_label_set_text(guider_ui.screen_STATE, "ON");
    // 设置ON状态为绿色
    lv_obj_set_style_text_color(guider_ui.screen_STATE, lv_color_hex(0x00ff00), LV_PART_MAIN|LV_STATE_DEFAULT);
    
    // 根据按钮初始状态设置全局任务运行标志（在创建任务前）
    g_dataTaskRunning = stateButton.getState();
    
    // 初始化界面状态（包括待机标签的显示/隐藏）
    updateButtonState(stateButton.getState());
    
    // 初始化U_SET显示
    char u_set_buf[16];
    sprintf(u_set_buf, "%.2f", u_set);
    lv_label_set_text(guider_ui.screen_U_SET, u_set_buf);
      // 创建FreeRTOS任务 - UI更新和LVGL刷新已合并为一个任务
    xTaskCreate(
        uiUpdateTask,        // 任务函数 - 现在同时处理UI更新和LVGL刷新
        "UI_LVGL_Task",      // 任务名称
        4096,               // 堆栈大小
        NULL,               // 任务参数
        UI_TASK_PRIORITY,   // 任务优先级
        &uiTaskHandle       // 任务句柄
    );      // 在系统初始化时创建数据采样任务，使用之前设置的g_dataTaskRunning控制执行
    // 创建数据采样任务
    xTaskCreate(
        dataSamplingTask,    // 任务函数
        "Data_Sampling",     // 任务名称
        2048,                // 堆栈大小
        NULL,                // 任务参数
        DATA_TASK_PRIORITY,  // 任务优先级
        &dataTaskHandle      // 任务句柄
    );
      // 创建编码器处理任务
    xTaskCreate(
        encoderTask,        // 任务函数
        "Encoder_Task",     // 任务名称
        2048,               // 堆栈大小
        NULL,               // 任务参数
        ENCODER_TASK_PRIORITY, // 任务优先级
        &encoderTaskHandle  // 任务句柄
    );
}

void loop()
{
    // 由于使用了FreeRTOS任务，主循环变得非常简单
    // 只需要定期更新按钮状态，这个功能暂时还保留在主循环中
    stateButton.update();
    
    // 主循环延时
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

// 编码器处理任务 - 专门负责读取编码器值并更新U_SET
void encoderTask(void* parameter) {
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(5); // 每5ms检查一次编码器，确保快速响应
    
    // 初始化最后唤醒时间
    xLastWakeTime = xTaskGetTickCount();
    
    while (true) {        // 检查确认事件和步进切换事件
        EventBits_t bits = xEventGroupWaitBits(
            systemEvents,       // 事件组句柄
            CONFIRM_EVENT | STEP_SWITCH_EVENT,      // 等待的事件位
            pdTRUE,            // 清除事件位
            pdFALSE,           // 任一事件均可唤醒
            0                   // 不等待，立即返回
        );
          // 如果收到确认事件
        if (bits & CONFIRM_EVENT) {
            confirmUSet();
            xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT); // 请求UI更新
        }
        
        // 如果收到步进切换事件
        if (bits & STEP_SWITCH_EVENT) {
            // 步进切换事件已在UI任务中处理
        }
        
        // 更新U_SET值（处理编码器输入）
        updateUSet();
        
        // 检查确认超时
        checkConfirmTimeout();
        
        // 确保任务以固定频率运行
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// UI更新和LVGL刷新合并任务 
void uiUpdateTask(void* parameter) {
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(33); // 每33ms刷新一次(约30fps)
    
    // 初始化最后唤醒时间
    xLastWakeTime = xTaskGetTickCount();
    
    // 设置任务的优先级，确保它能及时响应
    vTaskPrioritySet(NULL, UI_TASK_PRIORITY);
      while (true) {        // 等待数据就绪事件、UI更新事件、确认事件或步进切换事件
        EventBits_t bits = xEventGroupWaitBits(
            systemEvents,                      // 事件组句柄
            DATA_READY_EVENT | UI_UPDATE_EVENT | CONFIRM_EVENT | STEP_SWITCH_EVENT, // 等待的事件位
            pdTRUE,                            // 清除事件位
            pdFALSE,                           // 任一事件均可唤醒
            0                                  // 不等待，立即返回
        );
          bool needRefresh = false;  // 跟踪是否需要特殊刷新
        
        // 处理数据更新 - 只有在系统ON状态且有新数据时才更新显示
        if (g_dataTaskRunning && (bits & DATA_READY_EVENT)) {
            // 使用短暂的锁定时间，仅获取需要的数据
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                updateDisplay();
                xSemaphoreGive(dataMutex);
                needRefresh = true;
            }
        }
        
        // 处理UI强制更新事件 - 这通常在状态切换时发生
        if (bits & UI_UPDATE_EVENT) {
            // 强制刷新标志
            needRefresh = true;
        }
          // 处理确认事件
        if (bits & CONFIRM_EVENT) {
            // 确认事件已经在编码器任务中处理了，这里只需要设置刷新标志
            needRefresh = true;
        }
        
        // 处理步进切换事件
        if (bits & STEP_SWITCH_EVENT) {
            toggleStepSize();
            needRefresh = true;
        }
          // 处理LVGL任务，刷新屏幕
        handle_lvgl_tasks();
        
        // 如果需要额外刷新（如状态切换或数据更新后），再多刷新一次
        if (needRefresh) {
            vTaskDelay(5 / portTICK_PERIOD_MS);  // 短暂延时
            handle_lvgl_tasks();  // 额外的一次刷新
        }
        
        // 确保任务以固定频率运行
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}


// 初始化任务控制互斥量 - 简单的辅助函数
void initTaskControl() {
    if (taskControlMutex == NULL) {
        taskControlMutex = xSemaphoreCreateMutex();
    }
}

void dataSamplingTask(void* parameter) {
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 每1秒采样一次数据
    
    // 初始化最后唤醒时间
    xLastWakeTime = xTaskGetTickCount();
    
    // 首次运行先跳过一次采样，仅设置计时器基准点
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    while (true) {
        // 只有在全局运行标志为true时才进行数据采样
        if (g_dataTaskRunning) {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // 更新电压值（每次加0.1V，当达到15.0V时重置）
                voltage += 0.1;
                if (voltage > 15.0) {
                    voltage = 0.0;
                }
                
                // 可以在这里添加实际的数据采集代码，例如ADC读取等
                xSemaphoreGive(dataMutex);
                
                // 设置数据就绪事件，通知UI任务进行更新
                xEventGroupSetBits(systemEvents, DATA_READY_EVENT);
            }
        }
        
        // 确保任务以固定频率运行
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// 这些功能现在由myStateButton库处理

// 更新显示函数
void updateDisplay() {
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
}

// 更新U_SET值函数
void updateUSet() {
    // 读取编码器值
    int16_t encoderValue = encoder.read();
    
    // 如果有旋转，更新U_SET值
    if (encoderValue != 0) {
        // 获取互斥量
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            // 如果是确认状态的第一次调整，保存原始值并切换到未确认状态
            if (u_set_confirmed) {
                orig_u_set = u_set; // 保存原始值用于可能的回滚
                u_set_confirmed = false; // 切换到未确认状态
            }
            
            // 根据当前步进模式选择步进值
            float current_step = use_fine_step ? u_set_step_fine : u_set_step_coarse;
            
            // 更新U_SET值，每个计数单位对应一个步进值
            u_set += encoderValue * current_step;
            
            // 限制U_SET在允许范围内
            if (u_set > u_set_max) {
                u_set = u_set_max;
            } else if (u_set < u_set_min) {
                u_set = u_set_min;
            }
            
            // 更新UI显示
            char u_set_buf[16];
            sprintf(u_set_buf, "%.2f", u_set);
            lv_label_set_text(guider_ui.screen_U_SET, u_set_buf);
            
            // 如果处于未确认状态，将文本颜色更改为黄色
            if (!u_set_confirmed) {
                lv_obj_set_style_text_color(guider_ui.screen_U_SET, lv_color_hex(0xffff00), LV_PART_MAIN|LV_STATE_DEFAULT);
                lv_obj_set_style_text_color(guider_ui.screen_V_label_set, lv_color_hex(0xffff00), LV_PART_MAIN|LV_STATE_DEFAULT);
            }
            
            // 记录最后调整时间，用于超时检查
            last_adjustment_time = millis();
            
            // 释放互斥量
            xSemaphoreGive(dataMutex);
            
            // 请求UI更新
            xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT);
        }
    }
}

// 更新按钮状态函数
void updateButtonState(bool is_on) {
    // 使用互斥量保护数据访问
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        if (is_on) {
            // ON状态 - 绿色
            lv_label_set_text(guider_ui.screen_STATE, "ON");
            lv_obj_set_style_text_color(guider_ui.screen_STATE, lv_color_hex(0x0dff00), LV_PART_MAIN|LV_STATE_DEFAULT);
            
            // 隐藏待机提示标签 - 提前执行UI变更
            lv_obj_add_flag(guider_ui.screen_standby_label1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(guider_ui.screen_standby_label2, LV_OBJ_FLAG_HIDDEN);
            
            // 提前执行一次LVGL刷新，确保待机标签隐藏变更立即生效
            handle_lvgl_tasks();
              // 仅更新任务运行标志，而不创建新任务
            if (xSemaphoreTake(taskControlMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_dataTaskRunning = true;
                xSemaphoreGive(taskControlMutex);
            }
            
            // 恢复显示
            updateDisplay();
            
            // 手动执行一次LVGL刷新，确保显示立即更新
            handle_lvgl_tasks();
        } else {
            // OFF状态 - 红色
            lv_label_set_text(guider_ui.screen_STATE, "OFF");
            lv_obj_set_style_text_color(guider_ui.screen_STATE, lv_color_hex(0xff0027), LV_PART_MAIN|LV_STATE_DEFAULT);
            
            // 显示待机提示标签
            lv_obj_clear_flag(guider_ui.screen_standby_label1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(guider_ui.screen_standby_label2, LV_OBJ_FLAG_HIDDEN);
            
            // 执行一次LVGL刷新，确保待机标签显示立即生效
            handle_lvgl_tasks();
              // 仅更新任务运行标志，而不删除任务
            if (xSemaphoreTake(taskControlMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_dataTaskRunning = false;
                xSemaphoreGive(taskControlMutex);
            }
        }
        xSemaphoreGive(dataMutex);
    }
      // 设置UI更新事件，通知UI任务进行刷新
    xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT);
    
    // 执行一次强制刷新，确保立即显示更改
    handle_lvgl_tasks();
    
    // 等待适当时间确保状态切换完全生效
    vTaskDelay(is_on ? 80 : 50 / portTICK_PERIOD_MS);
    
    // 再次调用LVGL刷新和发送事件，确保所有UI变更都已应用
    handle_lvgl_tasks();
    xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT);
}

// 确认按钮中断处理函数
void IRAM_ATTR confirmButtonISR() {
    // 设置确认事件标志
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(systemEvents, CONFIRM_EVENT, &xHigherPriorityTaskWoken);
    if(xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// 步进切换按钮中断处理函数
void IRAM_ATTR stepSwitchISR() {
    // 设置步进切换事件标志
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(systemEvents, STEP_SWITCH_EVENT, &xHigherPriorityTaskWoken);
    if(xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// 切换步进值
void toggleStepSize() {
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        // 切换细调/粗调模式
        use_fine_step = !use_fine_step;
        
        // 更新UI显示 - 在屏幕上显示当前步进模式
        char step_info[20];
        if (use_fine_step) {
            sprintf(step_info, "步进:0.1V");
        } else {
            sprintf(step_info, "步进:1.0V");
        }
        
        // 在这里可以添加一个临时显示，比如更新某个标签
        // 如果您的UI中有可以显示当前步进值的组件，可以在这里更新它
        // 例如：lv_label_set_text(guider_ui.screen_step_label, step_info);
        
        // 或者输出到串口作为调试信息
        Serial.print("切换步进模式: ");
        Serial.println(step_info);
        
        xSemaphoreGive(dataMutex);
        
        // 请求UI更新
        xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT);
    }
}

// 确认电压设置
void confirmUSet() {
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        // 如果电压设置未确认，进行确认
        if (!u_set_confirmed) {
            u_set_confirmed = true;
            
            // 更新确认后的电压值
            temp_u_set = u_set;
            
            // 设置文本颜色为正常（白色）
            lv_obj_set_style_text_color(guider_ui.screen_U_SET, lv_color_hex(0xe0e0e0), LV_PART_MAIN|LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(guider_ui.screen_V_label_set, lv_color_hex(0xe0e0e0), LV_PART_MAIN|LV_STATE_DEFAULT);

            Serial.print("电压设置已确认: ");
            Serial.println(u_set);
        }
        xSemaphoreGive(dataMutex);
    }
}

// 检查确认超时
void checkConfirmTimeout() {
    // 如果电压设置没有确认，且超过确认超时时间，则回滚到原始值
    if (!u_set_confirmed && (millis() - last_adjustment_time > confirm_timeout)) {
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            // 回滚设置
            u_set = orig_u_set;
            u_set_confirmed = true;
            
            // 更新显示
            char u_set_buf[16];
            sprintf(u_set_buf, "%.2f", u_set);
            lv_label_set_text(guider_ui.screen_U_SET, u_set_buf);
            
            // 恢复正常文本颜色
            lv_obj_set_style_text_color(guider_ui.screen_U_SET, lv_color_hex(0xe0e0e0), LV_PART_MAIN|LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(guider_ui.screen_V_label_set, lv_color_hex(0xe0e0e0), LV_PART_MAIN|LV_STATE_DEFAULT);

            Serial.println("电压设置超时回滚");
            
            xSemaphoreGive(dataMutex);
            
            // 更新UI
            xEventGroupSetBits(systemEvents, UI_UPDATE_EVENT);
        }
    }
}