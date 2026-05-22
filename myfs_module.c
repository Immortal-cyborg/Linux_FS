// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_module.c — модуль ядра, реализующий учебную файловую систему MyFS.
 *
 * Архитектура диска:
 *  ┌─────────────────────────────────────────────────────┐
 *  │  Сектор sb_offset_1  → Суперблок (копия 1)          │
 *  │  Сектор sb_offset_2  → Суперблок (копия 2)          │
 *  │  Секторы data_start … → данные файлов               │
 *  │    file0: [data_start .. data_start+file_sectors-1] │
 *  │    file1: [data_start+file_sectors .. ]             │
 *  └─────────────────────────────────────────────────────┘
 *
 * Параметры модуля (insmod myfs.ko myfs_dev=loop0 ...):
 *   myfs_dev       — имя блочного устройства (/dev/<myfs_dev>)
 *   sb_offset_1    — сектор первой копии суперблока  (default 0)
 *   sb_offset_2    — сектор второй копии суперблока  (default 1)
 *   max_name_len   — максимальная длина имени файла   (default 64)
 *   file_sectors   — секторов на файл (M)             (default 8)
 *
 * Совместимость: ядро 6.6–6.12 (Debian 13 / Ubuntu 24.04+)
 *
 * Исправления по сравнению с предыдущей версией:
 *   1. Параметр переименован dev_name → myfs_dev (конфликт с dev_name()
 *      из linux/device.h, которая появляется через linux/blk_types.h)
 *   2. i_atime/i_mtime/i_ctime убраны из struct inode в 6.6+;
 *      теперь используем inode_set_{a,m,c}time_to_ts() + current_time()
 *   3. blkdev_get_by_path() заменён на bdev_open_by_path() (6.8+)
 *   4. blkdev_put() заменён на bdev_release() (6.8+)
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

#include "myfs.h"

/* ──────────────────────────────────────────────
 * Параметры модуля
 *
 * ВАЖНО: параметр называется myfs_dev, а НЕ dev_name,
 * потому что dev_name — встроенная inline-функция ядра
 * (linux/device.h), и конфликт имён даёт ошибку компиляции.
 * ────────────────────────────────────────────── */
static char *myfs_dev    = "sdb";       /* имя устройства: /dev/<myfs_dev> */
static int   sb_offset_1 = MYFS_SB_OFFSET_1;
static int   sb_offset_2 = MYFS_SB_OFFSET_2;
static int   max_name_len= MYFS_MAX_NAME_LEN;
static int   file_sectors= MYFS_MAX_FILE_SECTS;

module_param(myfs_dev,     charp, 0444);
module_param(sb_offset_1,  int,   0444);
module_param(sb_offset_2,  int,   0444);
module_param(max_name_len, int,   0444);
module_param(file_sectors, int,   0444);

MODULE_PARM_DESC(myfs_dev,    "Block device name, e.g. loop0 or sdb");
MODULE_PARM_DESC(sb_offset_1, "Sector for superblock copy 1 (default 0)");
MODULE_PARM_DESC(sb_offset_2, "Sector for superblock copy 2 (default 1)");
MODULE_PARM_DESC(max_name_len,"Max filename length (default 64)");
MODULE_PARM_DESC(file_sectors,"File size in sectors M (default 8)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Simple educational block-based filesystem (MyFS)");
MODULE_VERSION("1.1");

/* ──────────────────────────────────────────────
 * Внутреннее состояние ФС (in-memory структуры)
 * ────────────────────────────────────────────── */

/* Хранится в sb->s_fs_info после монтирования */
struct myfs_fs_info {
    struct myfs_super_block  sb_disk;   /* копия суперблока, прочитанного с диска */
    struct bdev_handle       *bdev_h;   /* дескриптор открытого блочного устройства
                                         * (bdev_open_by_path возвращает handle,
                                         *  а не сырой block_device* начиная с 6.8) */
    struct block_device      *bdev;     /* удобный указатель bdev_h->bdev          */
    struct myfs_file_info    *files;    /* массив метаданных файлов (in-memory)    */
    struct mutex              lock;     /* мьютекс для защиты I/O операций         */
    unsigned int              num_files;
};

/* ──────────────────────────────────────────────
 * Вспомогательные функции: работа с диском
 * ────────────────────────────────────────────── */

/*
 * myfs_calc_checksum — XOR всех 32-битных слов суперблока,
 * кроме самого поля checksum (последнее поле структуры).
 *
 * Такой алгоритм прост, но достаточен для учебной задачи:
 * обнаруживает случайное повреждение любого поля.
 */
static __u32 myfs_calc_checksum(const struct myfs_super_block *sb)
{
    const __u32 *p  = (const __u32 *)sb;
    size_t nwords   = (sizeof(*sb) - sizeof(__u32)) / sizeof(__u32);
    __u32  result   = 0;
    size_t i;

    for (i = 0; i < nwords; i++)
        result ^= p[i];

    return result;
}

/*
 * myfs_read_sector — читает 512-байтовый сектор с устройства bdev
 * в буфер dst.  Использует buffer_head API (__bread).
 */
static int myfs_read_sector(struct block_device *bdev, sector_t sector,
                             void *dst)
{
    struct buffer_head *bh;

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
 * myfs_write_sector — записывает 512 байт из src на устройство.
 * mark_buffer_dirty() + sync_dirty_buffer() — синхронная запись.
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
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);  /* ждём физической записи на диск */
    brelse(bh);
    return 0;
}

/* Читает все секторы файла file_idx в буфер data (размер >= num_sectors*512) */
static int myfs_read_file_data(struct myfs_fs_info *fsi, int file_idx,
                               void *data)
{
    struct myfs_file_info *fi = &fsi->files[file_idx];
    int i, ret;

    for (i = 0; i < (int)fi->num_sectors; i++) {
        ret = myfs_read_sector(fsi->bdev,
                               fi->start_sector + i,
                               (char *)data + i * MYFS_SECTOR_SIZE);
        if (ret)
            return ret;
    }
    return 0;
}

/*
 * Записывает size байт из data в секторы файла.
 * Остаток сектора (если size не кратно 512) заполняется нулями.
 */
static int myfs_write_file_data(struct myfs_fs_info *fsi, int file_idx,
                                const void *data, size_t size)
{
    struct myfs_file_info *fi = &fsi->files[file_idx];
    char   buf[MYFS_SECTOR_SIZE];
    int    i, ret;
    size_t written = 0;

    for (i = 0; i < (int)fi->num_sectors; i++) {
        memset(buf, 0, MYFS_SECTOR_SIZE);
        if (written < size) {
            size_t chunk = min((size_t)MYFS_SECTOR_SIZE, size - written);
            memcpy(buf, (const char *)data + written, chunk);
            written += chunk;
        }
        ret = myfs_write_sector(fsi->bdev, fi->start_sector + i, buf);
        if (ret)
            return ret;
    }
    return 0;
}

/* ──────────────────────────────────────────────
 * Суперблок: запись и чтение с верификацией
 * ────────────────────────────────────────────── */

/* Вычисляет checksum и записывает sb в обе копии (sb_offset_1 и sb_offset_2) */
static int myfs_write_superblock(struct block_device *bdev,
                                 struct myfs_super_block *sb)
{
    int ret;
    sb->checksum = myfs_calc_checksum(sb);

    ret = myfs_write_sector(bdev, sb->sb_offset_1, sb);
    if (ret) return ret;
    return myfs_write_sector(bdev, sb->sb_offset_2, sb);
}

/* Читает суперблок из сектора sector, проверяет magic и checksum */
static int myfs_read_and_verify_sb(struct block_device *bdev,
                                   sector_t sector,
                                   struct myfs_super_block *sb)
{
    __u32 expected;
    int   ret;

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
                "stored=0x%08X expected=0x%08X\n",
                (unsigned long long)sector, sb->checksum, expected);
        return -EINVAL;
    }
    return 0;
}

/*
 * myfs_format — первичная инициализация устройства:
 *   • строит структуру суперблока,
 *   • обнуляет все секторы данных,
 *   • записывает обе копии суперблока.
 */
static int myfs_format(struct block_device *bdev,
                       __u32 sb1, __u32 sb2,
                       __u32 mname, __u32 fsects,
                       struct myfs_super_block *sb_out)
{
    struct myfs_super_block sb;
    char   zero[MYFS_SECTOR_SIZE];
    __u32  total_sectors, data_start, num_files, i;
    int    ret;

    total_sectors = (u32)bdev_nr_sectors(bdev);
    data_start    = max(sb1, sb2) + 1;

    if (data_start >= total_sectors) {
        pr_err("myfs: device too small for superblocks\n");
        return -EINVAL;
    }

    num_files = (total_sectors - data_start) / fsects;
    if (num_files == 0) {
        pr_err("myfs: device too small for any files "
               "(total=%u data_start=%u fsects=%u)\n",
               total_sectors, data_start, fsects);
        return -EINVAL;
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic         = MYFS_MAGIC;
    sb.version       = 1;
    sb.sector_size   = MYFS_SECTOR_SIZE;
    sb.total_sectors = total_sectors;
    sb.sb_offset_1   = sb1;
    sb.sb_offset_2   = sb2;
    sb.max_name_len  = mname;
    sb.file_sectors  = fsects;
    sb.data_start    = data_start;
    sb.num_files     = num_files;

    /* Обнуляем все секторы данных */
    memset(zero, 0, MYFS_SECTOR_SIZE);
    for (i = 0; i < num_files * fsects; i++) {
        ret = myfs_write_sector(bdev, data_start + i, zero);
        if (ret) return ret;
    }

    ret = myfs_write_superblock(bdev, &sb);
    if (ret) return ret;

    memcpy(sb_out, &sb, sizeof(sb));
    pr_info("myfs: formatted: %u files × %u sectors, data_start=%u\n",
            num_files, fsects, data_start);
    return 0;
}

/* ──────────────────────────────────────────────
 * VFS: файловые операции (read / write)
 * ────────────────────────────────────────────── */

static ssize_t myfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct inode        *inode    = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi      = inode->i_sb->s_fs_info;
    int                  file_idx = (int)(inode->i_ino - 1);
    size_t               file_size;
    loff_t               pos      = iocb->ki_pos;
    size_t               count, to_copy;
    char                *buf;
    int                  ret;

    if (file_idx < 0 || file_idx >= (int)fsi->num_files)
        return -EINVAL;

    file_size = (size_t)fsi->files[file_idx].num_sectors * MYFS_SECTOR_SIZE;

    if (pos >= (loff_t)file_size)
        return 0; /* EOF */

    count   = iov_iter_count(to);
    to_copy = min(count, (size_t)(file_size - pos));

    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);
    ret = myfs_read_file_data(fsi, file_idx, buf);
    mutex_unlock(&fsi->lock);

    if (ret) { kfree(buf); return ret; }

    if (copy_to_iter(buf + pos, to_copy, to) != to_copy) {
        kfree(buf);
        return -EFAULT;
    }

    iocb->ki_pos += to_copy;
    kfree(buf);
    return (ssize_t)to_copy;
}

static ssize_t myfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode        *inode    = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi      = inode->i_sb->s_fs_info;
    int                  file_idx = (int)(inode->i_ino - 1);
    size_t               file_size;
    loff_t               pos      = iocb->ki_pos;
    size_t               count, to_write;
    char                *buf;
    int                  ret;

    if (file_idx < 0 || file_idx >= (int)fsi->num_files)
        return -EINVAL;

    file_size = (size_t)fsi->files[file_idx].num_sectors * MYFS_SECTOR_SIZE;

    if (pos >= (loff_t)file_size)
        return -ENOSPC;

    count    = iov_iter_count(from);
    to_write = min(count, (size_t)(file_size - pos));

    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);

    /* Читаем текущее содержимое, чтобы не затереть соседние данные */
    ret = myfs_read_file_data(fsi, file_idx, buf);
    if (ret) goto out;

    if (copy_from_iter(buf + pos, to_write, from) != to_write) {
        ret = -EFAULT;
        goto out;
    }

    ret = myfs_write_file_data(fsi, file_idx, buf, file_size);
    if (!ret) {
        iocb->ki_pos += to_write;
        fsi->files[file_idx].crc32 = crc32(0, buf, file_size);
    }

out:
    mutex_unlock(&fsi->lock);
    kfree(buf);
    return ret ? ret : (ssize_t)to_write;
}

static const struct file_operations myfs_file_ops = {
    .owner      = THIS_MODULE,
    .read_iter  = myfs_file_read_iter,
    .write_iter = myfs_file_write_iter,
    .llseek     = generic_file_llseek,
};

static const struct inode_operations myfs_file_inode_ops = {
    /* Намеренно пусто: rename/link/unlink не предусмотрены по заданию */
};

/* ──────────────────────────────────────────────
 * VFS: инициализация inode для файла
 *
 * Изменение относительно старых ядер:
 *   До 6.6:  inode->i_atime = inode->i_mtime = inode->i_ctime = ts;
 *   С  6.6:  поля убраны; используем inode_set_*time_to_ts()
 * ────────────────────────────────────────────── */
static struct inode *myfs_get_inode(struct super_block *sb, int file_idx)
{
    struct myfs_fs_info   *fsi  = sb->s_fs_info;
    struct myfs_file_info *fi   = &fsi->files[file_idx];
    struct inode          *inode;
    struct timespec64      ts;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    inode->i_ino   = (unsigned long)(file_idx + 1); /* 0 зарезервирован для root */
    inode->i_mode  = S_IFREG | 0666;
    inode->i_uid   = GLOBAL_ROOT_UID;
    inode->i_gid   = GLOBAL_ROOT_GID;
    inode->i_size  = (loff_t)fi->num_sectors * MYFS_SECTOR_SIZE;
    inode->i_blocks= fi->num_sectors;

    /* В ядре 6.6+ прямого доступа к i_atime/i_mtime/i_ctime нет.
     * current_time() возвращает struct timespec64, которую нужно
     * передавать через специальные setter-функции.               */
    ts = current_time(inode);
    inode_set_atime_to_ts(inode, ts);
    inode_set_mtime_to_ts(inode, ts);
    inode_set_ctime_to_ts(inode, ts);

    inode->i_op  = &myfs_file_inode_ops;
    inode->i_fop = &myfs_file_ops;

    return inode;
}

/* ──────────────────────────────────────────────
 * VFS: операции над корневой директорией
 * ────────────────────────────────────────────── */

static int myfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct super_block  *sb  = file->f_inode->i_sb;
    struct myfs_fs_info *fsi = sb->s_fs_info;
    unsigned int i;

    if (!dir_emit_dots(file, ctx))
        return 0;

    for (i = 0; i < fsi->num_files; i++) {
        struct myfs_file_info *fi = &fsi->files[i];

        if (ctx->pos > (loff_t)(i + 2))
            continue;

        if (!dir_emit(ctx, fi->name, strlen(fi->name),
                      (ino_t)(i + 1), DT_REG))
            return 0;

        ctx->pos++;
    }
    return 0;
}

static struct dentry *myfs_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
    struct super_block  *sb  = dir->i_sb;
    struct myfs_fs_info *fsi = sb->s_fs_info;
    unsigned int i;

    for (i = 0; i < fsi->num_files; i++) {
        if (strcmp(fsi->files[i].name, dentry->d_name.name) == 0) {
            struct inode *inode = myfs_get_inode(sb, (int)i);
            if (!inode)
                return ERR_PTR(-ENOMEM);
            return d_splice_alias(inode, dentry);
        }
    }
    /* Файл не найден → negative dentry */
    d_add(dentry, NULL);
    return NULL;
}

static const struct file_operations myfs_dir_ops = {
    .owner          = THIS_MODULE,
    .iterate_shared = myfs_readdir,
    .llseek         = generic_file_llseek,
};

static const struct inode_operations myfs_dir_inode_ops = {
    .lookup = myfs_lookup,
};

/* ──────────────────────────────────────────────
 * IOCTL через /dev/myfs_ctl (miscdevice)
 * ────────────────────────────────────────────── */

static struct myfs_fs_info *g_fsi = NULL; /* глобальный указатель для IOCTL */

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
        zero_buf = kzalloc(
            (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE, GFP_KERNEL);
        if (!zero_buf) return -ENOMEM;

        mutex_lock(&fsi->lock);
        for (i = 0; i < fsi->num_files; i++) {
            ret = myfs_write_file_data(fsi, (int)i, zero_buf,
                (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE);
            if (ret) break;
            fsi->files[i].crc32 = crc32(0, zero_buf,
                (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE);
        }
        mutex_unlock(&fsi->lock);
        kfree(zero_buf);
        pr_info("myfs: IOCTL ZERO_ALL done\n");
        break;

    /* ── Стереть ФС (обнулить оба суперблока) ── */
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

    /* ── Получить метаинформацию (CRC32) всех файлов ── */
    case MYFS_IOC_GET_META: {
        struct myfs_file_info __user *uinfo =
            (struct myfs_file_info __user *)arg;
        char *buf = kmalloc(
            (size_t)fsi->sb_disk.file_sectors * MYFS_SECTOR_SIZE, GFP_KERNEL);
        if (!buf) return -ENOMEM;

        mutex_lock(&fsi->lock);
        for (i = 0; i < fsi->num_files; i++) {
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

    /* ── Маппинг секторов для заданного файла ── */
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
        pr_info("myfs: IOCTL GET_SECTOR_MAP file=%u start=%u nsect=%u\n",
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

static struct miscdevice myfs_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "myfs_ctl",
    .fops  = &myfs_ctl_fops,
};

/* ──────────────────────────────────────────────
 * VFS: суперблок
 * ────────────────────────────────────────────── */

static void myfs_put_super(struct super_block *sb)
{
    struct myfs_fs_info *fsi = sb->s_fs_info;

    if (fsi) {
        /* Закрываем блочное устройство через handle (API 6.8+) */
        if (fsi->bdev_h)
            bdev_release(fsi->bdev_h);
        kfree(fsi->files);
        kfree(fsi);
        sb->s_fs_info = NULL;
    }
    g_fsi = NULL;
    pr_info("myfs: unmounted\n");
}

static const struct super_operations myfs_super_ops = {
    .put_super  = myfs_put_super,
    .statfs     = simple_statfs,
    .drop_inode = generic_drop_inode,
};

/*
 * myfs_fill_super — главная функция инициализации, вызывается при mount(2).
 *
 * Изменения API относительно старых ядер:
 *   • bdev_open_by_path() вместо blkdev_get_by_path() (6.8+)
 *     Возвращает struct bdev_handle*, а не struct block_device*.
 *   • bdev_release()      вместо blkdev_put()          (6.8+)
 */
static int myfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct myfs_fs_info     *fsi;
    struct myfs_super_block *disk_sb;
    struct inode            *root_inode;
    struct dentry           *root_dentry;
    struct bdev_handle      *bdev_h;
    char                     dev_path[80];
    struct timespec64        ts;
    bool                     need_format = false;
    unsigned int             i;
    int                      ret;

    /* ── Открываем блочное устройство ── */
    snprintf(dev_path, sizeof(dev_path), "/dev/%s", myfs_dev);

    /*
     * bdev_open_by_path — актуальный API начиная с ядра 6.8.
     * Возвращает struct bdev_handle* или ERR_PTR.
     * Флаги BLK_OPEN_READ | BLK_OPEN_WRITE соответствуют FMODE_READ | FMODE_WRITE.
     */
    bdev_h = bdev_open_by_path(dev_path, BLK_OPEN_READ | BLK_OPEN_WRITE,
                               NULL, NULL);
    if (IS_ERR(bdev_h)) {
        pr_err("myfs: cannot open %s: %ld\n", dev_path, PTR_ERR(bdev_h));
        return PTR_ERR(bdev_h);
    }

    /* ── Выделяем in-memory структуру ── */
    fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    if (!fsi) { ret = -ENOMEM; goto err_bdev; }

    fsi->bdev_h = bdev_h;
    fsi->bdev   = bdev_h->bdev;   /* удобный прямой указатель */
    mutex_init(&fsi->lock);
    disk_sb = &fsi->sb_disk;

    /* ── Пробуем прочитать суперблок (сначала копия 1, затем 2) ── */
    ret = myfs_read_and_verify_sb(fsi->bdev, (sector_t)sb_offset_1, disk_sb);
    if (ret) {
        pr_warn("myfs: primary SB invalid, trying backup\n");
        ret = myfs_read_and_verify_sb(fsi->bdev, (sector_t)sb_offset_2, disk_sb);
        if (ret) {
            pr_info("myfs: no valid SB, formatting device...\n");
            need_format = true;
        }
    }

    if (need_format) {
        ret = myfs_format(fsi->bdev,
                          (__u32)sb_offset_1, (__u32)sb_offset_2,
                          (__u32)max_name_len, (__u32)file_sectors,
                          disk_sb);
        if (ret) goto err_fsi;
    }

    fsi->num_files = disk_sb->num_files;

    /* ── Строим массив метаданных файлов (in-memory) ── */
    fsi->files = kcalloc(fsi->num_files, sizeof(*fsi->files), GFP_KERNEL);
    if (!fsi->files) { ret = -ENOMEM; goto err_fsi; }

    for (i = 0; i < fsi->num_files; i++) {
        fsi->files[i].index        = i;
        fsi->files[i].start_sector = disk_sb->data_start + i * disk_sb->file_sectors;
        fsi->files[i].num_sectors  = disk_sb->file_sectors;
        /* Имя вида "file00000", "file00001", … */
        snprintf(fsi->files[i].name, disk_sb->max_name_len, "file%05u", i);
    }

    /* ── Настраиваем VFS суперблок ── */
    sb->s_magic          = MYFS_MAGIC;
    sb->s_op             = &myfs_super_ops;
    sb->s_fs_info        = fsi;
    sb->s_blocksize      = MYFS_SECTOR_SIZE;
    sb->s_blocksize_bits = blksize_bits(MYFS_SECTOR_SIZE);
    sb->s_maxbytes       = (loff_t)disk_sb->file_sectors * MYFS_SECTOR_SIZE;

    /* ── Создаём корневой inode (директория) ── */
    root_inode = new_inode(sb);
    if (!root_inode) { ret = -ENOMEM; goto err_files; }

    root_inode->i_ino  = 0;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_uid  = GLOBAL_ROOT_UID;
    root_inode->i_gid  = GLOBAL_ROOT_GID;

    /* Устанавливаем временные метки через API 6.6+ */
    ts = current_time(root_inode);
    inode_set_atime_to_ts(root_inode, ts);
    inode_set_mtime_to_ts(root_inode, ts);
    inode_set_ctime_to_ts(root_inode, ts);

    root_inode->i_op   = &myfs_dir_inode_ops;
    root_inode->i_fop  = &myfs_dir_ops;
    set_nlink(root_inode, 2); /* "." + ".." */

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) { ret = -ENOMEM; goto err_files; }

    sb->s_root = root_dentry;
    g_fsi = fsi;

    pr_info("myfs: mounted %s — %u files × %u sectors\n",
            dev_path, fsi->num_files, disk_sb->file_sectors);
    return 0;

err_files:
    kfree(fsi->files);
err_fsi:
    kfree(fsi);
err_bdev:
    bdev_release(bdev_h);  /* bdev_release вместо blkdev_put (6.8+) */
    return ret;
}

static struct dentry *myfs_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev, void *data)
{
    /*
     * mount_nodev: устройство не передаётся через аргумент mount(8),
     * а задаётся параметром модуля myfs_dev.
     */
    return mount_nodev(fs_type, flags, data, myfs_fill_super);
}

static struct file_system_type myfs_type = {
    .owner   = THIS_MODULE,
    .name    = "myfs",
    .mount   = myfs_mount,
    .kill_sb = kill_anon_super,
};

/* ──────────────────────────────────────────────
 * Инициализация и выгрузка модуля
 * ────────────────────────────────────────────── */

static int __init myfs_init(void)
{
    int ret;

    ret = register_filesystem(&myfs_type);
    if (ret) {
        pr_err("myfs: register_filesystem failed: %d\n", ret);
        return ret;
    }

    ret = misc_register(&myfs_miscdev);
    if (ret) {
        pr_err("myfs: misc_register failed: %d\n", ret);
        unregister_filesystem(&myfs_type);
        return ret;
    }

    pr_info("myfs: loaded (dev=/dev/%s sb1=%d sb2=%d name=%d sects=%d)\n",
            myfs_dev, sb_offset_1, sb_offset_2, max_name_len, file_sectors);
    return 0;
}

static void __exit myfs_exit(void)
{
    misc_deregister(&myfs_miscdev);
    unregister_filesystem(&myfs_type);
    pr_info("myfs: unloaded\n");
}

module_init(myfs_init);
module_exit(myfs_exit);