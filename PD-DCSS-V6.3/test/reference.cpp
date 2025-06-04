#include <Arduino.h>
#include <SPI.h>

// TLC5615 引脚定义保持不变
#define TLC5615_CS_PIN    5
#define TLC5615_MOSI_PIN  7
#define TLC5615_SCK_PIN   6

// ADC引脚定义 - 使用GPIO1、2、3、4作为ADC输入
#define ADC_PIN1          1  // GPIO1 用作ADC输入
#define ADC_PIN2          2  // GPIO2 用作ADC输入
#define ADC_PIN3          3  // GPIO3 用作ADC输入
#define ADC_PIN4          4  // GPIO4 用作ADC输入

// DAC控制参数
#define DAC_UPDATE_INTERVAL 5000  // DAC更新间隔(毫秒)
#define DAC_MIN_VALUE       0     // DAC最小值
#define DAC_MAX_VALUE       1023  // DAC最大值
#define DAC_STEP            50    // DAC每次增加的步长

uint16_t currentDacValue = 0;     // 当前DAC输出值
unsigned long lastDacUpdateTime = 0; // 上次DAC更新时间

// TLC5615类定义保持不变
class TLC5615 {
private:
    uint8_t cs_pin;
    SPIClass* spi;

public:
    TLC5615(uint8_t cs_pin, uint8_t mosi_pin, uint8_t sck_pin) : cs_pin(cs_pin) {
        spi = new SPIClass(HSPI);
        spi->begin(sck_pin, -1, mosi_pin, -1); // MISO pin not used
        pinMode(cs_pin, OUTPUT);
        digitalWrite(cs_pin, HIGH);
    }

    // 设置DAC输出值（0-1023，对应0-4.096V）
    void setValue(uint16_t value) {
        value &= 0x3FF; // 确保值在10位范围内
        uint16_t data = value << 2; // TLC5615要求数据左移2位

        digitalWrite(cs_pin, LOW);
        spi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        spi->transfer16(data);
        spi->endTransaction();
        digitalWrite(cs_pin, HIGH);
    }
};

TLC5615 dac(TLC5615_CS_PIN, TLC5615_MOSI_PIN, TLC5615_SCK_PIN);

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("ESP32S3 多通道ADC测试 + DAC输出控制");
    
    // 配置ADC引脚为下拉输入模式
    pinMode(ADC_PIN1, INPUT_PULLDOWN);
    pinMode(ADC_PIN2, INPUT_PULLDOWN);
    pinMode(ADC_PIN3, INPUT_PULLDOWN);
    pinMode(ADC_PIN4, INPUT_PULLDOWN);
    
    // 配置ADC
    analogReadResolution(12); // ESP32S3的ADC是12位分辨率
    analogSetAttenuation(ADC_11db); 
    
    // 打印ESP32S3 ADC信息
    Serial.println("使用GPIO1, GPIO2, GPIO3, GPIO4作为ADC输入引脚");
    Serial.println("所有ADC引脚已配置为下拉输入模式");
    Serial.println("TLC5615 DAC输出值范围: 0-1023 (对应 0-4.096V)");
    
    // 初始化DAC输出为0
    dac.setValue(currentDacValue);
    Serial.print("DAC初始输出值: ");
    Serial.print(currentDacValue);
    Serial.print(" (");
    Serial.print((currentDacValue / 1023.0) * 4.096, 3);
    Serial.println("V)");
}

void loop() {
    // 更新DAC输出值
    unsigned long currentTime = millis();
    if (currentTime - lastDacUpdateTime >= DAC_UPDATE_INTERVAL) {
        // 更新DAC值
        currentDacValue += DAC_STEP;
        if (currentDacValue > DAC_MAX_VALUE) {
            currentDacValue = DAC_MIN_VALUE;  // 重置为最小值
        }
        
        // 设置DAC输出
        dac.setValue(currentDacValue);
        
        // 计算实际电压并打印信息
        float dacVoltage = (currentDacValue / 1023.0) * 4.096;
        Serial.print("\nDAC输出值更新: ");
        Serial.print(currentDacValue);
        Serial.print(" (");
        Serial.print(dacVoltage, 3);
        Serial.println("V)");
        
        lastDacUpdateTime = currentTime;
    }
    
    // 读取四个ADC通道的值
    int adcRawValue1 = analogRead(ADC_PIN1);
    int adcRawValue2 = analogRead(ADC_PIN2);
    int adcRawValue3 = analogRead(ADC_PIN3);
    int adcRawValue4 = analogRead(ADC_PIN4);
    
    // 转换ADC值为电压 (ESP32S3 ADC参考电压为3.3V，12位分辨率)
    float adcVoltage1 = (adcRawValue1 / 4095.0) * 3.3;
    float adcVoltage2 = (adcRawValue2 / 4095.0) * 3.3;
    float adcVoltage3 = (adcRawValue3 / 4095.0) * 3.3;
    float adcVoltage4 = (adcRawValue4 / 4095.0) * 3.3;
    
    // 打印结果
    Serial.println("=========================================");
    
    Serial.print("ADC1 (GPIO1) - 原始值: ");
    Serial.print(adcRawValue1);
    Serial.print(" 电压: ");
    Serial.print(adcVoltage1, 3);
    Serial.println("V");
    
    Serial.print("ADC2 (GPIO2) - 原始值: ");
    Serial.print(adcRawValue2);
    Serial.print(" 电压: ");
    Serial.print(adcVoltage2, 3);
    Serial.println("V");
    
    Serial.print("ADC3 (GPIO3) - 原始值: ");
    Serial.print(adcRawValue3);
    Serial.print(" 电压: ");
    Serial.print(adcVoltage3, 3);
    Serial.println("V");
    
    Serial.print("ADC4 (GPIO4) - 原始值: ");
    Serial.print(adcRawValue4);
    Serial.print(" 电压: ");
    Serial.print(adcVoltage4, 3);
    Serial.println("V");
    
    delay(1000); // 每1秒更新一次ADC读数
}