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

# Built from the source, not against libluna_client: the test supplies a fake
# libluna so it can force the order of a reply and LSCallSetTimeout().
pkg_check_modules(LS2_TEST REQUIRED luna-service2)
add_executable (test_luna_client test_luna_client.cpp
                ${CMAKE_SOURCE_DIR}/src/libs/luna_client/luna_client.cpp)
target_include_directories (test_luna_client PRIVATE ${LS2_TEST_INCLUDE_DIRS})
target_link_libraries (test_luna_client ${WEBOS_GTEST_LIBRARIES} ${PMLOGLIB_LDFLAGS}
                       ${GLIB2_LDFLAGS} pthread)
install(TARGETS test_luna_client DESTINATION ${WEBOS_INSTALL_SBINDIR})
