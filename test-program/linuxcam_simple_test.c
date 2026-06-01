/*
 * Linux V4L2 性能测试程序
 * 与 starrycam_perf_test.c 对标，公平比较 Linux vs StarryOS 帧率
 *
 * 测试内容：
 * 1. 帧率测试（FPS）
 * 2. 延迟测试（单帧时间 + 百分位分布）
 * 3. 持续帧率测试
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <time.h>

#define BUFFER_COUNT     4
#define DEFAULT_FRAMES   100
#define DEFAULT_DURATION 10
#define WARMUP_FRAMES    10

struct buffer {
    void *start;
    size_t length;
};

/* 时间工具函数 - 与 StarryOS 测试一致 */
static inline double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

/* 取一帧（阻塞 DQBUF + QBUF），返回帧大小，失败返回 0 */
static int grab_frame(int fd, struct v4l2_buffer *buf) {
    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_DQBUF, buf) < 0)
        return 0;

    int size = buf->bytesused;

    if (ioctl(fd, VIDIOC_QBUF, buf) < 0)
        return 0;

    return size;
}

/* 打印统计 */
static void print_stats(const char *name, double *times, size_t *sizes,
                         size_t count) {
    double sum = 0, min_t = 1e9, max_t = 0;
    size_t total_bytes = 0;

    for (size_t i = 0; i < count; i++) {
        sum += times[i];
        if (times[i] < min_t) min_t = times[i];
        if (times[i] > max_t) max_t = times[i];
        total_bytes += sizes[i];
    }

    double avg = sum / count;
    double fps = 1000.0 / avg;

    printf("\n=== %s ===\n", name);
    printf("总帧数: %zu\n", count);
    printf("总时间: %.2f ms\n", sum);
    printf("帧率:   %.2f FPS\n", fps);
    printf("帧时间: 最小=%.2f ms, 最大=%.2f ms, 平均=%.2f ms\n",
           min_t, max_t, avg);
    printf("总数据: %zu 字节 (%.2f MB)\n", total_bytes,
           total_bytes / 1024.0 / 1024.0);
    printf("吞吐量: %.2f Mbps\n",
           (total_bytes * 8.0) / (sum / 1000.0) / 1000000.0);
}

/* 打印延迟百分位 */
static void print_latency_percentiles(double *times, size_t count) {
    double *sorted = malloc(count * sizeof(double));
    memcpy(sorted, times, count * sizeof(double));

    /* 冒泡排序（小数据量够用） */
    for (size_t i = 0; i < count - 1; i++)
        for (size_t j = 0; j < count - i - 1; j++)
            if (sorted[j] > sorted[j + 1]) {
                double tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }

    printf("\n延迟分布:\n");
    printf("  P50 (中位数): %.2f ms\n", sorted[count / 2]);
    printf("  P90:          %.2f ms\n", sorted[(size_t)(count * 0.9)]);
    printf("  P95:          %.2f ms\n", sorted[(size_t)(count * 0.95)]);
    printf("  P99:          %.2f ms\n", sorted[(size_t)(count * 0.99)]);
    printf("  最小:         %.2f ms\n", sorted[0]);
    printf("  最大:         %.2f ms\n", sorted[count - 1]);

    free(sorted);
}

/* 测试1: 基本帧率测试 */
static int test_basic_fps(int fd, size_t num_frames) {
    printf("\n[测试1] 基本帧率测试 (%zu 帧)\n", num_frames);

    double *frame_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));
    if (!frame_times || !frame_sizes) return -1;

    /* 预热 */
    printf("预热中 (%d 帧)...\n", WARMUP_FRAMES);
    struct v4l2_buffer buf;
    for (int i = 0; i < WARMUP_FRAMES; i++)
        grab_frame(fd, &buf);

    /* 正式测试 */
    printf("开始测试...\n");
    for (size_t i = 0; i < num_frames; i++) {
        double start = get_time_us();
        int size = grab_frame(fd, &buf);
        double end = get_time_us();

        frame_times[i] = (end - start) / 1000.0;
        frame_sizes[i] = size;

        if (i % 10 == 0)
            printf("  帧 %zu: %.2f ms, %zu 字节\n", i, frame_times[i], frame_sizes[i]);
    }

    print_stats("基本帧率", frame_times, frame_sizes, num_frames);
    print_latency_percentiles(frame_times, num_frames);

    free(frame_times);
    free(frame_sizes);
    return 0;
}

/* 测试2: 持续帧率测试 */
static int test_sustained_fps(int fd, int duration_sec) {
    printf("\n[测试2] 持续帧率测试 (%d 秒)\n", duration_sec);

    /* 预热 */
    printf("预热中...\n");
    struct v4l2_buffer buf;
    for (int i = 0; i < WARMUP_FRAMES; i++)
        grab_frame(fd, &buf);

    printf("开始持续测试...\n");
    size_t frame_count = 0;
    size_t total_bytes = 0;
    double start_time = get_time_ms();
    double last_report = start_time;

    while (1) {
        double now = get_time_ms();
        if (now - start_time >= duration_sec * 1000.0) break;

        if (now - last_report >= 1000.0) {
            double elapsed = (now - start_time) / 1000.0;
            printf("  %.1f秒: %zu帧, %.2f FPS, %.2f Mbps\n",
                   elapsed, frame_count, frame_count / elapsed,
                   (total_bytes * 8.0) / elapsed / 1000000.0);
            last_report = now;
        }

        int size = grab_frame(fd, &buf);
        if (size > 0) {
            frame_count++;
            total_bytes += size;
        }
    }

    double total_time = (get_time_ms() - start_time) / 1000.0;
    printf("\n持续帧率测试结果:\n");
    printf("  测试时长: %.2f 秒\n", total_time);
    printf("  总帧数:   %zu\n", frame_count);
    printf("  平均帧率: %.2f FPS\n", frame_count / total_time);
    printf("  总数据量: %zu 字节 (%.2f MB)\n", total_bytes,
           total_bytes / 1024.0 / 1024.0);
    printf("  吞吐量:   %.2f Mbps\n",
           (total_bytes * 8.0) / total_time / 1000000.0);

    return 0;
}

static void print_help(const char *prog) {
    printf("用法: %s [选项]\n", prog);
    printf("\n选项:\n");
    printf("  -d <设备>    指定设备节点 (默认: /dev/video0)\n");
    printf("  -n <帧数>    测试帧数 (默认: %d)\n", DEFAULT_FRAMES);
    printf("  -t <秒数>    持续测试时长 (默认: %d)\n", DEFAULT_DURATION);
    printf("  -a           运行所有测试\n");
    printf("  -1           只运行基本帧率测试\n");
    printf("  -2           只运行持续帧率测试\n");
    printf("  -h           显示帮助\n");
}

/* V4L2 初始化 */
static int v4l2_init(const char *device, struct buffer *buffers,
                     int *buffer_count) {
    /* 1. 打开设备（阻塞模式） */
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("[FAIL] 打开失败: %s\n", strerror(errno));
        return -1;
    }
    printf("[OK] 设备已打开 (fd=%d)\n", fd);

    /* 2. 查询设备能力 */
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        printf("[FAIL] 查询失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("[OK] 驱动: %s, 卡: %s, 总线: %s\n", cap.driver, cap.card, cap.bus_info);

    /* 3. 设置格式 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1280;
    fmt.fmt.pix.height = 720;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[WARN] 设置MJPEG失败，尝试YUYV...\n");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            printf("[FAIL] 设置格式失败: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("[FAIL] 获取格式失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("[OK] 分辨率: %ux%u, 格式: 0x%08X, 大小: %u\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage);

    /* 4. 请求缓冲区 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        printf("[FAIL] 缓冲区请求失败\n");
        close(fd);
        return -1;
    }
    *buffer_count = req.count;

    /* 5. 映射缓冲区 */
    for (int i = 0; i < *buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            printf("[FAIL] 查询缓冲区%d失败\n", i);
            close(fd);
            return -1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            printf("[FAIL] 映射缓冲区%d失败\n", i);
            close(fd);
            return -1;
        }
    }

    /* 6. 入队所有缓冲区 */
    for (int i = 0; i < *buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            printf("[FAIL] 入队缓冲区%d失败\n", i);
            close(fd);
            return -1;
        }
    }

    /* 7. 开始捕获 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[FAIL] 开始捕获失败\n");
        close(fd);
        return -1;
    }
    printf("[OK] 捕获已开始\n");

    return fd;
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/video0";
    size_t num_frames = DEFAULT_FRAMES;
    int duration_sec = DEFAULT_DURATION;
    int run_all = 0, run_test1 = 0, run_test2 = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            num_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0) run_all = 1;
        else if (strcmp(argv[i], "-1") == 0) run_test1 = 1;
        else if (strcmp(argv[i], "-2") == 0) run_test2 = 1;
        else if (strcmp(argv[i], "-h") == 0) { print_help(argv[0]); return 0; }
        else { printf("未知选项: %s\n", argv[i]); print_help(argv[0]); return 1; }
    }

    if (!run_all && !run_test1 && !run_test2) run_all = 1;

    printf("=== Linux V4L2 性能测试 ===\n");
    printf("设备: %s, 帧数: %zu, 时长: %d 秒\n", device, num_frames, duration_sec);

    struct buffer buffers[BUFFER_COUNT];
    int buffer_count = 0;
    int fd = v4l2_init(device, buffers, &buffer_count);
    if (fd < 0) return 1;

    if (run_all || run_test1)
        test_basic_fps(fd, num_frames);

    if (run_all || run_test2)
        test_sustained_fps(fd, duration_sec);

    /* 清理 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < buffer_count; i++)
        if (buffers[i].start && buffers[i].start != MAP_FAILED)
            munmap(buffers[i].start, buffers[i].length);
    close(fd);

    printf("\n=== 测试完成 ===\n");
    return 0;
}
