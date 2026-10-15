# Smart Granary Detect System

基于 FS-MP1A（Debian armv7l）与多模态大模型的嵌入式智能粮仓检测系统。

## 功能（对照功能说明 PDF）

- 可调档位电压传感器：ADC 0~3V → 供电 `U = ADC×2`（0~6V）；`U∉[4.8,5.2]` → **E01**
- 模拟功率：`I = 0.10 + 0.15(风扇) + 0.08(蜂鸣器) + 0.05(三灯闪烁)`，`P = U×I`
- 温湿度超阈值 → **E02**（LED2 + 风扇）
- 人体红外 / 火焰传感器 + AI 识别 → **E03/E04/E05**（三灯交替闪 + 高频蜂鸣）
  - E03 人员非法闯入
  - E04 粮情异常：霉变结块 / 粮面塌陷 / 虫害堆积 / 杂物遗留
  - E05 明火隐患
- KEY1 启停巡检；KEY2 手动抓拍并即时多模态分析（默认不落盘，`GRANARY_KEY2_SAVE=1` 时保存）；KEY3 蜂鸣器静音
- V4L2 摄像头周期采集、Base64、多模态 HTTP API（可选 libcurl）
- 局域网 MJPEG/HTTP 网页推流，叠加数据/告警，支持告警历史与快照画廊
- 告警自动保存 JPG 到 `./snapshots/YYYY-MM-DD/`（失败则 `/tmp/granary-snapshots`），按保留天数自动清理
- 传感器读取失败时对应项显示 `ERR`，不影响其他功能
- 摄像头热插拔：打开失败后每 2s 自动重连
- 线程看门狗：任一工作线程心跳超时（默认 30s）即主动退出，交由 systemd 重启

## 架构

```
main.c          入口：创建 7 个线程（sensor/key/camera/alarm/ai/http/watchdog）
app/
  app.c/.h       线程编排、按键分发、传感器轮询、AI/抓拍触发、看门狗
  state.c/.h     全局状态 + 运行期配置（环境变量）+ 告警历史 + 线程心跳
  alarm.c/.h     告警判定（E01~E05）与执行机构控制
  multimodal.c/.h  Base64 + OpenAI 兼容 API + 关键词解析（中英文）
  http_server.c/.h MJPEG 推流 + 状态 JSON + 快照浏览 + 控制端点
  snapshot.c/.h  JPG 落盘（按日期分目录）与保留策略清理
  display.c/.h   stdout 显示（LCD 替代）+ framebuffer stub
  log.c/.h       日志（级别过滤 + 可选落盘）
driver/         LED/蜂鸣器/风扇/传感器/按键/V4L2 摄像头/tiny_jpeg
```

线程间通过 `app_state_t` + 互斥锁/条件变量共享数据；定时器回调不涉及外设，
全部采集与控制在对应线程中完成。

## 构建

### 依赖

- CMake ≥ 3.10、GCC（或交叉工具链）
- pthread、libm、librt
- 可选：`libcurl`（启用真实多模态 HTTP；无则 stub）
- 板端摄像头需支持 **MJPEG**（常见 UVC：`uvcvideo`）

### 本机（开发机冒烟编译/测试）

```bash
cmake -S . -B build
cmake --build build -j
bash tests/smoke.sh        # 无硬件冒烟测试（HTTP 端点 + 控制）
```

### 交叉编译（armv7l 板子）

```bash
export CROSS_COMPILE=arm-linux-gnueabihf-   # 按你的工具链改前缀
cmake -S . -B build-arm
cmake --build build-arm -j
```

可选 GPIO 路径：

```bash
cmake -S . -B build-arm \
  -DPIR_SENSOR_SYSFS_PATH=/sys/class/gpio/gpio115/value \
  -DFLAME_SENSOR_SYSFS_PATH=/sys/class/gpio/gpio116/value
```

## 部署到板子

板子示例 IP：`10.90.193.197`（以 `ip a` 为准）。

```bash
scp build-arm/smart_granary_detect_system root@10.90.193.197:/root/project/
ssh root@10.90.193.197 'mkdir -p /root/project/snapshots'
```

### systemd 开机自启（推荐）

```bash
scp deploy/smart-granary.service root@10.90.193.197:/etc/systemd/system/
ssh root@10.90.193.197 'systemctl daemon-reload && systemctl enable --now smart-granary'
ssh root@10.90.193.197 'journalctl -u smart-granary -f'   # 查看日志
```

密钥建议放 `/root/project/granary.env`（`chmod 600`），在服务单元中用
`EnvironmentFile=` 引入，**切勿提交到仓库**。

## 运行

```bash
cd /root/project   # 或你的部署目录
# 可选：无实体按键时自动进入巡检
# export GRANARY_AUTO_START=1

# 多模态 API（切勿把密钥写进代码）
export MULTIMODAL_API_BASE="https://api.openai.com/v1"   # 或兼容网关
export MULTIMODAL_API_KEY="sk-..."
export MULTIMODAL_MODEL="gpt-4o-mini"
# export MULTIMODAL_PROMPT="自定义巡检提示词..."

./smart_granary_detect_system
```

浏览器打开：`http://10.90.193.197:8080/`

- `/` 监控页（MJPEG 画面 + 传感器/AI/告警叠加 + 告警历史 + 快照画廊 + 控制按钮）
- `/stream` MJPEG 视频流
- `/status.json` 状态 JSON（读数/告警/历史/统计）
- `/snapshots.json` 快照文件列表
- `/snapshot.jpg?name=YYYY-MM-DD/snap_HHMMSS_tag.jpg` 读取单张快照
- `/control?action=pause|resume|toggle|mute` 巡检启停 / 蜂鸣器静音

KEY1 暂停时：停止周期采集、传感器轮询、告警待机、推流暂停。

## 环境变量总览

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GRANARY_VOLTAGE_MIN` / `GRANARY_VOLTAGE_MAX` | 4.8 / 5.2 | 电压安全区间 (V) |
| `GRANARY_TEMP_MAX` | 30 | 温度上限 (℃) |
| `GRANARY_RH_MAX` | 65 | 湿度上限 (%RH) |
| `GRANARY_FAN_PWM` | 200 | E02 风扇占空比 (0~255) |
| `GRANARY_BUZZER_LOW_HZ` / `GRANARY_BUZZER_HIGH_HZ` | 800 / 2500 | 蜂鸣器频率 (Hz) |
| `GRANARY_SENSOR_PERIOD_MS` | 200 | 传感器轮询周期 |
| `GRANARY_CAMERA_PERIOD_MS` | 200 | 摄像头采集周期（≈5fps） |
| `GRANARY_AI_PERIOD_MS` | 15000 | 周期 AI 分析间隔 |
| `GRANARY_DISPLAY_PERIOD_MS` | 2000 | 终端刷新周期 |
| `GRANARY_ALARM_PERIOD_MS` | 50 | 执行机构刷新周期 |
| `GRANARY_HTTP_PORT` | 8080 | HTTP 服务端口 |
| `GRANARY_SNAPSHOT_DIR` | ./snapshots | 快照根目录 |
| `GRANARY_SNAPSHOT_KEEP_DAYS` | 7 | 快照保留天数（0=不清理） |
| `GRANARY_KEY2_SAVE` | 0 | KEY2 是否额外保存 JPG |
| `GRANARY_AUTO_START` | - | 无按键设备时自动进入巡检 |
| `GRANARY_LOG_LEVEL` | info | 日志级别 debug/info/warn/error |
| `GRANARY_LOG_FILE` | - | 日志文件（默认仅 stderr） |
| `CAMERA_DEVICE` | 自动探测 | 摄像头节点（/dev/videoN） |
| `MULTIMODAL_API_BASE` / `MULTIMODAL_API_KEY` / `MULTIMODAL_MODEL` / `MULTIMODAL_PROMPT` | - | 多模态 API 配置 |

编译期可覆盖项：`-DGRANARY_KEY1_CODE=...`（按键码）、`-DPIR_SENSOR_SYSFS_PATH=...`、
`-DFLAME_SENSOR_SYSFS_PATH=...`、`-DPIR_ACTIVE_LOW=1`、`-DFLAME_ACTIVE_LOW=0`。

## 摄像头

插入 UVC 摄像头后内核日志类似：

```text
uvcvideo: Found UVC 1.00 device HD Camera
USB Video Class driver (1.1.1)
```

确认节点：

```bash
ls -l /dev/video*
v4l2-ctl --device=/dev/video0 --list-formats-ext   # 若已安装 v4l2-utils
```

本程序请求 **MJPEG**（YUYV 会经内置 tiny_jpeg 转码）。若仅有 YUYV 且转码不满足
性能要求，需换摄像头。

## 人体红外 / 火焰 GPIO

默认（可改）：

| 传感器 | 默认 sysfs | 极性 |
|--------|------------|------|
| PIR    | `/sys/class/gpio/gpio115/value` | 高电平=有人（`PIR_ACTIVE_LOW=0`） |
| 火焰   | `/sys/class/gpio/gpio116/value` | 低电平=有火（`FLAME_ACTIVE_LOW=1`） |

上板后请用 `ls /sys/class/gpio` / 原理图核对，并在 CMake 或头文件宏中覆盖。
设备文件不存在时读失败，按「未触发」处理，不影响其它功能。

## 按键

默认 Linux 键码：`KEY_1` / `KEY_2` / `KEY_3`。
若板子映射不同，编译时加 `-DGRANARY_KEY1_CODE=...` 等（见 `linux/input.h`）。

## 阈值（可宏覆盖/环境变量覆盖）

- 电压安全：`4.8 ~ 5.2 V`
- 温度上限：`30 °C`
- 湿度上限：`65 %RH`

## LCD

未绑定固定 LCD 驱动路径时：传感器与 AI 文本打印到 **stdout**，同时推到网页叠加。
framebuffer 接口为 stub（`display_framebuffer_stub`），接入 `/dev/fb0` 或 SPI
LCD 驱动时在此实现。

## 日志

默认输出 stderr 并进入 journal（systemd）。可选落盘：

```bash
export GRANARY_LOG_LEVEL=debug
export GRANARY_LOG_FILE=/var/log/smart-granary.log
```

格式：`[YYYY-MM-DD HH:MM:SS.mmm] LEVEL 消息`。

## CI

GitHub Actions（`.github/workflows/build.yml`）在 push/PR 时执行：

1. **with libcurl**：完整构建 + `tests/smoke.sh` 无硬件冒烟测试
2. **no libcurl**：验证 stub 降级路径可编译

## 目录结构

```text
main.c
app/          状态机、告警、推流、多模态、抓拍、显示、日志
driver/       LED/蜂鸣器/风扇/传感器/按键/V4L2 摄像头/tiny_jpeg
tests/        冒烟测试
deploy/       systemd 服务单元
snapshots/    告警 JPG（运行时创建，已 gitignore）
```
