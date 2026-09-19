#!/usr/bin/env bash
#
# Guard removable-media automounting. Armada extends the SteamOS/jupiter
# helper (udev -> block-device-event.sh -> steamos-automount.sh) so SD
# cards AND USB drives auto-mount in Game Mode and the desktop alike, for
# every filesystem Steam can read (ext4/f2fs/btrfs/vfat/exfat/ntfs).

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/system_files/usr/lib/hwsupport/steamos-automount.sh"
UDEV="$ROOT/system_files/usr/lib/udev/rules.d/99-steamos-automount.rules"
BASE_PKGS="$ROOT/build_files/10-base-packages.sh"
AUTOMOUNT_CONF="$ROOT/system_files/etc/default/armada-automount"

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

[[ -f "$SCRIPT" ]] || fail "missing automount helper: $SCRIPT"
[[ -f "$UDEV" ]] || fail "missing udev rule: $UDEV"
[[ -f "$AUTOMOUNT_CONF" ]] || fail "missing /etc/default/armada-automount skeleton: $AUTOMOUNT_CONF"

# The standalone Python daemon and its session drop-ins are gone; the udev
# based helper needs no session plumbing because it runs outside sessions.
[[ ! -e "$ROOT/system_files/usr/libexec/armada/armada-automount" ]] \
    || fail "Python daemon must be removed: system_files/usr/libexec/armada/armada-automount"
[[ ! -e "$ROOT/system_files/usr/lib/systemd/user/armada-automount.service" ]] \
    || fail "Python daemon unit must be removed"
[[ ! -e "$ROOT/system_files/usr/lib/systemd/user/gamescope-session-plus@steam.service.d/20-armada-automount.conf" ]] \
    || fail "Game Mode drop-in must be removed (udev covers all sessions)"
[[ ! -e "$ROOT/system_files/usr/lib/systemd/user/plasma-workspace.target.d/armada-automount.conf" ]] \
    || fail "desktop drop-in must be removed (udev covers all sessions)"

[[ -x "$SCRIPT" ]] || fail "steamos-automount.sh must be executable"
bash -n "$SCRIPT"
bash -n "$BASE_PKGS"

# The ext4-only filter that blocked FAT/NTFS mounts in Game Mode is gone.
if grep -Eq 'ID_FS_TYPE[[:space:]]*!=[[:space:]]*"ext4"' "$SCRIPT"; then
    fail "ext4-only automount filter still present"
fi

# Every filesystem Steam can use as a library root (Steam formats to ext4,
# but users bring vfat/exfat/ntfs/f2fs/btrfs drives).
for fs in ext4 f2fs btrfs vfat exfat ntfs; do
    grep -Fq "${fs})" "$SCRIPT" || fail "missing ${fs} in filesystem table"
done

# FAT/NTFS mounts need uid/gid and udisks2 must be told to accept them.
grep -Fq 'UDISKS2_ALLOW' "$SCRIPT" || fail "missing udisks2 mount-option allowlist"
grep -Fq 'mount_options.conf' "$SCRIPT" || fail "missing mount_options.conf handling"
grep -Fq 'FSCKTOOL' "$SCRIPT" || fail "missing per-filesystem fsck handling"

# Per-system knobs, sourced from a root-owned /etc/default file like
# Bazzite's steamos-btrfs: mount options per fs and the btrfs subvolume.
grep -Fq '/etc/default/armada-automount' "$SCRIPT" || fail "missing /etc/default/armada-automount sourcing"
grep -Fq 'ARMADA_AUTOMOUNT_EXFAT_MOUNT_OPTS' "$SCRIPT" || fail "missing mount-option override knob"
grep -Fq 'ARMADA_AUTOMOUNT_BTRFS_SUBVOL' "$SCRIPT" || fail "missing btrfs subvolume override knob"
grep -Fq 'ARMADA_AUTOMOUNT_BTRFS_SUBVOL' "$AUTOMOUNT_CONF" \
    || fail "missing btrfs subvolume knob in config skeleton"

# NTFS must not create Windows-invalid file names (Proton crash source).
grep -Fq 'windows_names' "$SCRIPT" || fail "missing windows_names on NTFS mounts"

# btrfs niceties: mount the main subvolume, give Steam NOCOW downloads.
grep -Fq 'subvol=' "$SCRIPT" || fail "missing btrfs subvolume mount support"
grep -Fq 'btrfs subvolume create' "$SCRIPT" || fail "missing btrfs NOCOW subvolume creation"
grep -Fq 'chattr +C' "$SCRIPT" || fail "missing +C on btrfs download subvolumes"
grep -Fq 'rm -rf' "$SCRIPT" || fail "missing Bazzite-style discard of leftover download dirs on btrfs"
grep -Fq 'steamapps' "$SCRIPT" || fail "missing steamapps special-casing"

# compatdata bind is opt-in and off by default (same tradeoff as Bazzite).
grep -Fq 'ARMADA_AUTOMOUNT_COMPATDATA_BIND_MOUNT:-0' "$SCRIPT" \
    || fail "missing compatdata bind knob (default 0)"
grep -Fq 'mount --rbind' "$SCRIPT" || fail "missing compatdata bind mount"
grep -Fq 'umount -l -R' "$SCRIPT" || fail "missing lazy teardown of compatdata bind"
grep -Fq 'ARMADA_AUTOMOUNT_COMPATDATA_BIND_MOUNT' "$AUTOMOUNT_CONF" \
    || fail "missing compatdata knob in config skeleton"

# Removable-only: never grab the OS disk, internal eMMC or internal SATA.
grep -Fq 'armada_is_sd_storage_device' "$SCRIPT" || fail "missing SD device gate"
grep -Fq 'armada_is_system_storage_device' "$SCRIPT" || fail "missing system-storage guard"
grep -Fq 'is_removable_device' "$SCRIPT" || fail "missing removable-device gate"

# Still talks to udisks2 as the user, like Valve's helper.
grep -Fq 'make_dbus_udisks_call' "$SCRIPT" || fail "missing udisks2 Mount call"
grep -Fq 'as-user' "$SCRIPT" || fail "missing as-user mount"
grep -Fq 'auth.no_user_interaction' "$SCRIPT" || fail "missing no-prompt mount flag"

# The udev rule must watch SD partitions and USB drives (sd*), filtered to
# real filesystems, and kick off the helper without blocking udev.
grep -Fq 'mmcblk*p*|sd*' "$UDEV" || fail "udev rule must cover SD partitions and USB drives"
grep -Fq 'ID_FS_USAGE' "$UDEV" || fail "udev rule must filter by filesystem usage"
grep -Fq 'block-device-event.sh' "$UDEV" || fail "udev rule must invoke block-device-event.sh"

grep -Fq 'udisks2' "$BASE_PKGS"

echo "automount: udev/jupiter helper covers SD + USB for all filesystems"