cmake_minimum_required(VERSION 3.20)
project(billy_cli C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(SOXR REQUIRED IMPORTED_TARGET soxr)
find_package(Threads REQUIRED)

add_executable(billy_cli
    main.c
    http.c
    json_util.c
    wav.c
    audio_io.c
)

target_link_libraries(billy_cli PRIVATE
    PkgConfig::CURL
    PkgConfig::PORTAUDIO
    PkgConfig::SOXR
    Threads::Threads
)
