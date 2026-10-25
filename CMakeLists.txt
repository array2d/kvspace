cmake_minimum_required(VERSION 3.10)
project(kvspace VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

# 默认 prefix=/usr（与 kvspace-c 对齐），前端与后端同装 <prefix>/lib/kvspace。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()

set(KVSPACE_SRC src/frontend.c src/codec_abi.c src/xvalue_head.c src/xvalue_meta.c)
add_library(kvspace SHARED ${KVSPACE_SRC})

# 静态库（单二进制）：同一份源码；后端通过 kvspaceStaticRegisterBackend 注册，
# 命中静态表时不 dlopen。共享库构建里注册符号是 hidden，公共 ABI 不变。
# 默认关：普通安装/CI 不产出 .a，只有单二进制构建显式开启。
option(KVSPACE_BUILD_STATIC "Build libkvspace-static.a for single-binary" OFF)
if(KVSPACE_BUILD_STATIC)
    add_library(kvspace-static STATIC ${KVSPACE_SRC})
    target_include_directories(kvspace-static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_definitions(kvspace-static PRIVATE _GNU_SOURCE KVSPACE_STATIC_BUILD)
    target_link_libraries(kvspace-static PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
    install(TARGETS kvspace-static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_xvalue_head tests/test_xvalue_head.c)
    target_include_directories(test_xvalue_head PRIVATE src)
    target_link_libraries(test_xvalue_head PRIVATE kvspace)
    add_test(NAME xvalue_head COMMAND test_xvalue_head)
    add_executable(test_codec_abi tests/test_codec_abi.c)
    target_include_directories(test_codec_abi PRIVATE src)
    target_link_libraries(test_codec_abi PRIVATE kvspace)
    add_test(NAME codec_abi COMMAND test_codec_abi)
endif()

# 前端 SONAME：libkvspace.so.1
set_target_properties(kvspace PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

target_include_directories(kvspace
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 活跃句柄表（退出兜底落盘）用 pthread 互斥；dlopen/dlsym 的宿主库 Linux 为 libdl，
# macOS 在 libSystem 内（CMAKE_DL_LIBS 两者皆正确）。
find_package(Threads REQUIRED)
target_link_libraries(kvspace PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

target_compile_definitions(kvspace PRIVATE _GNU_SOURCE)

install(TARGETS kvspace
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/xvalue_head.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xvalue_meta.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kvspace)

configure_file(kvspace.pc.in ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── CLI（cli/ 独立 Cargo 项目）──────────────────────────────────────
# 走本仓 dispatch 前端的 C ABI，故须在 kvspace 目标之后构建；
# 安装到 <prefix>/bin（prefix 默认 /usr → /usr/bin，与 kvlang 的 CLI 一致）。
find_program(CARGO_EXECUTABLE cargo)
if(CARGO_EXECUTABLE)
  set(KVSPACE_CLI_BIN "${CMAKE_CURRENT_SOURCE_DIR}/cli/target/release/kvspace")
  add_custom_target(kvspace-cli ALL
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=-L native=$<TARGET_FILE_DIR:kvspace>"
            "${CARGO_EXECUTABLE}" build --release
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/cli/Cargo.toml"
    BYPRODUCTS "${KVSPACE_CLI_BIN}"
    DEPENDS kvspace
    COMMENT "构建 kvspace CLI（cli/）")
  install(PROGRAMS "${KVSPACE_CLI_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  message(STATUS "未找到 cargo，跳过 kvspace CLI（cli/）")
endif()
