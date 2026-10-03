# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

# camera_types.cpp is not a library of its own; the services compile it in
# directly, so the test does too.
set(UNIT_TEST_SOURCES
    test_camera_types.cpp
    ${CMAKE_SOURCE_DIR}/src/services/common/camera_types.cpp )

add_executable (test_camera_types ${UNIT_TEST_SOURCES})
target_link_libraries (test_camera_types ${WEBOS_GTEST_LIBRARIES}
                       ${GLIB2_LDFLAGS} ${LS2++_LDFLAGS} ${PMLOGLIB_LDFLAGS} pthread)
install(TARGETS test_camera_types DESTINATION ${WEBOS_INSTALL_SBINDIR})
