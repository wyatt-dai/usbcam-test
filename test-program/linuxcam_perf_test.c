/*
 * Linux USB 摄像头性能测试程序
 *
 * 使用V4L2 API测试USB摄像头性能
 * 与starrycam_perf_test.c输出格式一致，便于对比
 *
 * 测试内容：
 * 1. 帧率测试（FPS）
 * 2. 延迟测试（单帧时间）
 * 3. 吞吐量测试（数据量）
 * 4. 缓冲区拷贝开销分析
 * 5. YOLO推理性能基准
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
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <time.h>

/* 帧缓冲区大小 */
#define FRAME_BUFFER_SIZE (2 * 1024 * 1024)

/* 测试配置 */
#define DEFAULT_TEST_DURATION  10  /* 默认测试时长（秒） */
#define DEFAULT_TEST_FRAMES  100   /* 默认测试帧数 */
#define WARMUP_FRAMES         10   /* 预热帧数 */

/* 缓冲区数量 */
#define BUFFER_COUNT 4

/* 时间工具函数 */
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

/* V4L2缓冲区结构 */
struct buffer {
    void *start;
    size_t length;
};

/* 摄像头信息结构 */
struct camera_info {
    uint32_t width;
    uint32_t height;
    uint32_t format;    /* V4L2像素格式 */
    uint32_t fps;
};

/* 性能统计结构 */
struct perf_stats {
    double total_time_ms;
    double min_frame_time_ms;
    double max_frame_time_ms;
    double avg_frame_time_ms;
    double fps;
    double throughput_mbps;
    size_t total_bytes;
    size_t valid_frames;
    size_t invalid_frames;
    size_t total_frames;
};

/* 计算性能统计 */
static void calculate_stats(struct perf_stats *stats, double *frame_times,
                           size_t *frame_sizes, size_t count) {
    double sum_time = 0;
    double min_time = 1e9;
    double max_time = 0;
    size_t total_bytes = 0;
    size_t valid_frames = 0;

    for (size_t i = 0; i < count; i++) {
        sum_time += frame_times[i];
        if (frame_times[i] < min_time) min_time = frame_times[i];
        if (frame_times[i] > max_time) max_time = frame_times[i];
        total_bytes += frame_sizes[i];
        if (frame_sizes[i] > 0) valid_frames++;
    }

    stats->total_time_ms = sum_time;
    stats->min_frame_time_ms = min_time;
    stats->max_frame_time_ms = max_time;
    stats->avg_frame_time_ms = sum_time / count;
    stats->fps = 1000.0 / stats->avg_frame_time_ms;
    stats->throughput_mbps = (total_bytes * 8.0) / (sum_time / 1000.0) / 1000000.0;
    stats->total_bytes = total_bytes;
    stats->valid_frames = valid_frames;
    stats->invalid_frames = count - valid_frames;
    stats->total_frames = count;
}

/* 打印性能统计 */
static void print_stats(const char *test_name, const struct perf_stats *stats) {
    printf("\n=== %s 性能统计 ===\n", test_name);
    printf("总帧数: %zu\n", stats->total_frames);
    printf("有效帧: %zu (%.1f%%)\n", stats->valid_frames,
           100.0 * stats->valid_frames / stats->total_frames);
    printf("无效帧: %zu\n", stats->invalid_frames);
    printf("总时间: %.2f ms\n", stats->total_time_ms);
    printf("帧率:   %.2f FPS\n", stats->fps);
    printf("吞吐量: %.2f Mbps\n", stats->throughput_mbps);
    printf("帧时间: 最小=%.2f ms, 最大=%.2f ms, 平均=%.2f ms\n",
           stats->min_frame_time_ms, stats->max_frame_time_ms,
           stats->avg_frame_time_ms);
    printf("总数据: %zu 字节 (%.2f MB)\n", stats->total_bytes,
           stats->total_bytes / 1024.0 / 1024.0);
}

/* 打印摄像头信息 */
static void print_camera_info(const struct camera_info *info) {
    printf("\n摄像头信息:\n");
    printf("  分辨率: %ux%u\n", info->width, info->height);
    printf("  格式:   0x%08X", info->format);

    switch (info->format) {
        case V4L2_PIX_FMT_MJPEG:
            printf(" (MJPEG)");
            break;
        case V4L2_PIX_FMT_YUYV:
            printf(" (YUYV)");
            break;
        case V4L2_PIX_FMT_NV12:
            printf(" (NV12)");
            break;
        default:
            printf(" (未知)");
            break;
    }
    printf("\n");

    printf("  帧率:   %u FPS\n", info->fps);
}

/* 查询摄像头支持的帧率 */
static void enumerate_frame_intervals(int fd, uint32_t pixel_format,
                                      uint32_t width, uint32_t height) {
    struct v4l2_frmivalenum frmival;

    printf("  支持的帧率:\n");

    memset(&frmival, 0, sizeof(frmival));
    frmival.pixel_format = pixel_format;
    frmival.width = width;
    frmival.height = height;

    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            uint32_t fps = frmival.discrete.denominator / frmival.discrete.numerator;
            printf("    %u FPS (1/%u)\n", fps, frmival.discrete.numerator);
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            printf("    %u-%u FPS (连续)\n",
                   frmival.stepwise.min.denominator / frmival.stepwise.min.numerator,
                   frmival.stepwise.max.denominator / frmival.stepwise.max.numerator);
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            printf("    %u-%u FPS (步进)\n",
                   frmival.stepwise.min.denominator / frmival.stepwise.min.numerator,
                   frmival.stepwise.max.denominator / frmival.stepwise.max.numerator);
        }
        frmival.index++;
    }

    if (frmival.index == 0) {
        printf("    (无法枚举帧率)\n");
    }
}

/* 查询并设置最佳帧率 */
static uint32_t query_and_set_framerate(int fd, uint32_t pixel_format,
                                        uint32_t width, uint32_t height) {
    struct v4l2_frmivalenum frmival;
    uint32_t best_fps = 0;

    /* 枚举支持的帧率 */
    memset(&frmival, 0, sizeof(frmival));
    frmival.pixel_format = pixel_format;
    frmival.width = width;
    frmival.height = height;

    /* 找到最大帧率 */
    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            uint32_t fps = frmival.discrete.denominator / frmival.discrete.numerator;
            if (fps > best_fps) {
                best_fps = fps;
            }
        }
        frmival.index++;
    }

    /* 如果没有找到，使用默认值 */
    if (best_fps == 0) {
        printf("[WARN] 无法查询帧率，使用默认值\n");
        best_fps = 30;
    }

    /* 设置帧率 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = best_fps;

    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        printf("[WARN] 设置帧率 %u FPS 失败\n", best_fps);
    } else {
        printf("[OK] 帧率已设置为 %u FPS\n", best_fps);
    }

    /* 获取实际帧率 */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
        uint32_t actual_fps = parm.parm.capture.timeperframe.denominator /
                              parm.parm.capture.timeperframe.numerator;
        if (actual_fps != best_fps) {
            printf("[INFO] 实际帧率: %u FPS (请求 %u FPS)\n", actual_fps, best_fps);
            best_fps = actual_fps;
        }
    }

    return best_fps;
}

/* 初始化V4L2设备 */
static int init_v4l2_device(const char *device, struct camera_info *info,
                            struct buffer **buffers, int *buffer_count) {
    int fd;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    /* 打开设备 */
    fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("[FAIL] 打开设备失败: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    /* 查询设备能力 */
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        printf("[FAIL] 查询设备能力失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("设备信息:\n");
    printf("  驱动: %s\n", cap.driver);
    printf("  卡:   %s\n", cap.card);
    printf("  总线: %s\n", cap.bus_info);

    /* 检查是否支持视频捕获 */
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        printf("[FAIL] 设备不支持视频捕获\n");
        close(fd);
        return -1;
    }

    /* 设置格式 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1280;
    fmt.fmt.pix.height = 720;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[WARN] 设置MJPEG格式失败，尝试YUYV...\n");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            printf("[FAIL] 设置格式失败: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    /* 获取实际格式 */
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("[FAIL] 获取格式失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    info->width = fmt.fmt.pix.width;
    info->height = fmt.fmt.pix.height;
    info->format = fmt.fmt.pix.pixelformat;

    printf("  分辨率: %ux%u\n", info->width, info->height);
    printf("  格式: 0x%08X\n", info->format);

    /* 查询支持的帧率 */
    enumerate_frame_intervals(fd, info->format, info->width, info->height);

    /* 查询并设置最佳帧率 */
    info->fps = query_and_set_framerate(fd, info->format, info->width, info->height);

    /* 请求缓冲区 */
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("[FAIL] 请求缓冲区失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (req.count < 2) {
        printf("[FAIL] 缓冲区数量不足\n");
        close(fd);
        return -1;
    }

    printf("[OK] 缓冲区数量: %u\n", req.count);

    /* 分配缓冲区 */
    *buffers = calloc(req.count, sizeof(struct buffer));
    if (!*buffers) {
        printf("[FAIL] 内存分配失败\n");
        close(fd);
        return -1;
    }

    *buffer_count = req.count;

    /* 映射缓冲区 */
    for (int i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            printf("[FAIL] 查询缓冲区失败: %s\n", strerror(errno));
            free(*buffers);
            close(fd);
            return -1;
        }

        (*buffers)[i].length = buf.length;
        (*buffers)[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, buf.m.offset);

        if ((*buffers)[i].start == MAP_FAILED) {
            printf("[FAIL] 映射缓冲区失败: %s\n", strerror(errno));
            for (int j = 0; j < i; j++) {
                munmap((*buffers)[j].start, (*buffers)[j].length);
            }
            free(*buffers);
            close(fd);
            return -1;
        }

        printf("[OK] 缓冲区%d: 长度=%zu\n", i, (*buffers)[i].length);
    }

    return fd;
}

/* 开始捕获 */
static int start_capture(int fd, int buffer_count) {
    enum v4l2_buf_type type;

    /* 入队所有缓冲区 */
    for (int i = 0; i < buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            printf("[FAIL] 入队缓冲区失败: %s\n", strerror(errno));
            return -1;
        }
    }

    /* 开始捕获 */
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[FAIL] 开始捕获失败: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* 停止捕获 */
static void stop_capture(int fd) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
}

/* 捕获一帧（带超时） */
static int capture_frame(int fd, struct buffer *buffers, void *frame_buffer,
                         size_t *frame_size) {
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;
    int r;

    /* 使用select等待数据可用 */
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    tv.tv_sec = 2;  /* 2秒超时 */
    tv.tv_usec = 0;

    r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r < 0) {
        if (errno == EINTR) {
            return 0;  /* 被信号中断 */
        }
        printf("[FAIL] select失败: %s\n", strerror(errno));
        return -1;
    }
    if (r == 0) {
        printf("[WARN] 等待帧超时\n");
        return 0;  /* 超时 */
    }

    /* 出队缓冲区 */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            return 0;  /* 没有可用帧 */
        }
        printf("[FAIL] 出队缓冲区失败: %s\n", strerror(errno));
        return -1;
    }

    /* 复制帧数据 */
    if (buf.bytesused > 0) {
        memcpy(frame_buffer, buffers[buf.index].start, buf.bytesused);
        *frame_size = buf.bytesused;
    } else {
        *frame_size = 0;
        printf("[WARN] 帧大小为0\n");
    }

    /* 重新入队缓冲区 */
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        printf("[FAIL] 重新入队缓冲区失败: %s\n", strerror(errno));
        return -1;
    }

    return 1;
}

/* 清理资源 */
static void cleanup(int fd, struct buffer *buffers, int buffer_count) {
    if (buffers) {
        for (int i = 0; i < buffer_count; i++) {
            if (buffers[i].start && buffers[i].start != MAP_FAILED) {
                munmap(buffers[i].start, buffers[i].length);
            }
        }
        free(buffers);
    }

    if (fd >= 0) {
        close(fd);
    }
}

/* 测试1: 基本帧率测试 */
static int test_basic_fps(int fd, struct buffer *buffers, size_t num_frames) {
    printf("\n[测试1] 基本帧率测试 (%zu 帧)\n", num_frames);

    uint8_t *frame_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!frame_buffer) {
        printf("[FAIL] 内存分配失败\n");
        return -1;
    }

    double *frame_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));
    if (!frame_times || !frame_sizes) {
        printf("[FAIL] 内存分配失败\n");
        free(frame_buffer);
        free(frame_times);
        free(frame_sizes);
        return -1;
    }

    /* 预热 */
    printf("预热中...\n");
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        size_t size;
        capture_frame(fd, buffers, frame_buffer, &size);
    }

    /* 正式测试 */
    printf("开始测试...\n");
    for (size_t i = 0; i < num_frames; i++) {
        size_t size;
        double start = get_time_us();
        int ret = capture_frame(fd, buffers, frame_buffer, &size);
        double end = get_time_us();

        frame_times[i] = (end - start) / 1000.0;  /* 转换为ms */
        frame_sizes[i] = (ret > 0) ? size : 0;

        if (i % 10 == 0) {
            printf("  帧 %zu: %.2f ms, %zu 字节\n", i, frame_times[i], frame_sizes[i]);
        }
    }

    /* 计算统计 */
    struct perf_stats stats;
    calculate_stats(&stats, frame_times, frame_sizes, num_frames);
    print_stats("基本帧率", &stats);

    free(frame_buffer);
    free(frame_times);
    free(frame_sizes);

    return 0;
}

/* 测试2: 延迟分布测试 */
static int test_latency_distribution(int fd, struct buffer *buffers, size_t num_frames) {
    printf("\n[测试2] 延迟分布测试 (%zu 帧)\n", num_frames);

    uint8_t *frame_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!frame_buffer) {
        printf("[FAIL] 内存分配失败\n");
        return -1;
    }

    double *frame_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));
    if (!frame_times || !frame_sizes) {
        printf("[FAIL] 内存分配失败\n");
        free(frame_buffer);
        free(frame_times);
        free(frame_sizes);
        return -1;
    }

    /* 预热 */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        size_t size;
        capture_frame(fd, buffers, frame_buffer, &size);
    }

    /* 测试 */
    for (size_t i = 0; i < num_frames; i++) {
        size_t size;
        double start = get_time_us();
        int ret = capture_frame(fd, buffers, frame_buffer, &size);
        double end = get_time_us();

        frame_times[i] = (end - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? size : 0;
    }

    /* 计算百分位数 */
    double *sorted_times = malloc(num_frames * sizeof(double));
    memcpy(sorted_times, frame_times, num_frames * sizeof(double));

    /* 简单冒泡排序 */
    for (size_t i = 0; i < num_frames - 1; i++) {
        for (size_t j = 0; j < num_frames - i - 1; j++) {
            if (sorted_times[j] > sorted_times[j + 1]) {
                double temp = sorted_times[j];
                sorted_times[j] = sorted_times[j + 1];
                sorted_times[j + 1] = temp;
            }
        }
    }

    printf("\n延迟分布:\n");
    printf("  P50 (中位数): %.2f ms\n", sorted_times[num_frames / 2]);
    printf("  P90:          %.2f ms\n", sorted_times[(size_t)(num_frames * 0.9)]);
    printf("  P95:          %.2f ms\n", sorted_times[(size_t)(num_frames * 0.95)]);
    printf("  P99:          %.2f ms\n", sorted_times[(size_t)(num_frames * 0.99)]);
    printf("  最小:         %.2f ms\n", sorted_times[0]);
    printf("  最大:         %.2f ms\n", sorted_times[num_frames - 1]);

    free(sorted_times);
    free(frame_buffer);
    free(frame_times);
    free(frame_sizes);

    return 0;
}

/* 测试3: 缓冲区拷贝开销分析 */
static int test_copy_overhead(int fd, struct buffer *buffers, size_t num_frames) {
    printf("\n[测试3] 缓冲区拷贝开销分析 (%zu 帧)\n", num_frames);

    uint8_t *v4l2_buffer = malloc(FRAME_BUFFER_SIZE);
    uint8_t *user_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!v4l2_buffer || !user_buffer) {
        printf("[FAIL] 内存分配失败\n");
        free(v4l2_buffer);
        free(user_buffer);
        return -1;
    }

    double *capture_times = malloc(num_frames * sizeof(double));
    double *copy_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));

    if (!capture_times || !copy_times || !frame_sizes) {
        printf("[FAIL] 内存分配失败\n");
        free(v4l2_buffer);
        free(user_buffer);
        free(capture_times);
        free(copy_times);
        free(frame_sizes);
        return -1;
    }

    /* 预热 */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        size_t size;
        capture_frame(fd, buffers, v4l2_buffer, &size);
    }

    /* 测试：分离捕获和拷贝时间 */
    printf("测试中...\n");
    for (size_t i = 0; i < num_frames; i++) {
        size_t size;

        /* 测量捕获时间（包含V4L2到用户空间拷贝） */
        double start = get_time_us();
        int ret = capture_frame(fd, buffers, v4l2_buffer, &size);
        double mid = get_time_us();

        capture_times[i] = (mid - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? size : 0;

        /* 测量用户空间拷贝时间 */
        if (frame_sizes[i] > 0) {
            double copy_start = get_time_us();
            memcpy(user_buffer, v4l2_buffer, frame_sizes[i]);
            double copy_end = get_time_us();
            copy_times[i] = (copy_end - copy_start) / 1000.0;
        } else {
            copy_times[i] = 0;
        }

        if (i % 10 == 0) {
            printf("  帧 %zu: capture=%.2f ms, copy=%.2f ms\n",
                   i, capture_times[i], copy_times[i]);
        }
    }

    /* 计算统计 */
    double total_capture = 0, total_copy = 0;
    size_t valid_count = 0;

    for (size_t i = 0; i < num_frames; i++) {
        if (frame_sizes[i] > 0) {
            total_capture += capture_times[i];
            total_copy += copy_times[i];
            valid_count++;
        }
    }

    double avg_capture = total_capture / valid_count;
    double avg_copy = total_copy / valid_count;
    double copy_ratio = avg_copy / avg_capture * 100.0;

    printf("\n缓冲区拷贝开销分析:\n");
    printf("  平均捕获时间:  %.2f ms\n", avg_capture);
    printf("  平均拷贝时间:   %.2f ms\n", avg_copy);
    printf("  拷贝占比:       %.1f%%\n", copy_ratio);
    printf("  拷贝vs捕获:    %.2f 倍\n", avg_copy / avg_capture);

    printf("\n结论:\n");
    if (copy_ratio > 50) {
        printf("  [!] 缓冲区拷贝是主要瓶颈 (%.1f%%)\n", copy_ratio);
        printf("      建议使用mmap零拷贝优化\n");
    } else if (copy_ratio > 20) {
        printf("  [!] 缓冲区拷贝是显著开销 (%.1f%%)\n", copy_ratio);
        printf("      可以考虑mmap优化\n");
    } else {
        printf("  [OK] 缓冲区拷贝开销较小 (%.1f%%)\n", copy_ratio);
        printf("      优化收益有限\n");
    }

    free(v4l2_buffer);
    free(user_buffer);
    free(capture_times);
    free(copy_times);
    free(frame_sizes);

    return 0;
}

/* 测试4: 持续帧率测试（带存储） */
static int test_sustained_fps(int fd, struct buffer *buffers, int duration_sec,
                              const char *save_dir) {
    printf("\n[测试4] 持续帧率测试 (%d 秒)\n", duration_sec);
    if (save_dir) {
        printf("保存目录: %s\n", save_dir);
    }

    uint8_t *frame_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!frame_buffer) {
        printf("[FAIL] 内存分配失败\n");
        return -1;
    }

    /* 预热 */
    printf("预热中...\n");
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        size_t size;
        capture_frame(fd, buffers, frame_buffer, &size);
    }

    /* 测试 */
    printf("开始持续测试...\n");
    size_t frame_count = 0;
    size_t total_bytes = 0;
    size_t saved_count = 0;
    double start_time = get_time_ms();
    double last_report = start_time;
    double report_interval = 1000.0;  /* 每秒报告一次 */

    while (1) {
        double current_time = get_time_ms();
        if (current_time - start_time >= duration_sec * 1000.0) {
            break;
        }

        /* 每秒报告 */
        if (current_time - last_report >= report_interval) {
            double elapsed = (current_time - start_time) / 1000.0;
            double fps = frame_count / elapsed;
            double throughput = (total_bytes * 8.0) / elapsed / 1000000.0;
            printf("  %.1f秒: %zu帧, %.2f FPS, %.2f Mbps, 已保存%zu帧\n",
                   elapsed, frame_count, fps, throughput, saved_count);
            last_report = current_time;
        }

        /* 捕获帧 */
        size_t size;
        int ret = capture_frame(fd, buffers, frame_buffer, &size);
        if (ret > 0) {
            frame_count++;
            total_bytes += size;

            /* 保存帧到文件 */
            if (save_dir && size > 0) {
                char save_path[256];
                snprintf(save_path, sizeof(save_path), "%s/frame_%06zu.jpg",
                         save_dir, frame_count);
                FILE *fp = fopen(save_path, "wb");
                if (fp) {
                    fwrite(frame_buffer, 1, size, fp);
                    fclose(fp);
                    saved_count++;
                }
            }
        }
    }

    double end_time = get_time_ms();
    double total_time = (end_time - start_time) / 1000.0;

    printf("\n持续帧率测试结果:\n");
    printf("  测试时长: %.2f 秒\n", total_time);
    printf("  总帧数:   %zu\n", frame_count);
    printf("  平均帧率: %.2f FPS\n", frame_count / total_time);
    printf("  总数据量: %zu 字节 (%.2f MB)\n", total_bytes,
           total_bytes / 1024.0 / 1024.0);
    printf("  吞吐量:   %.2f Mbps\n", (total_bytes * 8.0) / total_time / 1000000.0);
    if (save_dir) {
        printf("  已保存:   %zu 帧\n", saved_count);
    }

    free(frame_buffer);
    return 0;
}

/* 测试5: YOLO推理性能基准 */
static int test_yolo_benchmark(int fd, struct buffer *buffers, size_t num_frames) {
    printf("\n[测试5] YOLO推理性能基准 (%zu 帧)\n", num_frames);
    printf("注意: 此测试模拟YOLO处理，实际性能取决于具体实现\n");

    uint8_t *frame_buffer = malloc(FRAME_BUFFER_SIZE);
    uint8_t *process_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!frame_buffer || !process_buffer) {
        printf("[FAIL] 内存分配失败\n");
        free(frame_buffer);
        free(process_buffer);
        return -1;
    }

    double *capture_times = malloc(num_frames * sizeof(double));
    double *process_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));

    if (!capture_times || !process_times || !frame_sizes) {
        printf("[FAIL] 内存分配失败\n");
        free(frame_buffer);
        free(process_buffer);
        free(capture_times);
        free(process_times);
        free(frame_sizes);
        return -1;
    }

    /* 预热 */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        size_t size;
        capture_frame(fd, buffers, frame_buffer, &size);
    }

    /* 测试 */
    printf("测试中...\n");
    for (size_t i = 0; i < num_frames; i++) {
        size_t size;

        /* 捕获帧 */
        double start = get_time_us();
        int ret = capture_frame(fd, buffers, frame_buffer, &size);
        double mid = get_time_us();

        capture_times[i] = (mid - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? size : 0;

        /* 模拟YOLO处理（简单内存操作） */
        if (frame_sizes[i] > 0) {
            double process_start = get_time_us();

            /* 模拟JPEG解码（内存拷贝） */
            memcpy(process_buffer, frame_buffer, frame_sizes[i]);

            /* 模拟预处理（简单计算） */
            for (size_t j = 0; j < frame_sizes[i]; j += 100) {
                process_buffer[j] = process_buffer[j] / 2;
            }

            double process_end = get_time_us();
            process_times[i] = (process_end - process_start) / 1000.0;
        } else {
            process_times[i] = 0;
        }

        if (i % 10 == 0) {
            printf("  帧 %zu: capture=%.2f ms, process=%.2f ms\n",
                   i, capture_times[i], process_times[i]);
        }
    }

    /* 计算统计 */
    double total_capture = 0, total_process = 0;
    size_t valid_count = 0;

    for (size_t i = 0; i < num_frames; i++) {
        if (frame_sizes[i] > 0) {
            total_capture += capture_times[i];
            total_process += process_times[i];
            valid_count++;
        }
    }

    double avg_capture = total_capture / valid_count;
    double avg_process = total_process / valid_count;
    double total_time = avg_capture + avg_process;
    double fps = 1000.0 / total_time;

    printf("\nYOLO推理性能基准:\n");
    printf("  平均捕获时间: %.2f ms\n", avg_capture);
    printf("  平均处理时间: %.2f ms\n", avg_process);
    printf("  平均总时间:   %.2f ms\n", total_time);
    printf("  预期帧率:     %.2f FPS\n", fps);
    printf("  捕获占比:     %.1f%%\n", avg_capture / total_time * 100.0);
    printf("  处理占比:     %.1f%%\n", avg_process / total_time * 100.0);

    printf("\nYOLO优化建议:\n");
    if (avg_capture > avg_process * 2) {
        printf("  [!] 捕获是主要瓶颈，建议优化摄像头驱动\n");
        printf("      - 使用mmap零拷贝\n");
        printf("      - 降低分辨率\n");
        printf("      - 使用硬件解码\n");
    } else if (avg_process > avg_capture * 2) {
        printf("  [!] 处理是主要瓶颈，建议优化YOLO模型\n");
        printf("      - 使用更轻量的模型\n");
        printf("      - 使用GPU/NPU加速\n");
        printf("      - 优化预处理\n");
    } else {
        printf("  [OK] 捕获和处理时间相近\n");
        printf("      - 可以并行处理\n");
        printf("      - 使用流水线优化\n");
    }

    free(frame_buffer);
    free(process_buffer);
    free(capture_times);
    free(process_times);
    free(frame_sizes);

    return 0;
}

/* 打印帮助信息 */
static void print_help(const char *prog_name) {
    printf("用法: %s [选项]\n", prog_name);
    printf("\n选项:\n");
    printf("  -d <设备>    指定设备节点 (默认: /dev/video0)\n");
    printf("  -n <帧数>    测试帧数 (默认: 100)\n");
    printf("  -t <秒数>    持续测试时长 (默认: 10)\n");
    printf("  -s <目录>    保存帧到目录 (用于持续测试)\n");
    printf("  -a           运行所有测试\n");
    printf("  -1           只运行基本帧率测试\n");
    printf("  -2           只运行延迟分布测试\n");
    printf("  -3           只运行缓冲区拷贝开销分析\n");
    printf("  -4           只运行持续帧率测试\n");
    printf("  -5           只运行YOLO推理性能基准\n");
    printf("  -h           显示帮助\n");
    printf("\n示例:\n");
    printf("  %s -a                    # 运行所有测试\n", prog_name);
    printf("  %s -1 -n 200             # 基本帧率测试，200帧\n", prog_name);
    printf("  %s -3 -n 50              # 缓冲区拷贝分析，50帧\n", prog_name);
    printf("  %s -4 -t 30              # 持续帧率测试，30秒\n", prog_name);
    printf("  %s -4 -t 10 -s /tmp/frames  # 持续测试并保存帧\n", prog_name);
    printf("  %s -5 -n 100             # YOLO性能基准，100帧\n", prog_name);
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/video0";
    size_t num_frames = DEFAULT_TEST_FRAMES;
    int duration_sec = DEFAULT_TEST_DURATION;
    const char *save_dir = NULL;  /* 保存目录 */
    int run_all = 0;
    int run_test[5] = {0, 0, 0, 0, 0};

    /* 参数解析 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            save_dir = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            run_all = 1;
        } else if (strcmp(argv[i], "-1") == 0) {
            run_test[0] = 1;
        } else if (strcmp(argv[i], "-2") == 0) {
            run_test[1] = 1;
        } else if (strcmp(argv[i], "-3") == 0) {
            run_test[2] = 1;
        } else if (strcmp(argv[i], "-4") == 0) {
            run_test[3] = 1;
        } else if (strcmp(argv[i], "-5") == 0) {
            run_test[4] = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            printf("未知选项: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    /* 如果没有指定测试，运行所有 */
    if (!run_all && !run_test[0] && !run_test[1] && !run_test[2] &&
        !run_test[3] && !run_test[4]) {
        run_all = 1;
    }

    printf("=== Linux USB 摄像头性能测试 ===\n");
    printf("设备: %s\n", device);
    printf("帧数: %zu\n", num_frames);
    printf("时长: %d 秒\n", duration_sec);
    if (save_dir) {
        printf("保存: %s\n", save_dir);
        /* 创建保存目录 */
        mkdir(save_dir, 0755);
    }

    /* 初始化设备 */
    struct camera_info info;
    struct buffer *buffers = NULL;
    int buffer_count = 0;

    int fd = init_v4l2_device(device, &info, &buffers, &buffer_count);
    if (fd < 0) {
        return 1;
    }

    print_camera_info(&info);

    /* 开始捕获 */
    if (start_capture(fd, buffer_count) < 0) {
        cleanup(fd, buffers, buffer_count);
        return 1;
    }

    /* 运行测试 */
    if (run_all || run_test[0]) {
        test_basic_fps(fd, buffers, num_frames);
    }

    if (run_all || run_test[1]) {
        test_latency_distribution(fd, buffers, num_frames);
    }

    if (run_all || run_test[2]) {
        test_copy_overhead(fd, buffers, num_frames);
    }

    if (run_all || run_test[3]) {
        test_sustained_fps(fd, buffers, duration_sec, save_dir);
    }

    if (run_all || run_test[4]) {
        test_yolo_benchmark(fd, buffers, num_frames);
    }

    /* 停止捕获并清理 */
    stop_capture(fd);
    cleanup(fd, buffers, buffer_count);

    printf("\n=== 测试完成 ===\n");

    return 0;
}
