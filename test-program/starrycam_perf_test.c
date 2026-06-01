/*
 * StarryOS USB 摄像头性能测试程序
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

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

/* 帧缓冲区大小 */
#define FRAME_BUFFER_SIZE (2 * 1024 * 1024)

/* JPEG 标记 */
#define JPEG_MARKER_START 0xFFD8
#define JPEG_MARKER_END   0xFFD9

/* 测试配置 */
#define DEFAULT_TEST_DURATION  10  /* 默认测试时长（秒） */
#define DEFAULT_TEST_FRAMES  100   /* 默认测试帧数 */
#define WARMUP_FRAMES         10   /* 预热帧数 */

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

/* 测试1: 基本帧率测试 */
static int test_basic_fps(int fd, size_t num_frames) {
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
        ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
    }

    /* 正式测试 */
    printf("开始测试...\n");
    for (size_t i = 0; i < num_frames; i++) {
        double start = get_time_us();
        int ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
        double end = get_time_us();

        frame_times[i] = (end - start) / 1000.0;  /* 转换为ms */
        frame_sizes[i] = (ret > 0) ? ret : 0;

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
static int test_latency_distribution(int fd, size_t num_frames) {
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
        ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
    }

    /* 测试 */
    for (size_t i = 0; i < num_frames; i++) {
        double start = get_time_us();
        int ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
        double end = get_time_us();

        frame_times[i] = (end - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? ret : 0;
    }

    /* 计算百分位数 */
    /* 先排序 */
    double *sorted_times = malloc(num_frames * sizeof(double));
    memcpy(sorted_times, frame_times, num_frames * sizeof(double));

    /* 简单冒泡排序（小数据量） */
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
static int test_copy_overhead(int fd, size_t num_frames) {
    printf("\n[测试3] 缓冲区拷贝开销分析 (%zu 帧)\n", num_frames);

    uint8_t *kernel_buffer = malloc(FRAME_BUFFER_SIZE);
    uint8_t *user_buffer = malloc(FRAME_BUFFER_SIZE);
    if (!kernel_buffer || !user_buffer) {
        printf("[FAIL] 内存分配失败\n");
        free(kernel_buffer);
        free(user_buffer);
        return -1;
    }

    double *ioctl_times = malloc(num_frames * sizeof(double));
    double *copy_times = malloc(num_frames * sizeof(double));
    size_t *frame_sizes = malloc(num_frames * sizeof(size_t));

    if (!ioctl_times || !copy_times || !frame_sizes) {
        printf("[FAIL] 内存分配失败\n");
        free(kernel_buffer);
        free(user_buffer);
        free(ioctl_times);
        free(copy_times);
        free(frame_sizes);
        return -1;
    }

    /* 预热 */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, kernel_buffer);
    }

    /* 测试：分离ioctl和拷贝时间 */
    printf("测试中...\n");
    for (size_t i = 0; i < num_frames; i++) {
        /* 测量ioctl时间（包含内核到用户空间拷贝） */
        double start = get_time_us();
        int ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, kernel_buffer);
        double mid = get_time_us();

        ioctl_times[i] = (mid - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? ret : 0;

        /* 测量用户空间拷贝时间 */
        if (frame_sizes[i] > 0) {
            double copy_start = get_time_us();
            memcpy(user_buffer, kernel_buffer, frame_sizes[i]);
            double copy_end = get_time_us();
            copy_times[i] = (copy_end - copy_start) / 1000.0;
        } else {
            copy_times[i] = 0;
        }

        if (i % 10 == 0) {
            printf("  帧 %zu: ioctl=%.2f ms, copy=%.2f ms\n",
                   i, ioctl_times[i], copy_times[i]);
        }
    }

    /* 计算统计 */
    double total_ioctl = 0, total_copy = 0;
    size_t valid_count = 0;

    for (size_t i = 0; i < num_frames; i++) {
        if (frame_sizes[i] > 0) {
            total_ioctl += ioctl_times[i];
            total_copy += copy_times[i];
            valid_count++;
        }
    }

    double avg_ioctl = total_ioctl / valid_count;
    double avg_copy = total_copy / valid_count;
    double copy_ratio = avg_copy / avg_ioctl * 100.0;

    printf("\n缓冲区拷贝开销分析:\n");
    printf("  平均ioctl时间:  %.2f ms\n", avg_ioctl);
    printf("  平均拷贝时间:   %.2f ms\n", avg_copy);
    printf("  拷贝占比:       %.1f%%\n", copy_ratio);
    printf("  拷贝vsioctl:    %.2f 倍\n", avg_copy / avg_ioctl);

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

    free(kernel_buffer);
    free(user_buffer);
    free(ioctl_times);
    free(copy_times);
    free(frame_sizes);

    return 0;
}

/* 测试4: 持续帧率测试（带存储） */
static int test_sustained_fps(int fd, int duration_sec, const char *save_dir) {
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
        ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
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
        int ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
        if (ret > 0) {
            frame_count++;
            total_bytes += ret;

            /* 保存帧到文件 */
            if (save_dir && ret > 0) {
                char save_path[256];
                snprintf(save_path, sizeof(save_path), "%s/frame_%06zu.jpg",
                         save_dir, frame_count);
                FILE *fp = fopen(save_path, "wb");
                if (fp) {
                    fwrite(frame_buffer, 1, ret, fp);
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
static int test_yolo_benchmark(int fd, size_t num_frames) {
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
        ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
    }

    /* 测试 */
    printf("测试中...\n");
    for (size_t i = 0; i < num_frames; i++) {
        /* 捕获帧 */
        double start = get_time_us();
        int ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_FRAME, frame_buffer);
        double mid = get_time_us();

        capture_times[i] = (mid - start) / 1000.0;
        frame_sizes[i] = (ret > 0) ? ret : 0;

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
    printf("  -d <设备>    指定设备节点 (默认: /dev/cvi-usb-camera0)\n");
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
    const char *device = "/dev/cvi-usb-camera0";
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

    printf("=== StarryOS USB 摄像头性能测试 ===\n");
    printf("设备: %s\n", device);
    printf("帧数: %zu\n", num_frames);
    printf("时长: %d 秒\n", duration_sec);
    if (save_dir) {
        printf("保存: %s\n", save_dir);
        /* 创建保存目录 */
        mkdir(save_dir, 0755);
    }

    /* 打开设备 */
    printf("\n打开设备...\n");
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("[FAIL] 打开失败: %s (errno=%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("[OK] 设备已打开 (fd=%d)\n", fd);

    /* 初始化摄像头 */
    printf("\n初始化摄像头...\n");
    int ret = ioctl(fd, CVI_CAMERA_IOCTL_INIT, 0);
    if (ret < 0) {
        printf("[FAIL] 初始化失败: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("[OK] 初始化成功\n");

    /* 获取摄像头信息 */
    struct camera_info info;
    memset(&info, 0, sizeof(info));
    ret = ioctl(fd, CVI_CAMERA_IOCTL_GET_INFO, &info);
    if (ret < 0) {
        printf("[FAIL] 获取信息失败: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("\n摄像头信息:\n");
    printf("  分辨率: %ux%u\n", info.width, info.height);
    printf("  格式:   %u (%s)\n", info.format,
           info.format == 1 ? "MJPEG" : "未知");
    printf("  连接:   %u (%s)\n", info.connected,
           info.connected ? "已连接" : "未连接");

    /* 运行测试 */
    if (run_all || run_test[0]) {
        test_basic_fps(fd, num_frames);
    }

    if (run_all || run_test[1]) {
        test_latency_distribution(fd, num_frames);
    }

    if (run_all || run_test[2]) {
        test_copy_overhead(fd, num_frames);
    }

    if (run_all || run_test[3]) {
        test_sustained_fps(fd, duration_sec, save_dir);
    }

    if (run_all || run_test[4]) {
        test_yolo_benchmark(fd, num_frames);
    }

    /* 清理 */
    close(fd);
    printf("\n=== 测试完成 ===\n");

    return 0;
}
