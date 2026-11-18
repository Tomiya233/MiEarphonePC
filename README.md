# 小米耳机 · PC 原生控制中心 (MiEarphonePC)

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Direct2D](https://img.shields.io/badge/UI-Direct2D%20%2F%20DirectWrite-0078D4?logo=windows&logoColor=white)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-2ea44f)
![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20External%20DLLs-FF6900)

脱离「小米电脑管家」与第三方运行环境，以纯原生独立 `.exe` 在 Windows 10/11 上通过蓝牙 RFCOMM (`AF_BTH`) 直连控制小米 / Redmi 耳机。已在国行 **Redmi Buds 5 Pro (`N76`)** 真机完整验证。

---

## 界面预览

![主界面（浅色模式 · 降噪与日常控制）](assets/noise-light.png)

| 通透模式 · 深色主题 (HyperOS Dark) | 音效预设与 10 段自定义均衡器 |
| :---: | :---: |
| ![深色主题与通透模式](assets/noise-dark.png) | ![10段自定义均衡器](assets/eq.png) |

| 空间音频 3D 雷达与查找耳机 | 系统托盘与无感常驻 |
| :---: | :--- |
| ![空间音频与查找耳机](assets/spatial.png) | - **动态状态托盘图标**：根据未连接 / 关闭 / 降噪 / 通透实时切换配色<br>- **悬停三路电量预览**：鼠标指向托盘图标即看左耳、右耳、充电盒电量<br>- **托盘右键快捷菜单**：不打开主窗口直接切降噪档位、自适应降噪、EQ 预设、双设备连接与浅/深色主题<br>- **后台零动画开销**：最小化或隐藏至托盘时自动停用动画计时器，后台 `0%` 渲染占用 |

---

## 核心特性

- **零依赖极速原生架构**：C++20 + Direct2D / DirectWrite 硬件加速流体动效界面（浅/深色平滑过渡、70fps 按需动画引擎、Per-Monitor V2 高分屏缩放适配）+ WinSock2 `AF_BTH` 蓝牙协议栈。单文件无任何第三方运行时 DLL 依赖，冷启动 `< 20 ms`。
- **现代化交互与声学可视化**：
  - **270° 环形三路电量仪表盘**：实时平滑扫掠显示左耳、右耳、充电盒独立电量、充电闪电标识 (`bit7`) 与离线状态 (`0xFF`)。
  - **声学动态可视化**：分段滑动切换标签页、阻尼拨动开关、降噪/通透实时多谐波声波可视化、10 段 EQ 三次贝塞尔平滑曲线与填充渐变、3D 空间音频声场雷达、语法着色协议帧终端及悬浮玻璃 Toast 提示。
- **全功能声学与日常控制**（均已对照官方小米耳机 APK `1.39.0` 逆向核实并真机实测）：

| 功能模块 | 协议指令 / Config ID | 支持能力说明 |
| :--- | :--- | :--- |
| **分体三路电量** | `0x02` attr `7` / `0x0E` attr `0` | 左耳 / 右耳 / 充电盒独立百分比 + 充电状态实时推送 |
| **降噪与通透控制** | `CFG 11` + `CFG 37` | 关闭 / 降噪（轻度 · 均衡 · 深度 + 独立「✦ 自适应降噪」）/ 通透（标准通透 · 人声增强 · 环境增强） |
| **EQ 预设与自定义** | `CFG 7` / `CFG 55` | 8 种官方调音预设 + `62Hz ~ 16kHz` 10 段自定义均衡器（拖动实时预览、写入耳机、一键归零） |
| **空间音频与沉浸声** | `CFG 29/30` / `CFG 54` | 空间音频开关、动态头部追踪 + 场景沉浸声渲染（标准 / 音乐 / 视频 / 游戏 / 有声书） |
| **日常快捷开关** | `CFG 4 / 47 / 59 / 72 / 3` | 双设备连接、低延迟游戏模式、个性化降噪、自适应音量、来电自动接听 |
| **查找耳机响铃** | `CFG 9` | 双耳 / 仅左耳 / 仅右耳响铃查找与一键停止 |

---

## 构建与使用

需将 MinGW-w64（推荐 UCRT64，支持 C++20）的 `g++` 与 `windres` 加入系统 `PATH`：

```powershell
# 1. 编译资源与主程序
mkdir -Force build | Out-Null
windres -c 65001 -I . app_res.rc -O coff -o build/app_res.o
g++ -std=c++20 -O2 -Wall -Wextra `
    main_ui.cpp auth_cipher.cpp rcsp.cpp bt_session.cpp build/app_res.o `
    -o build/MiEarphonePC.exe `
    -static -mwindows -municode `
    -ld2d1 -ldwrite -ldwmapi -lws2_32 -lbthprops -lbcrypt -lshell32 -luser32 -lgdi32 -ladvapi32 -lole32

# 2. （可选）编译并运行协议自检单测
g++ -std=c++20 -O2 -Wall -Wextra `
    test_core.cpp auth_cipher.cpp rcsp.cpp bt_session.cpp `
    -o build/test_core.exe -static -lws2_32 -lbthprops -lbcrypt -ladvapi32
.\build\test_core.exe
```

- **直接运行**：双击 `build\MiEarphonePC.exe`（加 `--tray` 参数可静默驻留系统托盘）。

> ⚠️ **注意**：连接前请先彻底退出「小米电脑管家」，避免管家独占同一条蓝牙 RFCOMM (`0000FD2D`) 控制通道。

---

## 目录结构

```text
assets/                     README 界面实机渲染预览图
main_ui.cpp                 Direct2D/DirectWrite 流体界面、系统托盘与快捷菜单
bt_session.hpp / .cpp       WinSock2 AF_BTH RFCOMM 会话、事件接收与自动重连
rcsp.hpp / .cpp             RCSP 帧流拆包、TLV、分体电量、10段EQ、空间音频编解码
auth_cipher.hpp / .cpp      自定义 8 轮分组密码双向认证实现
config_store.hpp            INI 配置持久化与开机自启注册表管理
icon_gen.hpp                矢量 SDF 超采样抗锯齿动态图标渲染器
app_res.rc / app.manifest   Windows 资源、图标 (app.ico) 与 PerMonitorV2 清单
test_core.cpp               C++ 协议与真机端到端自检程序
```
