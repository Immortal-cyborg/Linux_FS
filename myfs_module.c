// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_module.c — модуль ядра MyFS.
 *
 * Архитектура диска:
 *  ┌──────────────────────────────────────────────────────┐
 *  │  Сектор sb_offset_1  → Суперблок (копия 1)           │
 *  │  Сектор sb_offset_2  → Суперблок (копия 2)           │
 *  │  Секторы data_start … → данные файлов                │
 *  │    file0: [data_start .. data_start+file_sectors-1]  │
 *  │    file1: [data_start+file_sectors .. ]              │
 *  └──────────────────────────────────────────────────────┘
 *
 * Параметры (insmod myfs.ko myfs_dev=loop0 ...):
 *   myfs_dev      — имя блочного устройства (/dev/<myfs_dev>)
 *   sb_offset_1   — сектор первой копии суперблока  (default 0)
 *   sb_offset_2   — сектор второй копии суперблока  (default 1)
 *   max_name_len  — максимальная длина имени файла   (default 64)
 *   file_sectors  — секторов на файл (M)             (default 8)
 *   format        — 1 = форматировать устройство при монтировании
 *                   (создать ФС заново). 0 = только монтировать
 *                   существующую ФС, при двух повреждённых
 *                   суперблоках вернуть ошибку.       (default 0)
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
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
 * Параметр НАМЕРЕННО называется myfs_dev, а не dev_name:
 * dev_name — это inline-функция в linux/device.h, её имя
 * конфликтует с переменной модуля и даёт ошибку компиляции.
 * ────────────────────────────────────────────── */
static char *myfs_dev    = "sdb";
static int   sb_offset_1 = MYFS_SB_OFFSET_1;
static int   sb_offset_2 = MYFS_SB_OFFSET_2;
static int   max_name_len= MYFS_MAX_NAME_LEN;
static int   file_sectors= MYFS_MAX_FILE_SECTS;
static int   format      = 0;

module_param(myfs_dev,     charp, 0444);
module_param(sb_offset_1,  int,   0444);
module_param(sb_offset_2,  int,   0444);
module_param(max_name_len, int,   0444);
module_param(file_sectors, int,   0444);
module_param(format,       int,   0444);

MODULE_PARM_DESC(myfs_dev,    "Block device name, e.g. loop0 or sdb");
MODULE_PARM_DESC(sb_offset_1, "Sector for superblock copy 1 (default 0)");
MODULE_PARM_DESC(sb_offset_2, "Sector for superblock copy 2 (default 1)");
MODULE_PARM_DESC(max_name_len,"Max filename length (default 64)");
MODULE_PARM_DESC(file_sectors,"File size in sectors M (default 8)");
MODULE_PARM_DESC(format,      "1 = (re)format device on mount (default 0)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Simple educational block-based filesystem (MyFS)");
MODULE_VERSION("1.2");

/* ──────────────────────────────────────────────
 * In-memory структура смонтированной ФС
 * ────────────────────────────────────────────── */
struct myfs_fs_info {
    struct myfs_super_block  sb_disk;     /* копия суперблока с диска          */
    struct file             *bdev_file;   /* struct file* (API 6.9+)           */
    struct block_device     *bdev;        /* file_bdev(bdev_file)              */
    struct myfs_file_info   *files;       /* массив метаданных файлов           */
    struct mutex             lock;        /* сериализация доступа к ФС/IOCTL    */
    unsigned int             num_files;
    unsigned int             sector_size; /* реальный размер сектора устройства */
    bool                     invalidated; /* true после ERASE_FS               */
};

/* ──────────────────────────────────────────────
 * bio I/O: единый примитив чтения/записи
 *
 * dev_sector — номер сектора в единицах реального размера сектора
 * устройства (fsi->sector_size). bio адресуется в 512-байтовых
 * единицах, поэтому переводим: bi_sector = dev_sector * (sector_size/512).
 *
 * buf обязан быть физически непрерывным (kmalloc/kzalloc, НЕ vmalloc),
 * len — кратен sector_size.
 * ────────────────────────────────────────────── */
static int myfs_bio_io(struct myfs_fs_info *fsi, sector_t dev_sector,
                       void *buf, size_t len, blk_opf_t opf)
{
    struct bio  *bio;
    unsigned int per_fs_sector = fsi->sector_size >> SECTOR_SHIFT;
    sector_t     bi_sector     = dev_sector * per_fs_sector;
    unsigned int nr_pages      = DIV_ROUND_UP(offset_in_page(buf) + len, PAGE_SIZE);
    char        *p             = buf;
    size_t       remaining     = len;
    int          ret;

    bio = bio_alloc(fsi->bdev, nr_pages, opf, GFP_KERNEL);
    if (!bio)
        return -ENOMEM;
    bio->bi_iter.bi_sector = bi_sector;

    while (remaining) {
        size_t poff  = offset_in_page(p);
        size_t chunk = min(remaining, (size_t)(PAGE_SIZE - poff));
        if (bio_add_page(bio, virt_to_page(p), chunk, poff) != chunk) {
            bio_put(bio);
            return -EIO;
        }
        p         += chunk;
        remaining -= chunk;
    }

    ret = submit_bio_wait(bio);
    bio_put(bio);
    return ret;
}

/*
 * myfs_calc_checksum — XOR всех 32-битных слов суперблока,
 * кроме последнего поля (checksum).
 */
static __u32 myfs_calc_checksum(const struct myfs_super_block *sb)
{
    const __u32 *p = (const __u32 *)sb;
    size_t nwords  = (sizeof(*sb) - sizeof(__u32)) / sizeof(__u32);
    __u32  result  = 0;
    size_t i;
    for (i = 0; i < nwords; i++)
        result ^= p[i];
    return result;
}

/* ──────────────────────────────────────────────
 * Чтение / запись данных файла одним батчевым bio
 * ────────────────────────────────────────────── */

/* Читает все секторы файла idx в буфер data (size = num_sectors*sector_size) */
static int myfs_read_file_data(struct myfs_fs_info *fsi, int idx, void *data)
{
    struct myfs_file_info *fi  = &fsi->files[idx];
    size_t                 len = (size_t)fi->num_sectors * fsi->sector_size;
    return myfs_bio_io(fsi, fi->start_sector, data, len, REQ_OP_READ);
}

/*
 * Записывает все секторы файла idx одним bio.
 * data должен содержать полный размер файла (последний неполный
 * сектор уже дополнен нулями вызывающей стороной).
 */
static int myfs_write_file_data(struct myfs_fs_info *fsi, int idx, void *data)
{
    struct myfs_file_info *fi  = &fsi->files[idx];
    size_t                 len = (size_t)fi->num_sectors * fsi->sector_size;
    return myfs_bio_io(fsi, fi->start_sector, data, len, REQ_OP_WRITE);
}

/* ──────────────────────────────────────────────
 * Суперблок: чтение/проверка/запись копий
 * ────────────────────────────────────────────── */

/* Читает запись суперблока из сектора sec в sb */
static int myfs_read_sb_copy(struct myfs_fs_info *fsi, __u32 sec,
                             struct myfs_super_block *sb)
{
    void *buf = kzalloc(fsi->sector_size, GFP_KERNEL);
    int   ret;
    if (!buf)
        return -ENOMEM;
    ret = myfs_bio_io(fsi, sec, buf, fsi->sector_size, REQ_OP_READ);
    if (!ret)
        memcpy(sb, buf, sizeof(*sb));
    kfree(buf);
    return ret;
}

/* Записывает запись суперблока в сектор sec (остаток сектора — нули) */
static int myfs_write_sb_copy(struct myfs_fs_info *fsi, __u32 sec,
                              const struct myfs_super_block *sb)
{
    void *buf = kzalloc(fsi->sector_size, GFP_KERNEL);
    int   ret;
    if (!buf)
        return -ENOMEM;
    memcpy(buf, sb, sizeof(*sb));
    ret = myfs_bio_io(fsi, sec, buf, fsi->sector_size, REQ_OP_WRITE);
    kfree(buf);
    return ret;
}

/* Проверяет magic и checksum уже прочитанного суперблока */
static int myfs_verify_sb(const struct myfs_super_block *sb)
{
    if (sb->magic != MYFS_MAGIC)
        return -EINVAL;
    if (sb->checksum != myfs_calc_checksum(sb))
        return -EINVAL;
    return 0;
}

/* Пересчитывает checksum и пишет sb в оба сектора */
static int myfs_write_superblock(struct myfs_fs_info *fsi,
                                 struct myfs_super_block *sb)
{
    int ret;
    sb->checksum = myfs_calc_checksum(sb);
    ret = myfs_write_sb_copy(fsi, sb->sb_offset_1, sb);
    if (ret)
        return ret;
    return myfs_write_sb_copy(fsi, sb->sb_offset_2, sb);
}

/*
 * myfs_format — создание ФС заново: записывает суперблок и зануляет
 * каждый файл (по одному батчевому bio на файл).
 */
static int myfs_format(struct myfs_fs_info *fsi,
                       __u32 sb1, __u32 sb2, __u32 mname, __u32 fsects)
{
    struct myfs_super_block *sb = &fsi->sb_disk;
    void  *zbuf;
    __u32  total, data_start, num_files, i;
    size_t fsz;
    int    ret;

    total      = (__u32)(bdev_nr_bytes(fsi->bdev) / fsi->sector_size);
    data_start = max(sb1, sb2) + 1;

    if (data_start >= total) {
        pr_err("myfs: device too small for superblocks\n");
        return -EINVAL;
    }
    num_files = (total - data_start) / fsects;
    if (num_files == 0) {
        pr_err("myfs: device too small for any file\n");
        return -EINVAL;
    }

    memset(sb, 0, sizeof(*sb));
    sb->magic         = MYFS_MAGIC;
    sb->version       = 1;
    sb->sector_size   = fsi->sector_size;
    sb->total_sectors = total;
    sb->sb_offset_1   = sb1;
    sb->sb_offset_2   = sb2;
    sb->max_name_len  = mname;
    sb->file_sectors  = fsects;
    sb->data_start    = data_start;
    sb->num_files     = num_files;

    /* Зануление файлов: один батчевый bio на файл */
    fsz  = (size_t)fsects * fsi->sector_size;
    zbuf = kzalloc(fsz, GFP_KERNEL);
    if (!zbuf)
        return -ENOMEM;
    for (i = 0; i < num_files; i++) {
        ret = myfs_bio_io(fsi, data_start + i * fsects, zbuf, fsz,
                          REQ_OP_WRITE);
        if (ret) {
            kfree(zbuf);
            return ret;
        }
    }
    kfree(zbuf);

    ret = myfs_write_superblock(fsi, sb);
    if (ret)
        return ret;

    pr_info("myfs: formatted: %u files × %u sectors (sector_size=%u), data_start=%u\n",
            num_files, fsects, fsi->sector_size, data_start);
    return 0;
}

/* ──────────────────────────────────────────────
 * VFS: файловые операции read / write
 * ────────────────────────────────────────────── */

static ssize_t myfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct inode        *inode = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi   = inode->i_sb->s_fs_info;
    int                  idx   = (int)(inode->i_ino - 1);
    loff_t               pos   = iocb->ki_pos;
    size_t               file_size, count, to_copy;
    char                *buf;
    int                  ret;

    if (idx < 0 || idx >= (int)fsi->num_files)
        return -EINVAL;
    if (fsi->invalidated)
        return -EIO;

    file_size = (size_t)fsi->files[idx].num_sectors * fsi->sector_size;
    if (pos >= (loff_t)file_size)
        return 0;

    count   = iov_iter_count(to);
    to_copy = min(count, (size_t)(file_size - pos));

    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);
    ret = fsi->invalidated ? -EIO : myfs_read_file_data(fsi, idx, buf);
    mutex_unlock(&fsi->lock);
    if (ret) { kfree(buf); return ret; }

    if (copy_to_iter(buf + pos, to_copy, to) != to_copy)
        { kfree(buf); return -EFAULT; }

    iocb->ki_pos += to_copy;
    kfree(buf);
    return (ssize_t)to_copy;
}

static ssize_t myfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode        *inode = iocb->ki_filp->f_inode;
    struct myfs_fs_info *fsi   = inode->i_sb->s_fs_info;
    int                  idx   = (int)(inode->i_ino - 1);
    loff_t               pos   = iocb->ki_pos;
    size_t               file_size, count, to_write;
    char                *buf;
    int                  ret;

    if (idx < 0 || idx >= (int)fsi->num_files)
        return -EINVAL;
    if (fsi->invalidated)
        return -EIO;

    /*
     * Позиция берётся из iocb->ki_pos и наращивается на число
     * записанных байт (ниже). При O_APPEND VFS передаёт ki_pos = f_pos,
     * который корректно продвигается между последовательными записями
     * и не сбрасывается в начало. Файлы фиксированного размера, поэтому
     * запись за пределы ёмкости → -ENOSPC.
     */
    file_size = (size_t)fsi->files[idx].num_sectors * fsi->sector_size;
    if (pos >= (loff_t)file_size)
        return -ENOSPC;

    count    = iov_iter_count(from);
    to_write = min(count, (size_t)(file_size - pos));

    buf = kmalloc(file_size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    mutex_lock(&fsi->lock);
    if (fsi->invalidated) { ret = -EIO; goto out; }

    /* read-modify-write: дочитываем существующее содержимое */
    ret = myfs_read_file_data(fsi, idx, buf);
    if (ret) goto out;

    if (copy_from_iter(buf + pos, to_write, from) != to_write)
        { ret = -EFAULT; goto out; }

    ret = myfs_write_file_data(fsi, idx, buf);
    if (!ret) {
        iocb->ki_pos += to_write;
        fsi->files[idx].crc32 = crc32(0, buf, file_size);
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
 * VFS: создание inode для файла
 * ────────────────────────────────────────────── */
static struct inode *myfs_get_inode(struct super_block *sb, int idx)
{
    struct myfs_fs_info   *fsi = sb->s_fs_info;
    struct myfs_file_info *fi  = &fsi->files[idx];
    struct timespec64      ts;
    struct inode          *inode = new_inode(sb);
    if (!inode)
        return NULL;

    inode->i_ino    = (unsigned long)(idx + 1);
    inode->i_mode   = S_IFREG | 0666;
    inode->i_uid    = GLOBAL_ROOT_UID;
    inode->i_gid    = GLOBAL_ROOT_GID;
    inode->i_size   = (loff_t)fi->num_sectors * fsi->sector_size;
    inode->i_blocks = (blkcnt_t)fi->num_sectors * (fsi->sector_size >> 9);

    ts = current_time(inode);
    inode_set_atime_to_ts(inode, ts);
    inode_set_mtime_to_ts(inode, ts);
    inode_set_ctime_to_ts(inode, ts);

    inode->i_op  = &myfs_file_inode_ops;
    inode->i_fop = &myfs_file_ops;
    return inode;
}

/* ──────────────────────────────────────────────
 * VFS: корневая директория — readdir + lookup
 * ────────────────────────────────────────────── */

static int myfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct myfs_fs_info *fsi = file->f_inode->i_sb->s_fs_info;
    unsigned int         i;

    if (!dir_emit_dots(file, ctx))
        return 0;

    /* После ERASE_FS ФС инвалидирована — файлы не показываем */
    if (fsi->invalidated)
        return 0;

    /* ctx->pos: 0 и 1 — "." и "..", далее i = pos - 2 */
    for (i = ctx->pos - 2; i < fsi->num_files; i++) {
        struct myfs_file_info *fi = &fsi->files[i];
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
    struct myfs_fs_info *fsi = dir->i_sb->s_fs_info;
    unsigned int         i;

    if (!fsi->invalidated) {
        for (i = 0; i < fsi->num_files; i++) {
            if (strcmp(fsi->files[i].name, dentry->d_name.name) == 0) {
                struct inode *inode = myfs_get_inode(dir->i_sb, (int)i);
                if (!inode)
                    return ERR_PTR(-ENOMEM);
                return d_splice_alias(inode, dentry);
            }
        }
    }
    d_add(dentry, NULL);   /* negative dentry — файл не найден */
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

static struct myfs_fs_info *g_fsi = NULL;
static DEFINE_MUTEX(g_fsi_lock);   /* защищает указатель g_fsi */

static long myfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct myfs_fs_info *fsi;
    __u32 i;
    int   ret = 0;

    /* Захватываем актуальный смонтированный экземпляр ФС */
    mutex_lock(&g_fsi_lock);
    fsi = g_fsi;
    if (fsi)
        mutex_lock(&fsi->lock);
    mutex_unlock(&g_fsi_lock);

    if (!fsi)
        return -ENODEV;
    if (fsi->invalidated && cmd != MYFS_IOC_ERASE_FS) {
        mutex_unlock(&fsi->lock);
        return -ENODEV;
    }

    switch (cmd) {

    /* ── Обнулить все файлы (батчевый bio на файл) ── */
    case MYFS_IOC_ZERO_ALL: {
        size_t fsz  = (size_t)fsi->sb_disk.file_sectors * fsi->sector_size;
        char  *zbuf = kzalloc(fsz, GFP_KERNEL);
        if (!zbuf) { ret = -ENOMEM; break; }
        for (i = 0; i < fsi->num_files && !ret; i++) {
            ret = myfs_write_file_data(fsi, (int)i, zbuf);
            if (!ret)
                fsi->files[i].crc32 = crc32(0, zbuf, fsz);
        }
        kfree(zbuf);
        pr_info("myfs: ZERO_ALL done\n");
        break;
    }

    /* ── Стереть ФС: обнулить оба суперблока + инвалидировать ── */
    case MYFS_IOC_ERASE_FS: {
        void *sec = kzalloc(fsi->sector_size, GFP_KERNEL);
        if (!sec) { ret = -ENOMEM; break; }
        myfs_bio_io(fsi, fsi->sb_disk.sb_offset_1, sec, fsi->sector_size,
                    REQ_OP_WRITE);
        myfs_bio_io(fsi, fsi->sb_disk.sb_offset_2, sec, fsi->sector_size,
                    REQ_OP_WRITE);
        kfree(sec);
        fsi->invalidated = true;   /* файлы исчезают, I/O запрещён до re-mount */
        pr_info("myfs: ERASE_FS done — filesystem invalidated, re-mount required\n");
        break;
    }

    /* ── Метаинформация (CRC32) всех файлов ── */
    case MYFS_IOC_GET_META: {
        struct myfs_file_info __user *uinfo = (struct myfs_file_info __user *)arg;
        size_t fsz = (size_t)fsi->sb_disk.file_sectors * fsi->sector_size;
        char  *buf = kmalloc(fsz, GFP_KERNEL);
        if (!buf) { ret = -ENOMEM; break; }
        for (i = 0; i < fsi->num_files; i++) {
            if (!myfs_read_file_data(fsi, (int)i, buf))
                fsi->files[i].crc32 = crc32(0, buf, fsz);
            if (copy_to_user(&uinfo[i], &fsi->files[i],
                             sizeof(struct myfs_file_info))) {
                ret = -EFAULT;
                break;
            }
        }
        kfree(buf);
        pr_info("myfs: GET_META done (%u files)\n", fsi->num_files);
        break;
    }

    /* ── Маппинг секторов для файла (по имени) ── */
    case MYFS_IOC_GET_SECTOR_MAP: {
        struct myfs_sector_map map;
        bool found = false;
        if (copy_from_user(&map, (void __user *)arg, sizeof(map))) {
            ret = -EFAULT;
            break;
        }
        map.name[sizeof(map.name) - 1] = '\0';
        for (i = 0; i < fsi->num_files; i++) {
            if (strcmp(fsi->files[i].name, map.name) == 0) {
                map.file_index   = i;
                map.start_sector = fsi->files[i].start_sector;
                map.num_sectors  = fsi->files[i].num_sectors;
                map.sector_size  = fsi->sector_size;
                found = true;
                break;
            }
        }
        if (!found) { ret = -ENOENT; break; }
        if (copy_to_user((void __user *)arg, &map, sizeof(map))) {
            ret = -EFAULT;
            break;
        }
        pr_info("myfs: GET_SECTOR_MAP '%s' → idx=%u start=%u nsect=%u\n",
                map.name, map.file_index, map.start_sector, map.num_sectors);
        break;
    }

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&fsi->lock);
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
 * VFS суперблок: fill_super / put_super
 * ────────────────────────────────────────────── */

static void myfs_put_super(struct super_block *sb)
{
    struct myfs_fs_info *fsi = sb->s_fs_info;

    mutex_lock(&g_fsi_lock);
    if (g_fsi == fsi)
        g_fsi = NULL;
    mutex_unlock(&g_fsi_lock);

    if (fsi) {
        /* Дожидаемся завершения IOCTL, удерживающего мьютекс ФС */
        mutex_lock(&fsi->lock);
        mutex_unlock(&fsi->lock);
        if (fsi->bdev_file)
            fput(fsi->bdev_file);
        mutex_destroy(&fsi->lock);
        kfree(fsi->files);
        kfree(fsi);
        sb->s_fs_info = NULL;
    }
    pr_info("myfs: unmounted\n");
}

static const struct super_operations myfs_super_ops = {
    .put_super  = myfs_put_super,
    .statfs     = simple_statfs,
    .drop_inode = generic_drop_inode,
};

static int myfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct myfs_fs_info     *fsi;
    struct myfs_super_block *disk_sb;
    struct inode            *root_inode;
    struct dentry           *root_dentry;
    struct file             *bdev_file;
    struct timespec64        ts;
    char                     dev_path[80];
    unsigned int             i;
    int                      ret;

    snprintf(dev_path, sizeof(dev_path), "/dev/%s", myfs_dev);

    bdev_file = bdev_file_open_by_path(dev_path,
                                       BLK_OPEN_READ | BLK_OPEN_WRITE,
                                       NULL, NULL);
    if (IS_ERR(bdev_file)) {
        pr_err("myfs: cannot open %s: %ld\n", dev_path, PTR_ERR(bdev_file));
        return PTR_ERR(bdev_file);
    }

    fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    if (!fsi) { ret = -ENOMEM; goto err_bdev; }

    fsi->bdev_file   = bdev_file;
    fsi->bdev        = file_bdev(bdev_file);
    fsi->invalidated = false;
    mutex_init(&fsi->lock);
    disk_sb = &fsi->sb_disk;

    /* Размер сектора определяем автоматически с устройства */
    fsi->sector_size = bdev_logical_block_size(fsi->bdev);
    if (fsi->sector_size < 512 || (fsi->sector_size & (fsi->sector_size - 1))) {
        pr_err("myfs: unsupported logical block size %u\n", fsi->sector_size);
        ret = -EINVAL;
        goto err_fsi;
    }

    if (format) {
        /* Явный запрос на форматирование */
        pr_info("myfs: format=1 — (re)formatting %s\n", dev_path);
        ret = myfs_format(fsi, (__u32)sb_offset_1, (__u32)sb_offset_2,
                          (__u32)max_name_len, (__u32)file_sectors);
        if (ret)
            goto err_fsi;
    } else {
        /*
         * Логика выбора суперблока:
         *   первичный исправен          → использовать его;
         *   первичный битый, резерв ОК   → использовать резерв и
         *                                  ВОССТАНОВИТЬ первичный;
         *   обе копии битые              → ошибка монтирования,
         *                                  НЕ форматировать (данные не теряем).
         */
        ret = myfs_read_sb_copy(fsi, (__u32)sb_offset_1, disk_sb);
        if (ret || myfs_verify_sb(disk_sb)) {
            struct myfs_super_block backup;
            pr_warn("myfs: primary SB invalid, trying backup\n");
            ret = myfs_read_sb_copy(fsi, (__u32)sb_offset_2, &backup);
            if (ret || myfs_verify_sb(&backup)) {
                pr_err("myfs: both superblocks invalid — refusing to mount "
                       "(use format=1 to create a new filesystem)\n");
                ret = -EINVAL;
                goto err_fsi;
            }
            *disk_sb = backup;
            /* Восстанавливаем первичную копию из резервной */
            ret = myfs_write_sb_copy(fsi, disk_sb->sb_offset_1, disk_sb);
            if (ret)
                pr_warn("myfs: failed to restore primary SB: %d\n", ret);
            else
                pr_info("myfs: primary SB restored from backup\n");
        }
        /* Доверяем размеру сектора из суперблока, но сверяем с устройством */
        if (disk_sb->sector_size != fsi->sector_size) {
            pr_err("myfs: SB sector_size %u != device %u — refusing to mount\n",
                   disk_sb->sector_size, fsi->sector_size);
            ret = -EINVAL;
            goto err_fsi;
        }
    }

    fsi->num_files = disk_sb->num_files;

    /* Строим in-memory массив метаданных файлов */
    fsi->files = kcalloc(fsi->num_files, sizeof(*fsi->files), GFP_KERNEL);
    if (!fsi->files) { ret = -ENOMEM; goto err_fsi; }

    for (i = 0; i < fsi->num_files; i++) {
        size_t cap = min((size_t)disk_sb->max_name_len,
                         sizeof(fsi->files[i].name));
        fsi->files[i].index        = i;
        fsi->files[i].start_sector = disk_sb->data_start + i * disk_sb->file_sectors;
        fsi->files[i].num_sectors  = disk_sb->file_sectors;
        snprintf(fsi->files[i].name, cap, "file%05u", i);
    }

    /* Настраиваем VFS суперблок */
    sb->s_magic          = MYFS_MAGIC;
    sb->s_op             = &myfs_super_ops;
    sb->s_fs_info        = fsi;
    sb->s_blocksize      = fsi->sector_size;
    sb->s_blocksize_bits = blksize_bits(fsi->sector_size);
    sb->s_maxbytes       = (loff_t)disk_sb->file_sectors * fsi->sector_size;

    /* Корневой inode (директория) */
    root_inode = new_inode(sb);
    if (!root_inode) { ret = -ENOMEM; goto err_files; }

    root_inode->i_ino  = 0;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_uid  = GLOBAL_ROOT_UID;
    root_inode->i_gid  = GLOBAL_ROOT_GID;
    ts = current_time(root_inode);
    inode_set_atime_to_ts(root_inode, ts);
    inode_set_mtime_to_ts(root_inode, ts);
    inode_set_ctime_to_ts(root_inode, ts);
    root_inode->i_op   = &myfs_dir_inode_ops;
    root_inode->i_fop  = &myfs_dir_ops;
    set_nlink(root_inode, 2);

    root_dentry = d_make_root(root_inode);   /* при ошибке сам делает iput */
    if (!root_dentry) { ret = -ENOMEM; goto err_files; }

    sb->s_root = root_dentry;

    mutex_lock(&g_fsi_lock);
    g_fsi = fsi;
    mutex_unlock(&g_fsi_lock);

    pr_info("myfs: mounted %s — %u files × %u sectors (sector_size=%u)\n",
            dev_path, fsi->num_files, disk_sb->file_sectors, fsi->sector_size);
    return 0;

err_files:
    kfree(fsi->files);
err_fsi:
    mutex_destroy(&fsi->lock);
    kfree(fsi);
err_bdev:
    fput(bdev_file);
    return ret;
}

static struct dentry *myfs_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev, void *data)
{
    return mount_nodev(fs_type, flags, data, myfs_fill_super);
}

static struct file_system_type myfs_type = {
    .owner   = THIS_MODULE,
    .name    = "myfs",
    .mount   = myfs_mount,
    .kill_sb = kill_anon_super,
};

/* ──────────────────────────────────────────────
 * Инициализация / выгрузка модуля
 * ────────────────────────────────────────────── */

static int __init myfs_init(void)
{
    int ret = register_filesystem(&myfs_type);
    if (ret) { pr_err("myfs: register_filesystem: %d\n", ret); return ret; }

    ret = misc_register(&myfs_miscdev);
    if (ret) {
        pr_err("myfs: misc_register: %d\n", ret);
        unregister_filesystem(&myfs_type);
        return ret;
    }

    pr_info("myfs: loaded dev=/dev/%s sb1=%d sb2=%d names=%d sects=%d format=%d\n",
            myfs_dev, sb_offset_1, sb_offset_2, max_name_len, file_sectors, format);
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
