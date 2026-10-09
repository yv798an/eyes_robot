#include "app.h"

// ==================== 数学工具 ====================
bool sameFrame(Frame a, Frame b) {
  return (fabs(a.tl - b.tl) < 0.1f) && (fabs(a.bl - b.bl) < 0.1f) &&
         (fabs(a.tr - b.tr) < 0.1f) && (fabs(a.br - b.br) < 0.1f) &&
         (fabs(a.x - b.x) < 0.1f)   && (fabs(a.y - b.y) < 0.1f);
}

float lerpFloat(float a, float b, float t) {
  return a + (b - a) * t;
}

Frame lerpFrame(Frame a, Frame b, float t) {
  Frame r;
  r.tl = lerpFloat(a.tl, b.tl, t);
  r.bl = lerpFloat(a.bl, b.bl, t);
  r.tr = lerpFloat(a.tr, b.tr, t);
  r.br = lerpFloat(a.br, b.br, t);
  r.x  = lerpFloat(a.x,  b.x,  t);
  r.y  = lerpFloat(a.y,  b.y,  t);
  return r;
}

// 五次 S 型缓动（速度与加速度都平滑，零急动）
float smootherStep(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ==================== Servo Controller（唯一舵机输出入口） ====================
int angleToUs(float deg) {
  deg = constrain(deg, 0.0f, 180.0f);
  return (int)round(500.0f + deg * (2000.0f / 180.0f));
}

void servoWriteAll(Frame f) {
  tl.writeMicroseconds(angleToUs(f.tl));
  bl.writeMicroseconds(angleToUs(f.bl));
  tr.writeMicroseconds(angleToUs(f.tr));
  br.writeMicroseconds(angleToUs(f.br));
  Xarm.writeMicroseconds(angleToUs(f.x));
  Yarm.writeMicroseconds(angleToUs(f.y));
}

void servoWriteIfChanged(Frame f) {
  if (hasWritten && sameFrame(f, lastWritten)) return;
  servoWriteAll(f);
  lastWritten = f;
  hasWritten = true;
}

// ==================== Motion Planner（轨迹规划） ====================
// 六轴同步：取最慢轴决定总时长，再用 S 曲线同步到终点
void planTo(Frame target, unsigned long minDur, unsigned long maxDur) {
  planStart = planCurrent;
  planTarget = target;

  float lidMax = fabs(target.tl - planStart.tl);
  lidMax = max(lidMax, fabs(target.bl - planStart.bl));
  lidMax = max(lidMax, fabs(target.tr - planStart.tr));
  lidMax = max(lidMax, fabs(target.br - planStart.br));
  float lidTime = lidMax / EYELID_MAX_SPEED * 1000.0f;

  float xyMax = max(fabs(target.x - planStart.x), fabs(target.y - planStart.y));
  float xyTime = xyMax / EYE_MAX_SPEED * 1000.0f;

  float tMax = max(lidTime, xyTime);
  planDuration = constrain((unsigned long)round(tMax), minDur, maxDur);
  planStartTime = millis();
  planActive = true;
}

void updatePlanner(unsigned long now) {
  if (!planActive) return;

  float t = (float)(now - planStartTime) / (float)planDuration;

  if (t >= 1.0f) {
    planCurrent = planTarget;
    planActive = false;
  } else {
    planCurrent = lerpFrame(planStart, planTarget, smootherStep(t));
  }
}

// ==================== 输出合成 ====================
// 表情基帧（慢） ⊕ 眼球微动（微扫视+漂移） ⊕ 眨眼（覆盖眼皮）
Frame composeOutputFrame() {
  Frame o = planCurrent;

#if BEHAVIOR_ENABLED
  o.x = constrain(o.x + behaviorEyeOffX, 50.0f, 120.0f);
  o.y = constrain(o.y + behaviorEyeOffY, 75.0f, 120.0f);

  if (behaviorBlink > 0.001f) {
    float b = behaviorBlink;
    o.tl = lerpFloat(o.tl, CLOSE.tl, b);
    o.bl = lerpFloat(o.bl, CLOSE.bl, b);
    o.tr = lerpFloat(o.tr, CLOSE.tr, b);
    o.br = lerpFloat(o.br, CLOSE.br, b);
  }
#endif

  return o;
}
