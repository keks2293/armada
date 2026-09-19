#!/usr/bin/bash

set -euo pipefail

# Only one automount at a time: udisks2 may otherwise race on its config.
if [[ "${FLOCKER:-}" != "$0" ]]; then
    exec env FLOCKER="$0" flock -e -w 20 "$0" "$0" "$@"
fi

. /usr/lib/hwsupport/common-functions

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
            OPTS="rw,noatime"
            FSCKTOOL="fsck.ext4"
            ;;
        f2fs)
            OPTS="rw,noatime"
            FSCKTOOL="fsck.f2fs"
            ;;
        btrfs)
            # btrfs is self-checking and must not be fsck'ed while active.
            OPTS="rw,noatime"
            FSCKTOOL=""
            ;;
        vfat)
            OPTS="rw,noatime,uid=${DECK_UID},gid=${DECK_GID},utf8=1,umask=000,flush"
            FSCKTOOL="fsck.vfat"
            UDISKS2_ALLOW='uid,gid,flush,utf8,shortname,umask,dmask,fmask,codepage,iocharset,usefree,showexec'
            ;;
        exfat)
            OPTS="rw,noatime,uid=${DECK_UID},gid=${DECK_GID}"
            FSCKTOOL="fsck.exfat"
            UDISKS2_ALLOW='uid,gid,dmask,errors,fmask,iocharset,namecase,umask'
            ;;
        ntfs)
            OPTS="rw,noatime,uid=${DECK_UID},gid=${DECK_GID}"
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
