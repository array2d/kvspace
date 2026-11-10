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

# frontend_storage.c 是新存储 ABI（storage.h）的 dispatch，独立 TU：新旧两份 ABI 头
# 在 kvspaceConnect/kvspaceClose/kvspaceCp 上同名、在 ListLen/Listlen 等上只差大小写，
# 同一个编译单元里无法共存。
add_library(kvspace SHARED
    src/frontend.c
    src/frontend_storage.c
    src/codec_abi.c
    src/xvalue_head.c
    src/kvlang_xvalue.c)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_xvalue_head tests/test_xvalue_head.c)
    target_include_directories(test_xvalue_head PRIVATE src include)
    target_link_libraries(test_xvalue_head PRIVATE kvspace)
    add_test(NAME xvalue_head COMMAND test_xvalue_head)
    add_executable(test_codec_abi tests/test_codec_abi.c)
    target_include_directories(test_codec_abi PRIVATE src include)
    target_link_libraries(test_codec_abi PRIVATE kvspace)
    add_test(NAME codec_abi COMMAND test_codec_abi)
    # wire 金标准：打印同一批操作的字节，供 kvspace-c / kvspace-durable 逐字节比对
    # （tests/compare_wire_golden.sh）。它不做断言，只产出可比对的文本。
    add_executable(xvalue_wire_golden tests/xvalue_wire_golden.c)
    target_include_directories(xvalue_wire_golden PRIVATE src include)
    target_link_libraries(xvalue_wire_golden PRIVATE kvspace)

    # 新存储 ABI 的 dispatch 测试：同一份 fake_backend.c 编两次。
    #   full —— 导出 storage.h 的九个符号，验参数原样转发；
    #   bare —— 不导出它们，验缺符号返回 KVSPACE_BACKEND 且写 err。
    # 两份都必须存在：只有 full 会漏掉「缺符号当成功」这类最危险的退化。
    #
    # 两个假后端必须**顶着真后端 soname**（前端按 DSN scheme 选 soname，不给测试开后门），
    # 但**绝不能落在主构建目录**：那里的 libkvspace-c.so.1 会被任何跑真后端的人
    # 静默加载成空实现——比测试失败严重得多。
    #
    # 解法：全部塞进独立子目录 fakebackend/，并把**前端的软链**也放进去。
    # 前端用 dladdr 按自身 .so 所在目录找后端，软链进去后它就在那里找到假后端，
    # 主构建目录保持干净。软链由下面的 test_frontend_storage 编译前置步骤建立。
    set(FAKE_BACKEND_DIR "${CMAKE_CURRENT_BINARY_DIR}/fakebackend")
    add_library(fakebackend SHARED tests/fake_backend.c)
    target_include_directories(fakebackend PRIVATE include)
    target_compile_definitions(fakebackend PRIVATE WITH_STORAGE_ABI=1)
    set_target_properties(fakebackend PROPERTIES
        OUTPUT_NAME "kvspace-c" SUFFIX ".so.1"
        LIBRARY_OUTPUT_DIRECTORY "${FAKE_BACKEND_DIR}")

    add_library(fakebackend_bare SHARED tests/fake_backend.c)
    target_include_directories(fakebackend_bare PRIVATE include)
    set_target_properties(fakebackend_bare PROPERTIES
        OUTPUT_NAME "kvspace_durable" SUFFIX ".so.1"
        LIBRARY_OUTPUT_DIRECTORY "${FAKE_BACKEND_DIR}")

    # 第三变体：旧五参 kvspaceMkindex，无新 ABI 独有符号。探测闸必须拦下它。
    #
    # 它必须**顶着真后端 soname** 且落在独立目录里：前端按 DSN scheme 选 soname
    # （fs:// → libkvspace_durable.so.1），再在自身 .so 同目录找。所以这一份输出
    # 到 legacybackend/ 子目录并用 durable 的 soname，配一个该目录下的 libkvspace
    # 软链，测试用 fs:// DSN 就能加载到它，而主构建目录保持干净。
    set(FAKE_LEGACY_DIR "${CMAKE_CURRENT_BINARY_DIR}/legacybackend")
    add_library(fakebackend_legacy SHARED tests/fake_backend.c)
    target_include_directories(fakebackend_legacy PRIVATE include)
    target_compile_definitions(fakebackend_legacy PRIVATE WITH_LEGACY_MKINDEX=1)
    set_target_properties(fakebackend_legacy PROPERTIES
        OUTPUT_NAME "kvspace_durable" SUFFIX ".so.1"
        LIBRARY_OUTPUT_DIRECTORY "${FAKE_LEGACY_DIR}")
    add_custom_command(
        OUTPUT "${FAKE_LEGACY_DIR}/libkvspace.so.1"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE:kvspace>" "${FAKE_LEGACY_DIR}/libkvspace.so.1"
        DEPENDS kvspace
        COMMENT "链接 libkvspace 到 legacybackend/ 供前端 dladdr 定位旧后端")
    add_custom_target(fake_legacy_link ALL
        DEPENDS "${FAKE_LEGACY_DIR}/libkvspace.so.1")
    add_dependencies(fake_legacy_link fakebackend_legacy)

    # 半迁移变体：复刻「有部分新符号但 kvspaceMkindex 还是旧五参」的形态。
    # 判据若是析取就会放行并崩；必须被合取判据拦下。
    #
    # 两个变体各自一个目录 + 前端软链，和 legacy 同一套做法：前端按 DSN scheme
    # 选 soname 再在自身目录找，所以每份都要能当「唯一的后端」被加载。
    foreach(variant 1 2)
        set(DIR "${CMAKE_CURRENT_BINARY_DIR}/partialbackend${variant}")
        add_library(fakebackend_partial${variant} SHARED tests/fake_backend.c)
        target_include_directories(fakebackend_partial${variant} PRIVATE include)
        target_compile_definitions(fakebackend_partial${variant}
                                   PRIVATE WITH_PARTIAL_ABI=${variant})
        set_target_properties(fakebackend_partial${variant} PROPERTIES
            OUTPUT_NAME "kvspace_durable" SUFFIX ".so.1"
            LIBRARY_OUTPUT_DIRECTORY "${DIR}")
        add_custom_command(
            OUTPUT "${DIR}/libkvspace.so.1"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE:kvspace>" "${DIR}/libkvspace.so.1"
            DEPENDS kvspace
            COMMENT "链接 libkvspace 到 partialbackend${variant}/")
        add_custom_target(fake_partial${variant}_link ALL
            DEPENDS "${DIR}/libkvspace.so.1")
        add_dependencies(fake_partial${variant}_link fakebackend_partial${variant})
    endforeach()

    # 把 libkvspace 的软链放进假后端目录，让前端的 dladdr 指向那里。
    # 用 add_custom_command + 软链而不是再编一份前端：两份前端就是两个符号世界。
    add_custom_command(
        OUTPUT "${FAKE_BACKEND_DIR}/libkvspace.so.1"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE:kvspace>" "${FAKE_BACKEND_DIR}/libkvspace.so.1"
        DEPENDS kvspace
        COMMENT "链接 libkvspace 到 fakebackend/ 供前端 dladdr 定位假后端")
    add_custom_target(fake_backend_link ALL
        DEPENDS "${FAKE_BACKEND_DIR}/libkvspace.so.1")
    add_dependencies(fake_backend_link fakebackend fakebackend_bare)

    # 关键：**不链接** libkvspace，改为 dlopen argv[1] 传入。链接会让前端符号进入
    # 全局作用域，于是前端 dlsym 新符号时会命中自己而递归段错——那是测试自己的
    # 链接方式造出来的假象，真实部署里后端 .so 不链接前端。
    add_executable(test_frontend_storage tests/test_frontend_storage.c)
    target_include_directories(test_frontend_storage PRIVATE src include)
    target_link_libraries(test_frontend_storage PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_frontend_storage kvspace fakebackend fakebackend_bare
                     fake_backend_link)

    # 两个用例各起一进程：满配后端与裸后端在同进程里会因 dlsym 命中彼此而互相污染。
    # argv[1] 传**假后端目录里的**前端软链，让 dladdr 落在那里而不是主构建目录。
    add_test(NAME frontend_storage_forwarding
             COMMAND test_frontend_storage
                     "${FAKE_BACKEND_DIR}/libkvspace.so.1" forwarding)
    add_test(NAME frontend_storage_missing
             COMMAND test_frontend_storage
                     "${FAKE_BACKEND_DIR}/libkvspace.so.1" missing)
    # 探测闸回归：旧五参后端必须被拦下，而不是被当新四参调用。
    # argv[1] 指 legacybackend/ 里的前端软链 → dladdr 落在该目录 → fs:// DSN
    # 加载到那里的 libkvspace_durable.so.1（即旧五参假后端）。
    add_test(NAME frontend_storage_legacy_gate
             COMMAND test_frontend_storage
                     "${FAKE_LEGACY_DIR}/libkvspace.so.1" legacy-mkindex
                     "$<TARGET_FILE:fakebackend_legacy>")

    # ABI 表面回归：只允许 kvspace.h 的十二个符号，旧符号族必须一个都 dlsym
    # 不到。它防的是「多导出一个名字」——那在动态链接下不会有任何编译错误，
    # 只在误用者手里以参数错位的形式炸出来。
    add_executable(test_abi_surface tests/test_abi_surface.c)
    target_link_libraries(test_abi_surface PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_abi_surface kvspace)
    add_test(NAME abi_surface
             COMMAND test_abi_surface "$<TARGET_FILE:kvspace>")
    # 未提供后端时测试返回 77：ctest 显示 Skipped 而不是 Passed。
    # 返回 0 会让「后端没验」混进全绿汇总里，只靠 stdout 的 SKIP 没人会看。
    set_tests_properties(abi_surface PROPERTIES SKIP_RETURN_CODE 77)

    # ── 真后端契约 smoke ────────────────────────────────────────────────
    # 与上面的假后端测试**不能同构建目录**：假后端顶着真后端 soname 放在这里，
    # 真 smoke 会加载到假后端。故本段只在 KVSPACE_REAL_BACKEND_TESTS=ON 时启用，
    # 要求使用独立构建目录（见 tests/README-real-backend.md）。
    #
    # 链接 libkvspace 是安全的：真后端 .so 不导出前端符号，不会出现
    # 「dlsym 命中自己」；这点与 test_frontend_storage 不同。
    if(KVSPACE_REAL_BACKEND_TESTS)
        add_executable(test_storage_shm tests/test_storage_shm.c)
        target_include_directories(test_storage_shm PRIVATE include)
        target_link_libraries(test_storage_shm PRIVATE kvspace)
        add_test(NAME storage_shm_contract COMMAND test_storage_shm)

        add_executable(test_storage_durable tests/test_storage_durable.c)
        target_include_directories(test_storage_durable PRIVATE include)
        target_link_libraries(test_storage_durable PRIVATE kvspace)
        add_test(NAME storage_durable_contract COMMAND test_storage_durable)
    endif()
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

configure_file(kvspace.pc.in ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── CLI（cli/ 独立 Cargo 项目）──────────────────────────────────────
# 走本仓 dispatch 前端的 C ABI，故须在 kvspace 目标之后构建；
# 安装到 <prefix>/bin（prefix 默认 /usr → /usr/bin，与 kvlang 的 CLI 一致）。
# Embed relative RUNPATH entries:
#   $ORIGIN/../lib                      -- release tarball (bin/ + lib/)
#   $ORIGIN/../<libdir>/kvspace         -- cmake --install (<prefix>/<libdir>/kvspace)
# Use @loader_path on macOS; escape $ as $$ for CMake.
# release.yml packages this binary directly without rebuilding.
find_program(CARGO_EXECUTABLE cargo)
if(CARGO_EXECUTABLE)
  set(KVSPACE_CLI_BIN "${CMAKE_CURRENT_SOURCE_DIR}/cli/target/release/kvspace")
  if(APPLE)
    set(KVSPACE_CLI_ORIGIN "@loader_path")
  else()
    set(KVSPACE_CLI_ORIGIN "$$ORIGIN")
  endif()
  add_custom_target(kvspace-cli ALL
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=-L native=$<TARGET_FILE_DIR:kvspace> -C link-arg=-Wl,-rpath,'${KVSPACE_CLI_ORIGIN}/../lib' -C link-arg=-Wl,-rpath,'${KVSPACE_CLI_ORIGIN}/../${CMAKE_INSTALL_LIBDIR}/kvspace'"
            "${CARGO_EXECUTABLE}" build --release
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/cli/Cargo.toml"
    BYPRODUCTS "${KVSPACE_CLI_BIN}"
    DEPENDS kvspace
    COMMENT "构建 kvspace CLI（cli/）")
  install(PROGRAMS "${KVSPACE_CLI_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  message(STATUS "未找到 cargo，跳过 kvspace CLI（cli/）")
endif()
