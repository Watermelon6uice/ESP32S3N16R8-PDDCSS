/**
 * TLC5615 DAC示例代码
 * 此示例演示如何使用重写的myDAC库在多任务环境中控制DAC输出
 */

#include <Arduino.h>
#include "myDAC.h"

// TLC5615 引脚定义
#define DAC_CS_PIN    5  // CS引脚
#define DAC_MOSI_PIN  7  // MOSI引脚
#define DAC_SCK_PIN   6  // SCK引脚

// DAC控制参数
#define DAC_UPDATE_INTERVAL 5000  // DAC更新间隔(毫秒)
#define DAC_MIN_VALUE       0.0   // DAC最小电压值
#define DAC_MAX_VALUE       4.0   // DAC最大电压值
#define DAC_STEP            0.2   // DAC每次增加的电压步长

// 全局变量
MyDAC *dac = NULL;
float currentVoltage = 0.0;
unsigned long lastUpdateTime = 0;

// 主程序DAC更新任务
void dacUpdateTask(void * parameter) {
  while (1) {
    unsigned long currentTime = millis();
    
    // 每隔DAC_UPDATE_INTERVAL更新一次电压值
    if (currentTime - lastUpdateTime >= DAC_UPDATE_INTERVAL) {
      // 更新电压值
      currentVoltage += DAC_STEP;
      if (currentVoltage > DAC_MAX_VALUE) {
        currentVoltage = DAC_MIN_VALUE;
      }
      
      // 通过队列设置DAC电压
      setDACVoltage(currentVoltage);
      
      // 打印信息
      Serial.print("\n设置DAC电压: ");
      Serial.print(currentVoltage);
      Serial.println("V");
      
      lastUpdateTime = currentTime;
    }
    
    // 延时100ms
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32 TLC5615 DAC测试 - FreeRTOS多任务版本");
  
  // 创建和初始化DAC对象
  dac = new MyDAC(DAC_CS_PIN, DAC_MOSI_PIN, DAC_SCK_PIN);
  dac->begin();
  
  // 创建DAC控制任务(接收电压队列值并控制DAC)
  // 使用核心1和优先级1
  createDACTask(dac, 1, 1);
  
  // 创建DAC更新任务(周期性更新电压值)
  // 使用核心0和优先级1
  xTaskCreatePinnedToCore(
    dacUpdateTask,   // 任务函数
    "DAC_Update",    // 任务名称
    2048,            // 堆栈大小
    NULL,            // 任务参数
    1,               // 优先级
    NULL,            // 任务句柄
    0                // 在核心0上运行
  );
  
  Serial.println("所有任务已初始化完成");
}

void loop() {
  // 主循环保持空闲，所有工作由任务处理
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
