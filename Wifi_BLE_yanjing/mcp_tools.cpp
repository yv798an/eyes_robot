#include "app.h"

// ==================== MCP 工具注册 ====================
void registerTools() {
  mcpClient.registerTool(
    "eye_expression",
    "控制仿生眼的单个表情。用户表达笑、开心、困、累、想睡、惊讶、吃惊、媚眼、眨眼、左睁眼、右睁眼、鄙视、不屑、闭眼、睡觉、睁眼、正常、初始、复位等单个动作时，必须调用本工具。一次只执行一个表情。",
    R"({
      "type": "object",
      "properties": {
        "expression": {
          "type": "string",
          "description": "目标表情，只能选择：init=睁眼/正常/初始/复位；close=闭眼/睡觉；dis=鄙视/不屑；surprise=惊讶/吃惊；tired=困/累/想睡；smile=笑/开心；wink=媚眼/抛媚眼；left_open=左睁眼（左眼睁开右眼闭合）；right_open=右睁眼（右眼睁开左眼闭合）；blink=眨眼。",
          "enum": ["init", "close", "dis", "surprise", "tired", "smile", "wink", "left_open", "right_open", "blink"]
        }
      },
      "required": ["expression"],
      "additionalProperties": false
    })",
    [](const String& args) {
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, args);

      if (err) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }

      String raw = doc["expression"].as<String>();
      raw.trim();

      ActionItem items[8];
      int count = parseActionsFromText(raw, items, 8, DEFAULT_ACTION_HOLD_MS);

      if (count <= 0) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"无法识别表情\"}");
      }

      if (count == 1) {
        if (!startSingleExpr(items[0].expr)) {
          return WebSocketMCP::ToolResponse("{\"status\":\"busy\",\"message\":\"动作流运行中，暂不接受其他动作\"}");
        }

        String res = "{\"status\":\"ok\",\"mode\":\"single\",\"expression\":\"";
        res += items[0].expr;
        res += "\"}";
        return WebSocketMCP::ToolResponse(res);
      }

      if (!startSequence(items, count)) {
        return WebSocketMCP::ToolResponse("{\"status\":\"busy\",\"message\":\"动作流运行中，暂不接受其他动作\"}");
      }

      String res = "{\"status\":\"ok\",\"mode\":\"sequence\",\"count\":";
      res += String(count);
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "eye_expression_sequence",
    "按顺序控制仿生眼执行多个表情。用户说“先...再...然后...”“连续做几个动作”“眨眼后微笑”“闭眼再睁眼”等复合动作时，必须调用本工具，而不是只调用单动作工具。",
    R"({
      "type": "object",
      "properties": {
        "actions": {
          "type": "array",
          "description": "需要按顺序执行的表情数组。每个元素只能是 init, close, dis, surprise, tired, smile, wink, left_open, right_open, blink。",
          "items": {
            "type": "string",
            "enum": ["init", "close", "dis", "surprise", "tired", "smile", "wink", "left_open", "right_open", "blink"]
          },
          "minItems": 1,
          "maxItems": 8
        },
        "interval_ms": {
          "type": "integer",
          "description": "每个动作保持时间，默认3000毫秒。建议范围1000到5000。",
          "minimum": 300,
          "maximum": 5000,
          "default": 3000
        }
      },
      "required": ["actions"],
      "additionalProperties": false
    })",
    [](const String& args) {
      DynamicJsonDocument doc(2048);
      DeserializationError err = deserializeJson(doc, args);

      if (err) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }

      uint32_t intervalMs = DEFAULT_ACTION_HOLD_MS;

      if (doc.containsKey("interval_ms")) {
        intervalMs = doc["interval_ms"].as<uint32_t>();
        intervalMs = constrain(intervalMs, (uint32_t)300, (uint32_t)5000);
      }

      ActionItem items[8];
      int count = 0;

      if (doc["actions"].is<JsonArray>()) {
        JsonArray arr = doc["actions"].as<JsonArray>();

        for (JsonVariant v : arr) {
          if (count >= 8) break;

          String raw = v.as<String>();
          const char* expr = canonicalizeExpr(raw);

          if (strlen(expr) == 0 || !isValidExpr(expr)) continue;

          strncpy(items[count].expr, expr, sizeof(items[count].expr) - 1);
          items[count].expr[sizeof(items[count].expr) - 1] = '\0';
          items[count].holdMs = intervalMs;
          count++;
        }
      }

      if (count <= 0) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"动作数组为空或无法识别\"}");
      }

      if (count == 1) {
        if (!startSingleExpr(items[0].expr)) {
          return WebSocketMCP::ToolResponse("{\"status\":\"busy\",\"message\":\"动作流运行中，暂不接受其他动作\"}");
        }
      } else {
        if (!startSequence(items, count)) {
          return WebSocketMCP::ToolResponse("{\"status\":\"busy\",\"message\":\"动作流运行中，暂不接受其他动作\"}");
        }
      }

      String res = "{\"status\":\"ok\",\"mode\":\"sequence\",\"count\":";
      res += String(count);
      res += ",\"interval_ms\":";
      res += String(intervalMs);
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  // ---- 动作流（程序）相关工具 ----
  String flowEnum = "[";
  for (int i = 0; i < flowCount(); i++) {
    if (i) flowEnum += ",";
    flowEnum += "\"";
    flowEnum += flowNameAt(i);
    flowEnum += "\"";
  }
  flowEnum += "]";

  String runSchema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"要运行的动作流名称\",\"enum\":";
  runSchema += flowEnum;
  runSchema += "}},\"required\":[\"name\"],\"additionalProperties\":false}";

  mcpClient.registerTool(
    "flow_run",
    "运行一个预置动作流（程序）。用户说“做个自检”“打个招呼”“演示一下”“进入休眠”“唤醒”“巡视一圈”“报警动作”等成套动作时，调用本工具。动作流运行期间会独占眼睛，其他动作请等它结束。",
    runSchema,
    [](const String& args) {
      DynamicJsonDocument doc(512);
      DeserializationError err = deserializeJson(doc, args);
      if (err) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }

      String name = doc["name"].as<String>();
      name.trim();
      String resolved = resolveFlowName(name);

      if (resolved.length() == 0) {
        return WebSocketMCP::ToolResponse("{\"status\":\"unknown\",\"message\":\"未找到该动作流\"}");
      }

      if (!flowStart(resolved.c_str())) {
        return WebSocketMCP::ToolResponse("{\"status\":\"busy\",\"message\":\"动作流运行中或暂不可用\"}");
      }

      String res = "{\"status\":\"ok\",\"name\":\"";
      res += resolved;
      res += "\"}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "flow_stop",
    "停止当前正在运行的动作流。用户说“停下”“别动了”“停止动作流”时调用。",
    R"({"type":"object","properties":{},"additionalProperties":false})",
    [](const String&) {
      flowStop(true);
      return WebSocketMCP::ToolResponse("{\"status\":\"ok\"}");
    }
  );

  mcpClient.registerTool(
    "flow_list",
    "列出设备上所有可用的动作流名称。用户问“有哪些动作流/能做什么动作”时调用。",
    R"({"type":"object","properties":{},"additionalProperties":false})",
    [](const String&) {
      String res = "{\"flows\":[";
      for (int i = 0; i < flowCount(); i++) {
        if (i) res += ",";
        res += "\"";
        res += flowNameAt(i);
        res += "\"";
      }
      res += "]}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "flow_status",
    "查询动作流当前是否在运行、名称和进度。用户问“现在在做什么/跑到哪了”时调用。",
    R"({"type":"object","properties":{},"additionalProperties":false})",
    [](const String&) {
      bool running = flowIsRunning();
      String res = "{\"running\":";
      res += running ? "true" : "false";
      res += ",\"name\":\"";
      res += running ? flowCurrentName() : "";
      res += "\",\"progress\":";
      res += String(flowProgress());
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "sensor_read",
    "读取环境传感器（DHT20）的温度和湿度。用户问“现在温度多少”“湿度多少”“环境怎么样”时调用。",
    R"({"type":"object","properties":{},"additionalProperties":false})",
    [](const String&) {
      float t = 0.0f, h = 0.0f;
      bool valid = false;
      unsigned long ageMs = 0;
      sensorGet(t, h, valid, ageMs);

      String res = "{\"sensor\":\"dht20\",\"valid\":";
      res += valid ? "true" : "false";
      if (valid) {
        res += ",\"temperature\":";
        res += String(t, 1);
        res += ",\"humidity\":";
        res += String(h, 1);
        res += ",\"unit_temperature\":\"°C\",\"unit_humidity\":\"%RH\",\"age_ms\":";
        res += String(ageMs);
      } else {
        res += ",\"message\":\"传感器暂未读取到有效数据\"";
      }
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "relay_control",
    "手动控制继电器开关。用户说“打开继电器/开启继电器/关闭继电器/关掉继电器/继电器状态”时调用。",
    R"({"type":"object","properties":{"state":{"type":"string","description":"on=打开, off=关闭, status=查询","enum":["on","off","status"]}},"required":["state"],"additionalProperties":false})",
    [](const String& args) {
      DynamicJsonDocument doc(256);
      if (deserializeJson(doc, args)) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }
      String s = doc["state"].as<String>();
      s.trim();
      s.toLowerCase();

      if (s == "on") {
        relaySet(true, "manual_on");
        relayAnnounce();
        return WebSocketMCP::ToolResponse("{\"status\":\"ok\",\"relay\":\"on\"}");
      }
      if (s == "off") {
        relaySet(false, "manual_off");
        relayAnnounce();
        return WebSocketMCP::ToolResponse("{\"status\":\"ok\",\"relay\":\"off\"}");
      }
      return WebSocketMCP::ToolResponse(relayStatusJson());
    }
  );

  mcpClient.registerTool(
    "relay_set_threshold",
    "设置继电器温度联动阈值（摄氏度）。用户说“温度超过 X 度打开继电器/把阈值设为 X 度”时调用。",
    R"({"type":"object","properties":{"value":{"type":"number","description":"温度阈值，单位摄氏度"}},"required":["value"],"additionalProperties":false})",
    [](const String& args) {
      DynamicJsonDocument doc(256);
      if (deserializeJson(doc, args) || !doc.containsKey("value")) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"缺少 value\"}");
      }
      relaySetThreshold(doc["value"].as<float>());
      relayAnnounce();
      String res = "{\"status\":\"ok\",\"threshold\":";
      res += String(relayGetThreshold(), 1);
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool(
    "relay_auto",
    "开启或关闭继电器的自动温控。关闭后温度不再自动控制继电器。",
    R"({"type":"object","properties":{"enabled":{"type":"boolean","description":"true=开启自动温控，false=关闭"}},"required":["enabled"],"additionalProperties":false})",
    [](const String& args) {
      DynamicJsonDocument doc(256);
      if (deserializeJson(doc, args)) {
        return WebSocketMCP::ToolResponse("{\"status\":\"error\",\"message\":\"JSON解析失败\"}");
      }
      bool en = doc["enabled"].as<bool>();
      relaySetAuto(en);
      relayAnnounce();
      String res = "{\"status\":\"ok\",\"auto\":";
      res += en ? "true" : "false";
      res += "}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  Serial.println("✅ MCP 工具已注册：eye_expression / eye_expression_sequence / flow_run / flow_stop / flow_list / flow_status / sensor_read / relay_control / relay_set_threshold / relay_auto");
}

void onConn(bool ok) {
  if (ok) registerTools();
}
