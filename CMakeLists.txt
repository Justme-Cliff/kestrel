cmake_minimum_required(VERSION 3.25)
project(kestrel LANGUAGES C CXX OBJCXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)

# Optimized with symbols unless asked otherwise: the emulator's device and input
# paths need the speed, and crash logs stay readable.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
option(KESTREL_SANITIZE "Build with ASan + UBSan" OFF)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  message(FATAL_ERROR "Kestrel currently supports Apple Silicon (arm64) only. Intel is Phase 10.")
endif()

# Strict warnings for our code only (third-party code is not ours to fix).
set(KESTREL_WARNINGS -Wall -Wextra -Wpedantic -Werror)
if(KESTREL_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Every binary that touches Hypervisor.framework must carry the hypervisor entitlement.
function(kestrel_sign target)
  add_custom_command(TARGET ${target} POST_BUILD
    # Signed through a copy outside the (iCloud-synced) project folder: see tools/sign.sh.
    COMMAND ${CMAKE_SOURCE_DIR}/tools/sign.sh $<TARGET_FILE:${target}> ${CMAKE_SOURCE_DIR}/kestrel.entitlements
    COMMENT "Signing ${target} with hypervisor entitlement")
endfunction()

find_package(ZLIB REQUIRED)

# ---- GPU acceleration: gfxstream (Apache 2.0) built as its own CMake project ----
# Guest Vulkan/GLES command streams are decoded by gfxstream and run on the Mac's
# GPU through MoltenVK (docs/DESIGN.md §5.6).
include(ExternalProject)
set(GFXSTREAM_BUILD ${CMAKE_BINARY_DIR}/gfxstream)
set(GFXSTREAM_LIB ${GFXSTREAM_BUILD}/libgfxstream_backend.dylib)
ExternalProject_Add(gfxstream_ext
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/gfxstream
  BINARY_DIR ${GFXSTREAM_BUILD}
  CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEPENDENCY_RESOLUTION=AOSP
             -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
  BUILD_COMMAND ${CMAKE_COMMAND} --build ${GFXSTREAM_BUILD} --target gfxstream_backend
  INSTALL_COMMAND ""
  BUILD_ALWAYS ON  # let gfxstream's own (incremental) build notice source changes
  BUILD_BYPRODUCTS ${GFXSTREAM_LIB})
add_library(gfxstream_backend SHARED IMPORTED)
set_target_properties(gfxstream_backend PROPERTIES IMPORTED_LOCATION ${GFXSTREAM_LIB})
add_dependencies(gfxstream_backend gfxstream_ext)
set(GFXSTREAM_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/gfxstream/host/include)

# MoltenVK + Vulkan loader, found next to the program in lib64/vulkan (where
# gfxstream looks). MoltenVK is our patched build (third_party/patches/spirv-cross-kestrel.patch:
# Unity shaders with output arrays fail to compile otherwise), else Homebrew's.
set(KESTREL_MVK ${CMAKE_SOURCE_DIR}/third_party/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib)
if(EXISTS ${KESTREL_MVK})
  set(MOLTENVK_LIB ${KESTREL_MVK})
else()
  find_library(MOLTENVK_LIB MoltenVK PATHS /opt/homebrew/lib REQUIRED)
endif()
find_library(VULKAN_LOADER_LIB NAMES vulkan.1 vulkan PATHS /opt/homebrew/lib REQUIRED)

# Pure C++ logic: no Hypervisor.framework, unit-testable without signing.
add_library(kestrel_core STATIC
  src/core/log.cpp
  src/core/guest_memory.cpp
  src/core/mmio_bus.cpp
  src/arch/arm64/esr.cpp
  src/arch/arm64/psci.cpp
  src/boot/fdt_writer.cpp
  src/boot/kernel_image.cpp
  src/boot/virt_dtb.cpp
  src/boot/android_boot.cpp
  src/boot/android_modules.cpp
  src/boot/android_instance.cpp
  src/storage/android_sparse.cpp
  src/storage/composite_disk.cpp
  src/devices/platform/pl011.cpp
  src/devices/platform/pl031.cpp
  src/devices/virtio/virtqueue.cpp
  src/devices/virtio/virtio_transport.cpp
  src/devices/virtio/virtio_mmio.cpp
  src/devices/virtio/virtio_pci.cpp
  src/devices/pci/pci.cpp
  src/devices/virtio/virtio_rng.cpp
  src/devices/virtio/virtio_balloon.cpp
  src/devices/virtio/edid.cpp
  src/app/boot_progress.cpp
  src/android/apk_resources.cpp
  src/devices/virtio/virtio_blk.cpp
  src/devices/virtio/virtio_net.cpp
  src/devices/virtio/virtio_gpu.cpp
  src/devices/virtio/virtio_input.cpp
  src/devices/virtio/virtio_console.cpp
  src/devices/virtio/virtio_snd.cpp
  src/net/packet.cpp
  src/net/user_net.cpp
  src/net/vsock_host.cpp
  src/devices/virtio/virtio_vsock.cpp
  src/host/posix/block_file.cpp
  src/host/frame_file.cpp
  src/host/posix/control_server.cpp
  src/android/cuttlefish_sensors.cpp
  src/android/adb_client.cpp
  src/keymap/touch.cpp
  src/keymap/json.cpp
  src/keymap/keymapper.cpp
)
target_include_directories(kestrel_core PUBLIC src)
target_compile_options(kestrel_core PRIVATE ${KESTREL_WARNINGS})
target_link_libraries(kestrel_core PUBLIC ZLIB::ZLIB)

# Hypervisor.framework backend.
add_library(kestrel_hv STATIC
  src/core/machine.cpp
  src/hv/hvf/hvf_hypervisor.cpp
  src/hv/hvf/hvf_vcpu.cpp
  src/guest_programs/hello_program.cpp
)
target_link_libraries(kestrel_hv PUBLIC kestrel_core "-framework Hypervisor")
target_compile_options(kestrel_hv PRIVATE ${KESTREL_WARNINGS})

# macOS UI (Objective-C++, only here; docs/DESIGN.md §7.2).
add_library(kestrel_macos STATIC
  src/host/macos/mac_window.mm
  src/host/macos/raw_mouse.mm
  src/host/macos/recorder.mm
  src/host/macos/keycodes.cpp
  src/host/macos/core_audio.cpp
  src/app/android_assistant.cpp
  src/app/android_images.cpp
  src/app/game_controls.cpp
  src/app/app_watch.cpp
  src/app/crash_marker.cpp
  src/app/instances.cpp
  src/app/settings.cpp
  src/app/problem_report.cpp
)
target_link_libraries(kestrel_macos PUBLIC kestrel_core "-framework AppKit" "-framework QuartzCore"
  "-framework AudioToolbox" "-framework CoreAudio" "-framework IOSurface"
  "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework GameController" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
target_compile_options(kestrel_macos PRIVATE ${KESTREL_WARNINGS} $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# The 3D GPU device (links gfxstream).
add_library(kestrel_gpu STATIC src/devices/virtio/virtio_gpu_gfxstream.cpp)
target_include_directories(kestrel_gpu PRIVATE ${GFXSTREAM_INCLUDE})
target_link_libraries(kestrel_gpu PUBLIC kestrel_core gfxstream_backend)
target_compile_options(kestrel_gpu PRIVATE ${KESTREL_WARNINGS})

add_executable(kestrel src/main.cpp src/host/terminal.cpp)
target_link_libraries(kestrel PRIVATE kestrel_hv kestrel_macos kestrel_gpu)
set_target_properties(kestrel PROPERTIES BUILD_RPATH "@executable_path;${GFXSTREAM_BUILD}"
                                         INSTALL_RPATH "@executable_path")
# Vulkan runtime files next to the binary: lib64/vulkan/{libvulkan.dylib, libMoltenVK.dylib, ICD}.
set(KESTREL_VK_DIR ${CMAKE_BINARY_DIR}/lib64/vulkan)
file(MAKE_DIRECTORY ${KESTREL_VK_DIR})
file(WRITE ${KESTREL_VK_DIR}/MoltenVK_icd.json
  "{ \"file_format_version\": \"1.0.0\", \"ICD\": { \"library_path\": \"./libMoltenVK.dylib\", \"api_version\": \"1.4.0\", \"is_portability_driver\": true } }\n")
add_custom_command(TARGET kestrel POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${MOLTENVK_LIB} ${KESTREL_VK_DIR}/libMoltenVK.dylib
  COMMAND ${CMAKE_COMMAND} -E copy ${VULKAN_LOADER_LIB} ${KESTREL_VK_DIR}/libvulkan.dylib
  # ANGLE (OpenGL ES on Metal) in lib64/gles_angle: games' GLES runs on it (§5.6).
  COMMAND ${CMAKE_SOURCE_DIR}/tools/fetch-angle.sh ${CMAKE_BINARY_DIR}/lib64/gles_angle)
target_compile_options(kestrel PRIVATE ${KESTREL_WARNINGS})
kestrel_sign(kestrel)

# ---- tests ----
enable_testing()
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

add_executable(kestrel_unit_tests
  tests/esr_test.cpp
  tests/fdt_test.cpp
  tests/guest_memory_test.cpp
  tests/kernel_image_test.cpp
  tests/mmio_bus_test.cpp
  tests/pl011_test.cpp
  tests/psci_test.cpp
  tests/pl031_test.cpp
  tests/virtqueue_test.cpp
  tests/virtio_blk_test.cpp
  tests/user_net_test.cpp
  tests/pci_test.cpp
  tests/storage_test.cpp
  tests/android_boot_test.cpp
  tests/touch_test.cpp
  tests/android_modules_test.cpp
  tests/game_controls_test.cpp
  tests/app_watch_test.cpp
  src/app/app_watch.cpp
  src/app/crash_marker.cpp
  src/app/game_controls.cpp
  tests/keymapper_test.cpp
  tests/edid_test.cpp
  tests/boot_progress_test.cpp
  tests/apk_resources_test.cpp
)
target_link_libraries(kestrel_unit_tests PRIVATE kestrel_core GTest::gtest_main)
target_compile_options(kestrel_unit_tests PRIVATE ${KESTREL_WARNINGS})
add_test(NAME unit COMMAND kestrel_unit_tests)

add_executable(kestrel_vm_tests tests/vm_test.cpp)
target_link_libraries(kestrel_vm_tests PRIVATE kestrel_hv GTest::gtest_main)
target_compile_options(kestrel_vm_tests PRIVATE ${KESTREL_WARNINGS})
kestrel_sign(kestrel_vm_tests)
add_test(NAME vm COMMAND kestrel_vm_tests)

if(EXISTS ${CMAKE_SOURCE_DIR}/guest/out/vmlinuz-virt)
  add_test(NAME boot_alpine
           COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/boot_alpine.sh $<TARGET_FILE:kestrel>)
  set_tests_properties(boot_alpine PROPERTIES TIMEOUT 60)
endif()

# ---- Kestrel.app (double-clickable app bundle) ----
set(KESTREL_VERSION 0.7.0)
# Where Help > Check for Updates looks: a JSON file {"version", "url", "notes"} or a
# GitHub ".../releases/latest" API address. Empty: this build doesn't check.
set(KESTREL_UPDATE_URL "" CACHE STRING "Update feed URL for Kestrel.app")
target_compile_definitions(kestrel PRIVATE KESTREL_VERSION_STRING="${KESTREL_VERSION}")
set(KESTREL_APP ${CMAKE_BINARY_DIR}/Kestrel.app)
configure_file(packaging/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
add_custom_target(app ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${KESTREL_APP}/Contents/MacOS ${KESTREL_APP}/Contents/Resources
  # Copy then rename: replaces the file atomically, so a running Kestrel isn't corrupted.
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kestrel> ${KESTREL_APP}/Contents/MacOS/kestrel.new
  COMMAND ${CMAKE_COMMAND} -E rename ${KESTREL_APP}/Contents/MacOS/kestrel.new ${KESTREL_APP}/Contents/MacOS/kestrel
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/Info.plist ${KESTREL_APP}/Contents/Info.plist
  COMMAND ${CMAKE_COMMAND} -E copy ${GFXSTREAM_LIB} ${KESTREL_APP}/Contents/MacOS/libgfxstream_backend.dylib
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${KESTREL_VK_DIR} ${KESTREL_APP}/Contents/MacOS/lib64/vulkan
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/lib64/gles_angle ${KESTREL_APP}/Contents/MacOS/lib64/gles_angle
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/Kestrel.icns ${KESTREL_APP}/Contents/Resources/Kestrel.icns
  COMMAND ${CMAKE_SOURCE_DIR}/tools/sign.sh ${KESTREL_APP} ${CMAKE_SOURCE_DIR}/kestrel.entitlements --deep
  DEPENDS kestrel
  COMMENT "Building Kestrel.app")
