cmake_minimum_required(VERSION 3.18)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

aux_source_directory(${CMAKE_SOURCE_DIR}/src SRC)

add_executable(main main.cpp ${SRC})

target_include_directories(main PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(main PRIVATE LLVM)
