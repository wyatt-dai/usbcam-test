/*
 * StarryOS USB 摄像头测试程序
 *
 * 测试 /dev/cvi-usb-camera0 设备是否可用
 * 使用与 ESP32 CAM 相同的 ioctl 命令号
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <errno.h>

/* ioctl 命令号 - 与 cvi_usb_camera.rs 一致 */
#define CVI_CAMERA_IOCTL_INIT      1
#define CVI_CAMERA_IOCTL_GET_INFO  2
#define CVI_CAMERA_IOCTL_GET_FRAME 3

/* CameraInfo 结构体 - 与 Rust 侧 repr(C) 一致 */
struct camera_info {
    uint16_t width;
    uint16_t height;
    uint8_t  format;    /* 1 = MJPEG */
    uint8_t  connected;
} __attribute__((packed));

/* 帧缓冲区大小 (2MB，足够存放一帧 JPEG) */
#define FRAME_BUFFER_SIZE (2 * 1024 * 1024)

/* JPEG 标记 */
#define JPEG_MARKER_START 0xFFD8
#define JPEG_MARKER_END   0xFFD9

static int check_jpeg_validity(const uint8_t *data, size_t size)
{
    if (size < 4) {
        printf("  [WARN] 帧太小 (%zu 字节)\n", size);
        return 0;
    }

    uint16_t start = (data[0] << 8) | data[1];
    uint16_t end   = (data[size - 2] << 8) | data[size - 1];

    if (start != JPEG_MARKER_START) {
        printf("  [WARN] JPEG 起始标记错误: 0x%04X (期望 0x%04X)\n",
               start, JPEG_MARKER_START);
        return 0;
    }

    if (end != JPEG_MARKER_END) {
        printf("  [WARN] JPEG 结束标记错误: 0x%04X (期望 0x%04X)\n",
               end, JPEG_MARKER_END);
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/cvi-usb-camera0";
    int save_frame = 0;
    const char *output_file = "test_frame.jpg";

    /* 简单参数解析 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0) {
            save_frame = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                output_file = argv[++i];
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [-d 设备] [-s [输出文件]]\n", argv[0]);
            printf("  -d 设备    指定设备节点 (默认: /dev/cvi-usb-camera0)\n");
            printf("  -s [文件]  保存帧到文件 (默认: test_frame.jpg)\n");
            printf("  -h         显示帮助\n");
            return 0;
        }
    }

    printf("=== StarryOS USB 摄像头测试 ===\n\n");

    /* 步骤 1: 打开设备 */
    printf("[1/4] 打开设备 %s ...\n", device);
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("  [FAIL] 打开失败: %s (errno=%d)\n", strerror(errno), errno);
        if (errno == ENOENT) {
            printf("  [HINT] 设备不存在，请检查:\n");
            printf("         - USB 摄像头是否已插入\n");
            printf("         - StarryOS 是否已启动 USB 支持\n");
            printf("         - 设备节点路径是否正确\n");
        }
        return 1;
    }
    printf("  [OK] 设备已打开 (fd=%d)\n", fd);

    /* 步骤 2: 初始化摄像头 */
    printf("\n[2/4] 初始化摄像头 (IOCTL_INIT) ...\n");
    int ret = ioctl(fd, CVI_CAMERA_IOCTL_INIT, 0);
    if (ret < 0) {
        printf("  [FAIL] 初始化失败: %s (errno=%d)\n", strerror(errno), errno);
        if (errno == EIO) {
            printf("  [HINT] 可能原因:\n");
            printf("         - USB 摄像头未正确连接\n");
            printf("         - USB 控制器初始化失败\n");
            printf("         - UVC 协商失败\n");
        }
        close(fd);
        return 1;
    }
    printf("  [OK] 初始化成功\n");

    /* 步骤 3: 获取摄像头信息 */
    printf("\n[3/4] 获取摄像头信息 (IOCTL_GET_INFO) ...\n");
    struct camera_info info;
    memset(&info, 0, sizeof(info));
    ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_INFO, &info);
    if (ret < 0) {
        printf("  [FAIL] 获取信息失败: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("  [OK] 摄像头信息:\n");
    printf("       分辨率: %ux%u\n", info.width, info.height);
    printf("       格式:   %u (%s)\n", info.format,
           info.format == 1 ? "MJPEG" : "未知");
    printf("       连接:   %u (%s)\n", info.connected,
           info.connected ? "已连接" : "未连接");

    /* 步骤 4: 抓取一帧 */
    printf("\n[4/4] 抓取一帧 (IOCTL_GET_FRAME) ...\n");
    uint8_t *frame_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!frame_buffer) {
        printf("  [FAIL] 内存分配失败\n");
        close(fd);
        return 1;
    }

    ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
    if (ret < 0) {
        printf("  [FAIL] 抓取失败: %s (errno=%d)\n", strerror(errno), errno);
        if (errno == EIO) {
            printf("  [HINT] 可能原因:\n");
            printf("         - USB 传输错误\n");
            printf("         - 摄像头数据流异常\n");
            printf("         - 帧缓冲区溢出\n");
        }
        free(frame_buffer);
        close(fd);
        return 1;
    }

    size_t frame_size = (size_t)ret;
    printf("  [OK] 抓取成功: %zu 字节\n", frame_size);

    /* 验证 JPEG 有效性 */
    printf("\n=== 帧验证 ===\n");
    if (check_jpeg_validity(frame_buffer, frame_size)) {
        printf("  [OK] JPEG 格式有效\n");
    } else {
        printf("  [WARN] JPEG 格式异常，但帧已获取\n");
    }

    /* 保存帧到文件 */
    if (save_frame) {
        printf("\n=== 保存帧 ===\n");
        FILE *fp = fopen(output_file, "wb");
        if (fp) {
            fwrite(frame_buffer, 1, frame_size, fp);
            fclose(fp);
            printf("  [OK] 帧已保存到 %s (%zu 字节)\n", output_file, frame_size);
        } else {
            printf("  [FAIL] 无法保存: %s\n", strerror(errno));
        }
    }

    /* 清理 */
    free(frame_buffer);
    close(fd);

    /* 总结 */
    printf("\n=== 测试总结 ===\n");
    printf("设备: %s\n", device);
    printf("状态: 可用\n");
    printf("分辨率: %ux%u\n", info.width, info.height);
    printf("帧大小: %zu 字节\n", frame_size);

    return 0;
}
