# USB摄像头测试程序

## 文件说明

| 文件 | 用途 | 运行环境 |
|------|------|----------|
| `starrycam_test.c` | StarryOS功能测试 | StarryOS (RISC-V) |
| `starrycam_perf_test.c` | StarryOS性能测试 | StarryOS (RISC-V) |
| `linuxcam_perf_test.c` | Linux性能测试 | Linux (RISC-V/x86) |
| `linuxcam_simple_test.c` | Linux简单诊断 | Linux (RISC-V/x86) |
| `Makefile` | 编译脚本 | 开发机 |

## 编译

```bash
# 编译所有程序
make all

# 只编译StarryOS程序
make func      # starrycam_test
make perf      # starrycam_perf_test

# 只编译Linux程序
make linux       # linuxcam_perf_test_rv (RISC-V)
make linux-x86   # linuxcam_perf_test (x86)

# 清理
make clean
```

## starrycam_test

功能测试程序，验证摄像头基本功能。

### 用法

```bash
starrycam_test [-d 设备] [-s [输出文件]]
```

### 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-d <设备>` | 设备节点 | `/dev/cvi-usb-camera0` |
| `-s [文件]` | 保存帧到文件 | `test_frame.jpg` |

### 示例

```bash
# 基本测试
starrycam_test

# 指定设备
starrycam_test -d /dev/cvi-usb-camera0

# 测试并保存帧
starrycam_test -s

# 测试并保存到指定文件
starrycam_test -s /tmp/frame.jpg
```

### 输出示例

```
=== StarryOS USB 摄像头测试 ===

[1/4] 打开设备 /dev/cvi-usb-camera0 ...
  [OK] 设备已打开 (fd=3)

[2/4] 初始化摄像头 (IOCTL_INIT) ...
  [OK] 初始化成功

[3/4] 获取摄像头信息 (IOCTL_GET_INFO) ...
  [OK] 摄像头信息:
       分辨率: 1280x720
       格式:   1 (MJPEG)
       连接:   1 (已连接)

[4/4] 抓取一帧 (IOCTL_GET_FRAME) ...
  [OK] 抓取成功: 79496 字节

=== 帧验证 ===
  [OK] JPEG 格式有效

=== 测试总结 ===
设备: /dev/cvi-usb-camera0
状态: 可用
分辨率: 1280x720
帧大小: 79496 字节
```

---

## starrycam_perf_test

StarryOS性能测试程序，测试帧率、延迟、拷贝开销。

### 用法

```bash
starrycam_perf_test [选项]
```

### 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-d <设备>` | 设备节点 | `/dev/cvi-usb-camera0` |
| `-n <帧数>` | 测试帧数 | 100 |
| `-t <秒数>` | 持续测试时长 | 10 |
| `-s <目录>` | 保存帧到目录 | 无 |
| `-a` | 运行所有测试 | - |
| `-1` | 只运行基本帧率测试 | - |
| `-2` | 只运行延迟分布测试 | - |
| `-3` | 只运行缓冲区拷贝分析 | - |
| `-4` | 只运行持续帧率测试 | - |
| `-5` | 只运行YOLO推理基准 | - |

### 示例

```bash
# 运行所有测试
starrycam_perf_test -a

# 只运行帧率测试，200帧
starrycam_perf_test -1 -n 200

# 持续测试10秒
starrycam_perf_test -4 -t 10

# 持续测试并保存帧
starrycam_perf_test -4 -t 10 -s /tmp/frames
```

### 输出示例

```
=== StarryOS USB 摄像头性能测试 ===
设备: /dev/cvi-usb-camera0
帧数: 100
时长: 10 秒

摄像头信息:
  分辨率: 1280x720
  格式:   1 (MJPEG)
  连接:   1 (已连接)

[测试1] 基本帧率测试 (100 帧)
预热中...
开始测试...
  帧 0: 31.73 ms, 79442 字节
  帧 10: 33.36 ms, 81086 字节
  帧 20: 33.35 ms, 76138 字节

=== 基本帧率 ===
总帧数: 100
总时间: 3308.24 ms
帧率:   30.23 FPS
帧时间: 最小=30.16 ms, 最大=39.49 ms, 平均=33.08 ms
总数据: 7949622 字节 (7.58 MB)
吞吐量: 19.22 Mbps

延迟分布:
  P50 (中位数): 33.35 ms
  P90:          33.37 ms
  P95:          33.37 ms
  P99:          33.39 ms
  最小:         33.21 ms
  最大:         33.39 ms

[测试3] 缓冲区拷贝开销分析 (100 帧)
  帧 0: ioctl=32.01 ms, copy=0.62 ms
  帧 10: ioctl=33.16 ms, copy=0.07 ms

缓冲区拷贝开销分析:
  平均ioctl时间:  32.85 ms
  平均拷贝时间:   0.08 ms
  拷贝占比:       0.3%

[测试4] 持续帧率测试 (10 秒)
  1.0秒: 31帧, 30.07 FPS, 20.51 Mbps
  2.1秒: 61帧, 29.56 FPS, 19.69 Mbps

持续帧率测试结果:
  测试时长: 10.03 秒
  总帧数:   300
  平均帧率: 29.92 FPS
  总数据量: 24060022 字节 (22.95 MB)
  吞吐量:   19.19 Mbps

[测试5] YOLO推理性能基准 (100 帧)
  帧 0: capture=32.12 ms, process=0.11 ms
  帧 10: capture=33.21 ms, process=0.11 ms

YOLO推理性能基准:
  平均捕获时间: 32.75 ms
  平均处理时间: 0.11 ms
  预期帧率:     30.43 FPS
  捕获占比:     99.7%
  处理占比:     0.3%

=== 测试完成 ===
```

---

## linuxcam_perf_test

Linux性能测试程序，与starrycam_perf_test功能对标。

### 用法

```bash
linuxcam_perf_test_rv [选项]   # RISC-V版本
linuxcam_perf_test [选项]      # x86版本
```

### 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-d <设备>` | 设备节点 | `/dev/video0` |
| `-n <帧数>` | 测试帧数 | 100 |
| `-t <秒数>` | 持续测试时长 | 10 |
| `-s <目录>` | 保存帧到目录 | 无 |
| `-a` | 运行所有测试 | - |
| `-1` | 只运行基本帧率测试 | - |
| `-2` | 只运行延迟分布测试 | - |
| `-3` | 只运行缓冲区拷贝分析 | - |
| `-4` | 只运行持续帧率测试 | - |
| `-5` | 只运行YOLO推理基准 | - |

### 示例

```bash
# 运行所有测试
linuxcam_perf_test_rv -a

# 持续测试并保存帧
linuxcam_perf_test_rv -4 -t 10 -s /tmp/linux_frames

# 只运行帧率测试
linuxcam_perf_test_rv -1 -n 200
```

### 输出示例

```
=== Linux USB 摄像头性能测试 ===
设备: /dev/video0
帧数: 100
时长: 10 秒

设备信息:
  驱动: uvcvideo
  卡:   HD Camera: HD Camera
  总线: usb-4340000.usb-1

摄像头信息:
  分辨率: 1280x720
  格式:   0x47504A4D (MJPEG)
  帧率:   30 FPS

支持的帧率:
    30 FPS (1/1)
    25 FPS (1/1)
    15 FPS (1/1)
    10 FPS (1/1)
[OK] 帧率已设置为 30 FPS

[测试1] 基本帧率测试 (100 帧)
预热中 (10 帧)...
开始测试...
  帧 0: 35.57 ms, 108094 字节
  帧 10: 32.30 ms, 122944 字节

=== 基本帧率 ===
总帧数: 100
总时间: 3333.98 ms
帧率:   29.99 FPS
帧时间: 最小=31.81 ms, 最大=35.67 ms, 平均=33.34 ms
总数据: 12736824 字节 (12.15 MB)
吞吐量: 30.56 Mbps

延迟分布:
  P50 (中位数): 32.27 ms
  P90:          35.56 ms
  P95:          35.62 ms
  P99:          35.71 ms
  最小:         32.04 ms
  最大:         35.71 ms

[测试4] 持续帧率测试 (10 秒)
保存目录: /tmp/linux_frames
  1.0秒: 30帧, 29.98 FPS, 20.93 Mbps, 已保存30帧
  2.0秒: 60帧, 29.99 FPS, 21.50 Mbps, 已保存60帧

持续帧率测试结果:
  测试时长: 10.00 秒
  总帧数:   300
  平均帧率: 30.00 FPS
  总数据量: 27485890 字节 (26.21 MB)
  吞吐量:   21.99 Mbps
  已保存:   300 帧

=== 测试完成 ===
```

---

## linuxcam_simple_test

Linux简单诊断程序，用于排查摄像头问题。

### 用法

```bash
linuxcam_simple_test_rv [设备] [帧数]
```

### 参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| 设备 | 设备节点 | `/dev/video0` |
| 帧数 | 测试帧数 | 10 |

### 示例

```bash
# 基本诊断
linuxcam_simple_test_rv

# 指定设备和帧数
linuxcam_simple_test_rv /dev/video0 20
```

### 输出示例

```
=== Linux V4L2 简单测试 ===
设备: /dev/video0
帧数: 10

[1] 打开设备...
[OK] 设备已打开 (fd=3)

[2] 查询设备能力...
[OK] 驱动: uvcvideo
[OK] 卡:   HD Camera: HD Camera
[OK] 总线: usb-4340000.usb-1

[3] 设置格式...
[OK] 分辨率: 1280x720
[OK] 格式: 0x47504A4D
[OK] 字节每行: 1280
[OK] 图像大小: 1843200

[4] 请求缓冲区...
[OK] 缓冲区数量: 4

[5] 映射缓冲区...
[OK] 缓冲区0: 长度=1843200, 地址=0x3f8a0000
[OK] 缓冲区1: 长度=1843200, 地址=0x3f6e0000
[OK] 缓冲区2: 长度=1843200, 地址=0x3f520000
[OK] 缓冲区3: 长度=1843200, 地址=0x3f360000

[6] 入队缓冲区...
[OK] 缓冲区0已入队
[OK] 缓冲区1已入队
[OK] 缓冲区2已入队
[OK] 缓冲区3已入队

[7] 开始捕获...
[OK] 捕获已开始

[8] 捕获帧...
帧 0: 成功, 大小=108094字节, 序列=1, 时间戳=1234567890.123456
帧 1: 成功, 大小=122944字节, 序列=2, 时间戳=1234567890.156789
帧 2: 成功, 大小=124060字节, 序列=3, 时间戳=1234567890.190123

[9] 停止捕获...
[OK] 捕获已停止

[10] 清理...
[OK] 清理完成

=== 测试完成 ===
```

---

## Linux vs StarryOS对比测试

```bash
# Linux测试
linuxcam_perf_test_rv -a > linux_results.txt

# StarryOS测试
starrycam_perf_test -a > starry_results.txt

# 带存储测试
linuxcam_perf_test_rv -4 -t 10 -s /tmp/linux_frames
starrycam_perf_test -4 -t 10 -s /tmp/starry_frames
```
