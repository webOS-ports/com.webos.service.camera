# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

project(camera-shm-fuzz CXX)

include_directories(${CMAKE_SOURCE_DIR}/src/libs/include/private)
include_directories(${CMAKE_SOURCE_DIR}/src/libs/include/public/camera)

add_executable(${PROJECT_NAME} camera_shm_fuzz.cpp)
target_link_libraries(${PROJECT_NAME} camera_shared_memory pthread rt)
install(TARGETS ${PROJECT_NAME} DESTINATION ${WEBOS_INSTALL_SBINDIR})
