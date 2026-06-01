# StarryOS USB摄像头驱动项目

SG2002 (LicheeRV Nano) 开发板上StarryOS的USB摄像头驱动实现、测试和性能对比。

## 目录结构

```
usbcam-test/
├── README.md                              # 本文件
├── usbcam_quick_start.md                  # 快速开始指南
├── sg2002-licheervnano-setup.md           # 开发板环境搭建
├── comprehensive-usbcam-reproduction-doc.md # 完整复现文档
└── test-program/                          # 测试程序
    ├── README-FINAL.md                    # 测试程序使用说明
    ├── Makefile                           # 编译脚本
    ├── starrycam_test.c                   # StarryOS功能测试
    ├── starrycam_perf_test.c              # StarryOS性能测试
    ├── linuxcam_perf_test.c               # Linux性能测试
    └── linuxcam_simple_test.c             # Linux简单诊断
```

