#include "myEncoder.h"

// 初始化静态变量
portMUX_TYPE myEncoder::mux = portMUX_INITIALIZER_UNLOCKED;
myEncoder* myEncoder::instance = nullptr;
bool myEncoder::stepButtonPressed = false;
bool myEncoder::stepButtonReleaseHandled = true;
unsigned long myEncoder::lastStepDebounceTime = 0;

// 基本构造函数
myEncoder::myEncoder(int pinA, int pinB) : 
    myEncoder(pinA, pinB, -1, -1, 5.00, 2.00, 15.00, 0.10, 1.00, 5000) // 调用扩展构造函数
{
}

// 扩展构造函数
myEncoder::myEncoder(
    int pinA,
    int pinB,
    int confirmPin,
    int stepSwitchPin,
    float initialUSet,
    float minUSet,
    float maxUSet,
    float fineStep,
    float coarseStep,
    unsigned long confirmTimeoutMs
) : 
    pinA(pinA), 
    pinB(pinB),
    confirmPin(confirmPin),
    stepSwitchPin(stepSwitchPin),
    encoderCount(0),
    lastDirection(0),  // 初始化方向跟踪变量
    u_set(initialUSet),
    orig_u_set(initialUSet),
    last_u_set(initialUSet),  // 初始化last_u_set
    u_set_min(minUSet),
    u_set_max(maxUSet),
    u_set_step_fine(fineStep),
    u_set_step_coarse(coarseStep),
    use_fine_step(true),
    u_set_confirmed(true),
    confirm_timeout(confirmTimeoutMs),
    systemEventsPtr(nullptr),
    stepSwitchQueue(nullptr),
    stepButtonTaskHandle(nullptr),    _uSetDisplayCallback(nullptr),    lastPinAState(false),    lastPinBState(false),
    lastDebounceTime(0),
    debounceDelay(0) // 完全移除消抖延时，以最大限度提高响应速度
{
    instance = this;
    dataMutex = xSemaphoreCreateMutex();
    
    // 如果配置了步进切换按钮，创建消息队列
    if (stepSwitchPin >= 0) {
        stepSwitchQueue = xQueueCreate(10, sizeof(uint32_t));
    }
}

// 初始化编码器
void myEncoder::begin() {    
    // 清除之前可能存在的中断
    if (pinA >= 0) {
        detachInterrupt(digitalPinToInterrupt(pinA));
    }
    if (pinB >= 0) {
        detachInterrupt(digitalPinToInterrupt(pinB));
    }
    if (confirmPin >= 0) {
        detachInterrupt(digitalPinToInterrupt(confirmPin));
    }
    if (stepSwitchPin >= 0) {
        detachInterrupt(digitalPinToInterrupt(stepSwitchPin));
    }

    // 配置编码器引脚
    pinMode(pinA, INPUT_PULLUP);
    pinMode(pinB, INPUT_PULLUP);
    
    // 为A相和B相都设置中断，与STM32参考实现相似
    attachInterrupt(digitalPinToInterrupt(pinA), isrA, CHANGE);
    attachInterrupt(digitalPinToInterrupt(pinB), isrB, CHANGE);
    Serial.print("编码器A相已设置为电平变化中断，引脚号: ");
    Serial.println(pinA);
    Serial.print("编码器B相已设置为电平变化中断，引脚号: ");
    Serial.println(pinB);
    
    // 如果设置了确认按钮，配置相应的中断
    if (confirmPin >= 0) {
        pinMode(confirmPin, INPUT_PULLDOWN);
        attachInterrupt(digitalPinToInterrupt(confirmPin), confirmButtonISR, RISING);
        Serial.print("确认按钮已设置为中断模式, 引脚号: ");
        Serial.println(confirmPin);
    }
    
    // 如果设置了步进切换按钮，配置相应的中断和任务
    if (stepSwitchPin >= 0) {
        // 确保之前的任务被删除
        if (stepButtonTaskHandle != nullptr) {
            vTaskDelete(stepButtonTaskHandle);
            stepButtonTaskHandle = nullptr;
        }
        
        pinMode(stepSwitchPin, INPUT_PULLDOWN);
        attachInterrupt(digitalPinToInterrupt(stepSwitchPin), stepSwitchISR, CHANGE);
        Serial.print("步进切换按钮已设置为中断模式，引脚号: ");
        Serial.println(stepSwitchPin);
        
        // 创建步进按钮监控任务
        createStepButtonTask();
    }
}

// 读取并重置编码器计数 - 与STM32参考实现类似
int16_t myEncoder::read() {
    int16_t count;
    
    // 进入临界区，禁止中断干扰
    portENTER_CRITICAL(&mux);
    count = encoderCount;
    encoderCount = 0; // 读取后归零，与STM32实现一致
    portEXIT_CRITICAL(&mux);
    
    // 只在真实检测到旋转时输出信息
    if (count != 0) {
        Serial.printf("[编码器计数] 读取到计数值: %d, 方向: %s\n", 
                     count, 
                     count > 0 ? "顺时针" : "逆时针");
    }
    
    return count;
}

// 静态中断处理函数，转发到实例方法
void IRAM_ATTR myEncoder::isrA() {
    if (instance) {
        instance->handleIsrA();
    }
}

// B相的静态中断处理函数
void IRAM_ATTR myEncoder::isrB() {
    if (instance) {
        instance->handleIsrB();
    }
}

// 确认按钮中断处理函数
void IRAM_ATTR myEncoder::confirmButtonISR() {
    if (instance && instance->systemEventsPtr) {
        // 设置确认事件标志
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xEventGroupSetBitsFromISR(*(instance->systemEventsPtr), instance->CONFIRM_EVENT, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

// 步进切换按钮中断处理函数
void IRAM_ATTR myEncoder::stepSwitchISR() {
    if (instance && instance->stepSwitchQueue) {
        // 根据按钮状态发送不同消息
        uint32_t msg;
        if (digitalRead(instance->stepSwitchPin) == HIGH) {
            // 按钮按下
            msg = instance->MSG_STEP_BUTTON_PRESSED;
        } else {
            // 按钮释放
            msg = instance->MSG_STEP_BUTTON_RELEASED;
        }
        
        // 添加高优先级任务唴醒标志
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        // 发送消息到队列，带有任务唤醒功能
        xQueueSendFromISR(instance->stepSwitchQueue, &msg, &xHigherPriorityTaskWoken);
        
        // 如果有更高优先级的任务被唤醒，请求上下文切换
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

// A相中断处理 - 基于STM32参考实现
void myEncoder::handleIsrA() {
    // 极简实现，移除所有消抖和延时
    // 直接在A相为高电平时检查B相
    if (digitalRead(pinA) == HIGH) {
        // 检查B相状态，如果B相为低电平，减少计数（逆时针）
        if (digitalRead(pinB) == LOW) {
            portENTER_CRITICAL(&mux);
            encoderCount--;
            lastDirection = -1;
            portEXIT_CRITICAL(&mux);
#ifdef DEBUG_ENCODER
            Serial.println("[编码器A中断] 检测到逆时针旋转");
#endif
        }
    }
}

// B相中断处理 - 基于STM32参考实现
void myEncoder::handleIsrB() {
    // 极简实现，移除所有消抖和延时
    // 直接在B相为高电平时检查A相
    if (digitalRead(pinB) == HIGH) {
        // 当检测到B相上升沿时，检查A相状态
        // 如果A相为低电平，增加计数（顺时针）
        if (digitalRead(pinA) == LOW) {
            portENTER_CRITICAL(&mux);
            encoderCount++;
            lastDirection = 1;
            portEXIT_CRITICAL(&mux);
#ifdef DEBUG_ENCODER
            Serial.println("[编码器B中断] 检测到顺时针旋转");
#endif
        }
    }
}

// 创建步进按钮任务
void myEncoder::createStepButtonTask() {
    // 创建步进按钮监控任务
    xTaskCreate(
        stepButtonTask,       // 任务函数
        "StepButtonTask",     // 任务名称
        2048,                 // 堆栈大小
        this,                 // 任务参数
        1,                    // 高优先级(1是最高优先级)
        &stepButtonTaskHandle // 任务句柄
    );
    
    Serial.println("步进按钮监控任务已启动，优先级已优化");
}

// 步进按钮监控任务
void myEncoder::stepButtonTask(void* parameter) {
    myEncoder* encoder = static_cast<myEncoder*>(parameter);
    uint32_t msg;
    
    // 设置任务优先级较高，确保按钮响应及时
    vTaskPrioritySet(NULL, 1); // 在ESP-IDF FreeRTOS中，数字越小优先级越高
    
    while (1) {
        // 等待队列消息，无限等待
        if (xQueueReceive(encoder->stepSwitchQueue, &msg, portMAX_DELAY)) {
            // 消息处理
            unsigned long currentMillis = millis();
            
            // 按钮按下消息
            if (msg == encoder->MSG_STEP_BUTTON_PRESSED && !stepButtonPressed) {
                if (currentMillis - lastStepDebounceTime > STEP_DEBOUNCE_DELAY) {
                    lastStepDebounceTime = currentMillis;
                    
                    // 确认按钮按下，增强防抖检测
                    if (digitalRead(encoder->stepSwitchPin) == HIGH) {
                        // 短暂等待再次检查，增强防抖可靠性
                        ets_delay_us(1000); // 微秒级延迟，不影响任务调度
                        
                        // 再次确认按钮状态
                        if (digitalRead(encoder->stepSwitchPin) == HIGH) {
                            stepButtonPressed = true;
                            stepButtonReleaseHandled = false;
                            Serial.println("步进按钮已按下");
                        }
                    }
                }
            }
            
            // 按钮释放消息
            else if (msg == encoder->MSG_STEP_BUTTON_RELEASED && stepButtonPressed && !stepButtonReleaseHandled) {
                if (currentMillis - lastStepDebounceTime > STEP_DEBOUNCE_DELAY) {
                    lastStepDebounceTime = currentMillis;
                    
                    // 确认按钮释放，增强防抖检测
                    if (digitalRead(encoder->stepSwitchPin) == LOW) {
                        // 短暂等待再次检查，增强防抖可靠性
                        ets_delay_us(1000); // 微秒级延迟，不影响任务调度
                        
                        // 再次确认按钮状态
                        if (digitalRead(encoder->stepSwitchPin) == LOW) {
                            stepButtonPressed = false;
                            stepButtonReleaseHandled = true;
                            
                            Serial.println("步进按钮已释放，触发步进值切换");
                            
                            // 延迟50ms确保系统稳定，然后再发送事件
                            vTaskDelay(pdMS_TO_TICKS(50));
                            
                            if (encoder->systemEventsPtr) {
                                Serial.println("发送步进切换事件到UI任务...");
                                xEventGroupSetBits(*(encoder->systemEventsPtr), encoder->STEP_SWITCH_EVENT);
                            }
                        }
                    }
                }
            }
        }
        
        // 任务延时(减少CPU使用，但保持较快响应)
        vTaskDelay(3 / portTICK_PERIOD_MS);
    }
}

// 设置系统事件组
void myEncoder::setSystemEvents(EventGroupHandle_t* eventGroupHandle) {
    systemEventsPtr = eventGroupHandle;
}

// 获取系统事件组指针
EventGroupHandle_t* myEncoder::getSystemEventsPtr() {
    return systemEventsPtr;
}

// 获取当前电压设置值
float myEncoder::getUSet() {
    float value;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        value = u_set;
        xSemaphoreGive(dataMutex);
    }
    return value;
}

// 检查电压设置是否已确认
bool myEncoder::isUSetConfirmed() {
    bool confirmed;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        confirmed = u_set_confirmed;
        xSemaphoreGive(dataMutex);
    }
    return confirmed;
}

// 确认当前电压设置
void myEncoder::confirmUSet() {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 如果电压设置未确认，进行确认
        if (!u_set_confirmed) {
            u_set_confirmed = true;
            
            Serial.print("电压设置已确认: ");
            Serial.println(u_set);
            
            // 调用UI回调函数
            if (_uSetDisplayCallback) {
                _uSetDisplayCallback(u_set, true, use_fine_step, this);
            }
        }
        xSemaphoreGive(dataMutex);
    }
}

// 重置电压设置为原始值
void myEncoder::resetUSet() {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 回滚设置
        u_set = orig_u_set;
        u_set_confirmed = true;
        
        Serial.println("电压设置已重置");
        
        // 调用UI回调函数
        if (_uSetDisplayCallback) {
            _uSetDisplayCallback(u_set, true, use_fine_step, this);
        }
        
        xSemaphoreGive(dataMutex);
    }
}

// 切换步进大小，返回当前状态 (true=细调, false=粗调)
bool myEncoder::toggleStepSize() {
    bool currentState = false;
    
    // 使用互斥量保护共享数据访问
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {  // 增加超时时间确保获取到互斥锁
        // 保存当前确认状态
        bool was_confirmed = u_set_confirmed;
        
        // 切换细调/粗调模式
        use_fine_step = !use_fine_step;
        currentState = use_fine_step;
        
        // 输出到串口作为调试信息
        Serial.print("切换步进模式: ");
        Serial.print(use_fine_step ? "细调" : "粗调");
        Serial.print("，当前步进值: ");
        Serial.println(use_fine_step ? u_set_step_fine : u_set_step_coarse);
        
        // 调用UI回调函数
        if (_uSetDisplayCallback) {
            _uSetDisplayCallback(u_set, u_set_confirmed, use_fine_step, this);
        }
        
        xSemaphoreGive(dataMutex);
    }
    
    return currentState;
}

// 获取当前步进值
float myEncoder::getCurrentStepSize() {
    float step;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        step = use_fine_step ? u_set_step_fine : u_set_step_coarse;
        xSemaphoreGive(dataMutex);
    }
    return step;
}

// 检查确认超时
void myEncoder::checkConfirmTimeout() {
    // 如果电压设置没有确认，且超过确认超时时间，则回滚到原始值
    if (u_set_confirmed == false && (millis() - last_adjustment_time > confirm_timeout)) {
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            // 回滚设置
            u_set = orig_u_set;
            u_set_confirmed = true;
            
            Serial.println("电压设置超时回滚");
            
            // 调用UI回调函数
            if (_uSetDisplayCallback) {
                _uSetDisplayCallback(u_set, true, use_fine_step, this);
            }
            
            xSemaphoreGive(dataMutex);
            
            // 如果有系统事件组，设置UI更新事件
            if (systemEventsPtr) {
                xEventGroupSetBits(*systemEventsPtr, (1 << 0)); // UI_UPDATE_EVENT
            }
        }
    }
}

// 从编码器读取并更新U_SET值
void myEncoder::updateUSetFromEncoder() {
    static unsigned long lastUpdateTime = 0;
    unsigned long currentTime = millis();    // 控制更新频率，防止过快的重复读取
    if (currentTime - lastUpdateTime < 20) {  // 进一步减少到20ms以提高响应速度
        return;
    }
    
    // 读取编码器值
    int16_t encoderValue = 0;
    
    // 临界区保护读取和清零操作
    portENTER_CRITICAL(&mux);
    encoderValue = encoderCount;
    // 仅在有值时清零，避免清除刚刚产生但还未处理的脉冲
    if (encoderValue != 0) {
        encoderCount = 0;
    }
    portEXIT_CRITICAL(&mux);
    
    // 如果有旋转，更新U_SET值
    if (encoderValue != 0) {
        lastUpdateTime = currentTime; // 更新时间戳
        
        Serial.println("\n[编码器更新] ====开始====");
        Serial.printf("[编码器计数] 读取到计数值: %d, 方向: %s\n", 
                     encoderValue, 
                     encoderValue > 0 ? "顺时针" : "逆时针");
        Serial.printf("[编码器更新] 当前状态: 步进=%s, 已确认=%s\n",
                     use_fine_step ? "细调" : "粗调",
                     u_set_confirmed ? "是" : "否");
        
        // 获取互斥量
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            float oldValue = u_set;
            
            // 如果是确认状态的第一次调整，保存原始值并切换到未确认状态
            if (u_set_confirmed) {
                orig_u_set = u_set;
                u_set_confirmed = false;
                Serial.printf("[编码器更新] 首次调整，保存原始值: %.2f\n", orig_u_set);
            }
            
            // 根据当前步进模式选择步进值并计算新值
            float step = use_fine_step ? u_set_step_fine : u_set_step_coarse;
            float newValue = u_set + (encoderValue * step);
            
            Serial.printf("[编码器更新] 计算: %.2f %c %.2f = %.2f\n", 
                         u_set, 
                         (encoderValue * step) >= 0 ? '+' : '-',
                         fabs(encoderValue * step), 
                         newValue);
            
            // 限制范围
            if (newValue > u_set_max) {
                newValue = u_set_max;
                Serial.println("[编码器更新] 已达到最大值限制");
            }
            if (newValue < u_set_min) {
                newValue = u_set_min;
                Serial.println("[编码器更新] 已达到最小值限制");
            }
            
            // 更新值
            u_set = newValue;
            last_adjustment_time = millis();
            
            Serial.printf("[编码器更新] 值已更新: %.2f -> %.2f\n", oldValue, newValue);
            
            // 调用UI回调
            if (_uSetDisplayCallback) {
                _uSetDisplayCallback(u_set, u_set_confirmed, use_fine_step, this);
                Serial.println("[编码器更新] UI回调已执行");
            }
            
            // 设置UI更新事件
            if (systemEventsPtr) {
                xEventGroupSetBits(*systemEventsPtr, (1 << 0));
                Serial.println("[编码器更新] UI更新事件已设置");
            }
            
            xSemaphoreGive(dataMutex);
        } else {
            Serial.println("[编码器更新] 错误：无法获取互斥量!");
        }
        
        Serial.println("[编码器更新] ====结束====\n");
    }
}

// 反转编码器方向
void myEncoder::reverseDirection() {
    // 在双中断模式下，我们需要切换A和B中断处理程序的功能
    portENTER_CRITICAL(&mux);
    // 临时禁用中断
    detachInterrupt(digitalPinToInterrupt(pinA));
    detachInterrupt(digitalPinToInterrupt(pinB));
    
    // 重新附加中断，但交换A和B的角色
    attachInterrupt(digitalPinToInterrupt(pinA), isrB, RISING);
    attachInterrupt(digitalPinToInterrupt(pinB), isrA, RISING);
    portEXIT_CRITICAL(&mux);
    
    Serial.println("编码器方向已反转!");
}

// 设置UI回调函数
void myEncoder::setUSetDisplayCallback(USetDisplayCallback callback) {
    _uSetDisplayCallback = callback;
}
