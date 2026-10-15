# Smart Granary Detect System

基于 FS-MP1A（Debian armv7l）与多模态大模型的嵌入式智能粮仓检测系统。

实现功能（对照功能说明 PDF）：

- 可调档位电压传感器：ADC 0~3V → 供电 `U = ADC×2`（0~6V）；`U∉[4.8,5.2]` → **E01**
- 模拟功率：`I = 0.10 + 0.15(风扇) + 0.08(蜂鸣器) + 0.05(三灯闪烁)`，`P = U×I`
- 温湿度超阈值 → **E02**（LED2 + 风扇）
- 人体红外 / 火焰传感器 + AI 识别 → **E03/E04/E05**（三灯交替闪 + 高频蜂鸣）
- KEY1 启停巡检；KEY2 手动抓拍并即时多模态分析；KEY3 蜂鸣器静音
- V4L2 摄像头周期采集、Base64、多模态 HTTP API（可选 libcurl）
- 局域网 MJPEG/HTTP 网页推流与叠加数据/告警
- 告警自动保存 JPG 到 `./snapshots`（失败则 `/tmp/granary-snapshots`）

## 构建（本机 / 交叉）

### 依赖

- CMake ≥ 3.10、GCC（或交叉工具链）
- pthread、libm、librt
- 可选：`libcurl`（启用真实多模态 HTTP；无则 stub）
- 板端摄像头需支持 **MJPEG**（常见 UVC：`uvcvideo`）

### 本机（开发机冒烟编译）

```bash
cd smart-granary-detect-system
cmake -S . -B build
cmake --build build -j
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

## 运行

```bash
cd /root/project   # 或你的部署目录
# 可选：无实体按键时自动进入巡检
# export GRANARY_AUTO_START=1

# 多模态 API（切勿把密钥写进代码）
export MULTIMODAL_API_BASE="https://api.openai.com/v1"   # 或兼容网关
export MULTIMODAL_API_KEY="sk-..."
export MULTIMODAL_MODEL="gpt-4o-mini"

# 可选
# export CAMERA_DEVICE=/dev/video0
# export GRANARY_HTTP_PORT=8080

./smart_granary_detect_system
```

浏览器打开：`http://10.90.193.197:8080/`

- `/` 监控页（画面 + 传感器/AI/告警叠加）
- `/stream` MJPEG
- `/status.json` 状态 JSON

KEY1 暂停时：停止周期采集、传感器轮询、告警待机、推流暂停。

## 摄像头

插入 UVC 摄像头后内核日志类似：

```text
uvcvideo: Found UVC 1.00 device HD Camera
USB Video Class driver (1.1.1)
```

确认节点：

```bash
ls -l /dev/video*
v4l2-ctl --device=/dev/video0 --list-formats-ext   # 若已安装 v4l-utils
```

本程序请求 **MJPEG**。若仅有 YUYV，需换摄像头或自行加转码。

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

## 阈值（可宏覆盖）

- 电压安全：`4.8 ~ 5.2 V`
- 温度上限：`30 °C`
- 湿度上限：`65 %RH`

## LCD

未绑定固定 LCD 驱动路径时：传感器与 AI 文本打印到 **stdout**，同时推到网页叠加。可选 framebuffer 接口为 stub。

## 目录结构

```text
main.c
app/          状态机、告警、推流、多模态、抓拍、显示
driver/       LED/蜂鸣器/风扇/传感器/按键/V4L2 摄像头
snapshots/    告警 JPG（运行时创建）
```
