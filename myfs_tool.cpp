/*
 * myfs_tool.cpp — userspace-программа для демонстрации и тестирования MyFS.
 *
 * Возможности:
 *   1. Обход всего диска: в каждый файл записывается случайное число,
 *      затем оно читается обратно — проверяется корректность.
 *   2. CLI-интерфейс для вызова IOCTL-команд:
 *        zero                  Обнулить все файлы
 *        erase                 Стереть ФС (обнулить суперблоки)
 *        metadata              Получить хеши всех файлов
 *        sectormap <filename>  Получить маппинг секторов для файла
 *
 * Сборка:
 *   g++ -std=c++17 -Wall -O2 -o myfs_tool myfs_tool.cpp
 *
 * Примеры:
 *   sudo ./myfs_tool test /mnt
 *   sudo ./myfs_tool zero
 *   sudo ./myfs_tool metadata /mnt
 *   sudo ./myfs_tool sectormap file00005
 *   sudo ./myfs_tool erase
 */

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

/* Общий заголовок с IOCTL-определениями (в корне проекта рядом с этим файлом) */
#include "myfs.h"

static constexpr const char *CTL_DEV = "/dev/myfs_ctl";

/* ──────────────────────────────────────────────────────────────────────── */
/* Вспомогательные функции                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

/** Открывает управляющее устройство. Бросает исключение при ошибке. */
static int open_ctl()
{
    int fd = ::open(CTL_DEV, O_RDWR);
    if (fd < 0)
        throw std::runtime_error(std::string("Cannot open ") +
                                 CTL_DEV + ": " + ::strerror(errno));
    return fd;
}

/** Рисует ASCII прогресс-бар на одной строке. */
static void print_progress(std::size_t done, std::size_t total)
{
    constexpr int W = 40;
    int pos = (total > 0)
        ? static_cast<int>(static_cast<double>(done) /
                           static_cast<double>(total) * W)
        : 0;
    std::cout << "\r[";
    for (int i = 0; i < W; ++i)
        std::cout << (i < pos ? '=' : (i == pos ? '>' : ' '));
    std::cout << "] " << done << "/" << total;
    std::cout.flush();
}

/* ──────────────────────────────────────────────────────────────────────── */
/* Тест чтения/записи                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * run_rw_test — для каждого regular-файла в mount_point:
 *   1. Открывает файл на запись, пишет случайный uint64_t в начало.
 *   2. Открывает на чтение, читает те же 8 байт.
 *   3. Сравнивает записанное и прочитанное.
 */
static void run_rw_test(const std::string &mount_point)
{
    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(mount_point))
        if (e.is_regular_file())
            files.push_back(e.path());

    if (files.empty()) {
        std::cerr << "No files found in " << mount_point << "\n";
        return;
    }
    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " files, running R/W test...\n";

    std::srand(static_cast<unsigned>(std::time(nullptr)));

    std::size_t passed = 0, failed = 0;

    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path &p = files[i];

        /* Случайное 64-битное число (два вызова rand() по 32 бита) */
        uint64_t value =
            (static_cast<uint64_t>(std::rand()) << 32) |
             static_cast<uint64_t>(std::rand());

        /* ── Запись ────────────────────────────────────────────── */
        {
            /* ios::in обязателен: без него fstream усечёт файл до 0 */
            std::fstream out(p, std::ios::in | std::ios::out | std::ios::binary);
            if (!out) {
                std::cerr << "\nWrite open error: " << p << "\n";
                ++failed; continue;
            }
            out.seekp(0);
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
            if (!out) {
                std::cerr << "\nWrite error: " << p << "\n";
                ++failed; continue;
            }
            out.flush();
        }

        /* ── Чтение и проверка ─────────────────────────────────── */
        {
            std::ifstream in(p, std::ios::binary);
            if (!in) {
                std::cerr << "\nRead open error: " << p << "\n";
                ++failed; continue;
            }
            uint64_t rb = 0;
            in.read(reinterpret_cast<char *>(&rb), sizeof(rb));

            if (rb == value) {
                ++passed;
            } else {
                std::cerr << "\nMISMATCH " << p.filename().string()
                          << ": wrote 0x" << std::hex << value
                          << " got 0x"    << rb       << std::dec << "\n";
                ++failed;
            }
        }

        print_progress(i + 1, files.size());
    }

    std::cout << "\n\nResults: " << passed << " passed, "
              << failed << " failed / " << files.size() << " total.\n";
    if (failed == 0)
        std::cout << "✓ All tests PASSED!\n";
    else
        std::cerr << "✗ " << failed << " tests FAILED!\n";
}

/* ──────────────────────────────────────────────────────────────────────── */
/* IOCTL-обёртки                                                            */
/* ──────────────────────────────────────────────────────────────────────── */

/** Обнулить содержимое всех файлов (MYFS_IOC_ZERO_ALL). */
static void ioctl_zero_all()
{
    int fd = open_ctl();
    int rc = ::ioctl(fd, MYFS_IOC_ZERO_ALL);
    ::close(fd);
    if (rc < 0)
        throw std::runtime_error(std::string("ZERO_ALL: ") + ::strerror(errno));
    std::cout << "✓ All files zeroed.\n";
}

/** Стереть ФС — обнулить оба суперблока (MYFS_IOC_ERASE_FS). */
static void ioctl_erase_fs()
{
    std::cout << "WARNING: this erases the filesystem superblocks!\n"
              << "Type \"yes\" to confirm: ";
    std::string ans;
    std::cin >> ans;
    if (ans != "yes") { std::cout << "Aborted.\n"; return; }

    int fd = open_ctl();
    int rc = ::ioctl(fd, MYFS_IOC_ERASE_FS);
    ::close(fd);
    if (rc < 0)
        throw std::runtime_error(std::string("ERASE_FS: ") + ::strerror(errno));
    std::cout << "✓ FS erased and invalidated. A plain re-mount will now FAIL;\n"
              << "  use format=1 module param to create the filesystem anew.\n";
}

/**
 * Вывести таблицу с метаинформацией (хеши CRC32) всех файлов.
 * Количество файлов определяется по содержимому точки монтирования.
 */
static void ioctl_get_meta(const std::string &mount_point)
{
    namespace fs = std::filesystem;

    std::size_t n = 0;
    for (const auto &e : fs::directory_iterator(mount_point))
        if (e.is_regular_file()) ++n;

    if (n == 0) { std::cerr << "No files in " << mount_point << "\n"; return; }

    std::vector<myfs_file_info> info(n);
    int fd = open_ctl();
    int rc = ::ioctl(fd, MYFS_IOC_GET_META, info.data());
    ::close(fd);
    if (rc < 0)
        throw std::runtime_error(std::string("GET_META: ") + ::strerror(errno));

    std::cout << std::left
              << std::setw(8)  << "Idx"
              << std::setw(16) << "Name"
              << std::setw(14) << "StartSect"
              << std::setw(10) << "Sectors"
              << "CRC32\n"
              << std::string(58, '-') << "\n";

    for (const auto &fi : info) {
        std::cout << std::left
                  << std::setw(8)  << fi.index
                  << std::setw(16) << fi.name
                  << std::setw(14) << fi.start_sector
                  << std::setw(10) << fi.num_sectors
                  << "0x" << std::hex << std::setfill('0')
                  << std::setw(8) << fi.crc32
                  << std::dec    << std::setfill(' ') << "\n";
    }
}

/** Вывести маппинг секторов для файла по имени. */
static void ioctl_sector_map(const std::string &filename)
{
    myfs_sector_map map{};
    std::strncpy(map.name, filename.c_str(), sizeof(map.name) - 1);

    int fd = open_ctl();
    int rc = ::ioctl(fd, MYFS_IOC_GET_SECTOR_MAP, &map);
    ::close(fd);
    if (rc < 0)
        throw std::runtime_error(std::string("GET_SECTOR_MAP '") + filename +
                                 "': " + ::strerror(errno));

    uint64_t b_start = static_cast<uint64_t>(map.start_sector) * map.sector_size;
    uint64_t b_end   = static_cast<uint64_t>(map.start_sector + map.num_sectors)
                       * map.sector_size - 1;

    std::cout << "File \"" << filename << "\" (#" << map.file_index << "):\n"
              << "  start_sector : " << map.start_sector << "\n"
              << "  num_sectors  : " << map.num_sectors  << "\n"
              << "  sector_size  : " << map.sector_size  << "\n"
              << "  byte range   : [" << b_start << " … " << b_end << "]\n";
}

/* ──────────────────────────────────────────────────────────────────────── */
/* Справка                                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    std::cout
        << "MyFS userspace tool\n\n"
        << "Usage: " << prog << " <command> [arg]\n\n"
        << "Commands:\n"
        << "  test [mount]          R/W test on all files (default: /mnt)\n"
        << "  zero                  IOCTL: zero all file contents\n"
        << "  erase                 IOCTL: wipe superblocks (invalidate FS)\n"
        << "  metadata [mount]      IOCTL: print CRC32 metadata table\n"
        << "  sectormap <filename>  IOCTL: sector mapping for a file\n"
        << "  --help, -h            This help\n\n"
        << "Most commands require root.\n";
}

/* ──────────────────────────────────────────────────────────────────────── */
/* main                                                                     */
/* ──────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) { print_usage(argv[0]); return 1; }
    const std::string cmd = argv[1];

    try {
        if      (cmd == "test")       run_rw_test(argc >= 3 ? argv[2] : "/mnt");
        else if (cmd == "zero")       ioctl_zero_all();
        else if (cmd == "erase")      ioctl_erase_fs();
        else if (cmd == "metadata")   ioctl_get_meta(argc >= 3 ? argv[2] : "/mnt");
        else if (cmd == "sectormap") {
            if (argc < 3) { std::cerr << "sectormap needs a filename\n"; return 1; }
            ioctl_sector_map(argv[2]);
        }
        else if (cmd == "--help" || cmd == "-h") print_usage(argv[0]);
        else { std::cerr << "Unknown: " << cmd << "\n"; print_usage(argv[0]); return 1; }
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
