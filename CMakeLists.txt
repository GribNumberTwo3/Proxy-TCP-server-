cmake_minimum_required(VERSION 3.8.2)
project(ServerTCP)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pthread")
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
set(INCLUDEDIR ${PKG_CONFIG_SYSROOT_DIR}${includedir})

find_package(PkgConfig REQUIRED)
find_library(PTHREAD_LIBRARY pthread)

set(SOURCE_FILES
        main.cpp
        )

include_directories( ${CMAKE_SOURCE_DIR}  ${DIRECTORIES} )
add_executable(${PROJECT_NAME}
        ${SOURCE_FILES}
        )

target_link_libraries (${PROJECT_NAME} PUBLIC  ${LIBRARIES}  )

