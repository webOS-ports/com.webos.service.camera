# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

project(notifier-droid CXX)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
include_directories(${GSTREAMER_INCLUDE_DIRS})

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/droid_notifier.cpp)

add_library(${PROJECT_NAME} SHARED ${SRC})

target_link_libraries(${PROJECT_NAME}
                      ${PMLOGLIB_LDFLAGS}
                      ${GLIB2_LDFLAGS}
                      ${GSTREAMER_LDFLAGS}
                      )

set_target_properties (${PROJECT_NAME} PROPERTIES VERSION 1.0 SOVERSION 1)

install(TARGETS ${PROJECT_NAME} DESTINATION ${WEBOS_INSTALL_LIBDIR}/camera)
