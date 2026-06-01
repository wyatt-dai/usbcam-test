# StarryOS USB 摄像头驱动复现文档

> 从完成 StarryOS 基础环境搭建（见 `sg2002-licheervnano-setup.md`）后开始，
> 到成功使用 USB 摄像头拍照的完整流程。

## 前提条件

- 已完成 SG2002 LicheeRV Nano 的 StarryOS 环境搭建
- SD 卡已配置为 Linux + StarryOS 双启动（三分区）
- 宿主机：Ubuntu，已安装 Rust 工具链和 RISC-V 交叉编译器
- 两个 USB 摄像头：0c45:64ab (Sonix) 和 1e45:8022 (Suyin)

## 硬件

| 项目 | 规格 |
|------|------|
| 开发板 | LicheeRV Nano (SG2002 SoC, RISC-V 64) |
| SD 卡 | 64GB MicroSD，三分区布局 |
| 串口 | USB 串口线，115200 波特率 |
| 摄像头 1 | 0c45:64ab (Sonix/Microdia)，MJPEG + YUY2 |
| 摄像头 2 | 1e45:8022 (Suyin)，MJPEG + YUY2 |

---

## 一、驱动代码修复

### 1.1 问题：ioctl 在自旋锁内调用 sleep 导致 panic

#### 定位过程

**panic 报错**：

```
panicked at os/arceos/modules/axtask/src/api.rs:299:5:
sleeping or rescheduling is not allowed in atomic context: irq_enabled=false
```

关键信息：`irq_enabled=false` 说明当前处于中断禁用状态。

**追溯调用链**：

1. 应用调用 `ioctl(INIT)` → 触发 `CviCamera::ioctl()`
2. `ioctl()` 调用 `self.state.lock()` 获取锁
3. 锁类型定义在第 9 行：`use ax_kspin::SpinNoIrq as Mutex;`，`SpinNoIrq` 是一个**禁用中断的自旋锁**，获取时会关闭中断
4. 锁内调用 `ensure_initialized()` → `init_usb_camera()`
5. `init_usb_camera()` 第 152 行调用了 `ax_task::sleep(Duration::from_micros(2_000_000))`（sleep 2 秒等待 USB 硬件就绪）
6. `sleep()` 检测到中断被禁用 → panic

**完整的 panic 调用链**：

```
ioctl(INIT)
  → self.state.lock()              ← SpinNoIrq 获取锁，禁用中断
    → ensure_initialized()
      → init_usb_camera()
        → ax_task::sleep(2s)       ← 检测到 irq_enabled=false → panic!
```

#### 修复思路

将初始化逻辑拆为两步：锁外执行耗时的初始化（含 sleep），锁内只做快速的 session 赋值。

**修改文件**：`os/StarryOS/kernel/src/pseudofs/dev/cvi_usb_camera.rs`

**修改内容**：将初始化逻辑拆分为锁外执行初始化、锁内设置 session。

```rust
// 修改前
fn ioctl(&self, cmd: u32, arg: usize) -> VfsResult<usize> {
    match cmd {
        CVI_CAMERA_IOCTL_INIT => {
            self.state.lock().ensure_initialized()?;  // 锁内 sleep → panic
            Ok(0)
        }
        // ...
    }
}

// 修改后
impl UsbCameraState {
    fn is_initialized(&self) -> bool {
        self.session.is_some()
    }
    fn set_session(&mut self, session: UsbCameraSession) {
        self.session = Some(session);
    }
}

fn ioctl(&self, cmd: u32, arg: usize) -> VfsResult<usize> {
    match cmd {
        CVI_CAMERA_IOCTL_INIT => {
            let needs_init = !self.state.lock().is_initialized();  // 锁内快速检查
            if needs_init {
                let session = init_usb_camera()?;  // 锁外初始化（含 sleep）
                self.state.lock().set_session(session);  // 锁内设置
            }
            Ok(0)
        }
        // ...
    }
}
```

### 1.2 diff 总结

```diff
--- a/os/StarryOS/kernel/src/pseudofs/dev/cvi_usb_camera.rs
+++ b/os/StarryOS/kernel/src/pseudofs/dev/cvi_usb_camera.rs

- impl UsbCameraState {
-     fn ensure_initialized(&mut self) -> VfsResult<()> {
-         if self.session.is_none() {
-             self.session = Some(init_usb_camera().map_err(...)?);
-         }
-         Ok(())
-     }
+ impl UsbCameraState {
+     fn is_initialized(&self) -> bool {
+         self.session.is_some()
+     }
+     fn set_session(&mut self, session: UsbCameraSession) {
+         self.session = Some(session);
+     }

  fn ioctl(...)
      CVI_CAMERA_IOCTL_INIT => {
-         self.state.lock().ensure_initialized()?;
+         let needs_init = !self.state.lock().is_initialized();
+         if needs_init {
+             let session = init_usb_camera().map_err(...)?;
+             self.state.lock().set_session(session);
+         }
          Ok(0)
      }
```

---

## 二、编译 StarryOS

```bash
cd /workspace/cloneto2002/tgoskits

# 编译 StarryOS 内核
cargo xtask starry build --arch riscv64
```

生成文件：`target/riscv64gc-unknown-none-elf/release/starryos.uimg`

---

## 三、编译测试程序

测试程序源码位于 `/workspace/usbcam_test/starrycam_test.c`。

### 3.1 安装交叉编译器（如未安装）

```bash
sudo apt update
sudo apt install -y gcc-riscv64-linux-gnu
```

### 3.2 编译

```bash
cd /workspace/usbcam_test

# 交叉编译（需已安装 gcc-riscv64-linux-gnu）
make
```

Makefile 内容：

```makefile
CC = riscv64-linux-gnu-gcc
CFLAGS = -Wall -Wextra -O2 -static

TARGET = starrycam_test
SRC = starrycam_test.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
```

生成文件：`starrycam_test`（RISC-V 64 静态链接 ELF）

验证编译产物：

```bash
file starrycam_test
# ELF 64-bit LSB executable, UCB RISC-V, statically linked
```

---

## 四、部署到 SD 卡

在宿主机上操作：

```
复制测试程序到starryos的rootfs分区
sudo cp /workspace/usbcam_test/starrycam_test /mnt/sd/home/
```

## 五、启动 StarryOS

### 5.1 U-Boot 启动

串口连接后，上电时按任意键进入 U-Boot，执行：

```
ext4load mmc 0:2 0x80200000 /root/starryos.uimg; bootm 0x80200000 - $fdtcontroladdr;
```
### 5.2 插入摄像头

在 StarryOS 启动后将 USB 摄像头插入 SG2002 的 USB-C 口。

### 5.3 验证启动

看到以下提示说明 StarryOS 启动成功：

```
root@starry:/root #
```

---

## 六、测试 USB 摄像头

### 6.1 基本测试

```bash
cd /home
./starrycam_test
```

预期输出（以摄像头 1e45:8022 为例）：

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
  [OK] 抓取成功: 64010 字节

=== 帧验证 ===
  [OK] JPEG 格式有效

=== 测试总结 ===
设备: /dev/cvi-usb-camera0
状态: 可用
分辨率: 1280x720
帧大小: 64010 字节
```

### 6.2 保存帧到文件

```bash
./starrycam_test -s photo.jpg
```


## 七、测试程序接口说明

### 7.1 ioctl 命令

| 命令号 | 名称 | 功能 | 参数 |
|--------|------|------|------|
| 1 | INIT | 初始化 USB 硬件、枚举设备、UVC 协商、启动流 | 无 |
| 2 | GET_INFO | 获取摄像头信息 | 指向 CameraInfo 结构体的指针 |
| 3 | GET_FRAME | 抓取一帧 MJPEG 数据 | 指向接收缓冲区的指针，返回值为帧大小 |

### 7.2 CameraInfo 结构体

```c
struct camera_info {
    uint16_t width;      // 分辨率宽度
    uint16_t height;     // 分辨率高度
    uint8_t  format;     // 格式 (1 = MJPEG)
    uint8_t  connected;  // 连接状态 (1 = 已连接)
} __attribute__((packed));
```

### 7.3 命令行参数

| 参数 | 说明 |
|------|------|
| `-d 设备` | 指定设备节点 (默认: `/dev/cvi-usb-camera0`) |
| `-s [文件]` | 保存帧到文件 (默认: `test_frame.jpg`) |
| `-h` | 显示帮助 |

---

## 八、两个摄像头测试结果

| 摄像头 | VID:PID | 分辨率 | 帧大小 | 冷启动状态 |
|--------|---------|--------|--------|-----------|
| Sonix/Microdia | 0c45:64ab | 1280x720 | ~28KB | 可用 |
| Suyin | 1e45:8022 | 1280x720 | ~64-90KB | 可用 |

---

## 九、已知限制

1. **不支持热插拔**：摄像头必须在 StarryOS 启动前插入，运行中拔插会导致后续操作失败
2. **只支持 MJPEG 格式**：驱动只验证 JPEG 帧（FFD8...FFD9），不处理 YUY2 等未压缩格式
3. **非标准接口**：使用自定义 ioctl，不兼容 V4L2 标准应用（如 ffmpeg）

### 多线程问题（当前无多线程需求，暂不修复）

以下问题在单线程场景下不会触发，但如后续引入多线程（如识别与控制分离）需逐个解决：

| # | 问题 | 严重性 | 说明 |
|---|------|--------|------|
| 1 | INIT 竞态 | 高 | 锁外初始化导致多线程可能同时执行 init_usb_camera()，USB 硬件冲突 + session 被覆盖 |
| 2 | GET_FRAME 持锁过长 | 中 | capture_frame() 包含多次 Isoch 传输和重试，锁内执行会阻塞其他线程 |
| 3 | vm_write_slice 在自旋锁内 | 高 | 帧数据（几十 KB）在禁用中断状态下拷贝到用户态，可能长时间关中断 |
| 4 | DMA 缓冲区共享 | 中 | capture_frame 返回全局 DMA 缓冲区引用，多线程同时抓帧会互相覆盖 |
| 5 | reset_frame_continuity 全局状态 | 中 | 帧连续性状态是全局的，一个线程重置会影响其他线程的帧组装 |

---

## 十、驱动架构

```
用户态应用 (starrycam_test)
  │ ioctl(INIT / GET_INFO / GET_FRAME)
  ▼
cvi_usb_camera.rs (设备驱动层)
  │ 实现 DeviceOps::ioctl()
  │ 帧验证 (JPEG 标记检查)
  ▼
uvc.rs (UVC 协议层)
  │ 描述符解析、PROBE/COMMIT 协商
  │ 帧组装 (Isoch 包 → 完整 JPEG)
  ▼
dwc2/ (USB 控制器驱动)
  │ DWC2 寄存器操作、DMA、Isoch 传输
  ▼
DWC2 硬件 (USB 2.0 OTG @ 0x04340000)
  │
  ▼
USB 摄像头
```

---

## 十一、故障排查


### 抓帧失败 (errno=5)

**原因**：Isoch 传输错误或帧组装超时。
**排查**：
- 检查内核日志中是否有 `isoch err:` 前缀（Isoch 传输错误）
- 检查是否有 `UVC: capture timeout` 日志（组装超时）
