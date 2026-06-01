# USB 摄像头测试程序 Makefile

# 交叉编译器（RISC-V）
CC_RISCV = riscv64-linux-gnu-gcc
CFLAGS_RISCV = -Wall -Wextra -O2 -static

# 本地编译器（x86 Linux）
CC_X86 = gcc
CFLAGS_X86 = -Wall -Wextra -O2

# StarryOS 测试程序
TARGET_FUNC = starrycam_test
TARGET_PERF = starrycam_perf_test

SRC_FUNC = starrycam_test.c
SRC_PERF = starrycam_perf_test.c

# Linux 测试程序（RISC-V版本，用于SG2002 Linux）
TARGET_LINUX_RV = linuxcam_perf_test_rv
SRC_LINUX = linuxcam_perf_test.c

# Linux 测试程序（x86版本，用于开发机Linux）
TARGET_LINUX_X86 = linuxcam_perf_test

.PHONY: all clean func perf linux linux-x86

all: func perf linux

func: $(TARGET_FUNC)
perf: $(TARGET_PERF)
linux: $(TARGET_LINUX_RV)
linux-x86: $(TARGET_LINUX_X86)

# StarryOS 交叉编译
$(TARGET_FUNC): $(SRC_FUNC)
	$(CC_RISCV) $(CFLAGS_RISCV) -o $@ $<

$(TARGET_PERF): $(SRC_PERF)
	$(CC_RISCV) $(CFLAGS_RISCV) -o $@ $< -lm

# Linux RISC-V 交叉编译（用于SG2002 Linux）
$(TARGET_LINUX_RV): $(SRC_LINUX)
	$(CC_RISCV) $(CFLAGS_RISCV) -o $@ $< -lm

# Linux x86 本地编译（用于开发机）
$(TARGET_LINUX_X86): $(SRC_LINUX)
	$(CC_X86) $(CFLAGS_X86) -o $@ $< -lm

clean:
	rm -f $(TARGET_FUNC) $(TARGET_PERF) $(TARGET_LINUX_RV) $(TARGET_LINUX_X86)
