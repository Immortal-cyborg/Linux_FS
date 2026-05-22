# Makefile для модуля ядра MyFS
#
# Использование:
#   make              — собрать модуль
#   make clean        — удалить артефакты сборки
#
# Переменная KDIR должна указывать на каталог с заголовками ядра.
# По умолчанию используется uname -r текущей системы.

KDIR ?= /lib/modules/$(shell uname -r)/build

# Имя объектного файла модуля (без расширения)
obj-m := myfs.o

# Исходные файлы, составляющие модуль
myfs-objs := myfs_module.o

# Дополнительные флаги компилятора:
#   -DDEBUG — включает pr_debug() сообщения
#   -Wall   — предупреждения
ccflags-y := -Wall -Wextra

# Копируем общий заголовок в директорию сборки модуля
# (Makefile запускается из KDIR, поэтому нужен абсолютный путь)
EXTRA_CFLAGS += -I$(shell pwd)/..

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules

clean:
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean
