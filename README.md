# Codex Usage Monitor

ESP32-S3 + ST7305 反射式 LCD，通过 WiFi 接收 Codex 使用数据并实时显示，支持温湿度监测。

## 功能

- **使用量显示** — 5 小时/1 周额度剩余百分比 + 进度条
- **重置时间** — 显示额度窗口重置时间，实时同步
- **实时时钟** — 从服务器同步当前时间
- **温湿度** — SHTC3 传感器实时采集，3 秒刷新
- **连接状态** — SYNC OK / OFFLINE，20 秒无数据自动切离线
- **WiFi 自动重连** — 断线后自动重连

## 架构

```
PC (热点)                              ESP32-S3
┌──────────────────────────┐          ┌──────────────────┐
│ codex_usage_server.py    │  TCP    │  WiFi STA 模式    │
│  - Codex API 实时查询     │ ──────> │  TCP Client       │
│  - 本地 SQLite 降级       │  5s/次  │  解析 USAGE 帧    │
│  - HTTP :8765            │         │  驱动 ST7305 屏幕  │
│  - TCP  :8766            │         │  SHTC3 温湿度      │
└──────────────────────────┘          └──────────────────┘
```

数据源优先级: Codex API → 本地 SQLite 日志 → usage.auto.json → 手动 HTTP 更新

## 快速开始

### 1. 开启电脑热点

- 热点名称: `Evans`
- 热点密码: `Evans12345678`

### 2. 启动 Python 服务器

```bash
cd C:\Users\Administrator\Desktop\monitor
python tools/codex_usage_server.py
```

服务器会自动从 Codex API 拉取实时用量数据。需要 `~/.codex/auth.json` 中有有效的 access_token。

### 3. 烧录固件 (首次或更新代码后)

需要 ESP-IDF v6.0.1 环境:

```bash
idf.py build
idf.py -p COM3 flash
```

### 4. 查看串口日志 (可选)

```bash
idf.py -p COM3 monitor
```

正常输出:

```
WiFi connected, IP: 192.168.137.133
TCP connected to 192.168.137.1:8766
WiFi update: 5H=99% reset=23:39, 1W=69% reset=05/31, time=18:50
SHTC3 update: temp=26.3C rh=61%
```

## 服务器命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--tcp-port` | 8766 | TCP 推送端口 |
| `--tcp-push-seconds` | 5 | 推送间隔 (秒) |
| `--no-tcp` | - | 禁用 TCP 推送 |

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CODEX_USAGE_REFRESH_SECONDS` | 30 | 自动刷新间隔 (秒) |
| `CODEX_USAGE_MAX_AGE` | 3600 | 本地数据最大有效时间 (秒) |
| `CODEX_USAGE_LOCAL_CODEX` | 1 | 设为 0 禁用本地 SQLite 读取 |
| `CODEX_USAGE_SOURCE_URL` | (空) | 备用数据 URL |
| `CODEX_USAGE_TCP_PORT` | 8766 | TCP 推送端口 |

## HTTP 接口

| 路径 | 说明 |
|------|------|
| `GET /usage.json` | 获取当前使用数据 |
| `GET /refresh` | 触发数据刷新，返回数据源 |
| `GET /update?five_hour_remaining=99&week_remaining=85` | 手动更新数据 |

## TCP 推送帧格式

```
USAGE|<5小时剩余%>|<5小时重置时间>|<1周剩余%>|<1周重置日期>|<当前时间>\n
```

示例: `USAGE|99|23:39|69|05/31|18:50`

## 固件配置

在 `main/app_main.cpp` 顶部修改:

```cpp
constexpr char kWifiSsid[] = "Evans";           // WiFi 名称
constexpr char kWifiPassword[] = "Evans12345678"; // WiFi 密码
constexpr char kServerHost[] = "192.168.137.1";   // PC 热点网关 IP
constexpr uint16_t kServerPort = 8766;            // TCP 端口
```

## 硬件连接

| 功能 | GPIO |
|------|------|
| SPI CLK | GPIO11 |
| SPI MOSI | GPIO12 |
| SPI CS | GPIO40 |
| DC | GPIO5 |
| RST | GPIO41 |
| I2C SDA (SHTC3) | GPIO13 |
| I2C SCL (SHTC3) | GPIO14 |

## 故障排查

**WiFi 连接失败** — 检查热点是否开启，确认 SSID 和密码

**TCP 连接失败 (errno 113)** — 服务器未运行或防火墙阻止:

```powershell
Get-NetFirewallRule -DisplayName 'python*' | Remove-NetFirewallRule
```

**屏幕不更新** — 确认服务器正在运行，访问 `http://localhost:8765/usage.json` 验证

**重置时间不更新** — 确认固件是最新的，重置时间现在会动态重绘

**SPI 崩溃 (ESP_ERR_INVALID_STATE)** — 已通过 display_mutex 修复，多任务不会同时访问 SPI

## 文件结构

```
monitor/
├── main/
│   ├── app_main.cpp          # 主程序 (WiFi + 显示 + 传感器)
│   ├── rlcd_st7305.cpp       # ST7305 显示驱动
│   ├── rlcd_st7305.h         # 显示驱动头文件
│   └── CMakeLists.txt        # 组件依赖
├── tools/
│   └── codex_usage_server.py # PC 端服务器 (Codex API + TCP 推送)
├── components/
│   └── codec_board/          # ES8311 音频 codec 驱动 (备用)
├── CMakeLists.txt            # ESP-IDF 项目配置
├── sdkconfig.defaults        # 编译配置
└── README.md
```
