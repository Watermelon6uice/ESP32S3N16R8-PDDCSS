#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// 校准参数（根据实际校准结果修改）
uint16_t calData[5] = { 443, 3423, 304, 3409, 7 };

// 画笔颜色和大小
uint16_t penColor = TFT_RED;
uint8_t penSize = 3;

// 按钮区域定义
#define CLEAR_BUTTON_X 10
#define CLEAR_BUTTON_Y 10
#define CLEAR_BUTTON_WIDTH 100
#define CLEAR_BUTTON_HEIGHT 30

// 颜色选择按钮定义
#define COLOR_BUTTON_Y 50
#define COLOR_BUTTON_WIDTH 40
#define COLOR_BUTTON_HEIGHT 30
#define COLOR_BUTTON_SPACING 10

// 将函数定义移到setup()之前
void drawColorButtons() {
  // 红色
  tft.fillRect(10, COLOR_BUTTON_Y, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, TFT_RED);
  // 绿色
  tft.fillRect(60, COLOR_BUTTON_Y, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, TFT_GREEN);
  // 蓝色
  tft.fillRect(110, COLOR_BUTTON_Y, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, TFT_BLUE);
  // 黄色
  tft.fillRect(160, COLOR_BUTTON_Y, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, TFT_YELLOW);
  // 白色
  tft.fillRect(210, COLOR_BUTTON_Y, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, TFT_WHITE);
}

void initCanvas() {
  tft.fillScreen(TFT_BLACK);
  
  // 绘制清除按钮
  tft.fillRect(CLEAR_BUTTON_X, CLEAR_BUTTON_Y, CLEAR_BUTTON_WIDTH, CLEAR_BUTTON_HEIGHT, TFT_GREEN);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(CLEAR_BUTTON_X + 10, CLEAR_BUTTON_Y + 8);
  tft.print("清除");
  
  // 绘制颜色选择按钮
  drawColorButtons();
  
  // 绘制画布区域提示
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, tft.height() - 20);
  tft.print("触摸屏幕画画");
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);  // 根据屏幕实际方向调整
  
  // 应用校准参数
  tft.setTouch(calData);
  
  // 初始化画布
  initCanvas();
}

void loop() {
  uint16_t x, y;
  
  // 检测触摸
  if (tft.getTouch(&x, &y, 100)) {
    // 判断是否点击清除按钮
    if (x >= CLEAR_BUTTON_X && x <= CLEAR_BUTTON_X + CLEAR_BUTTON_WIDTH &&
        y >= CLEAR_BUTTON_Y && y <= CLEAR_BUTTON_Y + CLEAR_BUTTON_HEIGHT) {
      initCanvas();
      delay(300);  // 防抖
      return;
    }
    
    // 判断是否点击颜色按钮
    if (y >= COLOR_BUTTON_Y && y <= COLOR_BUTTON_Y + COLOR_BUTTON_HEIGHT) {
      if (x >= 10 && x <= 50) penColor = TFT_RED;
      if (x >= 60 && x <= 100) penColor = TFT_GREEN;
      if (x >= 110 && x <= 150) penColor = TFT_BLUE;
      if (x >= 160 && x <= 200) penColor = TFT_YELLOW;
      if (x >= 210 && x <= 250) penColor = TFT_WHITE;
      delay(300);  // 防抖
      return;
    }
    
    // 在触摸位置绘制点（避开按钮区域）
    if (y > COLOR_BUTTON_Y + COLOR_BUTTON_HEIGHT + 10) {
      tft.fillCircle(x, y, penSize, penColor);
    }
  }
  
  delay(10);  // 降低CPU使用率
}
// 代码结束