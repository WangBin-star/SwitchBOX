# SwitchBOX

## 本项目的目的

让 `Nintendo Switch` 可以像电视盒子一样使用，支持：

- 串流播放 `NAS / SMB` 共享视频
- 播放 `IPTV` 电视节目

注意：

- 本项目是 `Switch-first`，`Windows` 仅作为开发调试目标
- 不建议在“非大气层虚拟系统”环境下使用
- 在不合适的系统环境下，可能无法使用，也可能存在风险

## 参考项目 & 感谢

IPTV 方向参考：

- `TsVitch`
- https://github.com/giovannimirulla/TsVitch

SMB 流媒体方向参考：

- `nxmp`
- https://github.com/proconsule/nxmp

## 项目说明

### 当前开发方向

当前项目按下面的顺序推进：

1. 搭建仓库骨架
2. 打通桌面调试构建
3. 打通 Switch 构建并生成 `.nro`
4. 接入应用壳和 UI 框架
5. 验证播放链路
6. 先做 IPTV，再做 SMB 技术验证和 UX

当前已经完成：

- 基础 `CMake` 工程骨架
- `Desktop` 调试目标
- `Switch` 交叉编译
- `.nro` 生成链路

### 环境配置方式

当前推荐开发环境：

- `Windows`
- `VS Code`
- `Visual Studio 2026 Community`，并安装 `C++` 工作负载
- `Python 3`
- `devkitPro`

需要确认的系统环境变量：

```text
DEVKITPRO=C:\devkitPro
DEVKITA64=C:\devkitPro\devkitA64
```

可用命令验证：

```powershell
python --version
echo $env:DEVKITPRO
echo $env:DEVKITA64
```

期望结果：

- `python` 能输出版本号
- `DEVKITPRO` 指向 `C:\devkitPro`
- `DEVKITA64` 指向 `C:\devkitPro\devkitA64`

补充说明：

- `Desktop` 构建使用 `Visual Studio` 自带的 `cmake` / `ninja`
- `Switch` 构建使用 `devkitPro MSYS2` 自带的 `cmake`
- VS Code 任务已经处理好了这两套环境差异，日常开发优先直接用任务

### 现在可以直接使用的命令

#### 方式一：VS Code 任务

在 `VS Code` 中执行 `Tasks: Run Task`，可以直接使用：

- `switchbox: configure desktop`
- `switchbox: build desktop`
- `switchbox: run desktop`
- `switchbox: configure switch`
- `switchbox: build switch`

其中最常用的是：

- 桌面编译：`switchbox: build desktop`
- Switch 编译：`switchbox: build switch`

#### 方式二：PowerShell 直接执行

先进入项目目录：

```powershell
cd "D:\Github项目\SwitchBOX\SwitchBOX"
```

桌面目标配置：

```powershell
cmd /d /s /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" -S . -B build\desktop-vs -G Ninja -DCMAKE_MAKE_PROGRAM=\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe\" -DSWITCHBOX_BUILD_DESKTOP=ON -DSWITCHBOX_BUILD_SWITCH=OFF"
```

桌面目标编译：

```powershell
cmd /d /s /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" --build build\desktop-vs"
```

桌面目标运行：

```powershell
.\build\desktop-vs\app\switchbox_desktop.exe
```

Switch 目标配置：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe -S . -B build/switch -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch.cmake -DSWITCHBOX_BUILD_DESKTOP=OFF -DSWITCHBOX_BUILD_SWITCH=ON
```

Switch 目标编译：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --build build/switch
```

### 当前产物位置

桌面可执行文件：

- `build/desktop-vs/app/switchbox_desktop.exe`

Switch 可执行产物：

- `build/switch/app/switchbox_switch.elf`
- `build/switch/app/switchbox_switch.nacp`
- `build/switch/app/switchbox_switch.nro`
