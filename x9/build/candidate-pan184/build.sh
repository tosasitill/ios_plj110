#!/bin/bash
set -euo pipefail
OUT="$(cd -- "$(dirname -- "$0")" && pwd)"
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin
export DEVELOPER_DIR=/Users/tosasitill/Downloads/Xcode.app/Contents/Developer
export SDKROOT=/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
export X9_CC="$OUT/build-tools/x9-clang"
export X9_LD=/opt/homebrew/opt/lld@22/bin/ld.lld
/opt/anaconda3/bin/python3 -I "$OUT/packaging-source/x9/tools/boot-xnu.py" \
  --kernel "$OUT/kernel.development.sweet" --out "$OUT" \
  --ram-window segmented8g --ufs-native-manifest "$OUT/root-manifest.json" --max-image-size 0x7b80000 \
  --boot-args 'x9_statusbar_provider=1 x9_pmic_keys=1 x9_gpu_dependencies=1 x9_gpu_power_probe=1 x9_csf_prepare=1 x9_csf_boot=1 x9_csf_queue_probe=1 x9_csf_indirect=1 x9_cs_compute=1 x9_network_stack=1 x9_touch_lbp=1 x9_ram_8g=1 x9_skip_setup=1 x9_watchdog_prefs=1 sweetfb_stayon=1 x9_migration_seed=1 x9_metal_render=1 x9_usb_log=1 x9_cryptex_aliases=1 x9_ufs_hs=1 launchd_unsecure_cache=1 -apfs_shared_datavolume ignition_level=1 x9_init_cache=1 x9_bootstrap_dirs=1 debug=0xc serial=3 cpus=8 keepsyms=1 -v startup_debug=1 unentitled_ios_sim_launch=1 rd=ufsroot0 rootrw=1 ufsrw=1 launchdsuffix=release -nobsdmgroot' \
  --console-base 0xaec0f000 --console-size 0x200000 --pstore-mirror
