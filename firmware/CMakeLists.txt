cmake_minimum_required(VERSION 3.16)

# This working tree is NFS-mounted. The component manager fetches managed_components/ deps via a
# local (APFS) temp-dir clone, then copies them onto this NFS mount. macOS stamps every freshly
# written file with a com.apple.provenance xattr; NFS can't store xattrs inline, so macOS
# externalizes each one into a "._<name>" AppleDouble sidecar file alongside it — binary metadata,
# not source. A dependency's own CMakeLists.txt commonly globs "*.c"/"*.cc" broadly and sweeps
# these up as if they were source, and the compiler chokes on the binary content ("null
# character(s) ignored [-Werror]"). Deleting the local component-manager cache's xattrs (once,
# manually) prevents this for registry deps, which reuse that cache — but git deps (e.g.
# ESPMicroSpeechFeatures, pulled by components/wakeword) re-clone into a fresh temp dir on every
# fetch, so there's no stable cache to pre-clean. Strip sidecars from managed_components/ here,
# before project() ever lets a component's build glob see them, so this is self-healing regardless
# of dependency type. (Root-caused during Billy's wake-word bring-up, 2026-07 — see project memory.)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
    file(GLOB_RECURSE __apple_sidecar_files "${CMAKE_CURRENT_LIST_DIR}/managed_components/._*")
    if(__apple_sidecar_files)
        file(REMOVE ${__apple_sidecar_files})
    endif()
    unset(__apple_sidecar_files)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(billy_fish)

# esp-tflite-micro (vendored TensorFlow Lite Micro, pulled by components/wakeword) compiles with
# -Wshadow warnings throughout its reference kernels (lambda params reusing an outer name) — not
# our code, nothing to fix on our side. Silence just this component's shadow warnings rather than
# weakening -Wshadow project-wide; harmless if the component isn't present in a given build.
idf_component_get_property(tflite_micro_lib espressif__esp-tflite-micro COMPONENT_LIB)
if(tflite_micro_lib)
    target_compile_options(${tflite_micro_lib} PRIVATE -Wno-shadow)
endif()
