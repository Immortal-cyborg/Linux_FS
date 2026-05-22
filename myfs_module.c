// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_module.c — модуль ядра, реализующий учебную файловую систему MyFS.
 *
 * Архитектура:
 *  ┌─────────────────────────────────────────────────────┐
 *  │  Сектор sb_offset_1  → Суперблок (копия 1)          │
 *  │  Сектор sb_offset_2  → Суперблок (копия 2)          │
 *  │  Секторы data_start … → данные файлов               │
 *  │    file0: [data_start .. data_start+file_sectors-1] │
 *  │    file1: [data_start+file_sectors .. ]             │
 *  │    …                                                │
 *  └─────────────────────────────────────────────────────┘
 *
 * Параметры модуля (insmod myfs.ko dev_name=sdb ...):
 *   dev_name       — имя блочного устройства (/dev/<dev_name>)
 *   sb_offset_1    — сектор первой копии суперблока  (default 0)
 *   sb_offset_2    — сектор второй копии суперблока  (default 1)
 *   max_name_len   — максимальная длина имени файла   (default 64)
 *   file_sectors   — секторов на файл (M)             (default 8)
 *
 * Ядро: 6.12.x
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/crc32.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#include "myfs.h"

/* ──────────────────────────────────────────────
 * Параметры модуля
 * ────────────────────────────────────────────── */
static char *dev_name    = "sdb";
static int   sb_offset_1 = MYFS_SB_OFFSET_1;
static int   sb_offset_2 = MYFS_SB_OFFSET_2;
static int   max_name_len= MYFS_MAX_NAME_LEN;
static int   file_sectors= MYFS_MAX_FILE_SECTS;

module_param(dev_name,     charp, 0444);
module_param(sb_offset_1,  int,   0444);
module_param(sb_offset_2,  int,   0444);
module_param(max_name_len, int,   0444);
module_param(file_sectors, int,   0444);

MODULE_PARM_DESC(dev_name,    "Block device name (e.g. sdb)");
MODULE_PARM_DESC(sb_offset_1, "Sector offset for superblock copy 1");
MODULE_PARM_DESC(sb_offset_2, "Sector offset for superblock copy 2");
MODULE_PARM_DESC(max_name_len,"Max filename length");
MODULE_PARM_DESC(file_sectors,"File size in sectors (M)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Simple educational block-based filesystem (MyFS)");
MODULE_VERSION("1.0");

/* ──────────────────────────────────────────────
 * Внутреннее состояние ФС (in-memory)
 * ────────────────────────────────────────────── */

/* Информация о смонтированной ФС, хранящаяся в s_fs_info суперблока VFS */
struct myfs_fs_info {
    struct myfs_super_block  sb_disk;       /* копия суперблока с диска     */
    struct block_device     *bdev;          /* открытое блочное устройство  */
    struct myfs_file_info   *files;         /* массив метаданных файлов     */
    struct mutex             lock;          /* защита от гонок при I/O      */
    unsigned int             num_files;     /* количество файлов            */
};

/* ──────────────────────────────────────────────
 * Вспомогательные функции для работы с диском
 * ────────────────────────────────────────────── */

/*
 * myfs_calc_checksum — подсчёт XOR-хеша суперблока.
 * Хеш считается по всем 32-битным словам структуры, кроме самого поля checksum.
 */
static __u32 myfs_calc_checksum(const struct myfs_super_block *sb)
{
    const __u32 *p   = (const __u32 *)sb;
    /* checksum — последнее поле; не включаем его в расчёт */
    size_t nwords    = (sizeof(*sb) - sizeof(__u32)) / sizeof(__u32);
    __u32  result    = 0;
    size_t i;

    for (i = 0; i < nwords; i++)
        result ^= p[i];

    return result;
}

/*
 * myfs_read_sector — читает один сектор (512 байт) с блочного устройства
 * в выделенный буфер dst.  Использует submit_bio/buffer_head API.
 */
static int myfs_read_sector(struct block_device *bdev, sector_t sector,
                             void *dst)
{
    struct buffer_head *bh;

    /* getblk выделяет buffer_head для данного сектора */
    bh = __bread(bdev, sector, MYFS_SECTOR_SIZE);
    if (!bh) {
        pr_err("myfs: failed to read sector %llu\n",
               (unsigned long long)sector);
        return -EIO;
    }

    memcpy(dst, bh->b_data, MYFS_SECTOR_SIZE);
    brelse(bh);
    return 0;
}

/*
 * myfs_write_sector — записывает один сектор на блочное устройство.
 */
static int myfs_write_sector(struct block_device *bdev, sector_t sector,
                              const void *src)
{
    struct buffer_head *bh;

    bh = __bread(bdev, sector, MYFS_SECTOR_SIZE);
    if (!bh) {
        pr_err("myfs: failed to get buffer for sector %llu\n",
               (unsigned long long)sector);
        return -EIO;
    }

    memcpy(bh->b_data, src, MYFS_SECTOR_SIZE);
    mark_buffer_dirty(bh);   /* помечаем как "грязный", чтобы ядро сбросило */
    sync_dirty_buffer(bh);   /* синхронный сброс на диск                   */
    brelse(bh);
    return 0;
}

/*
 * myfs_read_file_data — читает все секторы одного файла в буфер data.
 * Размер буфера должен быть не менее file_sectors * MYFS_SECTOR_SIZE.
 */
static int myfs_read_file_data(struct myfs_fs_info *fsi, int file_idx,
                               void *data)
{
    struct myfs_file_info *fi = &fsi->files[file_idx];
    sector_t sec;
    int i, ret;

    for (i = 0; i < (int)fi->num_sectors; i++) {
        sec = fi->start_sector + i;
        ret = myfs_read_sector(fsi->bdev, sec,
                               (char *)data + i * MYFS_SECTOR_SIZE);
        if (ret)
            return ret;
    }
    return 0;
}

/*
 * myfs_write_file_data — записывает данные (size байт) в секторы файла.
 * Если size меньше полного размера файла — остаток заполняется нулями.
 */
static int myfs_write_file_data(struct myfs_fs_info *fsi, int file_idx,
                                const void *data, size_t size)
{
    struct myfs_file_info *fi = &fsi->files[file_idx];
    char   sector_buf[MYFS_SECTOR_SIZE];
    sector_t sec;
    int i, ret;
    size_t file_size = (size_t)fi->num_sectors * MYFS_SECTOR_SIZE;
    size_t written   = 0;

    for (i = 0; i < (int)fi->num_sectors; i++) {
        sec = fi->start_sector + i;
        memset(sector_buf, 0, MYFS_SECTOR_SIZE);

        if (written < size) {
            size_t chunk = min((size_t)MYFS_SECTOR_SIZE, size - written);
            memcpy(sector_buf, (const char *)data + written, chunk);
            written += chunk;
        }

        ret = myfs_write_sector(fsi->bdev, sec, sector_buf);
        if (ret)
            return ret;
    }
    (void)file_size;
    return 0;
}

/* ──────────────────────────────────────────────
 * Инициализация / чтение суперблока
 * ────────────────────────────────────────────── */

/*
 * myfs_write_superblock — записывает суперблок sb на диск в обе копии.
 */
static int myfs_write_superblock(struct block_device *bdev,
                                 struct myfs_super_block *sb)
{
    int ret;

    /* Вычисляем контрольную сумму перед записью */
    sb->checksum = myfs_calc_checksum(sb);

    ret = myfs_write_sector(bdev, sb->sb_offset_1, sb);
    if (ret) return ret;

    ret = myfs_write_sector(bdev, sb->sb_offset_2, sb);
    return ret;
}

/*
 * myfs_read_and_verify_sb — читает суперблок из сектора sector,
 * проверяет magic и checksum.  Возвращает 0 при успехе.
 */
static int myfs_read_and_verify_sb(struct block_device *bdev,
                                   sector_t sector,
                                   struct myfs_super_block *sb)
{
    __u32 expected;
    int ret;

    ret = myfs_read_sector(bdev, sector, sb);
    if (ret) return ret;

    if (sb->magic != MYFS_MAGIC) {
        pr_warn("myfs: bad magic 0x%08X at sector %llu\n",
                sb->magic, (unsigned long long)sector);
        return -EINVAL;
    }

    expected = myfs_calc_checksum(sb);
    if (sb->checksum != expected) {
        pr_warn("myfs: checksum mismatch at sector %llu: "
                "got 0x%08X, expected 0x%08X\n",
                (unsigned long long)sector, sb->checksum, expected);
        return -EINVAL;
    }

    return 0;
}

/*
 * myfs_format — записывает новый суперблок и инициализирует данные файлов.
 * Вызывается при монтировании, если существующий суперблок не найден.
 */
static int myfs_format(struct block_device *bdev,
                       __u32 sb1, __u32 sb2,
                       __u32 mname, __u32 fsects,
                       struct myfs_super_block *sb_out)
{
    struct myfs_super_block sb;
    char   zero_sector[MYFS_SECTOR_SIZE];
    loff_t dev_size;
    __u32  total_sectors, data_start, num_files;
    __u32  i;
    int    ret;

    /* Определяем размер устройства */
    dev_size     = bdev_nr_sectors(bdev) * MYFS_SECTOR_SIZE;
    total_sectors= (u32)(bdev_nr_sectors(bdev));

    /*
     * data_start — первый сектор после обоих суперблоков.
     * Берём максимум из двух смещений и добавляем 1.
     */
    data_start = max(sb1, sb2) + 1;

    if (data_start >= total_sectors) {
        pr_err("myfs: device too small for superblocks\n");
        return -EINVAL;
    }

    /* Каждый файл занимает fsects секторов */
    num_files = (total_sectors - data_start) / fsects;
    if (num_files == 0) {
        pr_err("myfs: device too small for any files\n");
        return -EINVAL;
    }

    /* Заполняем структуру суперблока */
    memset(&sb, 0, sizeof(sb));
    sb.magic        = MYFS_MAGIC;
    sb.version      = 1;
    sb.sector_size  = MYFS_SECTOR_SIZE;
    sb.total_sectors= total_sectors;
    sb.sb_offset_1  = sb1;
    sb.sb_offset_2  = sb2;
    sb.max_name_len = mname;
    sb.file_sectors = fsects;
    sb.data_start   = data_start;
    sb.num_files    = num_files;

    /* Обнуляем все секторы данных */
    memset(zero_sector, 0, MYFS_SECTOR_SIZE);
    for (i = 0; i < num_files * fsects; i++) {
        ret = myfs_write_sector(bdev, data_start + i, zero_sector);
        if (ret) return ret;
    }

    /* Записываем суперблок (обе копии) */
    ret = myfs_write_superblock(bdev, &sb);
    if (ret) return ret;

    memcpy(sb_out, &sb, sizeof(sb));
    pr_info("myfs: formatted device: %u files, %u sectors each, "
            "data_start=%u\n", num_files, fsects, data_start);
    return 0;
}

/* ──────────────────────────────────────────────
 * VFS: операции над файлами
 * ────────────────────────────────────────────── */

/*
 * myfs_file_read_iter — реализация read() для файлов MyFS.
 * Читает данные с диска и копирует в пространство пользователя.
 */
static ssize_t myfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct inode      *inode = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi = inode->i_sb->s_fs_info;
    int                file_idx = (int)(inode->i_ino - 1); /* ino начинается с 1 */
    size_t             file_size;
    loff_t             pos = iocb->ki_pos;
    size_t             count, to_copy;
    char              *buf;
    int                ret;

    if (file_idx < 0 || file_idx >= (int)fsi->num_files)
        return -EINVAL;

    file_size = (size_t)fsi->files[file_idx].num_sectors * MYFS_SECTOR_SIZE;

    if (pos >= (loff_t)file_size)
        return 0; /* EOF */

    count = iov_iter_count(to);
    to_copy = min(count, (size_t)(file_size - pos));

    /* Читаем весь файл с диска в промежуточный буфер */
    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);
    ret = myfs_read_file_data(fsi, file_idx, buf);
    mutex_unlock(&fsi->lock);

    if (ret) {
        kfree(buf);
        return ret;
    }

    /* Копируем нужный кусок в userspace */
    if (copy_to_iter(buf + pos, to_copy, to) != to_copy) {
        kfree(buf);
        return -EFAULT;
    }

    iocb->ki_pos += to_copy;
    kfree(buf);
    return (ssize_t)to_copy;
}

/*
 * myfs_file_write_iter — реализация write() для файлов MyFS.
 * Читает текущее содержимое файла, вносит изменения, записывает обратно.
 */
static ssize_t myfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode      *inode = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi = inode->i_sb->s_fs_info;
    int                file_idx = (int)(inode->i_ino - 1);
    size_t             file_size;
    loff_t             pos = iocb->ki_pos;
    size_t             count, to_write;
    char              *buf;
    int                ret;

    if (file_idx < 0 || file_idx >= (int)fsi->num_files)
        return -EINVAL;

    file_size = (size_t)fsi->files[file_idx].num_sectors * MYFS_SECTOR_SIZE;

    if (pos >= (loff_t)file_size)
        return -ENOSPC;

    count    = iov_iter_count(from);
    to_write = min(count, (size_t)(file_size - pos));

    /* Читаем текущее состояние файла */
    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);

    ret = myfs_read_file_data(fsi, file_idx, buf);
    if (ret) goto out;

    /* Копируем данные из userspace поверх нужного смещения */
    if (copy_from_iter(buf + pos, to_write, from) != to_write) {
        ret = -EFAULT;
        goto out;
    }

    /* Записываем изменённые данные обратно на диск */
    ret = myfs_write_file_data(fsi, file_idx, buf, file_size);
    if (!ret) {
        iocb->ki_pos += to_write;
        /* Обновляем CRC32 в метаданных файла */
        fsi->files[file_idx].crc32 = crc32(0, buf, file_size);
    }

out:
    mutex_unlock(&fsi->lock);
    kfree(buf);
    return ret ? ret : (ssize_t)to_write;
}

/* Таблица файловых операций для обычных файлов MyFS */
static const struct file_operations myfs_file_ops = {
    .owner      = THIS_MODULE,
    .read_iter  = myfs_file_read_iter,
    .write_iter = myfs_file_write_iter,
    .llseek     = generic_file_llseek,  /* стандартный seek */
};

/* ──────────────────────────────────────────────
 * VFS: операции над inode
 * ────────────────────────────────────────────── */

/* Таблица inode-операций для обычных файлов (read/write only, без rename и т.д.) */
static const struct inode_operations myfs_file_inode_ops = {
    /* Намеренно пустая: не поддерживаем create/link/rename/... */
};

/* Создаёт и инициализирует inode для файла с индексом file_idx */
static struct inode *myfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    int file_idx)
{
    struct myfs_fs_info  *fsi  = sb->s_fs_info;
    struct myfs_file_info *fi  = &fsi->files[file_idx];
    struct inode          *inode;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    /* inode number начинается с 1 (0 зарезервирован) */
    inode->i_ino   = (unsigned long)(file_idx + 1);
    inode->i_mode  = S_IFREG | 0666;   /* обычный файл, rw-rw-rw- */
    inode->i_uid   = GLOBAL_ROOT_UID;
    inode->i_gid   = GLOBAL_ROOT_GID;
    inode->i_size  = (loff_t)fi->num_sectors * MYFS_SECTOR_SIZE;
    inode->i_blocks= fi->num_sectors;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

    inode->i_op    = &myfs_file_inode_ops;
    inode->i_fop   = &myfs_file_ops;

    return inode;
}

/* ──────────────────────────────────────────────
 * VFS: операции над директорией (root)
 * ────────────────────────────────────────────── */

/*
 * myfs_readdir — заполняет буфер записями каталога для ls.
 * Корневой каталог содержит все файлы ФС плюс "." и "..".
 */
static int myfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct super_block  *sb  = file->f_inode->i_sb;
    struct myfs_fs_info *fsi = sb->s_fs_info;
    unsigned int i;

    /* Стандартные записи "." и ".." */
    if (!dir_emit_dots(file, ctx))
        return 0;

    /* Перечисляем все файлы ФС */
    for (i = 0; i < fsi->num_files; i++) {
        struct myfs_file_info *fi = &fsi->files[i];

        /* ctx->pos используется как курсор итерации */
        if (ctx->pos > (loff_t)(i + 2))
            continue;

        if (!dir_emit(ctx,
                      fi->name,
                      strlen(fi->name),
                      (ino_t)(i + 1),   /* inode number */
                      DT_REG))          /* тип: regular file */
            return 0;

        ctx->pos++;
    }

    return 0;
}

/*
 * myfs_lookup — ищет файл по имени в корневом каталоге.
 * Вызывается ядром при разрешении пути вида /mnt/file00001.
 */
static struct dentry *myfs_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
    struct super_block  *sb  = dir->i_sb;
    struct myfs_fs_info *fsi = sb->s_fs_info;
    unsigned int i;

    for (i = 0; i < fsi->num_files; i++) {
        struct myfs_file_info *fi = &fsi->files[i];

        if (strcmp(fi->name, dentry->d_name.name) == 0) {
            struct inode *inode = myfs_get_inode(sb, dir, (int)i);
            if (!inode)
                return ERR_PTR(-ENOMEM);
            /* Связываем dentry с найденным inode */
            return d_splice_alias(inode, dentry);
        }
    }

    /* Файл не найден — возвращаем "negative dentry" */
    d_add(dentry, NULL);
    return NULL;
}

static const struct file_operations myfs_dir_ops = {
    .owner   = THIS_MODULE,
    .iterate_shared = myfs_readdir,
    .llseek  = generic_file_llseek,
};

static const struct inode_operations myfs_dir_inode_ops = {
    .lookup  = myfs_lookup,
    /* create/mkdir/rmdir/rename — не реализованы намеренно */
};

/* ──────────────────────────────────────────────
 * IOCTL через /proc/fs/myfs/ctl (miscdevice)
 * ────────────────────────────────────────────── */

/* Глобальный указатель на текущую смонтированную ФС */
static struct myfs_fs_info *g_fsi = NULL;

static long myfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct myfs_fs_info *fsi = g_fsi;
    char  *zero_buf;
    __u32  i;
    int    ret = 0;

    if (!fsi)
        return -ENODEV;

    switch (cmd) {

    /* ── Обнулить все файлы ── */
    case MYFS_IOC_ZERO_ALL:
        zero_buf = kzalloc((size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE,
                           GFP_KERNEL);
        if (!zero_buf)
            return -ENOMEM;

        mutex_lock(&fsi->lock);
        for (i = 0; i < fsi->num_files; i++) {
            ret = myfs_write_file_data(fsi, (int)i, zero_buf,
                       (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE);
            if (ret) break;
            /* CRC32 пустого буфера */
            fsi->files[i].crc32 = crc32(0, zero_buf,
                (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE);
        }
        mutex_unlock(&fsi->lock);
        kfree(zero_buf);
        pr_info("myfs: IOCTL ZERO_ALL done\n");
        break;

    /* ── Стереть ФС (обнулить суперблоки) ── */
    case MYFS_IOC_ERASE_FS: {
        char sector[MYFS_SECTOR_SIZE];
        memset(sector, 0, sizeof(sector));

        mutex_lock(&fsi->lock);
        myfs_write_sector(fsi->bdev, fsi->sb_disk.sb_offset_1, sector);
        myfs_write_sector(fsi->bdev, fsi->sb_disk.sb_offset_2, sector);
        mutex_unlock(&fsi->lock);
        pr_info("myfs: IOCTL ERASE_FS done\n");
        break;
    }

    /* ── Получить метаинформацию (хеши) всех файлов ── */
    case MYFS_IOC_GET_META: {
        struct myfs_file_info __user *uinfo =
            (struct myfs_file_info __user *)arg;
        char *buf = kmalloc(
            (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE, GFP_KERNEL);
        if (!buf) return -ENOMEM;

        mutex_lock(&fsi->lock);
        for (i = 0; i < fsi->num_files; i++) {
            /* Обновляем CRC32 перед отдачей */
            ret = myfs_read_file_data(fsi, (int)i, buf);
            if (!ret)
                fsi->files[i].crc32 = crc32(0, buf,
                    (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE);

            if (copy_to_user(&uinfo[i], &fsi->files[i],
                             sizeof(struct myfs_file_info))) {
                ret = -EFAULT;
                break;
            }
        }
        mutex_unlock(&fsi->lock);
        kfree(buf);
        pr_info("myfs: IOCTL GET_META done\n");
        break;
    }

    /* ── Получить маппинг секторов для файла ── */
    case MYFS_IOC_GET_SECTOR_MAP: {
        struct myfs_sector_map map;

        if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
            return -EFAULT;

        if (map.file_index >= fsi->num_files)
            return -EINVAL;

        map.start_sector = fsi->files[map.file_index].start_sector;
        map.num_sectors  = fsi->files[map.file_index].num_sectors;

        if (copy_to_user((void __user *)arg, &map, sizeof(map)))
            return -EFAULT;

        pr_info("myfs: IOCTL GET_SECTOR_MAP file=%u -> start=%u, nsect=%u\n",
                map.file_index, map.start_sector, map.num_sectors);
        break;
    }

    default:
        ret = -ENOTTY;
    }

    return ret;
}

static const struct file_operations myfs_ctl_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = myfs_ioctl,
};

/* /dev/myfs_ctl — контрольный символьный псевдоустройство для IOCTL */
static struct miscdevice myfs_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "myfs_ctl",
    .fops  = &myfs_ctl_fops,
};

/* ──────────────────────────────────────────────
 * VFS: суперблок
 * ────────────────────────────────────────────── */

/*
 * myfs_put_super — вызывается при размонтировании.
 * Освобождаем все ресурсы.
 */
static void myfs_put_super(struct super_block *sb)
{
    struct myfs_fs_info *fsi = sb->s_fs_info;

    if (fsi) {
        kfree(fsi->files);
        kfree(fsi);
        sb->s_fs_info = NULL;
    }

    g_fsi = NULL;
    pr_info("myfs: unmounted\n");
}

static const struct super_operations myfs_super_ops = {
    .put_super   = myfs_put_super,
    .statfs      = simple_statfs,   /* базовая реализация из libfs */
    .drop_inode  = generic_drop_inode,
};

/*
 * myfs_fill_super — основная функция инициализации суперблока VFS.
 * Вызывается ядром при mount(2).
 */
static int myfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct myfs_fs_info  *fsi;
    struct myfs_super_block *disk_sb;
    struct inode         *root_inode;
    struct dentry        *root_dentry;
    struct block_device  *bdev;
    char                  dev_path[64];
    int                   ret;
    unsigned int          i;
    bool                  need_format = false;

    /* ── Открываем блочное устройство ── */
    snprintf(dev_path, sizeof(dev_path), "/dev/%s", dev_name);
    bdev = blkdev_get_by_path(dev_path, BLK_OPEN_READ | BLK_OPEN_WRITE,
                              NULL, NULL);
    if (IS_ERR(bdev)) {
        pr_err("myfs: cannot open device %s: %ld\n",
               dev_path, PTR_ERR(bdev));
        return PTR_ERR(bdev);
    }

    /* ── Выделяем память под внутреннее состояние ── */
    fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    if (!fsi) {
        ret = -ENOMEM;
        goto err_bdev;
    }

    fsi->bdev = bdev;
    mutex_init(&fsi->lock);
    disk_sb   = &fsi->sb_disk;

    /* ── Пробуем прочитать существующий суперблок ── */
    ret = myfs_read_and_verify_sb(bdev, (sector_t)sb_offset_1, disk_sb);
    if (ret) {
        /* Первая копия повреждена — пробуем вторую */
        pr_warn("myfs: primary superblock invalid, trying backup\n");
        ret = myfs_read_and_verify_sb(bdev, (sector_t)sb_offset_2, disk_sb);
        if (ret) {
            /* Обе копии отсутствуют — форматируем устройство */
            pr_info("myfs: no valid superblock found, formatting...\n");
            need_format = true;
        }
    }

    if (need_format) {
        ret = myfs_format(bdev,
                          (__u32)sb_offset_1, (__u32)sb_offset_2,
                          (__u32)max_name_len, (__u32)file_sectors,
                          disk_sb);
        if (ret) goto err_fsi;
    }

    fsi->num_files = disk_sb->num_files;

    /* ── Строим массив метаданных файлов ── */
    fsi->files = kcalloc(fsi->num_files, sizeof(struct myfs_file_info),
                         GFP_KERNEL);
    if (!fsi->files) {
        ret = -ENOMEM;
        goto err_fsi;
    }

    for (i = 0; i < fsi->num_files; i++) {
        fsi->files[i].index        = i;
        fsi->files[i].start_sector = disk_sb->data_start +
                                     i * disk_sb->file_sectors;
        fsi->files[i].num_sectors  = disk_sb->file_sectors;
        /* Имя файла: "fileNNNNN" */
        snprintf(fsi->files[i].name, disk_sb->max_name_len,
                 "file%05u", i);
        fsi->files[i].crc32        = 0; /* будет обновлено при чтении */
    }

    /* ── Настраиваем VFS суперблок ── */
    sb->s_magic    = MYFS_MAGIC;
    sb->s_op       = &myfs_super_ops;
    sb->s_fs_info  = fsi;
    sb->s_blocksize      = MYFS_SECTOR_SIZE;
    sb->s_blocksize_bits = blksize_bits(MYFS_SECTOR_SIZE);
    sb->s_maxbytes = (loff_t)disk_sb->file_sectors * MYFS_SECTOR_SIZE;

    /* ── Создаём корневой inode (директория) ── */
    root_inode = new_inode(sb);
    if (!root_inode) {
        ret = -ENOMEM;
        goto err_files;
    }

    root_inode->i_ino   = 0;              /* inode 0 — корень              */
    root_inode->i_mode  = S_IFDIR | 0755; /* директория, rwxr-xr-x        */
    root_inode->i_uid   = GLOBAL_ROOT_UID;
    root_inode->i_gid   = GLOBAL_ROOT_GID;
    root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime =
                          current_time(root_inode);
    root_inode->i_op    = &myfs_dir_inode_ops;
    root_inode->i_fop   = &myfs_dir_ops;
    set_nlink(root_inode, 2); /* "." и ".." */

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        ret = -ENOMEM;
        goto err_files;
    }

    sb->s_root = root_dentry;
    g_fsi = fsi; /* сохраняем для IOCTL */

    pr_info("myfs: mounted %s, %u files × %u sectors\n",
            dev_path, fsi->num_files, disk_sb->file_sectors);
    return 0;

err_files:
    kfree(fsi->files);
err_fsi:
    kfree(fsi);
err_bdev:
    blkdev_put(bdev, NULL);
    return ret;
}

/*
 * myfs_mount — точка входа для команды mount(8).
 * Передаём управление myfs_fill_super через mount_nodev
 * (ФС не привязана к конкретному устройству в смысле VFS,
 *  т.к. мы сами открываем bdev по параметру модуля).
 */
static struct dentry *myfs_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev,
                                 void *data)
{
    /*
     * Используем mount_nodev, потому что устройство задаётся параметром
     * модуля (dev_name), а не аргументом mount(8).
     */
    return mount_nodev(fs_type, flags, data, myfs_fill_super);
}

/* Дескриптор файловой системы, регистрируемый в VFS */
static struct file_system_type myfs_type = {
    .owner    = THIS_MODULE,
    .name     = "myfs",           /* имя для mount -t myfs ... */
    .mount    = myfs_mount,
    .kill_sb  = kill_anon_super,  /* стандартное освобождение суперблока */
};

/* ──────────────────────────────────────────────
 * Инициализация и выгрузка модуля
 * ────────────────────────────────────────────── */

static int __init myfs_init(void)
{
    int ret;

    /* Регистрируем тип ФС в VFS */
    ret = register_filesystem(&myfs_type);
    if (ret) {
        pr_err("myfs: failed to register filesystem: %d\n", ret);
        return ret;
    }

    /* Регистрируем misc-устройство для IOCTL */
    ret = misc_register(&myfs_miscdev);
    if (ret) {
        pr_err("myfs: failed to register misc device: %d\n", ret);
        unregister_filesystem(&myfs_type);
        return ret;
    }

    pr_info("myfs: module loaded (dev=%s, sb1=%d, sb2=%d, "
            "name_len=%d, file_sectors=%d)\n",
            dev_name, sb_offset_1, sb_offset_2,
            max_name_len, file_sectors);
    return 0;
}

static void __exit myfs_exit(void)
{
    misc_deregister(&myfs_miscdev);
    unregister_filesystem(&myfs_type);
    pr_info("myfs: module unloaded\n");
}

module_init(myfs_init);
module_exit(myfs_exit);
