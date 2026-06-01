# SG2002 LicheeRV Nano 环境搭建记录

## 硬件

- 开发板：LicheeRV Nano（SG2002 SoC，RISC-V 64）
- SD 卡：64GB MicroSD
- 连接方式：USB 串口线（同时供电和数据）
- USB 摄像头：两个（0c45:64ab Sonix 和 1e45:8022 Suyin）

## 一、烧录 Linux 官方镜像

从 Sipeed 官方获取 LicheeRV Nano Linux 镜像，使用 dd 烧录到 SD 卡：

```bash
# 宿主机上，找到 SD 卡设备号
lsblk

# 烧录（假设 SD 卡是 /dev/sdX）
sudo dd if=licheerv-nano-xxx.img of=/dev/sdX bs=4M status=progress
```

SD 卡分区布局：
```
p1: FAT32, 16MB, boot 分区（U-Boot、内核、DTB）
p2: ext4,  剩余空间, rootfs（完整 Linux 文件系统）
```

## 二、串口连接

- 串口设备：`/dev/ttyUSB0`，波特率 115200
- 使用 minicom 或 screen 连接：
  ```bash
  minicom -D /dev/ttyUSB0 -b 115200
  ```
- 注意：USB 串口线同时提供电源（vsys），不需要额外电源

## 三、Linux USB Host 模式切换

SG2002 的 USB-C 口默认是 **Device 模式**（OTG），需要切换为 **Host 模式**才能识别 USB 摄像头。

### 诊断

```bash
cat /sys/class/udc/*/state
# 输出 "not attached" 说明是 Device 模式

cat /sys/devices/platform/4340000.usb/uevent
# DRIVER=dwc2, OF_COMPATIBLE_0=cvitek,cv182x-usb
```

### 切换方法

通过修改 boot 分区上的标志文件控制 USB 模式（由 `/etc/init.d/S08usbdev` 脚本读取）：

```bash
mount -o rw,remount /boot
rm /boot/usb.dev
touch /boot/usb.host
umount /boot
reboot
```

重启后验证：

```bash
ls /dev/video*
# 应该看到 /dev/video0 /dev/video1

cat /sys/class/video4linux/video0/name
# 显示摄像头名称
```

### 注意事项

- 摄像头需要在 **启动完成后插入**，不支持插着摄像头启动
- 切换到 Host 模式后，USB gadget 网络（RNDIS/NCM）不可用
- WiFi 连接不受影响

## 四、摄像头测试（Linux）

```bash
# 查看摄像头
lsusb

# 抓一张图
ffmpeg -f video4linux2 -i /dev/video0 -frames:v 1 -y /tmp/test.jpg
```

两个摄像头在 Linux 下均可正常工作，输出 YUY2 格式 1280x720。

## 五、构建 StarryOS

### 5.1 编译

```bash
cd /workspace/cloneto2002/tgoskits

# 配置
argo starry defconfig licheerv-nano-sg2002

# 编译
cargo starry build
```

生成文件：`target/riscv64gc-unknown-none-elf/release/starryos.uimg`

### 5.2 生成 rootfs

```bash
cargo xtask starry rootfs --arch riscv64
```

生成文件：`tmp/axbuild/rootfs/rootfs-riscv64-alpine.img`（Alpine Linux 最小 rootfs）

## 六、准备 SD 卡（Linux + StarryOS 双启动）

在宿主机上操作 SD 卡（假设设备是 `/dev/sda`）。

### 6.1 缩小 Linux rootfs 分区

```bash
sudo umount /dev/sda2
sudo e2fsck -f /dev/sda2
sudo resize2fs /dev/sda2 4G
```

注意：resize2fs 设 4G 精确值，但 fdisk 的 `+4G` 分区会少 1 个块，需要额外修复：

```bash
sudo resize2fs /dev/sda2 1048575
```

### 6.2 用 fdisk 创建第三个分区

```bash
sudo umount /dev/sda1
sudo fdisk /dev/sda
```

在 fdisk 中依次输入（p2 起始扇区是 32769）：

```
d → 2 → n → p → 2 → 32769 → +4G → n → 回车（选 n 无需打 p）
→ 3 → 回车 → 回车 → a → 3 → w
```

最终分区布局：

| 分区 | 类型 | 大小 | 内容 |
|------|------|------|------|
| p1 | FAT32 | 16MB | U-Boot / 引导文件 |
| p2 | ext4 | 4GB | Linux rootfs |
| p3 | ext4 | 55.5GB | StarryOS rootfs |

### 6.3 格式化 p3（简单 ext4，兼容 StarryOS 驱动）

```bash
sudo mkfs.ext4 -F -O ^has_journal,^64bit,^metadata_csum -L starryrootfs /dev/sda3
```

关键参数说明：
- `^has_journal`：禁用日志，StarryOS 的 ext4 驱动不支持
- `^64bit`：禁用 64 位块地址
- `^metadata_csum`：禁用元数据校验

### 6.4 填充 StarryOS rootfs

```bash
sudo mkdir -p /mnt/starrootfs /mnt/p3
sudo mount -o loop tmp/axbuild/rootfs/rootfs-riscv64-alpine.img /mnt/starrootfs
sudo mount /dev/sda3 /mnt/p3
sudo cp -a /mnt/starrootfs/* /mnt/p3/
sudo umount /mnt/starrootfs /mnt/p3
```

### 6.5 复制内核到 p2

```bash
sudo mount /dev/sda2 /mnt/p3
sudo cp target/riscv64gc-unknown-none-elf/release/starryos.uimg /mnt/p3/root/
sudo umount /mnt/p3
```

## 七、U-Boot 启动 StarryOS

### 7.1 进入 U-Boot

上电时在串口快速按任意键，看到 `Hit any key to stop autoboot` 时停止自动启动。

如果电源通过 USB 串口线供电，拔插会导致串口断开。解决方法：
- 从 Linux 端执行 `reboot`，同时准备按键
- 或使用独立电源适配器供电

### 7.2 启动命令

```
ext4load mmc 0:2 0x80200000 /root/starryos.uimg; bootm 0x80200000 - $fdtcontroladdr;
```


### 7.3 验证启动成功

看到以下输出说明 StarryOS 启动成功：

```
root@starry:/root #
```

## 八、已知问题

1. **USB Host 模式切换**：需要修改 boot 分区标志文件，重启后生效
2. **StarryOS ext4 兼容性**：Linux rootfs 使用现代 ext4 特性，StarryOS 无法直接挂载，需要单独的简单 ext4 分区
3. **bootargs 来自 DTB**：axconfig 中的 `bootargs = "root=/dev/mmcblk0p3"` 不会被编译进内核，运行时从 FDT 读取，必须在 U-Boot 中手动设置
4. **USB 摄像头热插拔**：Linux 下需要在启动完成后插入摄像头
5. **USB 摄像头兼容性**：两个摄像头（Sonix 0c45:64ab 和 Suyin 1e45:8022）都支持 MJPEG 和 YUY2，但在 StarryOS 上的兼容性需要进一步测试
