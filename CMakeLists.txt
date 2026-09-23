# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(Telegram/cmake/foxmes.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(${FOXMES_APP_NAME}
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "${FOXMES_APP_DISPLAY_NAME}"
    HOMEPAGE_URL "${FOXMES_HOMEPAGE}"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)
include(cmake/external/qt/package.cmake)

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)

# range-v3 0.12.0 forward-declares the standard containers inside namespace
# std, but only on Apple clang (meta.hpp, guarded by __apple_build_version__),
# and it decorates every one of them with _LIBCPP_TEMPLATE_VIS. libc++ 22, the
# one that ships with Xcode 27, dropped that macro, so `class META_TEMPLATE_VIS
# allocator;` parses as a variable declaration and the build dies with
# "redefinition of 'allocator' as different kind of symbol" on the first header
# that pulls in <memory>.
#
# META_NO_STD_FORWARD_DECLARATIONS is the switch range-v3 provides for exactly
# this, and it only removes declarations Linux and Windows builds never had:
# outside Apple clang the same block is already compiled out.
target_compile_definitions(external_ranges INTERFACE META_NO_STD_FORWARD_DECLARATIONS)

add_subdirectory(Telegram)
