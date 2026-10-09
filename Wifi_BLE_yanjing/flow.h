#ifndef FLOW_H
#define FLOW_H

#include <Arduino.h>

// ==================== 动作流（程序）结构 ====================
// 与"待机微动"区分：动作流是有步骤、可循环、运行中独占的宏程序。

enum FlowStepType : uint8_t {
  FS_INIT = 0,   // 回到初始帧
  FS_EXPR,       // 播放表情并保持 holdMs
  FS_WAIT,       // 纯等待
  FS_POSE,       // 眼球直接定位 x,y 并保持
};

struct FlowStep {
  uint8_t  type;
  char     expr[16];
  float    x;
  float    y;
  uint16_t holdMs;
};

struct FlowDef {
  const char*     name;
  const FlowStep* steps;
  uint8_t         count;
  uint8_t         repeat;     // 循环次数，0/1 = 一次
  bool            autoInit;   // 开始前先 init
  bool            blockIdle;  // 运行期间压制待机微动
  bool            returnInit; // 结束后回 init
};

// ==================== 引擎接口 ====================
void flowInit();
bool flowStart(const char* name);          // 命令层调用，异步请求
void flowStop(bool gotoInit);              // 命令层调用，异步请求
void flowUpdate(unsigned long now);        // 舵机任务内驱动
bool flowIsRunning();
const char* flowCurrentName();
int  flowProgress();
int  flowCount();
const char* flowNameAt(int index);

#endif // FLOW_H
