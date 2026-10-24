# The Linux release: Zig's toolchain against glibc 2.28, so the .clap loads on Debian 10, Ubuntu 18.10,
# Fedora 29 and anything newer (a .clap built with the system compiler needs that system's glibc).
#   pip install ziglang
#   cmake -B build-zig -DCMAKE_TOOLCHAIN_FILE=cmake/zig-linux.cmake -DTS_ZIG=ON -DTS_BUILD_TESTS=OFF && cmake --build build-zig
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/../tools/zig/zcc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/../tools/zig/zcxx)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/../tools/zig/zar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/../tools/zig/zranlib CACHE FILEPATH "" FORCE)
