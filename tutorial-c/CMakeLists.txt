# C
add_executable(01_basic 01_basic.c)
target_link_libraries(01_basic kvspace-c)

# C++
add_executable(02_cpp 02_cpp.cpp)
target_link_libraries(02_cpp kvspace-c)

# integrity stress
add_executable(05_integrity 05_integrity.c)
target_link_libraries(05_integrity kvspace-c)

# multiprocess
add_executable(06_multiprocess 06_multiprocess.c)
target_link_libraries(06_multiprocess kvspace-c)
