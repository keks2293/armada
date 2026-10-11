#!/bin/bash
set -euxo pipefail

# Runs after 40-vendor-system-files: the initramfs bundles the armada splash,
# which is not yet installed when the kernel step runs.
KVER="$(ls /usr/lib/modules)"
IMG="/usr/lib/modules/${KVER}/initramfs.img"

# fedora-bootc ships /root -> var/roothome, absent in the build container;
# dracut-install aborts resolving /root without the target.
mkdir -p /var/roothome

# The ADSP firmware name comes from the DT, so MODULE_FIRMWARE introspection
# never finds it (91armada-adsp installs it explicitly). Without it the ADSP
# never boots, no VBUS is sourced and a USB boot hangs in initqueue. The same
# list tells the dracut module what to bundle, so it doubles as the contract.
FW_LIST="/usr/share/armada/initramfs-adsp-firmware.list"

# Fail before dracut if an entry matches nothing in the build root:
# 91armada-adsp/install() would skip it and dracut ignores module_install's
# exit code, so the check after dracut would be the first place to notice.
# compgen -G, not a quoted for-loop: glob chars inside "${fw}" would not
# expand in quotes.
missing_src=0
while read -r fw; do
    case "${fw}" in '' | '#'*) continue ;; esac
    if ! compgen -G "/usr/lib/firmware/${fw}" > /dev/null; then
        echo "ERROR: ${fw} matches nothing under /usr/lib/firmware" >&2
        missing_src=1
    fi
done < "${FW_LIST}"
if [ "${missing_src}" -ne 0 ]; then
    exit 1
fi

dracut \
    --force \
    --no-hostonly \
    --reproducible \
    --kver "${KVER}" \
    --add ostree \
    --add armada-splash \
    --add armada-ostree-fallback \
    --add armada-adsp \
    "${IMG}" "${KVER}"

# One listing snapshot for every check: `lsinitrd | grep -q` under
# `set -o pipefail` is a false-negative machine - grep exits at the first
# match, lsinitrd dies of SIGPIPE, and pipefail reports failure even though
# the file is present (this failed PR #704's build with both ADSP images
# "missing" although they were bundled).
listing="$(lsinitrd "${IMG}")"

# dracut drops modules silently: fail the build rather than ship without.
# Exact match on the last field: a substring test lets armada-splash-launcher
# pass for the binary.
required=(
    usr/lib/systemd/system/armada-splash-initrd.service \
    usr/lib/systemd/system/dracut-pre-mount.service.d/armada-splash.conf \
    usr/libexec/armada/armada-splash \
    usr/libexec/armada/armada-splash-launcher \
    usr/libexec/armada/device-env \
    usr/share/armada/splash/splash.asp \
    usr/libexec/armada/armada-ostree-fallback \
    usr/lib/systemd/system/ostree-prepare-root.service.d/armada-fallback.conf \
    usr/lib/ostree/ostree-prepare-root
    usr/lib/modules/${KVER}/kernel/drivers/remoteproc/qcom_q6v5_pas.ko
)

if ! awk '
    BEGIN {
        count = ARGC - 1
        for (i = 1; i <= count; i++) {
            paths[i] = ARGV[i]
            required[ARGV[i]] = 1
        }
        ARGC = 1
    }
    $NF in required { found[$NF] = 1 }
    END {
        for (i = 1; i <= count; i++) {
            if (!(paths[i] in found)) {
                print "ERROR: " paths[i] " missing from initramfs" > "/dev/stderr"
                missing = 1
            }
        }
        exit missing
    }
' "${required[@]}" <<< "${listing}"; then
    dracut --list-modules --kver "${KVER}" | grep -i armada || true
    exit 1
fi

# Same list, checked against the initramfs itself. Entries may be globs;
# expand each against the build root and require every match in the listing.
missing_fw=0
while read -r fw; do
    case "${fw}" in '' | '#'*) continue ;; esac
    # Glob-aware: iterate real matches, not the literal pattern.
    while read -r src; do
        rel="usr/lib/firmware/${src#/usr/lib/firmware/}"
        if ! awk -v path="${rel}" '
            $NF == path { found = 1 }
            END { exit !found }
        ' <<< "${listing}"; then
            echo "ERROR: ${rel} missing from initramfs" >&2
            missing_fw=1
        fi
    done < <(compgen -G "/usr/lib/firmware/${fw}" || :)
done < "${FW_LIST}"
if [ "${missing_fw}" -ne 0 ]; then
    exit 1
fi

echo "initramfs generated for ${KVER} with armada-splash"
