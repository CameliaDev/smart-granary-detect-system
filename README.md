# Smart Granary Detect System 智能粮仓检测系统

[![build](https://github.com/jiangnanlier/smart-granary-detect-system/actions/workflows/build.yml/badge.svg)](https://github.com/jiangnanlier/smart-granary-detect-system/actions/workflows/build.yml)

基于 **FS-MP1A 嵌入式 Linux 开发板（Debian armv7l）** 与 **火山方舟多模态大模型（豆包 doubao-seed-2-1-lite-260915，Responses API）** 的智能粮仓检测系统：摄像头周期性采集粮仓画面，开发板同步采集电压、温湿度、光照、人体红外、火焰传感器数据；图像经 Base64 编码送多模态大模型识别粮情与安全隐患；通过局域网网页实时查看监控画面与全部数据，并按级别触发 LED / 蜂鸣器 / 风扇硬件告警，异常时刻自动保存现场图片。

> 课程项目背景：多模态大模型嵌入式智能监控系统（图像采集 30 分 + 大模型识别 50 分 + 触发告警 20 分）。

---

## 目录

- [功能特性](#功能特性)
- [系统架构](#系统架构)
- [硬件清单](#硬件清单)
- [快速开始](#快速开始)
- [配置（环境变量）](#配置环境变量)
- [HTTP 接口](#http-接口)
- [告警规则](#告警规则)
- [多模态大模型](#多模态大模型)
- [摄像头](#摄像头)
- [日志](#日志)
- [测试与 CI](#测试与-ci)
- [部署（systemd）](#部署systemd)
- [目录结构](#目录结构)
- [常见问题](#常见问题)

---

## 功能特性

**环境监测**
- 可调档位电压传感器：ADC 0~3V 采样，换算 `U = ADC × 2`（0~6V），安全区间 4.8~5.2V
- 温湿度、光照传感器轮询采集；人体红外、火焰传感器 GPIO 检测
- 模拟功率计算：`I = 0.10A(待机) + 0.15A(风扇) + 0.08A(蜂鸣器) + 0.05A(三灯闪烁)`，`P = U × I`
- 传感器读取失败时对应项显示 `ERR`，不影响其他功能

**AI 图像识别（Responses API）**
- V4L2 摄像头周期采集（MJPEG 原生 / YUYV 经内置 tiny_jpeg 转码）
- JPEG → Base64 → Data URL 内嵌，一次 POST 发送
- 识别人员闯入、粮食霉变结块、粮面塌陷、虫害堆积、杂物遗留、明火烟雾（中英文关键词解析）
- `text.format = json_object` 强制结构化输出；流式 SSE 增量解析
- 无密钥 / 无 libcurl / 网络失败时自动降级 stub，不影响传感器告警

**分级硬件告警**
- E01 电压异常 → LED1 常亮 + 蜂鸣器低频间断（400ms 响 / 400ms 静）
- E02 温湿度超限 → LED2 常亮 + 风扇自动通风
- E03 人员闯入 / E04 粮情异常 / E05 明火 → LED1~LED3 交替闪烁 + 蜂鸣器高频持续
- 告警触发沿自动抓拍保存 JPG；网页同步叠加告警文字

**人机交互**
- KEY1 启动 / 暂停巡检（暂停时停止采集、轮询、告警待机、推流暂停）
- KEY2 手动抓拍并即时提交 AI 分析（默认不落盘，`GRANARY_KEY2_SAVE=1` 时保存）
- KEY3 蜂鸣器静音（保留灯光与网页告警）
- 网页端支持暂停/恢复巡检、蜂鸣器静音按钮

**局域网监控**
- 内嵌监控页：MJPEG 实时画面 + 传感器/AI/告警数据叠加
- 告警历史列表、快照画廊（缩略图浏览）
- `/status.json` 提供全部状态（读数、告警、历史、统计）

**工程化**
- 全部阈值/周期/端口/路径环境变量可配，无需重新编译
- 快照按 `YYYY-MM-DD/` 分目录存储，按保留天数自动清理
- 日志模块（级别过滤 + 可选落盘）
- 摄像头热插拔自动重连；线程看门狗（卡死自动退出，systemd 重启）
- CMake 构建 + 交叉编译 + GitHub Actions CI + 无硬件冒烟测试

---

## 系统架构

```
                          ┌──────────────────────────────────────────┐
                          │              main.c (入口)                │
                          │      创建并 join 以下 7 个线程            │
                          └──────────────────────────────────────────┘
                                              │
  ┌─────────────┬─────────────┬─────────────┬─┴──────────┬─────────────┬─────────────┐
  │             │             │             │            │             │             │
sensor       key          camera         alarm          ai          http        watchdog
(传感器轮询   (按键扫描     (V4L2 采集    (LED/蜂鸣器    (多模态      (网页推流    (线程看门狗
 +告警判定    KEY1/2/3)    +快照落盘      /风扇控制)     Responses    +状态JSON)   心跳超时
 +显示刷新)               +AI请求)                     API 分析)                主动退出)
```

线程间通过 `app_state_t`（互斥锁 + 条件变量）共享数据；定时节拍由各线程内部 `delay` 完成，回调不直接操作外设。

| 模块 | 职责 |
|---|---|
| `app/app.c` | 线程编排、按键分发、传感器轮询、抓拍/AI 触发、看门狗 |
| `app/state.c` | 全局状态 + 运行期配置（环境变量）+ 告警历史 + 线程心跳 |
| `app/alarm.c` | 告警判定（E01~E05）与执行机构控制 |
| `app/multimodal.c` | Base64 + 方舟 Responses API（流式 SSE）+ 关键词解析 |
| `app/http_server.c` | MJPEG 推流 + 状态 JSON + 快照浏览 + 控制端点 |
| `app/snapshot.c` | JPG 落盘（按日期分目录）与保留策略清理 |
| `app/display.c` | stdout 显示（LCD 替代）+ framebuffer stub |
| `app/log.c` | 日志（级别过滤 + 可选落盘） |

---

## 硬件清单

| # | 模块 | 接口 | 说明 |
|---|---|---|---|
| 1 | FS-MP1A 主控开发板 | - | Debian armv7l，系统主控 |
| 2 | 摄像头（UVC） | USB（V4L2） | 需支持 MJPEG；默认探测 `/dev/video1→video2→video0` |
| 3 | 可调档位电压传感器 | ADC | 模拟输出 0~3V，拨码切换档位模拟供电工况 |
| 4 | 温湿度传感器 | I2C（IIO） | 温度 / 相对湿度 |
| 5 | 光照传感器 | I2C（IIO） | 光照强度 (lux) |
| 6 | 人体红外传感器（PIR） | GPIO | 默认 `/sys/class/gpio/gpio115/value`，高电平有效 |
| 7 | 火焰传感器 | GPIO | 默认 `/sys/class/gpio/gpio116/value`，低电平有效 |
| 8 | LED ×3 | sysfs leds | `led1/led2/led3` 分级告警指示 |
| 9 | 直流风扇 | hwmon pwmfan | 温湿度超标时通风降湿 |
| 10 | 蜂鸣器 | input (EV_SND) | 声音告警 |
| 11 | 独立按键 ×3 | input (EV_KEY) | KEY1/KEY2/KEY3，默认键码 `KEY_1/2/3` |

> GPIO 编号为默认值，上板后请用 `ls /sys/class/gpio` 或原理图核对，通过 CMake 参数或头文件宏覆盖。

---

## 快速开始

### 依赖

- CMake ≥ 3.10、GCC（或交叉工具链）
- pthread、libm、librt
- 可选：`libcurl`（启用真实多模态 HTTP；无则 stub 降级）
- 板端摄像头需支持 MJPEG（常见 UVC：`uvcvideo`）

### 本机编译 + 冒烟测试

```bash
cmake -S . -B build
cmake --build build -j
bash tests/smoke.sh        # 无硬件冒烟测试（HTTP 端点 + 控制 + 页面）
```

### 交叉编译（armv7l）

```bash
export CROSS_COMPILE=arm-linux-gnueabihf-   # 按你的工具链改前缀
cmake -S . -B build-arm
cmake --build build-arm -j

# 可选：覆盖 GPIO 路径
cmake -S . -B build-arm \
  -DPIR_SENSOR_SYSFS_PATH=/sys/class/gpio/gpio115/value \
  -DFLAME_SENSOR_SYSFS_PATH=/sys/class/gpio/gpio116/value
```

### 部署到板子

```bash
scp build-arm/smart_granary_detect_system root@10.90.193.197:/root/project/
ssh root@10.90.193.197 'mkdir -p /root/project/snapshots'
```

### 运行

```bash
cd /root/project
export GRANARY_AUTO_START=1        # 无实体按键时自动进入巡检
export MULTIMODAL_API_KEY="ark-..." # 方舟 API Key（切勿写进代码）
./smart_granary_detect_system
```

浏览器打开 `http://10.90.193.197:8080/`（IP 以 `ip a` 为准）。

---

## 配置（环境变量）

所有参数均运行期生效，无需重新编译；编译期可覆盖项见文末。

### 告警阈值

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GRANARY_VOLTAGE_MIN` / `GRANARY_VOLTAGE_MAX` | 4.8 / 5.2 | 电压安全区间 (V) |
| `GRANARY_TEMP_MAX` | 30 | 温度上限 (℃) |
| `GRANARY_RH_MAX` | 65 | 湿度上限 (%RH) |
| `GRANARY_FAN_PWM` | 200 | E02 风扇占空比 (0~255) |
| `GRANARY_BUZZER_LOW_HZ` / `GRANARY_BUZZER_HIGH_HZ` | 800 / 2500 | 蜂鸣器频率 (Hz) |

### 任务周期 (ms)

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GRANARY_SENSOR_PERIOD_MS` | 200 | 传感器轮询周期 |
| `GRANARY_CAMERA_PERIOD_MS` | 200 | 摄像头采集周期（≈5fps） |
| `GRANARY_AI_PERIOD_MS` | 15000 | 周期 AI 分析间隔 |
| `GRANARY_DISPLAY_PERIOD_MS` | 2000 | 终端刷新周期 |
| `GRANARY_ALARM_PERIOD_MS` | 50 | 执行机构刷新周期 |

### 网络 / 存储 / 多模态

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GRANARY_HTTP_PORT` | 8080 | HTTP 服务端口 |
| `GRANARY_SNAPSHOT_DIR` | `./snapshots` | 快照根目录 |
| `GRANARY_SNAPSHOT_KEEP_DAYS` | 7 | 快照保留天数（0=不清理） |
| `GRANARY_KEY2_SAVE` | 0 | KEY2 是否额外保存 JPG |
| `GRANARY_AUTO_START` | - | 无按键设备时自动进入巡检 |
| `GRANARY_LOG_LEVEL` | info | 日志级别 debug/info/warn/error |
| `GRANARY_LOG_FILE` | - | 日志文件（默认仅 stderr） |
| `CAMERA_DEVICE` | 自动探测 | 摄像头节点（/dev/videoN） |
| `MULTIMODAL_API_BASE` | `https://ark.cn-beijing.volces.com/api/v3` | 方舟 Responses API 地址 |
| `MULTIMODAL_API_KEY` | - | 方舟 API Key（**必填**，缺失走 stub） |
| `MULTIMODAL_MODEL` | `doubao-seed-2-1-lite-260915` | 模型名 |
| `MULTIMODAL_PROMPT` | - | 覆盖内置巡检提示词 |

### 编译期可覆盖项

| 宏 | 默认值 | 说明 |
|---|---|---|
| `GRANARY_KEY1/2/3_CODE` | `KEY_1/2/3` | 按键码（见 `linux/input.h`） |
| `PIR_SENSOR_SYSFS_PATH` | `/sys/class/gpio/gpio115/value` | 人体红外 GPIO |
| `FLAME_SENSOR_SYSFS_PATH` | `/sys/class/gpio/gpio116/value` | 火焰 GPIO |
| `PIR_ACTIVE_LOW` | 0 | PIR 极性（1=低电平有效） |
| `FLAME_ACTIVE_LOW` | 1 | 火焰极性（1=低电平有效） |

---

## HTTP 接口

| 端点 | 说明 |
|---|---|
| `GET /` | 监控页：MJPEG 画面 + 数据/告警叠加 + 告警历史 + 快照画廊 + 控制按钮 |
| `GET /stream` | MJPEG 视频流（`multipart/x-mixed-replace`） |
| `GET /status.json` | 状态 JSON：传感器读数、有效标志、E01~E05、AI 文本、告警历史、快照数、uptime |
| `GET /snapshots.json` | 快照文件列表（名称/大小/时间） |
| `GET /snapshot.jpg?name=YYYY-MM-DD/snap_HHMMSS_tag.jpg` | 读取单张快照（名称已做安全校验，防路径穿越） |
| `GET /control?action=pause\|resume\|toggle\|mute` | 巡检启停 / 蜂鸣器静音 |

示例：

```bash
curl http://10.90.193.197:8080/status.json
curl "http://10.90.193.197:8080/control?action=toggle"   # 暂停/恢复巡检
```

KEY1 暂停时：停止周期采集、传感器轮询、告警待机、推流同步暂停。

---

## 告警规则

| 码 | 触发条件 | 灯光 | 声音 | 风扇 | 抓拍 | 网页 |
|---|---|---|---|---|---|---|
| E01 | `U < 4.8V` 或 `U > 5.2V` | LED1 常亮 | 低频间断 | - | ✅ | ✅ |
| E02 | `T > 30℃` 或 `RH > 65%` | LED2 常亮 | - | 自动通风 | - | ✅ |
| E03 | PIR 或 AI 识别到人员闯入 | LED1~3 交替闪 | 高频持续 | - | ✅ | ✅ |
| E04 | AI 识别到霉变/塌陷/虫害/杂物 | LED1~3 交替闪 | 高频持续 | - | ✅ | ✅ |
| E05 | 火焰传感器或 AI 识别明火/烟雾 | LED1~3 交替闪 | 高频持续 | - | ✅ | ✅ |

判定优先级：E05 > E03 > E04 > E01 > E02。KEY3 静音仅关闭蜂鸣器，灯光与网页告警保留。

---

## 多模态大模型

采用**火山方舟 Responses API**（流式 SSE）：

```text
POST {MULTIMODAL_API_BASE}/responses
Authorization: Bearer <MULTIMODAL_API_KEY>
{
  "model": "doubao-seed-2-1-lite-260915",
  "stream": true,
  "input": [{"role": "user", "content": [
    {"type": "input_text",  "text": "<巡检提示词>"},
    {"type": "input_image", "image_url": "data:image/jpeg;base64,<b64>"}
  ]}],
  "max_output_tokens": 300,
  "text": {"format": {"type": "json_object"}}
}
```

- 响应逐行解析 SSE `data:` 事件，累积 `response.output_text.delta` 的 `delta` 文本
- 提示词可用 `MULTIMODAL_PROMPT` 覆盖；返回文本经中英文关键词解析为告警标志
- 降级路径：未链接 libcurl / 无密钥 / 网络失败 → stub 分析（清除 AI 告警标志），系统其余功能不受影响
- 图片链路：V4L2 采集 → `camera_capture_jpeg` → Base64 → Data URL 内嵌，单次 POST 完成

---

## 摄像头

- 默认请求 **MJPEG**；仅支持 YUYV 时自动经内置 `tiny_jpeg.h` 转码为 JPEG
- 节点探测顺序：`CAMERA_DEVICE` 环境变量 → `/dev/video1` → `/dev/video2` → `/dev/video0`（自动跳过非捕获节点）
- 默认 640×480；采集周期默认 200ms（≈5fps），`GRANARY_CAMERA_PERIOD_MS` 可调
- 打开失败后每 2s 自动重连，支持热插拔

```bash
ls -l /dev/video*                                    # 确认节点
v4l2-ctl --device=/dev/video1 --list-formats-ext     # 确认像素格式
```

---

## 日志

默认输出 stderr（systemd 下进入 journal）。可选落盘：

```bash
export GRANARY_LOG_LEVEL=debug      # debug/info/warn/error
export GRANARY_LOG_FILE=/var/log/smart-granary.log
```

格式：`[YYYY-MM-DD HH:MM:SS.mmm] LEVEL 消息`。关键事件（启动、告警变化、抓拍、AI 结果、看门狗）均会记录。

---

## 测试与 CI

GitHub Actions（`.github/workflows/build.yml`）在 push/PR 时自动执行：

1. **with libcurl**：完整构建 + `tests/smoke.sh` 无硬件冒烟测试（启动程序 → 校验 `/`、`/status.json`、`/snapshots.json`、`/control` 端点）
2. **no libcurl**：`-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON` 构建，验证 stub 降级路径可编译

本地手动冒烟测试：

```bash
bash tests/smoke.sh
```

---

## 部署（systemd）

```bash
scp deploy/smart-granary.service root@10.90.193.197:/etc/systemd/system/
ssh root@10.90.193.197 'systemctl daemon-reload && systemctl enable --now smart-granary'
ssh root@10.90.193.197 'journalctl -u smart-granary -f'   # 查看日志
```

服务单元内置：开机自启、`Restart=always`（进程退出或看门狗主动退出后 3s 重启）、环境变量配置示例。密钥建议放 `/root/project/granary.env`（`chmod 600`）并通过 `EnvironmentFile=` 引入，**切勿提交到仓库**。

---

## 目录结构

```text
main.c                  入口：创建 7 个线程
app/                    应用层
  app.c/.h              线程编排、按键分发、传感器轮询、看门狗
  state.c/.h            全局状态 + 环境变量配置 + 告警历史 + 线程心跳
  alarm.c/.h            告警判定与执行机构控制
  multimodal.c/.h       方舟 Responses API 多模态识别
  http_server.c/.h      MJPEG 推流 + 状态 JSON + 快照浏览 + 控制端点
  snapshot.c/.h         JPG 落盘与保留策略
  display.c/.h          stdout 显示（LCD 替代）
  log.c/.h              日志模块
driver/                 驱动层
  LED/buzzer/fan        sysfs / input / hwmon 执行机构
  ht_sensor/light_sensor/voltage_sensor   IIO 传感器
  pir_sensor/flame_sensor                 GPIO 传感器
  key                   Linux input 按键
  camera.c/.h           V4L2 摄像头（MJPEG / YUYV→JPEG）
  tiny_jpeg.h           单文件 JPEG 编码器
  delay/file/my_timer   基础工具（my_timer 未参与编译，保留备用）
tests/smoke.sh          无硬件冒烟测试
deploy/                 systemd 服务单元
.github/workflows/      CI
snapshots/              告警 JPG（运行时创建，已 gitignore）
```

---

## 常见问题

**Q：没有摄像头/按键/传感器能跑吗？**
能。所有硬件缺失均优雅降级：无摄像头时推流与 AI 等待画面；无按键时用 `GRANARY_AUTO_START=1` 自动进入巡检；传感器读取失败显示 `ERR` 并屏蔽对应告警。

**Q：AI 一直显示 stub analysis？**
未链接 libcurl 或未设置 `MULTIMODAL_API_KEY`。安装 `libcurl4-openssl-dev` 重新编译，并配置密钥。密钥只从环境变量读取，不会写入程序和仓库。

**Q：推流卡顿？**
调小 `GRANARY_CAMERA_PERIOD_MS`（如 100）提高帧率；确认摄像头支持 MJPEG（YUYV 转码消耗 CPU）。

**Q：蜂鸣器不响但灯在闪？**
可能按了 KEY3 静音（网页可查看/取消），或蜂鸣器节点 `platform-beeper-event` 不存在（`ls /dev/input/by-path/`）。

**Q：人体红外/火焰误报或不报？**
GPIO 号或极性不对。用 `cat /sys/class/gpio/gpioXXX/value` 手动触发验证，通过 `-DPIR_SENSOR_SYSFS_PATH=` 等编译参数或 `PIR_ACTIVE_LOW`/`FLAME_ACTIVE_LOW` 修正。

**Q：程序过一会就退出？**
看门狗生效：某线程心跳超过 90s 未更新（通常是 AI 请求超过 60s 超时）。查看日志确认原因；systemd 下会自动重启。
