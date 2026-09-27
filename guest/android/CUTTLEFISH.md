# Booting Cuttlefish images in Kestrel

Pinned build: see `guest/BUILD_ID` (target `aosp_cf_arm64_only_phone-userdebug`;
ci.android.com publishes no `user` variant for this target).

## What the images need from the VM (found in Phase 4)

| Need | Why | Status |
|---|---|---|
| virtio over **PCI** | GKI kernel has `CONFIG_VIRTIO_MMIO` off, `CONFIG_PCI_HOST_GENERIC=y` | done |
| PCI host at `10000.pci` | `androidboot.boot_devices`; first-stage init only makes `/dev/block/by-name/*` for disks under it | done |
| One GPT disk with named partitions | fstab uses `/dev/block/by-name/{boot,init_boot,vendor_boot,vbmeta*,super,userdata,metadata,misc}` | done (`CompositeDisk`) |
| Bootconfig appended to the initrd | Android 12+ reads `androidboot.*` from `/proc/bootconfig`; cmdline needs `bootconfig` | done |
| Ramdisk = vendor ramdisk + init_boot ramdisk | what a bootloader does | done |
| `androidboot.vendor.apex.*` choices | apexd aborts if two APEXes provide one module (camera, keymint, ...) | done |
| virtio-gpu (2D is enough for software rendering) | minigbm gralloc needs `/dev/dri`; SurfaceFlinger waits for the allocator | next |
| virtio-input (multi-touch + keyboard) | touch | next |
| virtio-console ports hvc0..hvc12 | logcat, RIL, keymint-remote etc. talk over hvcN; order fixes the numbering | next |
| vsock | adb and some HALs | later |

## Boot parameters

Our defaults are in `src/boot/android_instance.cpp` (`kestrel_android_bootconfig`),
taken from Cuttlefish's `assemble_cvd/bootconfig_args.cpp` and
`vm_manager/crosvm_manager.cpp` (guest_swiftshader graphics mode). Override any of
them with `--bootconfig key=value`.

## Instance files

`<image dir>/instance/` holds `super.raw` (expanded once from the sparse
`super.img`), and blank sparse `userdata.img` (8 GB), `metadata.img`, `misc.img`.
Android formats userdata and metadata on first boot. Delete the folder for a
factory reset.
