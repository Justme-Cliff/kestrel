# Kestrel — design notes

> A lightweight Android emulator for macOS, written in C++, built from scratch,
> with the main goal of playing Free Fire with Mac keyboard and mouse controls.

How Kestrel is built and why: architecture, safety rules, components, plan and
status. When a decision in here changes, update this file in the same change.

---

## 1. What we are building

Kestrel is a native macOS app that runs a real Android system inside a
lightweight virtual machine and shows it in a Mac window. The user can install
Android games (primarily **Free Fire / Free Fire MAX**) and play them with the
keyboard and mouse through a built-in key-mapping layer.

Think "BlueStacks", but:

- **Light.** Low RAM, low CPU when idle, small download, no background services.
- **Native.** C++ core, Objective-C++ only where macOS APIs require it. No Electron,
  no Java, no bundled browser.
- **Clean.** Never touches the game's code or memory, so it does not trigger bans
  (see §4).
- **Ours.** We write the virtual machine monitor (VMM) and every virtual device
  ourselves. We reuse existing open-source code only where rewriting it would take
  years and add no value (see §3).

### 1.1 Target hardware

| Mac type | Support plan | How |
|---|---|---|
| **Apple Silicon** (M1–M4, A18 Pro, …) | **First target.** All early phases. | Apple `Hypervisor.framework` runs an **arm64** Android guest at near-native speed |
| **Intel Macs** (x86_64) | **Last phase (§6, Phase 10)**, high risk | `Hypervisor.framework` (VT-x) runs an **x86_64** Android guest, **plus** a translator for ARM game code (see §5.12) |

The developer's machine: **Apple A18 Pro, arm64, 8 GB RAM, macOS 26.** Everything
has to run well on this machine, which makes it our reference for a "light" Mac.

- Minimum macOS: **15** until Phase 8 (uses Apple's in-kernel GIC, §5.3). After
  our own GIC lands, the minimum drops to **13 Ventura**.
- Ship a **universal binary** (arm64 + x86_64) once Intel support exists.

### 1.2 Host operating systems: macOS, Linux, Windows

Kestrel must run on all three desktop OSes. Each OS has its own hypervisor API,
so the code that talks to it lives behind one interface, with a backend per OS:

| Host OS | Hypervisor API | Backend dir | When |
|---|---|---|---|
| macOS (Apple Silicon) | Hypervisor.framework | `src/hv/hvf/` | Now (Phases 0–9) |
| Linux (arm64 and x86_64) | KVM (`/dev/kvm`) | `src/hv/kvm/` | Phase 11 |
| Windows 10/11 (x86_64, arm64) | Windows Hypervisor Platform (WHP) | `src/hv/whp/` | Phase 12 |

Portability rules (apply from now on):
- Only `src/hv/<backend>/` may include hypervisor headers. Everything else uses
  the `Hypervisor` / `Vcpu` / `InterruptController` interfaces.
- Devices, virtio, boot, storage and keymapper code must be plain C++23 and must
  build on all three OSes. OS-specific I/O goes through `src/host/<os>/`.
- The UI is per-OS and thin: AppKit + Metal (macOS), Win32 + D3D12 or Vulkan
  (Windows), SDL/Wayland + Vulkan (Linux). The GPU path is Vulkan everywhere
  (MoltenVK only on macOS).
- **x86_64 PCs have the same problem as Intel Macs** (§5.12): Free Fire's code is
  ARM, so an x86 guest needs ARM translation. arm64 Linux and Windows-on-ARM hosts
  run the arm64 guest directly, like Apple Silicon.

### 1.3 Being honest about scope

This is a very large project. BlueStacks has had big teams working on it for more
than 10 years. A realistic timeline for one developer is
**1–2 years** to reach a smooth, playable Free Fire on Apple Silicon. Intel
support adds more on top. §6 splits the work into phases, and every phase ends
with something that runs and can be tested, so progress is always visible.

---

## 2. Architecture overview

```
┌──────────────────────────── macOS host (one process: Kestrel.app) ────────────────────────────┐
│                                                                                                │
│  UI thread (AppKit, .mm)          Keymapper                 Device I/O threads                 │
│  ┌──────────────────────┐   ┌────────────────────┐   ┌──────────────────────────────────────┐  │
│  │ Window + Metal layer │   │ keys/mouse → touch │   │ virtio-blk  virtio-net  virtio-snd   │  │
│  │ Settings, key editor │──▶│ profiles (JSON)    │──▶│ virtio-input virtio-gpu virtio-rng   │  │
│  └──────────────────────┘   └────────────────────┘   │ virtio-console virtio-balloon  vsock │  │
│            ▲                                         └──────────────▲───────────────────────┘  │
│            │ frames (Metal textures)                                │ MMIO exits / IRQs         │
│  ┌─────────┴────────────┐                             ┌─────────────┴────────────────────────┐  │
│  │ GPU renderer         │◀─── 3D command streams ─────│ VMM core: memory map, vCPU threads,  │  │
│  │ (gfxstream → Metal)  │                             │ interrupt controller, timers, PSCI   │  │
│  └──────────────────────┘                             └─────────────▲────────────────────────┘  │
│                                                                     │ hv_vcpu_run()             │
├─────────────────────────────────────────────────────────────────────┼──────────────────────────┤
│                        Apple Hypervisor.framework (hardware virtualization)                    │
├─────────────────────────────────────────────────────────────────────┼──────────────────────────┤
│  Guest VM:  Linux kernel (Android GKI, arm64)  →  Android (AOSP)  →  Free Fire                  │
└────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**How it works in one paragraph:** macOS gives us an empty virtual machine through
Hypervisor.framework. We map a block of our process memory into it as the guest's
RAM, load a Linux kernel and a device tree, and start the virtual CPUs. The guest
CPU runs at full hardware speed until it touches something that isn't plain RAM,
such as a device register. At that point the hardware pauses it and hands control
to us (a "VM exit"). We decode what the guest tried to do, emulate the device, and
resume the CPU. All devices are **virtio** devices, a standard designed for fast
virtual machines, and Linux and Android already include drivers for them. So we
don't write guest drivers: we write the host side.

---

## 3. What we write vs. what we reuse

The rule: **we write everything that is "the emulator".** We reuse the guest
operating system (we're building an emulator, not a new Android) and a few very
large graphics and networking libraries whose rewrite would take years with no
benefit to the user.

| Component | Write or reuse | Reason |
|---|---|---|
| VMM core (VM, memory, vCPUs, exits) | **Write** | This is the emulator |
| ARM64 platform: interrupt controller (GICv3), timers, PSCI, system registers | **Write** | Core of the emulator |
| Device tree (FDT) generator | **Write** | Small and simple |
| Boot loader (loading kernel, initrd, DTB) | **Write** | Small and simple |
| virtio transport + every virtio device | **Write** | This is the emulator |
| UART, RTC | **Write** | Small |
| Keymapper + profiles + editor UI | **Write** | Our main feature |
| macOS window, Metal display, audio, input capture | **Write** (Objective-C++) | Must be native |
| Disk image format (sparse copy-on-write) | **Write** | Simple and keeps things light |
| **Bring-up guest** (Phases 1–3) | Reuse: prebuilt **Alpine Linux** `virt` aarch64 kernel + initramfs | Nothing to compile |
| **Guest kernel** | Reuse: Google's **prebuilt** Android GKI kernel. Custom config only if needed, built in Docker on the Mac | It's Linux |
| **Guest OS** | Reuse: Google's **prebuilt AOSP Cuttlefish arm64** images from ci.android.com (Apache 2.0) | It's Android, and there's **no Linux build machine** (§5.10) |
| **GPU command translation** (guest OpenGL ES / Vulkan → Metal) | Reuse: **gfxstream** (Apache 2.0) + **MoltenVK** (Apache 2.0) | Writing a correct GLES/Vulkan-to-Metal translator is years of work. Google's own emulator uses these |
| **User-mode networking (NAT)** | **Write** (`src/net/user_net.cpp`): ARP, DHCP, DNS forwarding, UDP, ICMP echo, TCP termination | libslirp needs GLib (heavy). Ours is small, needs no root and no entitlements |
| Test framework | Reuse: GoogleTest | Standard |

Every reused library is added as a pinned git submodule under `third_party/`, with
its license in `third_party/LICENSES.md`. We never add a dependency without
updating this table.

---

## 4. Safety and anti-ban policy (MUST follow)

"Safe" means two things: safe for the **user's Mac**, and safe for the **user's
game account**.

### 4.1 How players actually get banned, and how we avoid it

Garena officially allows emulators. It detects them and puts emulator players in
**emulator-only matchmaking**, which is fair: keyboard and mouse against keyboard
and mouse. Emulator users who get banned are almost always caught doing one of
these things, and Kestrel never does any of them:

| Ban trigger | Kestrel's rule |
|---|---|
| Modified or repacked game APKs | **Never** modify, patch, re-sign or repack any game. The user installs the official game from the official store or official APK |
| Injecting code into the game (Frida, Xposed, Magisk modules, LD_PRELOAD) | **Never.** No hooks, no injection, no instrumentation in the guest |
| Reading or writing game memory | **Never.** The host never inspects guest memory belonging to app processes |
| Rooted system visible to apps | The shipped Android image is a **user** build with no `su` and no root. Debug images with root are for development only and must never be used to play online |
| Macros, auto-fire, turbo, recoil scripts, aim assist | **Never.** One user input produces exactly one touch action. No timed sequences, no automatic repeats, no pixel/screen reading to aim |
| Hiding the emulator or faking a real phone's identity | **Never.** Kestrel honestly reports itself as a virtual device. Spoofing device identity to get into mobile lobbies is itself treated as cheating and is a common ban reason |
| Speed hacks / clock manipulation | Guest time always follows real time. Only the normal clock is exposed |

So the anti-ban strategy is **to be a clean, honest emulator**, like the ones Garena
already tolerates. Kestrel refuses any feature that falls into the table above,
even if it's framed as a "safety" improvement.

### 4.2 Safety for the Mac

- **No root, no kernel extensions, no drivers.** Only user-space APIs.
- **One entitlement:** `com.apple.security.hypervisor`. Nothing else unless this
  file is updated with the reason.
- **App Sandbox** enabled for the release build. The app only accesses its own
  container (disk images, settings) and files the user explicitly picks.
- **Every guest-to-host input is untrusted.** The guest controls virtqueue
  descriptors, MMIO values and GPU command streams. All of it must be
  bounds-checked (§7.3). A bug there could let a guest escape to the host.
- Networking is user-mode NAT only. The guest can't reach the Mac's services
  unless forwarded, and nothing listens on external interfaces.
- Release builds are code-signed and notarized. Updates are signed.

### 4.3 Licensing and Google apps

- AOSP is Apache 2.0. The kernel is GPL, so we publish our kernel source and config.
- **Google Play Store / Play Services can't be redistributed** without a Google
  license (BlueStacks has one; we don't). Plan: ship clean AOSP. The user installs
  Free Fire from an official source. We'll look into Play Services options in Phase 8
  and document what's legal before doing anything.
- Some games check **Play Integrity**. We don't fake or bypass it (see §4.1). If a
  feature needs it, we document the limitation.

---

## 5. Components in detail

Each subsection describes one part of the emulator: what it does, how it works,
and the main pitfalls.

### 5.1 VMM core (`src/core/`)

**Job:** own the virtual machine's lifetime.

- `Vm`: creates the VM (`hv_vm_create` with an `hv_vm_config` that sets the IPA
  size), owns the guest memory map, and starts and stops vCPUs. RAII: the
  destructor always tears down cleanly.
- `GuestMemory`: guest RAM is one big `mmap(MAP_ANON | MAP_PRIVATE)` region
  mapped into the guest with `hv_vm_map`. **Untouched pages cost no real RAM**,
  which is key for lightness. Provides safe accessors
  `read(gpa, span)`/`write(gpa, span)`/`translate(gpa, len) -> optional<span>`
  that always bounds-check. Host page size is 16 KB on Apple Silicon, so every
  mapping must be 16 KB aligned.
- `MmioBus`: a sorted map of `[base, size) → Device*`. When a vCPU exits on a
  memory access outside RAM, the bus finds the device and calls
  `device->mmio_read/mmio_write(offset, size)`.
- `VcpuThread`: **one host thread per guest CPU.** Hypervisor.framework requires
  that a vCPU is only used from the thread that created it. The loop:
  `hv_vcpu_run` → inspect the exit → handle it → repeat.
- `EventLoop`: a kqueue-based loop for device I/O threads (disk, network, timers).

**Guest physical memory map (arm64):**

| Address | Size | What |
|---|---|---|
| `0x0800_0000` | 64 KB | GIC distributor |
| `0x0900_0000` | 4 KB | PL011 UART (early debug console) |
| `0x0901_0000` | 4 KB | PL031 RTC |
| `0x0A00_0000` | 512 B each | virtio-mmio devices (32 slots) |
| `0x0C00_0000` | 32 MB | GIC redistributors (Apple's `hv_gic` reserves 32 MB for all vCPUs) |
| `0x1000_0000` | … | reserved for PCI (later, if needed) |
| `0x4000_0000` | guest RAM | default 3 GB, user-configurable |

This layout is modeled on QEMU's `virt` board, so kernel configs and debugging
knowledge carry over.

### 5.2 ARM64 vCPU and exits (`src/arch/arm64/`)

`hv_vcpu_run` returns an exit reason. We handle:

| Exit | Meaning | Handler |
|---|---|---|
| `HV_EXIT_REASON_EXCEPTION`, EC `0x24` (data abort) | Guest touched MMIO | Decode `ESR_EL2` syndrome (ISV, SAS, SRT, WnR) → access size, register, direction → `MmioBus`. Advance PC by 4 |
| EC `0x16` (HVC) / `0x17` (SMC) | Firmware call | **PSCI**: CPU_ON (start secondary CPUs), CPU_OFF, SYSTEM_OFF, SYSTEM_RESET, PSCI_VERSION, FEATURES |
| EC `0x01` (WFI/WFE) | Guest is idle | **Sleep the thread** until the next timer deadline or interrupt. This is what makes idle CPU usage near zero |
| EC `0x18` (sysreg trap) | Guest accessed a trapped system register | Emulate or return a safe value. Log unknown ones |
| `HV_EXIT_REASON_VTIMER_ACTIVATED` | Guest's virtual timer fired | Raise the timer PPI (IRQ 27) in the GIC, mask the vtimer until the guest acknowledges it |
| `HV_EXIT_REASON_CANCELED` | We asked the vCPU to stop (`hv_vcpus_exit`) | Handle pause, shutdown, interrupt injection |

**Pitfall:** instructions without a valid syndrome (ISV=0, e.g. load/store pair to
MMIO) must be decoded manually from the instruction bytes. Linux rarely does this
to MMIO, but we log and handle it instead of crashing.

### 5.3 Interrupt controller: GICv3 (`src/arch/arm64/gic/`)

**Job:** deliver interrupts from devices to guest CPUs.

- **Bring-up (Phases 1–3): use Apple's in-kernel `hv_gic` (macOS 15+)**. The dev
  Mac runs macOS 26, so this skips weeks of work and gets Linux booting fast.
- **Later (Phase 8): our own user-space GICv3** (distributor + redistributors,
  injected with `hv_vcpu_set_pending_interrupt`) so macOS 13–14 Macs work too.
- Both sit behind one interface (`IInterruptController`), so the rest of the code
  doesn't change.
- Interrupt types: SGIs (CPU-to-CPU), PPIs (timer), SPIs (virtio devices, UART).
- **Pitfall:** interrupt handling is the #1 source of hangs and "boots but
  freezes" bugs. It has thorough unit tests (§8).

### 5.4 Boot (`src/boot/`)

1. Load the kernel `Image` at RAM base + 2 MB (arm64 boot protocol).
2. Load the initramfs (the Android ramdisk) above it.
3. Generate a **flattened device tree (FDT)** describing CPUs, memory, GIC, timer,
   PSCI, UART, RTC, every virtio-mmio slot and the kernel command line. We write
   our own small FDT builder (`FdtWriter`).
4. Set vCPU 0: `PC = kernel entry`, `X0 = DTB address`, `PSTATE = EL1h`, all
   interrupts masked. Other vCPUs wait for PSCI CPU_ON.

No UEFI and no bootloader: direct kernel boot is faster and lighter.

### 5.5 virtio (`src/devices/virtio/`)

**Transport:** virtio-mmio v2 for bring-up (simple, no PCI needed). Each device is
a 512-byte register window plus one SPI interrupt. Cuttlefish Android probably
expects **virtio-PCI** (a PCIe host bridge at `0x1000_0000`, ECAM config space,
MSI or INTx). Confirm at the start of Phase 4 by reading Cuttlefish's device tree
and kernel config, then add a `virtio-pci` transport. Devices are written against a
transport-independent `VirtioDevice` interface, so they work with both.

**Virtqueues** (`Virtqueue` class): split rings (descriptor table, available
ring, used ring) in guest memory. **Every descriptor address and length is
validated** against guest RAM before use. Chains are length-limited so a
malicious loop can't hang us. Event-index notification suppression is supported
to reduce exits (performance).

| Device | Job | Host side |
|---|---|---|
| **virtio-console** | Early boot logs, `adb` data channel | Terminal / log file |
| **virtio-blk** | Android `system`, `vendor`, `userdata` disks | Our sparse COW disk format (§5.9), async I/O on an I/O thread |
| **virtio-rng** | Randomness for the guest | `arc4random_buf` |
| **virtio-net** | Internet | Our user-mode NAT `net::UserNet` (TCP **and UDP**, since Free Fire uses UDP) |
| **virtio-input** | Touchscreen + keyboard | Fed by the keymapper (§5.7) |
| **virtio-gpu** | Display + 3D acceleration | §5.6 |
| **virtio-snd** | Game audio + mic | CoreAudio (§5.8) |
| **virtio-balloon** | Give unused guest RAM back to macOS | Free-page reporting → `madvise(MADV_FREE)` on the host (lightness) |
| **virtio-vsock** | Host↔guest control channel (our helper service, install APK, clipboard) | Unix socket on the host |

### 5.6 Graphics (`src/devices/virtio/gpu/`, `src/host/macos/render/`)

The hardest part for performance.

- **Guest side:** Android uses the gfxstream guest libraries (from AOSP) as its
  OpenGL ES / Vulkan driver. They encode API calls into a command stream and send
  it through virtio-gpu.
- **Our virtio-gpu device:** implements the virtio-gpu protocol (resources,
  contexts, blob resources, fences, cursor) and forwards 3D command streams to
  the host renderer.
- **Host renderer:** gfxstream host decoder → Vulkan → **MoltenVK** → **Metal**.
  Final frames are shown in a `CAMetalLayer`.
- **Zero-copy:** use blob resources so textures are shared through memory
  mapping instead of being copied every frame.
- **Frame pacing:** present on display vsync (`CADisplayLink`), support 60 and
  120 Hz, and never block the vCPU thread waiting on the GPU.
- **Fallback:** `--gpu software` runs Android with software rendering (SwiftShader in
  the guest) and virtio-gpu in 2D mode.
- **As built:** Vulkan-only host (`STREAM_RENDERER_FLAGS_USE_VK_BIT`): the guest uses
  ANGLE over Vulkan for GLES (`androidboot.hardware.egl=angle`, `vulkan=ranchu`,
  `gltransport=virtio-gpu-asg`), so the Mac needs no OpenGL. MoltenVK needs
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` for gfxstream's compositor shaders.
  Host-visible memory: 8 GB BAR in the 64-bit PCI window at 32 GB.

### 5.7 Keymapper (`src/keymap/`) — the main feature

**Job:** turn Mac keyboard and mouse input into touchscreen contacts on the
virtio-input device.

- The virtual touchscreen uses the Linux **multi-touch protocol B** (`ABS_MT_SLOT`,
  `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y`) with 10 slots. Touches enter the
  guest kernel as a real touchscreen. There's no adb, so latency stays under ~1 ms.
- **Control types** (each one is a single, direct mapping, see §4.1):
  - **Tap:** key → press/release at a point (jump, crouch, reload, …).
  - **D-pad / joystick:** WASD → one held contact moved around the joystick
    center. Diagonals are combined, and it releases when no key is held.
  - **Aim (mouse look):** the cursor is hidden and locked
    (`CGAssociateMouseAndMouseCursorPosition(false)`) and raw mouse deltas move a
    held contact in the camera area. When the contact nears the area's edge, it
    lifts and re-presses at the center, invisibly to the player. Sensitivity and
    Y-invert are settings.
  - **Fire:** left click → tap/hold at the fire button, while aiming stays active.
  - **Toggle aim mode:** a key (default: Left Alt or right-click) switches between
    aiming mode and free cursor mode (for menus).
- **Profiles:** JSON files per game (`profiles/freefire.json`), coordinates stored as
  **fractions of the screen (0.0–1.0)** so they work at any window size.
- **Editor:** an overlay on the game window where the user drags controls onto
  buttons.
- **Input capture:** `NSEvent` local monitors in the app window. We don't need
  system-wide keyboard hooks or the Accessibility permission, because the
  keymapper only acts when the Kestrel window has focus.

### 5.8 Audio (`src/host/macos/audio/`)

virtio-snd PCM streams → a lock-free ring buffer → a CoreAudio `AudioUnit` output
callback. Target latency ≤ 40 ms. Mic input only starts when the user enables it
(macOS will ask for permission).

### 5.9 Disks (`src/storage/`)

- **Base images** (`system.img`, `vendor.img`): read-only, shared, never modified.
- **Overlay format "KDSK":** our own simple copy-on-write format. Writes go to a
  sparse overlay file with a block map. That gives instant "factory reset" (delete
  the overlay), small disk usage, and multiple instances sharing one base image.
- `userdata` grows on demand (sparse), up to a cap the user sets.

### 5.10 Guest images (`guest/`) — no Linux build machine

**Rule: everything that runs inside the VM is downloaded prebuilt, or built on the
Mac. No Linux build server is needed.**

- **Bring-up (Phases 1–3):** Alpine Linux `virt` aarch64 kernel (`vmlinuz-virt`)
  and initramfs from the official Alpine mirrors. `guest/scripts/fetch-alpine.sh`
  downloads them and checks their SHA-256.
- **Android (Phase 4+):** Google's prebuilt **AOSP Cuttlefish** images
  (`aosp_cf_arm64_only_phone`, arm64) from **ci.android.com**. Cuttlefish is
  Google's Android made for virtual machines: it uses virtio devices and supports
  gfxstream graphics, which matches our design (§5.6).
  `guest/scripts/fetch-android.sh` downloads a **pinned build ID** (never
  "latest") and records it in `guest/BUILD_ID`.
  - Cuttlefish images normally boot through a bootloader (U-Boot) and use
    `super.img` / `vbmeta`. We boot the kernel and ramdisk directly (§5.4), and
    pass Android's needed boot arguments on the kernel command line (from
    Cuttlefish's `launch_cvd` configuration). Work out the exact list in Phase 4.
  - Our devices must match what Cuttlefish's Android expects: device types,
    vsock ports, console ports. Keep that list in `guest/android/CUTTLEFISH.md`.
- **Kernel:** use the kernel that ships with the Cuttlefish build. If we need a
  different config, build the Android Common Kernel in **Docker on the Mac**
  (`guest/scripts/build-kernel.sh`). A kernel build fits in 8 GB RAM and about
  30 GB of disk.
- **Customizing Android without rebuilding it:** on first boot, the host (over
  vsock/adb) installs our small helper APK, disables unneeded system apps
  (`pm disable-user`), sets zram and animation settings. All of it is scripted in
  `guest/firstboot/`.
- **Trade-off:** prebuilt images can't be trimmed as deeply as our own AOSP
  build, so Android uses a little more RAM and disk. If that becomes a real
  problem, a custom AOSP build (which needs a big Linux machine) stays a possible
  future option. It isn't part of the plan.
- **Release builds** must use a `user` (non-root) image, as §4.1 requires. Check
  whether ci.android.com publishes a `user` Cuttlefish variant. If only
  `userdebug` exists, the host must never enable root on it, and we must confirm
  it doesn't expose `su` to apps.
- **ABI note:** Apple Silicon can't run 32-bit ARM code (no AArch32 support). The
  guest is **arm64-only**, and apps must include arm64 libraries. Free Fire does.

### 5.11 macOS app (`src/app/`, `src/host/macos/`)

- AppKit (Objective-C++, `.mm` files). Only the thin UI layer is Obj-C++;
  logic stays in C++ behind interfaces.
- Main window: game view, a small toolbar (keymap editor, screenshot, volume,
  settings), and fullscreen.
- Settings: RAM (default 3 GB on 8 GB Macs), CPU count (default 4), resolution,
  FPS cap, mouse sensitivity.
- Install APK by drag-and-drop onto the window (sent over vsock to a guest helper).
- `Kestrel.app` bundle contains the binary, entitlements file and default
  profiles. Guest images download on first launch and are checked by SHA-256.

### 5.12 Intel Mac support (Phase 10, `src/arch/x86_64/`)

The hardest and least certain part. Two separate problems:

1. **The VMM:** Hypervisor.framework on Intel is low-level VT-x. We have to manage
   VMCS fields ourselves, decode x86 instructions for MMIO, and emulate a local
   APIC, I/O APIC and HPET/TSC. That's a new `arch/x86_64` backend behind the same
   interfaces as arm64.
2. **ARM game code on an x86 guest:** Free Fire ships ARM native libraries. An x86
   Android guest needs an **ARM→x86 binary translator** to run them. Existing ones
   are proprietary (Intel's Houdini, Google's libndk_translation). Our options are
   licensing one or writing our own, which is a project the size of all the
   previous phases combined.

Decide in Phase 10 after measuring the market. Until then, don't let x86 concerns
complicate the arm64 code: keep the arch interface clean and move on.

---

## 6. Build plan (phases)

Each phase ends with a **"Done when"** test that can be demonstrated. Don't start a
phase until the previous one is done. Estimates assume one developer working
part-time.

| # | Phase | Done when | Estimate |
|---|---|---|---|
| 0 | **Tooling:** git repo, CMake + Ninja, code signing with the hypervisor entitlement, GoogleTest, CI script, clang-format / clang-tidy | `ctest` passes, and a signed binary creates and destroys an empty VM | 1 week |
| 1 | **First instructions:** guest RAM, one vCPU, run a hand-written arm64 program that writes "hello" to our UART over MMIO | "hello" appears in the terminal from inside the VM | 1–2 weeks |
| 2 | **Linux boots:** FDT, boot protocol, PSCI, timer, `hv_gic`, PL011, PL031 | Prebuilt Alpine Linux boots to a shell on the UART, 1 CPU | 4–6 weeks |
| 3 | **Real VM:** SMP (4 vCPUs), virtio-mmio, blk, rng, net (own NAT), WFI idle sleep | Linux with 4 CPUs, a disk, `ping`/`curl` to the internet, <2% host CPU when idle | 4–6 weeks |
| 4 | **Android boots:** fetch pinned Cuttlefish arm64 images, virtio-pci transport (if needed), Android boot args, virtio-gpu 2D, software rendering, virtio-input (basic touch), KDSK disks | Android home screen in a Mac window, clickable with the mouse | 2–3 months |
| 5 | **GPU acceleration:** virtio-gpu 3D, gfxstream + MoltenVK, blob resources, vsync presentation | Android UI at 60 fps, and a 3D test app runs smoothly | 3–5 months |
| 6 | **Keymapper:** multi-touch, tap/joystick/aim/fire controls, profiles, editor overlay | Free Fire playable with WASD + mouse aim | 1–2 months |
| 7 | **Audio + polish:** virtio-snd/CoreAudio, balloon, vsock helper, APK drag-and-drop, clipboard | Full match with sound, APK install by drag-and-drop | 1–2 months |
| 8 | **Lightness + safety pass:** own GICv3 (macOS 13–14 support), profiling, RAM trimming, sandbox, fuzzing, notarization, Play Services legal research | Targets in §7.1 met, fuzzers run clean for 24 h | 1–2 months |
| 9 | **Release 1.0 (Apple Silicon)** | Signed, notarized app that friends can install | — |
| 10 | **x86_64 guest support** (§5.12): x86 backend for Intel Macs, ARM translation decision | x86 VMM boots Android; decision made on ARM translation | 6–12+ months |
| 11 | **Linux host** (§1.2): KVM backend, Linux UI (SDL + Vulkan), host I/O layer | Alpine then Android boot on an arm64 Linux machine; Free Fire playable | 1–3 months |
| 12 | **Windows host** (§1.2): WHP backend, Win32 UI (D3D12/Vulkan), host I/O layer | Android boots on Windows-on-ARM, then x86_64 with Phase 10 translation | 2–4 months |

**Update the "Current status" section (§11) at the end of every work session.**

---

## 7. Engineering rules

### 7.1 Lightness targets (measured, not guessed)

| Metric | Target |
|---|---|
| Kestrel host process overhead (excluding guest RAM) | < 150 MB |
| Guest RAM default on an 8 GB Mac | 3 GB, with ballooning returning unused memory |
| Host CPU while Android is idle | < 3% |
| Cold boot to home screen | < 20 s |
| Input latency (key press → guest touch event) | < 2 ms |
| App download (without guest images) | < 30 MB |

Rules that follow from this: no busy-waiting (always block/sleep); no per-frame
allocations in hot paths; no copying frames or disk blocks when a mapping will do;
lazy-load everything.

### 7.2 Code style

- **C++23**, Apple clang. `-Wall -Wextra -Wpedantic -Werror`.
- Objective-C++ (`.mm`) **only** in `src/host/macos/` and `src/app/`. The core must
  build without AppKit, which keeps it testable and portable.
- Errors: return `std::expected<T, Error>`. No exceptions across thread
  boundaries or in vCPU/device hot paths.
- RAII for every OS resource (VM, vCPU, file, mmap, Metal object).
- Naming: `PascalCase` types, `snake_case` functions and variables, `kPascalCase`
  constants, `member_` suffix for private members.
- Fixed-width integers (`uint32_t`, `uint64_t`) for anything guest-visible. Guest
  addresses use a `Gpa` strong type, so they can't be mixed with host pointers.
- `clang-format` (config in repo) runs before every commit.
- Comments explain **why**, especially for hardware or spec behavior. Cite the
  spec section (e.g. `// virtio 1.2 §2.7.7`).

### 7.3 Security rules for device code

- Treat all guest data as hostile. Every guest address, length, index and count is
  validated before use. Use `GuestMemory::translate()`; never do raw pointer math
  on guest addresses.
- Check for integer overflow on `addr + len`.
- Limit loop counts derived from guest data (descriptor chains, command buffers).
- Every device that parses guest data gets a **libFuzzer** target in `fuzz/`.

### 7.4 Threads

- vCPU threads: only run guest code and dispatch exits. They never block on disk
  or network. Slow work is posted to the device's I/O thread.
- Device state touched from multiple threads is protected by one mutex per
  device, or by lock-free queues for audio and input.
- UI and Metal presentation happen on the main thread only.

---

## 8. Testing

- **Unit tests** (GoogleTest, `tests/`): FDT output, virtqueue parsing, GIC state
  machine, ESR decoding, keymapper math, KDSK disk format. These run without
  Hypervisor.framework.
- **Integration tests** (`tests/integration/`): boot the Alpine guest headless
  and check for expected UART output (e.g. "login:"), with a timeout. They run on
  every change to `core/`, `arch/` or `devices/`.
- **Fuzzing** (`fuzz/`): virtqueue, virtio-gpu command parser, virtio-input config,
  FDT, KDSK.
- **Sanitizers:** debug builds use ASan + UBSan. A TSan build is used for
  threading work.
- **Performance checks:** a `bench/` tool records boot time, idle CPU and RSS, and
  compares them to §7.1.

A phase isn't done until its tests exist and pass.

---

## 9. Repository layout

```
emulator/
├── README.md
├── docs/DESIGN.md            ← this file
├── CMakeLists.txt
├── cmake/                    toolchain + signing helpers
├── kestrel.entitlements      com.apple.security.hypervisor (+ sandbox for release)
├── src/
│   ├── core/                 Vm, GuestMemory, MmioBus, VcpuThread, EventLoop, logging
│   ├── arch/
│   │   ├── arm64/            exits, ESR decode, sysregs, PSCI, timer, gic/
│   │   └── x86_64/           (Phase 10)
│   ├── boot/                 kernel loader, FdtWriter
│   ├── devices/
│   │   ├── platform/         pl011 UART, pl031 RTC
│   │   └── virtio/           mmio transport, Virtqueue, blk, net, console, rng,
│   │                         input, gpu/, snd, balloon, vsock
│   ├── storage/              KDSK copy-on-write disk format
│   ├── keymap/               controls, profiles, touch generator (pure C++)
│   ├── host/macos/           window, render (Metal), audio (CoreAudio), input capture (.mm)
│   └── app/                  AppKit app, settings, keymap editor UI (.mm)
├── guest/
│   ├── kernel/               optional kestrel.config fragment (Docker build)
│   ├── android/              CUTTLEFISH.md: device list, boot args, notes
│   ├── firstboot/            first-boot setup scripts (helper APK, disable apps)
│   ├── BUILD_ID              pinned ci.android.com build
│   ├── out/                  downloaded images (git-ignored)
│   └── scripts/              fetch-alpine.sh, fetch-android.sh, build-kernel.sh
├── profiles/                 default keymaps (freefire.json)
├── tests/                    unit + integration tests
├── fuzz/                     libFuzzer targets
├── bench/                    performance tools
├── tools/                    dev scripts (sign, run, debug)
└── third_party/              pinned submodules + LICENSES.md
```

---

## 10. Commands

```bash
# one-time setup
brew install ninja
git submodule update --init --recursive

# configure + build (add -DKESTREL_SANITIZE=ON for ASan/UBSan)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build   # also signs kestrel + kestrel_vm_tests with the hypervisor entitlement

# Phase 1 demo: guest program prints through our UART
./build/kestrel --hello

# tests
ctest --test-dir build --output-on-failure

# download the prebuilt bring-up guest (Alpine Linux)
./guest/scripts/fetch-alpine.sh

# run it (serial console in the terminal; Ctrl-] quits)
./build/kestrel --kernel guest/out/vmlinuz-virt --initrd guest/out/initramfs-virt

# Android: download the pinned Cuttlefish images once, then boot in a window
./guest/scripts/fetch-android.sh
./build/kestrel --android guest/out/android

# Android headless, for tests: screen to a file, scripted input over a socket
./build/kestrel --android guest/out/android --headless --screenshot /tmp/screen.ppm --control /tmp/k.sock
printf 'swipe 640 650 640 150 300\n' | nc -U /tmp/k.sock
```

Update this section whenever a command changes.

---

## 11. Current status

- **Done:** Phases 0–3 on macOS/Apple Silicon (Alpine Linux: 4 vCPUs, disk, internet,
  clock, RNG; idle CPU 0.2%).
- **Phase 4 mostly done:** the pinned Cuttlefish Android boots to the home screen
  in ~40 s (`./build/kestrel --android guest/out/android`), shown in a macOS window
  (`docs/android-in-mac-window.png`). Touch verified by script: swipe unlocks, tap
  opens an app (`--control` socket). Pieces: virtio-pci, GPT composite disk,
  bootconfig, virtio-gpu 2D, virtio-input, 20 hvc consoles (logcat to
  `instance/logcat.txt`), host side of the sensors HAL.
- **Known issues:**
  - (fixed) userdata now persists across reboots; `--wipe-data` is a factory reset.
    The earlier "new keys every boot" failure came from state corrupted by runs
    that deadlocked. Verified by `tests/integration/android_persistence.sh`.
  - adbd restarts once shortly after boot; scripts must retry `adb connect`.
  - Guest time zone is UTC. A "Screenshot taken with bug report" toast appeared
    once (cause unknown).
  - Mouse clicks in the window use the same touch path as the tested script, but
    haven't been clicked by hand yet.
- **adb + internet + APKs work:** virtio-vsock with a host bridge
  (`src/net/vsock_host.cpp`): `adb connect 127.0.0.1:6520` reaches adbd's vsock:5555.
  Three NICs like Cuttlefish (eth0 mobile, eth1 ethernet, eth2 wifi); after
  `guest/firstboot/android-setup.sh` Android is online over VirtWifi (DHCP from our
  NAT). F-Droid installed with `adb install`, launched, and loaded its catalog.
- **Kestrel.app** (`cmake --build build` makes `build/Kestrel.app`): double-click to
  run. First launch downloads the pinned Android build into
  `~/Library/Application Support/Kestrel/android` with a progress window. After
  boot, `AndroidAssistant` (built-in adb client, no SDK) turns wifi on, disables the
  lock screen and sets the host time zone. Menus: Install APK (also drag-and-drop
  or "Open With"), Home/Back/Recents, screenshot to Desktop, factory reset.
  Logs: `~/Library/Logs/Kestrel/`. CLI: `kestrel adb shell|install`.
- **Phase 5 core works:** `VirtioGpuGfxstream` (3D virtio-gpu: contexts, blobs,
  fences, host-visible memory mapped into the guest with `hv_vm_map`) on gfxstream +
  MoltenVK. Android reports `ANGLE (Apple, Vulkan 1.4 (Virtio-GPU GFXStream (Apple
  A18 Pro)))`. Default for Android; `--gpu software` falls back to 2D + SwiftShader.
  Frames are still read back to the CPU for display (zero-copy is a follow-up).
- **Instance lock:** a second VM on the same Android data is refused.
- **Phase 6 keymapper works** (`src/keymap/`, `src/app/game_controls.cpp`): JSON
  profiles in `~/Library/Application Support/Kestrel/profiles/` (default
  `freefire.json`, package `com.dts.freefireth`), switched on automatically when that
  app is in front. Controls: WASD joystick, Option (⌥) toggles aim mode (cursor captured,
  mouse turns the camera, left click fires, right click scopes), tap keys. Game
  menu: ⌘K on/off (+reload), ⇧⌘K opens the visual editor (drag markers onto the
  game's buttons, Save writes the profile; `docs/controls-editor.png`;
  `kestrel --preview-editor` shows it without a VM). Verified with Android's
  "show touches": all fingers land where the profile says
  (`docs/game-controls-touches.png`). Default button positions are estimates.
- **Sound works** (`virtio_snd.cpp` + `host/macos/core_audio.cpp`): Cuttlefish's card
  layout (stream 0 stereo out, stream 1 capture = silence). Playback buffers are read
  just before they play (~30 ms ahead) and returned once CoreAudio has played them,
  like a DMA sound card; returning them early caused ALSA underruns.
- **App polish (BlueStacks parity):** toolbar beside the game (Back, Home, Recents,
  Close app, volume, App Store, Install APK, send files, game controls + editor,
  screenshot, full screen, Settings); trackpad/wheel scrolling and pinch; right
  click = Back; ⌘V pastes Mac text; files dropped on the window go to Download.
  F-Droid + Aurora Store (Google Play catalog client) installed from f-droid.org on
  first boot. Settings sheet (resolution incl. match-screen, density, RAM, cores,
  graphics, aim sensitivity) in `settings.json`, applied by restarting Android.
  Density defaults to the tablet layout (`AppSettings::tablet_dpi`): the phone
  layout's "taskbar on phones" reserved a blank 72 px strip with no buttons.
  Android reboots restart the VM. Microphone via CoreAudio while an app records.
- **Stress tests (2026-09-26):** 60 min Free Fire soak in the app: 0 crashes, fd count flat,
  RSS flat at 1.3-1.5 GB. Input robot (87k steps, 10 min) plus a 200k-step random play
  unit test: no stuck fingers, no protocol errors. The controls editor reloads the
  profile from disk before editing, so Save can't write an older copy back.
- **Fixed:** crash on every quit (device threads raising IRQs after Machine's
  idle lock was destroyed); the old build rendered in software (slow).
- **Free Fire runs** on the Vulkan path with our own MoltenVK build
  (`tools/build-moltenvk.sh`, SPIRV-Cross patch for fragment output arrays; the app
  forces `VK_DRIVER_FILES` to the bundled copy). Host GLES via ANGLE Metal is opt-in
  (`KESTREL_HOST_GLES=1`): it stalls the Metal queue under load.
- **GPU commands** run on one worker thread in ring order (never on vCPU threads).
- **Network:** NAT TCP resends unacked data (RTO + fast retransmit) and supports
  window scaling: downloads ~4x faster, no stuck connections.
- **Lighter:** radio HAL gets `modem_simulator_ports` (stopped a restart loop of
  vendor.ril-daemon and com.android.phone ANRs); vCPU/GPU threads at
  USER_INTERACTIVE QoS; App Nap off while running. virtio-balloon with free page
  reporting is wired up, but this Cuttlefish image doesn't load the driver.
- **UI:** FPS readout (⇧⌘F, Settings), key hints over games (8 s on launch, or
  always: Game menu, eye button, ⌘/), aim toggle is Option (F1 = brightness on
  Macs), 35-key Free Fire layout measured from the real HUD, new app icon
  (`assets/icon/kestrel.svg`, `tools/make-icon.sh`).
- **v0.6 (upgrade round):**
  - Direct display: gfxstream presents frames straight into a Metal view behind our
    (transparent) game view (`stream_renderer_kestrel_show_window`, gfxstream patch);
    no per-frame readback. Screenshots read one frame back on demand.
  - Frame-rate cap = the display's refresh rate, sent as an EDID (`VIRTIO_GPU_F_EDID`,
    `src/devices/virtio/edid.cpp`); Android follows it (verified 30/60/120 Hz).
    Settings: Performance mode (Battery saver / Balanced / Max / Custom).
  - Boot: 18.6 s -> 6.4 s to home screen (whole-file reads instead of byte-by-byte;
    RelWithDebInfo is now the default build type). Quick Boot snapshots dropped: cold
    boot is already as fast as a RAM restore would be.
  - Slim Android: 24 unneeded system apps disabled once (`AndroidAssistant::slim_down`).
    Never disable com.android.bluetooth: system_server won't boot without it.
  - Pause when minimized (optional): vCPUs park in `wait_while_paused`; 0.3% CPU.
  - UI: boot splash with real progress (console milestones, `BootProgress`), one-time
    welcome, Library of apps with real names/icons (APK manifest + resources.arsc
    parser, `src/android/apk_resources.cpp`, cached per app version), controls editor
    with key rebinding, add/delete, right-click menu and per-game layouts
    (`profiles/layouts.json`), Help > Report a Problem (zip on the Desktop).
- **Free Fire "closes while downloading" = fd leak (fixed):** the game aborted with
  `acquireNextBufferLocked ... status=-24` (EMFILE) after ~9 min. Any app rendering
  through ANGLE on the gfxstream guest Vulkan driver leaked one `sync_file` per frame
  via VK_EXT_swapchain_maintenance1 present fences (found with strace on screenrecord,
  which renders the same way as the shell user). Fix: bootconfig
  `androidboot.hardware.angle_feature_overrides_disabled=supportsSwapchainMaintenance1`
  (Cuttlefish's init copies it into `debug.angle.feature_overrides_disabled`). Check
  with `grep FDSize /proc/$(pidof com.dts.freefireth)/status` (should stay ~1024).
- **Joystick:** pushes to 60% of the ring; pushing past it engages Free Fire's
  sprint lock (character kept running after keys were released).
- **v0.7 (2026-09-26):**
  - Aim: raw mouse via GameController (`host/macos/raw_mouse.mm`, auto-scaled to the
    old feel, setting to turn off); low-latency display = IMMEDIATE present in the
    window (`--low-latency`, setting); virtio-input never drops touch lifts when the
    guest stalls (backlog merges moves instead: `VirtioInput::compact_backlog`).
    `latency x y` control command + `tools/latency.py`: touch-to-frame 30 ms at 60 Hz,
    24 ms at 120 Hz (Android's own pipeline).
  - Performance: gfxstream render/post threads and the audio pacer at USER_INTERACTIVE;
    virtio_balloon now loads (read unmodified from system_dlkm by our own LP/erofs/LZ4
    readers, `boot/android_modules.cpp`, appended to the ramdisk module list).
  - Keymap: per-layout switch keys (aim mode and held WASD carry over), hold-to-use taps
    (scope), "Layout: X" notice, Game > Import/Export Controls, F4-F12, punctuation,
    middle/side mouse buttons. User profile: HUD 2 layout (old kept as "Old HUD").
  - Reliability: GPU stall watchdog (own condvar! sharing work_cv_ broke boot) with
    Restart Android prompt + thread sample; crash note -> report offer next launch;
    app-death notices from logcat (`app/app_watch.cpp`); `tools/soak.sh` (N-hour test
    on an APFS clone, `--input` robot, `--install-weekly`). gfxstream: post thread holds
    the surface lock (resize/close mid-frame crashed).
  - App: screen recording with sound (H.264/AAC, Movies/Kestrel, Shift-Cmd-R);
    multi-instance (File > New/Open Instance, `KESTREL_INSTANCE`, APFS-cloned images);
    Help > Check for Updates (feed in `KESTREL_UPDATE_URL`, empty = off);
    `tools/make-dmg.sh` (10 MB DMG; notarization needs a Developer ID).
  - Test tips: a second Kestrel takes adb port 6521 (`KESTREL_ADB_PORT=6521 kestrel adb`);
    never run tests against 6520 while the user's Kestrel is open.
- **Next:** 3-hour `tools/soak.sh` run when the user isn't playing; confirm recordings
  capture Free Fire's sound; user to fine-tune HUD 2 marker positions in the editor.
- **Performance notes:** Android frame times are healthy (median 6 ms, GPU 1 ms);
  WFI never traps with hv_gic (the kernel idles vCPUs); display readback ~1.4 ms
  per frame into an IOSurface.
- **Not done / can't be done here:**
  - Own GICv3 for macOS 13-14 (Phase 8), fuzzers, App Sandbox.
  - Notarization needs a paid Apple Developer ID (the user's account).
  - Intel Macs (Phase 10: x86 guest + ARM translation), Linux/KVM (Phase 11) and
    Windows/WHP (Phase 12): no such machines available to build or test on.
  - Google Play Services can't be shipped (licensing); Aurora Store covers downloads.
- **Dev machine:** Apple A18 Pro, 8 GB RAM, macOS 26.6.2, Apple clang 21, CMake 4.3, Ninja.

---
