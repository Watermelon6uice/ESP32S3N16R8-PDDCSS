#include <lvgl.h>

#include <TFT_eSPI.h>
#include <EEPROM.h>

#include "gui_guider.h"
#include "events_init.h"
#include "custom.h"

// 定义重启按钮的GPIO引脚，请根据您的实际硬件连接修改
#define RESET_BUTTON_PIN 0  // 通常ESP32开发板上的Boot按钮连接到GPIO0

//定义分辨率
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
//定义缓冲
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * 20 ];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */

// 触摸屏参考点坐标 - 这些点已经在LVGL界面上绘制
#define TS_POINT1_X 25
#define TS_POINT1_Y 25
#define TS_POINT2_X 295
#define TS_POINT2_Y 25
#define TS_POINT3_X 25
#define TS_POINT3_Y 215
#define TS_POINT4_X 295
#define TS_POINT4_Y 215

// 触摸屏校准标志地址在EEPROM中的位置
#define EEPROM_CALIBRATION_FLAG_ADDR 0
#define EEPROM_CALIBRATION_DATA_ADDR 4
#define CALIBRATION_FLAG 0x12345678

// 校准状态
bool calibration_needed = false;
uint16_t calibration_step = 0;
uint16_t ts_points[4][2] = {
    {TS_POINT1_X, TS_POINT1_Y},
    {TS_POINT2_X, TS_POINT2_Y},
    {TS_POINT3_X, TS_POINT3_Y},
    {TS_POINT4_X, TS_POINT4_Y}
};
uint16_t touch_points[4][2];

// LVGL校准界面对象
lv_obj_t *calib_screen = NULL;
lv_obj_t *calib_points[4] = {NULL};
lv_obj_t *calib_label = NULL;

// 函数声明
void create_calibration_screen();
void highlight_calibration_point();
void process_calibration_touch(uint16_t touchX, uint16_t touchY, bool touched);
void calculate_calibration_matrix();
bool check_calibration_needed();
void init_gui();
bool is_reset_button_pressed();
void clear_calibration_data();
void print_calibration_data(uint16_t calData[16]);

#if LV_USE_LOG != 0
/* Serial debugging  串口调试用*/
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

/* Display flushing 显示填充 与LCD驱动关联*/
void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.endWrite();

    lv_disp_flush_ready( disp );
}

/*Read the touchpad*/
/*输入设备，读取触摸板*/
void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
{
    uint16_t touchX, touchY;

    bool touched = tft.getTouch( &touchX, &touchY, 600 );

    if( !touched )
    {
        data->state = LV_INDEV_STATE_REL;
        
        // 在释放状态下，如果是校准模式，也需要处理释放事件
        if (calibration_needed) {
            process_calibration_touch(0, 0, false);
        }
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;

        // 如果校准已完成，则使用校准后的坐标
        if (!calibration_needed) {
            /*Set the coordinates*/
            data->point.x = touchX;
            data->point.y = touchY;
        } else {
            // 校准过程中，使用原始坐标
            data->point.x = touchX;
            data->point.y = touchY;
            
            // 处理校准点击
            process_calibration_touch(touchX, touchY, true);
        }

        Serial.print( "Data x " );
        Serial.println( touchX );

        Serial.print( "Data y " );
        Serial.println( touchY );
    }
}

// 创建LVGL校准界面
void create_calibration_screen() {
    // 创建校准屏幕
    calib_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(calib_screen, lv_color_black(), 0);
    
    // 创建校准标签
    calib_label = lv_label_create(calib_screen);
    lv_label_set_text(calib_label, "请依次点击四个校准点");
    lv_obj_align(calib_label, LV_ALIGN_TOP_MID, 0, 10);
    
    // 高亮当前校准点
    highlight_calibration_point();
    
    // 加载校准屏幕
    lv_scr_load(calib_screen);
}

// 高亮当前校准点
void highlight_calibration_point() {
    // 清除之前的点
    for(int i = 0; i < 4; i++) {
        if(calib_points[i] != NULL) {
            lv_obj_del(calib_points[i]);
            calib_points[i] = NULL;
        }
    }
    
    // 创建/更新校准指示标签
    char buf[64];
    sprintf(buf, "请点击第 %d 个校准点 (%d,%d)", 
            calibration_step + 1, 
            ts_points[calibration_step][0], 
            ts_points[calibration_step][1]);
    lv_label_set_text(calib_label, buf);
    
    // 更新校准点样式
    for (int i = 0; i < 4; i++) {
        calib_points[i] = lv_obj_create(calib_screen);
        lv_obj_set_size(calib_points[i], 10, 10);
        lv_obj_set_pos(calib_points[i], ts_points[i][0] - 5, ts_points[i][1] - 5);
        
        if (i == calibration_step) {
            // 当前校准点高亮显示
            lv_obj_set_style_bg_color(calib_points[i], lv_color_make(255, 0, 0), 0); // 红色
            // 添加十字标记
            lv_obj_t *h_line = lv_line_create(calib_screen);
            lv_obj_t *v_line = lv_line_create(calib_screen);
            
            static lv_point_t h_points[] = {{ts_points[i][0] - 10, ts_points[i][1]}, {ts_points[i][0] + 10, ts_points[i][1]}};
            static lv_point_t v_points[] = {{ts_points[i][0], ts_points[i][1] - 10}, {ts_points[i][0], ts_points[i][1] + 10}};
            
            lv_line_set_points(h_line, h_points, 2);
            lv_line_set_points(v_line, v_points, 2);
            
            lv_obj_set_style_line_color(h_line, lv_color_white(), 0);
            lv_obj_set_style_line_color(v_line, lv_color_white(), 0);
            lv_obj_set_style_line_width(h_line, 2, 0);
            lv_obj_set_style_line_width(v_line, 2, 0);
        } else {
            // 其他点灰色显示
            lv_obj_set_style_bg_color(calib_points[i], lv_color_make(100, 100, 100), 0);
        }
    }
}

// 处理校准触摸事件
void process_calibration_touch(uint16_t touchX, uint16_t touchY, bool touched) {
    static unsigned long last_touch_time = 0;
    static bool waiting_for_release = false;
    
    // 添加调试输出
    Serial.print("校准处理: touched=");
    Serial.print(touched);
    Serial.print(", waiting_for_release=");
    Serial.print(waiting_for_release);
    Serial.print(", time_since_last=");
    Serial.println(millis() - last_touch_time);
    
    if (touched && !waiting_for_release && (millis() - last_touch_time > 500)) {
        // 记录触摸点
        touch_points[calibration_step][0] = touchX;
        touch_points[calibration_step][1] = touchY;
        
        Serial.print("校准点 ");
        Serial.print(calibration_step + 1);
        Serial.print(" 触摸坐标: x=");
        Serial.print(touchX);
        Serial.print(", y=");
        Serial.println(touchY);
        
        waiting_for_release = true;
        last_touch_time = millis();
        
        // 添加视觉反馈 - 临时改变当前校准点颜色
        if (calib_points[calibration_step] != NULL) {
            lv_obj_set_style_bg_color(calib_points[calibration_step], lv_color_make(0, 255, 0), 0); // 绿色表示已点击
        }
    } 
    else if (!touched && waiting_for_release) {
        waiting_for_release = false;
        calibration_step++;
        
        // 校准完成
        if (calibration_step >= 4) {
            calculate_calibration_matrix();
            calibration_needed = false;
            
            // 销毁校准屏幕，加载主界面
            init_gui();
        } else {
            // 高亮下一个校准点
            highlight_calibration_point();
        }
        
        last_touch_time = millis();
    }
}

// 计算校准参数并保存
void calculate_calibration_matrix() {
    Serial.println("计算校准矩阵...");
    
    // 在这里使用已收集的四个点计算校准参数
    // 对于TFT_eSPI库，我们需要计算16个值的校准数据
    uint16_t calData[16];
    
    // 填充TFT_eSPI校准数据格式
    calData[0] = touch_points[0][0];
    calData[1] = touch_points[0][1];
    calData[2] = ts_points[0][0];
    calData[3] = ts_points[0][1];
    
    calData[4] = touch_points[1][0];
    calData[5] = touch_points[1][1];
    calData[6] = ts_points[1][0];
    calData[7] = ts_points[1][1];
    
    calData[8] = touch_points[2][0];
    calData[9] = touch_points[2][1];
    calData[10] = ts_points[2][0];
    calData[11] = ts_points[2][1];
    
    calData[12] = touch_points[3][0];
    calData[13] = touch_points[3][1];
    calData[14] = ts_points[3][0];
    calData[15] = ts_points[3][1];
    
    // 保存校准数据到EEPROM
    EEPROM.begin(512);
    EEPROM.put(EEPROM_CALIBRATION_DATA_ADDR, calData);
    EEPROM.put(EEPROM_CALIBRATION_FLAG_ADDR, CALIBRATION_FLAG);
    EEPROM.commit();
    
    // 应用校准数据
    tft.setTouch(calData);
    
    // 打印校准参数到串口
    print_calibration_data(calData);
    
    Serial.println("校准完成并保存");
}

// 检测重启按钮是否按下
bool is_reset_button_pressed() {
    // 读取按钮状态，通常按钮按下为LOW
    return (digitalRead(RESET_BUTTON_PIN) == LOW);
}

// 清除校准参数
void clear_calibration_data() {
    EEPROM.begin(512);
    // 写入一个无效的标志，使校准过程重新开始
    EEPROM.put(EEPROM_CALIBRATION_FLAG_ADDR, 0);
    EEPROM.commit();
    Serial.println("校准数据已清除，设备将进行重新校准");
}

// 打印校准参数
void print_calibration_data(uint16_t calData[16]) {
    Serial.println("\n----- 校准参数 -----");
    Serial.println("原始触摸坐标 -> 屏幕坐标映射:");
    
    for (int i = 0; i < 4; i++) {
        Serial.print("点 ");
        Serial.print(i + 1);
        Serial.print(": 触摸坐标(");
        Serial.print(calData[i*4]);
        Serial.print(",");
        Serial.print(calData[i*4+1]);
        Serial.print(") -> 屏幕坐标(");
        Serial.print(calData[i*4+2]);
        Serial.print(",");
        Serial.print(calData[i*4+3]);
        Serial.println(")");
    }
    
    Serial.println("----- 校准参数结束 -----\n");
}

// 检查是否需要校准
bool check_calibration_needed() {
    EEPROM.begin(512);
    
    // 首先检查重启按钮是否被按下
    if (is_reset_button_pressed()) {
        // 等待按钮释放，防止多次触发
        delay(50);
        while (is_reset_button_pressed()) {
            delay(10);
        }
        Serial.println("检测到重启按钮按下，将清除校准数据");
        clear_calibration_data();
        return true;
    }
    
    // 检查是否已经校准
    uint32_t calibration_flag;
    EEPROM.get(EEPROM_CALIBRATION_FLAG_ADDR, calibration_flag);
    
    if (calibration_flag != CALIBRATION_FLAG) {
        Serial.println("需要触摸屏校准");
        return true;
    }
    
    // 读取校准数据
    uint16_t calData[16];
    EEPROM.get(EEPROM_CALIBRATION_DATA_ADDR, calData);
    
    // 打印已保存的校准参数
    print_calibration_data(calData);
    
    // 应用已保存的校准数据
    tft.setTouch(calData);
    Serial.println("已加载校准数据");
    return false;
}

// 初始化GUI
void init_gui() {
    // 初始化UI
    setup_ui(&guider_ui);
    events_init(&guider_ui);
    custom_init(&guider_ui);
    Serial.println("GUI初始化完成");
}

void tft_init()
{
    tft.begin();          /* TFT init TFT初始化*/
    tft.setRotation( 1 ); /* Landscape orientation, flipped 设置方向*/
}

void lvgl_setup()
{
    lv_init();

#if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print ); /* register print function for debugging 注册打印功能以进行调试*/
#endif

    lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );

    /*Initialize the display*/
    /*初始化显示*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init( &disp_drv );
    /*Change the following line to your display resolution*/
    /*将以下行更改为您的显示分辨率*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register( &disp_drv );

    /*Initialize the (dummy) input device driver*/
    /*初始化（虚拟）输入设备驱动程序*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register( &indev_drv );
}

lv_ui guider_ui;//结构体包含所有屏幕与部件，必不可少，且不能放到setup里 。使用该指针可以找到程序里任何对象

void setup()
{
    Serial.begin( 115200 ); /* prepare for possible serial debug 为可能的串行调试做准备*/
    Serial.println("启动中...");
    
    // 设置重启按钮引脚为输入模式，启用内部上拉电阻
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    
    tft_init();
    lvgl_setup();
    
    // 检查是否需要校准
    calibration_needed = check_calibration_needed();
    
    if (!calibration_needed) {
        // 如果不需要校准，直接初始化GUI
        init_gui();
        Serial.println("已载入校准数据，直接进入GUI");
    } else {
        // 需要校准，创建校准界面
        create_calibration_screen();
        Serial.println("开始触摸屏校准");
    }
}

void loop()
{
    lv_timer_handler(); /* let the GUI do its work 让 GUI 完成它的工作 */
    delay( 5 );
}

