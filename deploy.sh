#!/usr/bin/env bash
# deploy.sh — вспомогательный скрипт для загрузки/тестирования/выгрузки MyFS.
#
# Использование:
#   ./deploy.sh load    [dev] [sb1] [sb2] [name_len] [file_sectors]
#   ./deploy.sh mount   [mountpoint]
#   ./deploy.sh test    [mountpoint]
#   ./deploy.sh umount  [mountpoint]
#   ./deploy.sh unload
#   ./deploy.sh status
#
# Значения по умолчанию:
#   dev           = sdb
#   sb1           = 0
#   sb2           = 1
#   name_len      = 64
#   file_sectors  = 8
#   mountpoint    = /mnt

set -euo pipefail

MODULE_NAME="myfs"
KMOD="kernel/myfs.ko"
TOOL="userspace/myfs_tool"

DEV="${2:-sdb}"
SB1="${3:-0}"
SB2="${4:-1}"
NLEN="${5:-64}"
FSECT="${6:-8}"
MNT="${2:-/mnt}"

# Цвета для вывода
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR]${NC}  $*" >&2; }

cmd="${1:-help}"

case "$cmd" in

# ── Загрузка модуля ────────────────────────────────────────────────────────
load)
    if lsmod | grep -q "^${MODULE_NAME} "; then
        warn "Module '${MODULE_NAME}' already loaded. Unload first."
        exit 1
    fi
    info "Loading myfs.ko (dev=${DEV}, sb1=${SB1}, sb2=${SB2}, " \
         "name_len=${NLEN}, file_sectors=${FSECT})"
    insmod "${KMOD}" \
        dev_name="${DEV}" \
        sb_offset_1="${SB1}" \
        sb_offset_2="${SB2}" \
        max_name_len="${NLEN}" \
        file_sectors="${FSECT}"
    info "Module loaded. dmesg tail:"
    dmesg | tail -5
    ;;

# ── Монтирование ──────────────────────────────────────────────────────────
mount)
    info "Mounting myfs at ${MNT}"
    mkdir -p "${MNT}"
    mount -t myfs none "${MNT}"
    info "Mounted. Files:"
    ls -la "${MNT}"
    ;;

# ── Тест чтения/записи ───────────────────────────────────────────────────
test)
    if [ ! -x "${TOOL}" ]; then
        info "Building userspace tool..."
        make -C userspace
    fi
    info "Running R/W test on ${MNT}..."
    "${TOOL}" --test "${MNT}"
    ;;

# ── Размонтирование ───────────────────────────────────────────────────────
umount)
    info "Unmounting ${MNT}"
    umount "${MNT}"
    info "Done."
    ;;

# ── Выгрузка модуля ───────────────────────────────────────────────────────
unload)
    info "Removing module '${MODULE_NAME}'"
    rmmod "${MODULE_NAME}"
    info "Module removed."
    ;;

# ── Статус ────────────────────────────────────────────────────────────────
status)
    echo "=== lsmod ==="
    lsmod | grep "${MODULE_NAME}" || echo "  (not loaded)"
    echo ""
    echo "=== /proc/filesystems ==="
    grep "${MODULE_NAME}" /proc/filesystems || echo "  (not registered)"
    echo ""
    echo "=== /proc/mounts ==="
    grep "${MODULE_NAME}" /proc/mounts || echo "  (not mounted)"
    echo ""
    echo "=== /dev/myfs_ctl ==="
    ls -la /dev/myfs_ctl 2>/dev/null || echo "  (not present)"
    ;;

# ── Полный цикл: загрузить → смонтировать → тест → размонтировать → выгрузить
full)
    bash "$0" load  "${DEV}" "${SB1}" "${SB2}" "${NLEN}" "${FSECT}"
    bash "$0" mount /mnt
    bash "$0" test  /mnt
    echo ""
    read -r -p "Press Enter to unmount and unload..."
    bash "$0" umount /mnt
    bash "$0" unload
    ;;

help|--help|-h|*)
    echo "Usage: $0 {load|mount|umount|unload|test|status|full} [args]"
    echo ""
    echo "  load   [dev] [sb1] [sb2] [name_len] [file_sectors]"
    echo "  mount  [mountpoint]"
    echo "  test   [mountpoint]"
    echo "  umount [mountpoint]"
    echo "  unload"
    echo "  status"
    echo "  full   [dev] [sb1] [sb2] [name_len] [file_sectors]"
    ;;
esac
