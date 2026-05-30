# MyFS — Учебная файловая система на базе блочного устройства

## Структура проекта

```
Linux_FS/
├── myfs.h                  # Общие определения (ядро + userspace)
├── myfs_module.c           # Модуль ядра
├── myfs_tool.cpp           # Тестовая программа (C++17)
├── Makefile                # Собирает И модуль, И утилиту
├── deploy.sh               # Скрипт загрузки / монтирования / тестирования
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
| `myfs_dev`      | charp | `sdb`        | Блочное устройство (`/dev/<myfs_dev>`) |
| `sb_offset_1`   | int   | `0`          | Сектор первой копии суперблока         |
| `sb_offset_2`   | int   | `1`          | Сектор второй копии суперблока         |
| `max_name_len`  | int   | `64`         | Максимальная длина имени файла         |
| `file_sectors`  | int   | `8`          | Число секторов на файл (M)             |
| `format`        | int   | `0`          | `1` — отформатировать устройство при монтировании |

---

## Целостность суперблока

Поле `checksum` в `struct myfs_super_block` — это XOR всех 32-битных слов
структуры, **кроме самого поля `checksum`**.

При монтировании:
1. Читается первичная копия (сектор `sb_offset_1`).
2. Проверяются `magic == MYFS_MAGIC` и `checksum`.
3. Если первичная повреждена, но резервная (`sb_offset_2`) исправна —
   используется резервная, а первичная **восстанавливается** из неё.
4. Если **обе** копии повреждены — монтирование завершается ошибкой
   (`-EINVAL`), устройство **НЕ форматируется** (данные не теряются).
   Создать ФС заново можно явно: загрузить модуль с `format=1`.

Так после `erase` (обнуление обоих суперблоков) обычный повторный
`mount` корректно падает, а не переформатирует диск.

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

### 1. Сборка модуля и утилиты

```bash
# Убедитесь, что установлены заголовки ядра:
sudo apt install linux-headers-$(uname -r)

make # собирает И myfs.ko, И myfs_tool
#   make modules     # только myfs.ko
#   make tool        # только myfs_tool
```

### 2. Подготовка виртуального блочного устройства (для тестов)

```bash
# Создаём образ 10 МБ
dd if=/dev/zero of=/tmp/myfs.img bs=1M count=10

# Прикрепляем как loop-устройство
sudo losetup /dev/loop0 /tmp/myfs.img
```

### 3. Загрузка модуля

```bash
# Устройство loop0, суперблок на секторах 0 и 1, файлы по 4 сектора.
# format=1 — создать ФС заново (нужно при первом использовании устройства).
sudo insmod myfs.ko myfs_dev=loop0 sb_offset_1=0 sb_offset_2=1 \
     max_name_len=32 file_sectors=4 format=1
```

### 4. Монтирование

```bash
sudo mount -t myfs none /mnt
ls /mnt         # должны появиться файлы вида file00000, file00001, …
```

### 5. Тестирование

```bash
# Записывает случайное число в каждый файл и читает обратно
sudo ./myfs_tool test /mnt

# IOCTL-команды
sudo ./myfs_tool metadata /mnt
sudo ./myfs_tool sectormap file00003
sudo ./myfs_tool zero
sudo ./myfs_tool erase
```

### 6. Размонтирование и выгрузка

```bash
sudo umount /mnt
sudo rmmod myfs
sudo losetup -d /dev/loop0
```

### Использование deploy.sh (всё вместе)

```bash
chmod +x deploy.sh

# Полный цикл на loop0 с параметрами по умолчанию (load выполняет format=1):
sudo ./deploy.sh full loop0

# Или пошагово (format=1 в 7-м аргументе load — создать ФС):
sudo ./deploy.sh load  loop0 0 1 32 4 1
sudo ./deploy.sh mount /mnt
sudo ./deploy.sh test  /mnt
sudo ./deploy.sh status
sudo ./deploy.sh umount /mnt
sudo ./deploy.sh unload

# Проверка сценария erase (mount после erase обязан упасть):
sudo ./deploy.sh erase-test /mnt
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
- `bdev_file_open_by_path` / `file_bdev` / `fput` (6.9+)
- `bio_alloc` / `bio_add_page` / `submit_bio_wait` — весь I/O через bio
- `bdev_logical_block_size` — автоопределение размера сектора
- `bdev_nr_bytes`
- `mount_nodev` / `kill_anon_super`
- `misc_register` для IOCTL-устройства
- `crc32()` из `<linux/crc32.h>`
