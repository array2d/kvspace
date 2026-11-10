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
# kvspace 就是标准实现本身：不再有 adapter/dispatch 层，也不 dlopen 任何后端。
# 单库直接包含 shm 引擎 + 值编解码 + 构造器。
add_library(kvspace SHARED
    src/shm/coord.c
    src/shm/kvspace.c
    src/codec_abi.c
    src/xvalue_head.c
    src/kvlang_xvalue.c)
target_include_directories(kvspace PRIVATE src/shm)

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


    # 存储契约 smoke：单库就是标准实现，直接链接即可，不再有「真/假后端」之分，
    # 也就不再需要独立构建目录与 dlopen 搜索路径。
    add_executable(test_storage_shm tests/test_storage_shm.c)
    target_include_directories(test_storage_shm PRIVATE include)
    target_link_libraries(test_storage_shm PRIVATE kvspace)
    add_test(NAME storage_shm_contract COMMAND test_storage_shm)
endif()

# shm 后端测试（原 kvspace-c/tests，已迁入 tests/shm/）。
# 统一 `shm_` 前缀：与前端测试有同名文件（xvalue_head、xvalue_wire_golden），
# 不前缀会在 ctest 里出现两个同名项，红的时候分不清是哪一个。
if(BUILD_TESTING)
    # 枚举 (ctest 名|源文件)：CMake 的 `;` 是列表分隔符，写进字符串会在
    # `foreach(IN LISTS)` 里被拆成两个元素（实测每个 pair 跑了两遍，第二次
    # 名字变成 "test_xxx" 就撞了前端 target）。故用 `|` 作内部分隔符。
    set(KVSPACE_SHM_TEST_PAIRS
        "art_scan|test_art_scan"
        "art_alignment|test_art_alignment"
        "anchor_hook|test_anchor_hook"
        "new_wire_shm|test_new_wire_shm"
        "shm_split|test_shm_split"
        "storage_abi|test_storage_abi"
        "xvalue_head|test_xvalue_head"
        # 该源文件不带 test_ 前缀。
        "xvalue_wire_golden|xvalue_wire_golden")
    foreach(pair IN LISTS KVSPACE_SHM_TEST_PAIRS)
        string(FIND "${pair}" "|" pair_sep)
        string(SUBSTRING "${pair}" 0 ${pair_sep} shm_name)
        math(EXPR pair_tail "${pair_sep} + 1")
        string(SUBSTRING "${pair}" ${pair_tail} -1 shm_source)
        add_executable(shm_${shm_name} tests/shm/${shm_source}.c)
        target_include_directories(shm_${shm_name} PRIVATE
            src/shm include)
        # 测试要调 P2_TEST_HOOKS 里的内部钩子（TestAnchorOf/ArtAlignCheck/
        # TestSetFormatVersion）。宏必须与库一致，否则头里的声明被 #ifdef 挡掉，
        # 调用方靠隐式声明链接成功——正是 -Werror=implicit-function-declaration
        # 要抓的那类缺口。
        target_compile_definitions(shm_${shm_name} PRIVATE P2_TEST_HOOKS)
        target_link_libraries(shm_${shm_name} PRIVATE kvspace)
        add_test(NAME shm_${shm_name} COMMAND shm_${shm_name})
    endforeach()
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

# 生产库只导出公开头声明的符号；内部实现（kvspaceShm*、测试钩子）不是 API。
# 测试构建需要直接链接内部函数，故保持默认可见。
# 这条随原 kvspace-c target 一起在单库合并时丢失过一次——缺它的后果是 58 个
# 符号全部导出，其中 27 个 kvspaceShm* 内部实现泄漏到公开面。
# C_VISIBILITY_PRESET 在部分 CMake 版本/生成器上不会落到 C 的编译命令行
# （实测 flags.make 里没有 -fvisibility=hidden），故两个都设——属性管预期，
# 编译选项管实际。缺了它 27 个 kvspaceShm* 会全部泄漏到公开面。
if(NOT BUILD_TESTING)
    set_target_properties(kvspace PROPERTIES C_VISIBILITY_PRESET hidden)
    target_compile_options(kvspace PRIVATE -fvisibility=hidden)
endif()

target_compile_definitions(kvspace PRIVATE _GNU_SOURCE)

# 测试构建启用内部观测/注入钩子（生产库不含）：kvspaceShmTestAnchorOf、
# kvspaceShmArtAlignCheck、kvspaceShmTestSetFormatVersion。
if(BUILD_TESTING)
    target_compile_definitions(kvspace PRIVATE P2_TEST_HOOKS)
endif()

# 门禁：隐式函数声明一律当错误。
#
# 它抓的是「实现导出了、头里没声明」这类缺口——C 的老默认（隐式返回 int）
# 会把 `kvspaceNewInt64(...)` 的返回值当 int，指针被截断，崩在别处。
# 改成硬错误后，每个未声明的调用都在**编译期**点名。
target_compile_options(kvspace PRIVATE -Werror=implicit-function-declaration)

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
