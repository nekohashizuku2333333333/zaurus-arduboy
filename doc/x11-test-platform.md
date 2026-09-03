# 远端 X11 图形测试平台

## 目标

在 `root@192.168.122.187` 上布设一个独立的 i686/X11 测试平台，用来在进入真机
SL-C750 前先图形化验证 Arduboy 模拟器核心、SSD1306 帧缓冲刷新、键盘映射和基础
UI 行为。

这个平台不是 Zaurus Qt/Embedded 主线，也不替代 ARM OABI 构建；它只作为远端桌面
调试工具使用。

## 远端环境核验

已核验的远端系统：

```text
Linux pdaX86 2.6.11-mm4 i686
```

已找到的 native i686 工具链：

```text
/opt/native/i686/3.4.5-2.2.5/bin
```

已找到的 Zaurus/Qt SDK 仍保持原样：

```text
/opt/Qtopia/qt-2.3.2
/opt/murphytalk-sdk/qtopia-free-1.7.0
/opt/cross/arm/2.95.3-2.15/bin
/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin
```

已确认的关键工具：

```text
/opt/cross/arm/2.95.3-2.15/bin/arm-cacko-linux-gnu-g++  -> 2.95.3
/opt/cross/arm/3.4.6-xscale-softvfp-akita/bin/armv5tel-cacko-linux-gcc -> 3.4.6
/opt/Qtopia/qt-2.3.2/src/moc/moc -> Qt meta object compiler
```

## 新增文件

X11 测试前端：

```text
src/x11_test_main.c
```

远端构建脚本：

```text
scripts/build_x11_remote.sh
```

构建输出目录：

```text
build/x11/
```

这个目录只放测试产物，不参与 Zaurus IPK 打包。

## X11 测试前端行为

窗口大小固定为：

```text
640x480
```

顶部 48 像素是手绘工具条：

```text
Load / Pause / Reset
```

当前 X11 测试版的 `.hex` 加载方式：

```sh
build/x11/arduboy_x11_test /path/to/game.hex
```

说明：X11 测试版优先用于验证图形刷新与模拟核心，所以 `Load` 按钮暂不打开文件
选择器；真正给 Zaurus 使用的 Qt/Embedded 前端已经实现了顶部 Load 按钮和内置
`.hex` 浏览器。

键盘映射：

```text
Arrow keys       -> D-pad
Z / Return/Space -> A
X / Escape       -> B
```

EEPROM 测试存档：

```text
$HOME/.arduboy-x11-eeprom.bin
```

## 构建命令

在远端源码目录中执行：

```sh
export PATH=/opt/native/i686/3.4.5-2.2.5/bin:$PATH
sh scripts/build_x11_remote.sh
```

脚本会使用：

```text
CC=/opt/native/i686/3.4.5-2.2.5/bin/gcc
AR=/opt/native/i686/3.4.5-2.2.5/bin/ar
```

输出：

```text
build/x11/arduboy_x11_test
build/x11/libzaurusarduboy.a
```

2026-09-03 已在远端 `/tmp/arduboy-x11-test` 完成构建。产物核验：

```text
build/x11/arduboy_x11_test: ELF 32-bit LSB executable, Intel 80386, for GNU/Linux 2.0.0
```

动态依赖：

```text
libX11.so.6 => /usr/X11R6/lib/libX11.so.6
libm.so.6
libc.so.6
libdl.so.2
/lib/ld-linux.so.2
```

## 运行命令

远端需要有可用 X server，并设置 `DISPLAY`：

```sh
export DISPLAY=:0
build/x11/arduboy_x11_test tests/fixtures/rjmp_self.hex
```

若通过 SSH 转发：

```sh
ssh -X root@192.168.122.187
cd /tmp/arduboy-x11-test
sh scripts/build_x11_remote.sh
build/x11/arduboy_x11_test tests/fixtures/rjmp_self.hex
```

实际连接仍需使用本项目记录的老 SSH 算法选项。

当前 SSH 构建会话中：

```text
DISPLAY=unset
```

因此直接运行测试程序会得到预期错误：

```text
Cannot open X display. Set DISPLAY first.
```

这表示 X11 测试程序已经可执行并正确链接，但需要远端本机 X 会话、VNC/Xvfb，或 SSH
X11 forwarding 提供 `DISPLAY` 后才能看到窗口。

## 隔离策略

为避免搞乱既有 SDK 和 ARM 主线：

```text
不修改 /opt/Qtopia/qt-2.3.2
不修改 /opt/murphytalk-sdk/qtopia-free-1.7.0
不修改 /opt/cross/arm/*
X11 测试构建只写 build/x11/
远端临时源码建议放 /tmp/arduboy-x11-test
```

Zaurus 主线仍使用：

```sh
sh scripts/build_zaurus.sh
sh scripts/package_ipk.sh
```
