#ifndef CONFIG_H
#define CONFIG_H

// ==================== 网络 / 云平台配置 ====================
static const char* ssid        = "oppofindx8";
static const char* password    = "eiku8997";
static const char* mcpEndpoint = "ws://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjkwNTU2MCwiYWdlbnRJZCI6MTczMjg2NywiZW5kcG9pbnRJZCI6ImFnZW50XzE3MzI4NjciLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzc2ODc0OTM1LCJleHAiOjE4MDg0MzI1MzV9.YHz8fjoOcp0W1FqgmuLMtS-8IC06r35QYN_nN3Hvi4gnO20m8eACLkGniKik96LD_ztNJRQNk8Dfa9X7JVSQlg";

#define MQTT_SERVER       "119.91.109.180"
#define MQTT_PORT         9501
#define MQTT_CLIENT_ID    "0cf0a6fc7b174a1cb6c716a17b671472"
#define MQTT_USERNAME     MQTT_CLIENT_ID
#define MQTT_PASSWORD     MQTT_CLIENT_ID

#define MQTT_TOPIC_CMD    "BMP006"   // 硬件【接收指令】
#define MQTT_TOPIC_STATUS "BMP004"   // 硬件【上报状态】

// ==================== 舵机引脚 ====================
#define TL_PIN 16
#define BL_PIN 17
#define TR_PIN 22
#define BR_PIN 23
#define X_PIN 18
#define Y_PIN 19

// ==================== 本地调试输入引脚 ====================
#define PINX     33      // 摇杆X轴
#define PINY     32      // 摇杆Y轴
#define PIN_BTN  13      // 摇杆按键SW
#define PIN_R    34      // 电位器中间位

#define USE_POT_CONTROL 1
#define ADC_RESOLUTION  12

// ==================== I2C 总线（DHT20 温湿度传感器） ====================
// 按接线顺序：D26 -> SCL，D27 -> SDA
// 若你的模块丝印相反，交换下面两行即可
#define I2C_SCL_PIN   26
#define I2C_SDA_PIN   27
#define I2C_CLOCK_HZ  100000
#define DHT20_ADDR    0x38
#define DHT20_READ_MS 1000      // 采集周期 ms（越小响应越快；DHT20 建议 ≥1s）

// ==================== 继电器（D21 / GPIO21，高电平 3V 触发） ====================
// D21 非 strapping 脚，上电安全；建议 GPIO21-GND 加 10k 下拉，保证上电断开
#define RELAY_ENABLED            1
#define RELAY_PIN                21       // D21 = GPIO21
#define RELAY_ACTIVE_HIGH        1        // 1=高电平吸合，0=低电平吸合
#define RELAY_BOOT_SAFE_MS       500      // 上电保持断开的时间 ms
#define RELAY_EVAL_MS            200      // 继电器评估节拍 ms（用缓存温度快速判定）

// 自动温控：温度高于阈值启动，降到(阈值-回差)关闭
#define RELAY_TEMP_AUTO_ENABLED  1        // 默认启用自动温控
#define RELAY_TEMP_THRESHOLD     30.0f    // 高于该温度启动继电器 °C
#define RELAY_TEMP_HYSTERESIS    0.5f     // 回差，降到(阈值-0.5)就关闭
#define RELAY_TEMP_DEBOUNCE_MS   500      // 连续超阈确认时间 ms（仅用于启动，防噪声）
#define RELAY_SENSOR_STALE_MS    10000    // 缓存温度超过该时长视为失效

// ==================== 本地调试角度参数 ====================
#define TOP_LEFT_CLOSE     65
#define TOP_LEFT_OPEN      110
#define BOTTOM_LEFT_CLOSE  115
#define BOTTOM_LEFT_OPEN   70
#define TOP_RIGHT_CLOSE    115
#define TOP_RIGHT_OPEN     70
#define BOTTOM_RIGHT_CLOSE 60
#define BOTTOM_RIGHT_OPEN  125

#define XARM_LEFT    53
#define XARM_RIGHT   117
#define YARM_TOP     74
#define YARM_BOTTOM  111

#define WIFI_ARGX -5
#define WIFI_ARGY 0

#define POT_LEFT_TOP      0
#define POT_LEFT_BOTTOM   0
#define POT_RIGHT_TOP     0
#define POT_RIGHT_BOTTOM  0

#define R_THVALUE 200

// ==================== 运动规划参数 ====================
#define EYELID_MAX_SPEED   110.0f        // 眼皮轴最大速度 deg/s
#define EYE_MAX_SPEED      80.0f         // 眼球 X/Y 轴最大速度 deg/s
#define PLAN_MIN_DURATION  300           // 表情过渡最短时长 ms
#define PLAN_MAX_DURATION  1000          // 表情过渡最长时长 ms
#define SERVO_TICK_MS      10            // 舵机输出固定节拍 ms
#define MANUAL_SAMPLE_MS   20            // 手动输入采样周期 ms

#define MANUAL_MAX_SPEED   180.0f        // 手动跟随最大速度 deg/s
#define MANUAL_MAX_ACCEL   2400.0f       // 手动跟随最大加速度 deg/s^2

// ==================== 行为层（动作流）开关与调参 ====================
// 0 = 退回旧的静态帧行为；1 = 开启眨眼 / 微扫视 / 漂移
#define BEHAVIOR_ENABLED   1

// 眨眼：间隔按对数正态抖动，包络"闭合快、张开慢"
#define BLINK_BASE_MS      4000.0f       // 间隔中位数 ms
#define BLINK_JITTER_SIGMA 0.9f          // 对数正态抖动强度
#define BLINK_MIN_MS       1500          // 间隔下限 ms
#define BLINK_MAX_MS       12000         // 间隔上限 ms
#define BLINK_CLOSE_MIN    60            // 闭合阶段最短 ms
#define BLINK_CLOSE_MAX    100           // 闭合阶段最长 ms
#define BLINK_HOLD_MIN     40            // 全闭保持最短 ms
#define BLINK_HOLD_MAX     120           // 全闭保持最长 ms
#define BLINK_OPEN_MIN     120           // 张开阶段最短 ms
#define BLINK_OPEN_MAX     250           // 张开阶段最长 ms

// 微扫视：注视时偶发的小幅跳视（拟人、低频，避免抽搐感）
#define MICRO_AMP_MAX      0.45f         // 微扫视最大幅度 deg
#define MICRO_MIN_MS       1500          // 微扫视间隔下限 ms
#define MICRO_MAX_MS       3500          // 微扫视间隔上限 ms
#define MICRO_TAU_MS       55.0f         // 微扫视收敛时间常数 ms（越大越柔和）

// 漂移：带阻尼的低频慢漂移（约 0.3Hz，取代原先的高频振荡）
#define DRIFT_ACC          0.006f        // 漂移噪声激励（越大越活）
#define DRIFT_REVERT       0.0018f       // 弹簧回复系数（决定漂移频率）
#define DRIFT_DAMP         0.075f        // 阻尼系数（抑制振荡，越大越稳）
#define DRIFT_MAX          0.8f          // 漂移最大幅度 deg

// ==================== 动作队列 ====================
#define ACTION_QUEUE_SIZE          12
#define DEFAULT_ACTION_HOLD_MS     3000

// ==================== 动作流（程序） ====================
#define FLOW_ENABLED                1
#define FLOW_MAX_STEPS              16
#define FLOW_STEP_TIMEOUT_MS        8000
#define FLOW_MAX_DURATION_MS        60000
#define FLOW_ABORT_ON_DISCONNECT    1
#define FLOW_RESUME_AFTER_ALARM     0

// ==================== 蓝牙 BLE（Nordic UART Service） ====================
#define BLE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#endif // CONFIG_H
