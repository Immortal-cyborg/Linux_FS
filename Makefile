# Makefile для модуля ядра MyFS
#
# Использование:
#   make              — собрать модуль (myfs.ko) и утилиту (myfs_tool)
#   make modules      — только модуль ядра
#   make tool         — только userspace-утилиту
#   make clean        — удалить все артефакты сборки
#
# Переменная KDIR должна указывать на каталог с заголовками ядра.
# По умолчанию используется uname -r текущей системы.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# ── Часть kbuild (модуль ядра) ──────────────────────────────────────────────
obj-m     := myfs.o
myfs-objs := myfs_module.o
ccflags-y := -Wall -Wextra

# ── Часть userspace (C++ утилита) ───────────────────────────────────────────
TOOL     := myfs_tool
TOOL_SRC := myfs_tool.cpp
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

.PHONY: all modules tool clean

all: modules tool

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

tool: $(TOOL)

$(TOOL): $(TOOL_SRC) myfs.h
	$(CXX) $(CXXFLAGS) -o $@ $(TOOL_SRC)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TOOL)
