#!/bin/bash
# Boot from USB-C (ABL "linux usb") leaves the kernel an initramfs that has to
# power the very port it booted from: on these boards VBUS sourcing lives behind
# the ADSP. Without it pmic-glink never brings up the charger PD service, so
# ucsi_glink never reaches ucsi_register(), no connector shows up under
# /sys/class/typec, no usb role switch is registered and the stick (or any
# keyboard) stays electrically invisible -> dracut-initqueue timeout.
#
# None of this needs Armada-specific kernel code: qcom_q6v5_pas, pmic_glink and
# ucsi_glink are all upstream and the board DT already sets
# compatible = "qcom,sm8550-adsp-pas" plus firmware-name. dracut simply never
# ships the pieces - its aarch64 module whitelist covers drivers/rpmsg and
# drivers/usb/* but not drivers/remoteproc, and it only pulls firmware that is
# baked into a module's MODULE_FIRMWARE metadata while the ADSP firmware name
# comes from the DT. Pull both in explicitly.
#
# Which ADSP images ride along is decided by the glob list below. The kernel
# already picks the right one at boot (firmware-name comes from the board DT,
# request_firmware() resolves it), so the list only has to say "every board's
# image": that keeps a single generic image bootable from USB on all of them.
# Raw cost is ~210 MB across boards, but the initramfs is compressed, so the
# image grows by far less.

ADSP_FW_LIST=/usr/share/armada/initramfs-adsp-firmware.list

check() {
    # 255 = explicit --add only, matching the other armada modules.
    return 255
}

depends() {
    echo systemd
    return 0
}

installkernel() {
    # qcom_q6v5_pas and its deps (q6v5_common, qcom_rproc_common, qcom_sysmon,
    # qcom_pil_info, mdt_loader) resolve through modules.dep; the directory is
    # a few hundred KB and keeps this working when a dependency moves.
    hostonly='' instmods '=drivers/remoteproc'
}

install() {
    local fw src
    [ -f "${ADSP_FW_LIST}" ] || return 1
    # Every entry is a glob (paths differ per board, e.g. the Ayaneo MDT set is
    # one .mdt plus dozens of .bNN segments). compgen -G expands them; a quoted
    # for-loop would keep the "*" literal. dracut ignores module_install's exit
    # code (dracut.sh calls it without || ...), so 55-generate-initramfs.sh
    # verifies the result: it checks the list against the build root before
    # dracut and against the initramfs after it.
    while read -r fw; do
        case "${fw}" in '' | '#'*) continue ;; esac
        while read -r src; do
            inst_simple "${src}" "${src}"
        done < <(compgen -G "/usr/lib/firmware/${fw}" || :)
    done < "${ADSP_FW_LIST}"
}
