# MyFS — Учебная файловая система на базе блочного устройства

## Структура проекта

```
myfs/
├── myfs.h                  # Общие определения (ядро + userspace)
├── kernel/
│   ├── myfs_module.c       # Модуль ядра
│   └── Makefile
├── userspace/
│   ├── myfs_tool.cpp       # Тестовая программа (C++17)
│   └── Makefile
├── deploy.sh               # Скрипт для загрузки / монтирования / тестирования
└── README.md
```

---

## Архитектура диска

```
Сектор  sb_offset_1  │ Суперблок (копия 1) + XOR-checksum
Сектор  sb_offset_2  │ Суперблок (копия 2) + XOR-checksum
─────────────────────┼────────────────────────────────────
Сектор  data_start+0 │ file00000, сектор 0
Сектор  data_start+1 │ file00000, сектор 1
...                  │ ...
Сектор  data_start+M │ file00001, сектор 0
...
```

- `data_start = max(sb_offset_1, sb_offset_2) + 1`
- `num_files  = (total_sectors − data_start) / file_sectors`

---

## Параметры модуля

| Параметр        | Тип   | По умолчанию | Описание                               |
|-----------------|-------|--------------|----------------------------------------|
| `dev_name`      | charp | `sdb`        | Блочное устройство (`/dev/<dev_name>`) |
| `sb_offset_1`   | int   | `0`          | Сектор первой копии суперблока         |
| `sb_offset_2`   | int   | `1`          | Сектор второй копии суперблока         |
| `max_name_len`  | int   | `64`         | Максимальная длина имени файла         |
| `file_sectors`  | int   | `8`          | Число секторов на файл (M)             |

---

## Целостность суперблока

Поле `checksum` в `struct myfs_super_block` — это XOR всех 32-битных слов
структуры, **кроме самого поля `checksum`**.

При монтировании:
1. Читается первичная копия (сектор `sb_offset_1`).
2. Проверяются `magic == MYFS_MAGIC` и `checksum`.
3. Если проверка не прошла — читается резервная копия (`sb_offset_2`).
4. Если обе повреждены — устройство форматируется заново.

---

## IOCTL-команды (`/dev/myfs_ctl`)

| Команда                  | Описание                                               |
|--------------------------|--------------------------------------------------------|
| `MYFS_IOC_ZERO_ALL`      | Обнулить содержимое всех файлов                        |
| `MYFS_IOC_ERASE_FS`      | Стереть ФС (обнулить оба суперблока)                   |
| `MYFS_IOC_GET_META`      | Вернуть массив `myfs_file_info` со CRC32 каждого файла |
| `MYFS_IOC_GET_SECTOR_MAP`| Вернуть `myfs_sector_map` для заданного файла          |

---

## Быстрый старт

### 1. Сборка модуля ядра

```bash
# Убедитесь, что установлены заголовки ядра:
sudo apt install linux-headers-$(uname -r)

cd myfs/kernel
make
# Результат: myfs.ko
```

### 2. Сборка userspace-утилиты

```bash
cd myfs/userspace
make
# Результат: myfs_tool
```

### 3. Подготовка виртуального блочного устройства (для тестов)

```bash
# Создаём образ 10 МБ
dd if=/dev/zero of=/tmp/myfs.img bs=1M count=10

# Прикрепляем как loop-устройство
sudo losetup /dev/loop0 /tmp/myfs.img
```

### 4. Загрузка модуля

```bash
# Устройство loop0, суперблок на секторах 0 и 1, файлы по 4 сектора
sudo insmod kernel/myfs.ko dev_name=loop0 sb_offset_1=0 sb_offset_2=1 \
     max_name_len=32 file_sectors=4
```

### 5. Монтирование

```bash
sudo mount -t myfs none /mnt
ls /mnt         # должны появиться файлы вида file00000, file00001, …
```

### 6. Тестирование

```bash
# Записывает случайное число в каждый файл и читает обратно
sudo ./userspace/myfs_tool --test /mnt

# IOCTL-команды
sudo ./userspace/myfs_tool --get-meta /mnt
sudo ./userspace/myfs_tool --sector-map 3
sudo ./userspace/myfs_tool --zero-all
sudo ./userspace/myfs_tool --erase-fs
```

### 7. Размонтирование и выгрузка

```bash
sudo umount /mnt
sudo rmmod myfs
sudo losetup -d /dev/loop0
```

### Использование deploy.sh (всё вместе)

```bash
chmod +x deploy.sh

# Полный цикл на loop0 с параметрами по умолчанию:
sudo ./deploy.sh full loop0

# Или пошагово:
sudo ./deploy.sh load  loop0 0 1 32 4
sudo ./deploy.sh mount /mnt
sudo ./deploy.sh test  /mnt
sudo ./deploy.sh status
sudo ./deploy.sh umount /mnt
sudo ./deploy.sh unload
```

---

## Ограничения (намеренные, по заданию)

- Нет поддержки создания/удаления/переименования файлов.
- Нет прав доступа / расширенных атрибутов.
- Нет журналирования.
- Все файлы создаются при форматировании и не изменяют размер.

---

## Версия ядра

Проверено на Linux **6.12.x**. Использует API:
- `blkdev_get_by_path` / `blkdev_put` (6.5+)
- `bdev_nr_sectors` (6.0+)
- `mount_nodev` / `kill_anon_super`
- `misc_register` для IOCTL-устройства
- `crc32()` из `<linux/crc32.h>`
