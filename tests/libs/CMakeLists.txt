# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

set(UNIT_TEST_SOURCES
    test_camera_shared_memory.cpp )

include_directories(${CMAKE_SOURCE_DIR}/src/libs/include/private)
include_directories(${CMAKE_SOURCE_DIR}/src/libs/include/public/camera)

add_executable (test_camera_shared_memory ${UNIT_TEST_SOURCES})
target_link_libraries (test_camera_shared_memory ${WEBOS_GTEST_LIBRARIES} camera_shared_memory
                       ${PMLOGLIB_LDFLAGS} pthread rt)
install(TARGETS test_camera_shared_memory DESTINATION ${WEBOS_INSTALL_SBINDIR})
