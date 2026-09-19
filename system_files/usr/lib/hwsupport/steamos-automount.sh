#!/usr/bin/bash

set -euo pipefail

# Only one automount at a time: udisks2 may otherwise race on its config.
if [[ "${FLOCKER:-}" != "$0" ]]; then
    exec env FLOCKER="$0" flock -e -w 20 "$0" "$0" "$@"
fi

. /usr/lib/hwsupport/common-functions

# Optional per-system overrides (mount options per filesystem, btrfs mount
# subvolume). Same idea as Bazzite's /etc/default/steamos-btrfs. The file is
# root-owned on the image; nothing is sourced if it does not exist.
if [[ -f /etc/default/armada-automount ]]; then
    source /etc/default/armada-automount
fi

# Originally from https://serverfault.com/a/767079

# This script is called from our systemd unit file to mount or unmount
# a USB drive.

usage()
{
    echo "Usage: $0 {add|remove} device_name (e.g. sdb1)"
    exit 1
}

if [[ $# -ne 2 ]]; then
    usage
fi

ACTION=$1
DEVBASE=$2
DEVICE="/dev/${DEVBASE}"
DECK_UID=1000
DECK_GID=$(id -g "${DECK_UID}")
DECK_USER=$(id -nu "${DECK_UID}")

send_steam_url()
{
  local command="$1"
  local arg="$2"
  local encoded=$(urlencode "$arg")
  if pgrep -x "steam" > /dev/null; then
      # TODO use -ifrunning and check return value - if there was a steam process and it returns -1, the message wasn't sent
      # need to retry until either steam process is gone or -ifrunning returns 0, or timeout i guess
      systemd-run --uid="${DECK_UID}" --collect --wait sh -c "/usr/bin/steam steam://${command}/${encoded@Q}"
      echo "Sent URL to steam: steam://${command}/${arg} (steam://${command}/${encoded})"
  else
      echo "Could not send steam URL steam://${command}/${arg} (steam://${command}/${encoded}) -- steam not running"
  fi
}

# From https://gist.github.com/HazCod/da9ec610c3d50ebff7dd5e7cac76de05
urlencode()
{
    [ -z "$1" ] || echo -n "$@" | hexdump -v -e '/1 "%02x"' | sed 's/\(..\)/%\1/g'
}

is_removable_device()
{
    # /sys/block/<disk>/removable is 1 for card readers and USB sticks.
    local disk="${DEVBASE%%[0-9]*}"
    [[ -r "/sys/block/${disk}/removable" ]] || return 1
    [[ "$(cat "/sys/block/${disk}/removable")" == "1" ]]
}

do_mount()
{
    declare -i ret
    # NOTE: these values are ABI, since they are sent to the Steam client
    readonly FSCK_ERROR=1
    readonly MOUNT_ERROR=2

    # Only automount removable media: SD cards and USB drives. Internal
    # partitions (eMMC, OS disks) must never be grabbed on their own.
    case "${DEVBASE}" in
        mmcblk[0-9]p*)
            if ! armada_is_sd_storage_device "${DEVICE}"; then
                echo "Skipping non-SD mmcblk storage device ${DEVICE}"
                exit 0
            fi
            ;;
        sd[a-z]*)
            if ! is_removable_device; then
                echo "Skipping non-removable storage device ${DEVICE}"
                exit 0
            fi
            ;;
        *)
            echo "Skipping unsupported storage device ${DEVICE}"
            exit 0
            ;;
    esac

    if armada_is_system_storage_device "${DEVICE}"; then
        echo "Skipping system storage device ${DEVICE}"
        exit 0
    fi

    if armada_is_mounted_device "${DEVICE}"; then
        echo "Skipping already-mounted device ${DEVICE}"
        exit 0
    fi

    # Get info for this drive: $ID_FS_LABEL, and $ID_FS_TYPE
    dev_json=$(lsblk -o PATH,LABEL,FSTYPE --json -- "$DEVICE" | jq '.blockdevices[0]')
    ID_FS_LABEL=$(jq -r '.label | select(type == "string")' <<< "$dev_json")
    ID_FS_TYPE=$(jq -r '.fstype | select(type == "string")' <<< "$dev_json")

    # Filesystem-specific mount options. An external Steam library works on
    # any of these; FAT/NTFS need uid/gid so the files belong to the user.
    case "${ID_FS_TYPE}" in
        ext4)
            OPTS="${ARMADA_AUTOMOUNT_EXT4_MOUNT_OPTS:-rw,noatime}"
            FSCKTOOL="fsck.ext4"
            ;;
        # f2fs support is kept for parity with Bazzite. The Armada kernel
        # (7.2.3 as of writing) ships no f2fs module, so this branch is inert
        # until the kernel gains it; harmless in the meantime.
        f2fs)
            OPTS="${ARMADA_AUTOMOUNT_F2FS_MOUNT_OPTS:-rw,noatime}"
            FSCKTOOL="fsck.f2fs"
            ;;
        btrfs)
            # btrfs is self-checking and must not be fsck'ed while active.
            # These defaults are plain rw,noatime (Bazzite adds lazytime and
            # compress-force=zstd); both sides expose the same knobs via
            # /etc/default/armada-automount.
            OPTS="${ARMADA_AUTOMOUNT_BTRFS_MOUNT_OPTS:-rw,noatime}"
            FSCKTOOL=""
            # Mount the main subvolume the card was laid out with, instead of
            # showing the empty top-level that holds only subvolumes.
            if command -v btrfs > /dev/null 2>&1; then
                subvol="${ARMADA_AUTOMOUNT_BTRFS_SUBVOL-@}"
                mount_point_tmp="/var/run/armada-automount-${DEVBASE}.tmp"
                mkdir -p "${mount_point_tmp}"
                if [[ -n "${subvol}" ]] && /bin/mount -t btrfs -o ro "${DEVICE}" "${mount_point_tmp}" 2>/dev/null; then
                    if [[ -d "${mount_point_tmp}/${subvol}" ]] && \
                        btrfs subvolume show "${mount_point_tmp}/${subvol}" &>/dev/null; then
                        OPTS+=",subvol=${subvol}"
                    fi
                    /bin/umount -l "${mount_point_tmp}" 2>/dev/null || true
                fi
                rmdir "${mount_point_tmp}" 2>/dev/null || true
            fi
            ;;
        vfat)
            OPTS="${ARMADA_AUTOMOUNT_VFAT_MOUNT_OPTS:-rw,noatime,uid=${DECK_UID},gid=${DECK_GID},utf8=1,umask=000,flush}"
            FSCKTOOL="fsck.vfat"
            UDISKS2_ALLOW='uid,gid,flush,utf8,shortname,umask,dmask,fmask,codepage,iocharset,usefree,showexec'
            ;;
        exfat)
            OPTS="${ARMADA_AUTOMOUNT_EXFAT_MOUNT_OPTS:-rw,noatime,uid=${DECK_UID},gid=${DECK_GID}}"
            FSCKTOOL="fsck.exfat"
            UDISKS2_ALLOW='uid,gid,dmask,errors,fmask,iocharset,namecase,umask'
            ;;
        # Unlike Bazzite we mount NTFS with the in-kernel ntfs3 driver (present
        # in the Armada kernel) instead of remapping to userspace lowntfs-3g
        # and registering the fstype in /etc/filesystems.
        ntfs)
            OPTS="${ARMADA_AUTOMOUNT_NTFS_MOUNT_OPTS:-rw,noatime,uid=${DECK_UID},gid=${DECK_GID},windows_names}"
            FSCKTOOL="ntfsfix"
            UDISKS2_ALLOW='uid,gid,umask,dmask,fmask,locale,norecover,ignore_case,windows_names,nls,sparse,showmeta,prealloc'
            ;;
        *)
            echo "Error mounting ${DEVICE}: wrong fstype: ${ID_FS_TYPE} - ${dev_json}"
            exit 2
            ;;
    esac

    # udisks2 whitelists the mount options it will accept. For filesystems
    # that need uid/gid (FAT, NTFS) add those options for this mount only.
    if [[ -n "${UDISKS2_ALLOW:-}" ]]; then
        udisks2_mount_options_conf='/etc/udisks2/mount_options.conf'
        restore_udisks2_conf=
        mkdir -p "$(dirname "${udisks2_mount_options_conf}")"
        if [[ -f "${udisks2_mount_options_conf}" && ! -f "${udisks2_mount_options_conf}.orig" ]]; then
            cp -a "${udisks2_mount_options_conf}" "${udisks2_mount_options_conf}.orig"
            restore_udisks2_conf=1
        fi
        printf '[defaults]\n%s_allow=%s,%s\n' "${ID_FS_TYPE}" "${UDISKS2_ALLOW}" "${OPTS}" > "${udisks2_mount_options_conf}"
        cleanup_udisks2_conf()
        {
            rm -f "${udisks2_mount_options_conf}"
            if [[ "${restore_udisks2_conf}" == "1" ]]; then
                mv -f "${udisks2_mount_options_conf}.orig" "${udisks2_mount_options_conf}"
            fi
        }
        trap cleanup_udisks2_conf EXIT
    fi

    # Try to repair the filesystem if it's known to have errors.
    # ret=0 means no errors, 1 means that errors were corrected.
    # In all other cases we try to mount the fs read-only and report an error.
    # Unlike Bazzite's fsck."${ID_FS_TYPE}" -y (which would try the
    # nonexistent fsck.btrfs) the tool is picked per filesystem above, and
    # btrfs is never fsck'ed.
    ret=0
    if [[ -n "${FSCKTOOL}" ]] && command -v "${FSCKTOOL}" > /dev/null 2>&1; then
        if [[ "${FSCKTOOL}" == "ntfsfix" ]]; then
            ntfsfix "${DEVICE}" || ret=$?
        else
            "${FSCKTOOL}" -y "${DEVICE}" || ret=$?
        fi
    fi
    if (( ret != 0 && ret != 1 )); then
        send_steam_url "system/devicemountresult" "${DEVBASE}/${FSCK_ERROR}"
        echo "Error running fsck on ${DEVICE} (status = $ret)"
        OPTS+=",ro"
    else
        OPTS+=",rw"
    fi

    # Ask udisks to auto-mount. This needs a version of udisks that supports the 'as-user' option.
    # Unlike Bazzite we do not add a fstype s "$FSTYPE" variant here
    # ('a{sv}' 4): they use it to remap NTFS to userspace lowntfs-3g, while we
    # rely on the detected ID_FS_TYPE and the kernel ntfs3 driver.
    mount_point=$(make_dbus_udisks_call call 'data[0]' s         \
                                 "block_devices/${DEVBASE}"      \
                                 Filesystem Mount                \
                                 'a{sv}' 3                       \
                                 as-user s "${DECK_USER}"        \
                                 auth.no_user_interaction b true \
                                 options s "$OPTS")

    # Ensure that the armada user can write to the root directory
    if ! setpriv --clear-groups --reuid "${DECK_UID}" --regid "${DECK_GID}" test -w "${mount_point}"; then
        chmod 777 "${mount_point}" || true
    fi

    # Workaround for the Steam compression bug on btrfs: Steam rewrites its
    # downloads in-place, which fights COW — and resumed leftovers would keep
    # COW otherwise. Like Bazzite, force NOCOW subvolumes on every mount,
    # discarding any plain leftover folder so downloads always run NOCOW.
    if [[ "${ID_FS_TYPE}" == "btrfs" ]] && command -v btrfs > /dev/null 2>&1 && command -v chattr > /dev/null 2>&1; then
        mkdir -p "${mount_point}"/steamapps
        for d in "${mount_point}"/steamapps/{downloading,temp}; do
            if ! btrfs subvolume show "${d}" &>/dev/null; then
                rm -rf -- "${d}"
                btrfs subvolume create "${d}" &>/dev/null || true
                chattr +C "${d}" 2>/dev/null || true
            fi
            chown "${DECK_UID}:${DECK_GID}" "${d}" 2>/dev/null || true
        done
        chown "${DECK_UID}:${DECK_GID}" "${mount_point}"/steamapps 2>/dev/null || true
    elif [[ "${ARMADA_AUTOMOUNT_COMPATDATA_BIND_MOUNT:-0}" == "1" ]] && \
        [[ "${ID_FS_TYPE}" == "vfat" || "${ID_FS_TYPE}" == "exfat" || "${ID_FS_TYPE}" == "ntfs" ]]; then
        # Bind mount the compatdata folder from the internal disk so Proton
        # games on Windows-formatted drives get a prefix that supports
        # symlinks and exec bits. Opt-in only: this breaks Steam's eject
        # on the drive (same tradeoff as Bazzite, default there is 0 too).
        deck_home="$(getent passwd "${DECK_USER}" | cut -d: -f6)"
        mkdir -p "${mount_point}"/steamapps/compatdata
        chown "${DECK_UID}:${DECK_GID}" "${mount_point}"/steamapps{,/compatdata}
        mkdir -p "${deck_home}"/.local/share/Steam/steamapps/compatdata
        chown "${DECK_UID}:${DECK_GID}" "${deck_home}"/.local{,/share{,/Steam{,/steamapps{,/compatdata}}}}
        mount --rbind "${deck_home}"/.local/share/Steam/steamapps/compatdata \
            "${mount_point}"/steamapps/compatdata
    fi

    # Create a symlink from /run/media to keep compatibility with apps
    # that use the older mount point (for SD cards only).
    case "${DEVBASE}" in
        mmcblk[0-9]p*)
            if [[ -z "${ID_FS_LABEL}" ]]; then
                old_mount_point="/run/media/${DEVBASE}"
            else
                old_mount_point="/run/media/${mount_point##*/}"
            fi
            if [[ ! -d "${old_mount_point}" ]]; then
                rm -f -- "${old_mount_point}"
                ln -s -- "${mount_point}" "${old_mount_point}"
            fi
            ;;
    esac

    echo "**** Mounted ${DEVICE} at ${mount_point} ****"
}

do_unmount()
{
    local mount_point=$(findmnt -fno TARGET "${DEVICE}" || true)
    if [[ -n $mount_point ]]; then
        # Release the compatdata bind mount (if any) before teardown; it is
        # lazy because Steam may still hold the mount in its namespace.
        if mountpoint -q "${mount_point}"/steamapps/compatdata; then
            /bin/umount -l -R "${mount_point}"/steamapps/compatdata 2>/dev/null || true
        fi
        # Remove symlink to the mount point that we're unmounting
        find /run/media -maxdepth 1 -xdev -type l -lname "${mount_point}" -exec rm -- {} \;
    else
        # If we don't know the mount point then remove all broken symlinks
        find /run/media -maxdepth 1 -xdev -xtype l -exec rm -- {} \;
    fi
}

case "${ACTION}" in
    add)
        do_mount
        ;;
    remove)
        do_unmount
        ;;
    *)
        usage
        ;;
esac
