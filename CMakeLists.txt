cmake_minimum_required(VERSION 3.10)
project(kvspace VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

                                                                  
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()

                                                        
                                       
                                                 
add_library(kvspace SHARED
    src/kvspace.c
    src/xvalue_head.c
    src/const.c)
target_include_directories(kvspace PRIVATE src)

                                                                    
                                                                     
                                                             
                                                                             
                      
 
                                                        
                 
 
                                     
 
                                                        
                                                   
 
                          
                                        
                                            
                   
                                                     
                                  
 
                                                                
                          
 
                                                         
                                    
foreach(header IN ITEMS blockmalloc/blockmalloc.h slotsboxmalloc/slotsboxobj.h)
    string(REGEX REPLACE "/[^/]+$" "" header_directory "${header}")
    set(pin_candidates
        "/usr/local/include"
        "/usr/include")
    find_path(KVSPACE_${header_directory}_INCLUDE_DIRECTORY
        NAMES ${header}
        PATHS ${pin_candidates}
        NO_DEFAULT_PATH)
    if(NOT KVSPACE_${header_directory}_INCLUDE_DIRECTORY)
                                                           
                                                         
        set(candidate_lines "")
        set(candidate_index 1)
        foreach(candidate IN LISTS pin_candidates)
            string(APPEND candidate_lines "    ${candidate_index}. ${candidate}\n")
            math(EXPR candidate_index "${candidate_index} + 1")
        endforeach()
        message(FATAL_ERROR
            "找不到 <${header}>。\n"
            "  候选目录（均未命中，按优先级）：\n"
            "${candidate_lines}"
            "  修复（二选一）：\n"
            "    1. 跑 ./ci/deps.sh —— 版本 pin 在它里面（本文件不重复写版本号）；\n"
            "    2. 用 -DKVSPACE_${header_directory}_INCLUDE_DIRECTORY=<包含 ${header_directory}/ 的目录> 显式指定。")
    endif()
    message(STATUS "kvspace: <${header}> → ${KVSPACE_${header_directory}_INCLUDE_DIRECTORY}")
    target_include_directories(kvspace PRIVATE
        "${KVSPACE_${header_directory}_INCLUDE_DIRECTORY}")
endforeach()

                                                 
                                                     
                                                  
                                    

include(CTest)
if(BUILD_TESTING)
                                                                
                              

                                              
                                
    add_executable(test_storage_shm tests/test_storage_shm.c)
    target_include_directories(test_storage_shm PRIVATE include)
    target_link_libraries(test_storage_shm PRIVATE kvspace)
    add_test(NAME storage_shm_contract COMMAND test_storage_shm)
endif()

                                             
                                                          
                                   
if(BUILD_TESTING)
                                                 
                                                     
                                                  
    set(KVSPACE_SHM_TEST_PAIRS
        "art_scan|test_art_scan"
        "art_alignment|test_art_alignment"
        "anchor_hook|test_anchor_hook"
        "anchor_cross_process|test_anchor_cross_process"
        "shm_split|test_shm_split"
        "storage_abi|test_storage_abi"
        "xvalue_layout|test_xvalue_layout"
        "storage_boundary|test_storage_boundary"
                                                                      
                                                  
        )
    foreach(pair IN LISTS KVSPACE_SHM_TEST_PAIRS)
        string(FIND "${pair}" "|" pair_sep)
        string(SUBSTRING "${pair}" 0 ${pair_sep} shm_name)
        math(EXPR pair_tail "${pair_sep} + 1")
        string(SUBSTRING "${pair}" ${pair_tail} -1 shm_source)
        add_executable(shm_${shm_name} tests/shm/${shm_source}.c)
        target_include_directories(shm_${shm_name} PRIVATE
            src include)
                                                                  
                                     
                                                                
                  
        target_compile_definitions(shm_${shm_name} PRIVATE RECOVERY_TEST_HOOKS)
        target_link_libraries(shm_${shm_name} PRIVATE kvspace)
        add_test(NAME shm_${shm_name} COMMAND shm_${shm_name})
    endforeach()

                                       
     
                                                 
                                                                   
                                                  
                                            
                 
    add_executable(test_export_surface tests/test_export_surface.c)
    target_include_directories(test_export_surface PRIVATE include)
    target_link_libraries(test_export_surface PRIVATE kvspace)
    add_test(NAME export_surface COMMAND test_export_surface)

    find_program(PYTHON3_EXECUTABLE NAMES python3)
    if(NOT PYTHON3_EXECUTABLE)
        message(FATAL_ERROR "python3 is required for the separator hardcode check")
    endif()
    add_test(NAME separator_hardcode
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_separator_hardcode.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

                           
set_target_properties(kvspace PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

target_include_directories(kvspace
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

                                                         
find_package(Threads REQUIRED)
target_link_libraries(kvspace PRIVATE Threads::Threads)

                                              
                         
                                                  
                                        
                                                    
                                                       
                                         
if(NOT BUILD_TESTING)
    set_target_properties(kvspace PROPERTIES C_VISIBILITY_PRESET hidden)
    target_compile_options(kvspace PRIVATE -fvisibility=hidden)
endif()

target_compile_definitions(kvspace PRIVATE _GNU_SOURCE)

                                                
                            
if(BUILD_TESTING)
    target_compile_definitions(kvspace PRIVATE RECOVERY_TEST_HOOKS)
endif()

                 
 
                                         
                                                 
                             
target_compile_options(kvspace PRIVATE -Werror=implicit-function-declaration)

install(TARGETS kvspace
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(kvspace.pc.in ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

                                                                
                                            
                                                                
                                 
                                                                        
                                                                                      
                                                      
                                                               
 
                                                                          
                                                                       
                                                                
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
