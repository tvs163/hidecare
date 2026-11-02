# ESPectre native firmware build
#
# Prerequisites:
#   - ESP-IDF v5.5+
#
# Build:
#   cd src/cpp/frontend/native/app
#   idf.py set-target esp32c3
#   idf.py build

cmake_minimum_required(VERSION 3.16)

# Hidecare: release version comes from HIDECARE_VERSION so builds get a clean
# MAJOR.MINOR.PATCH without a git-describe suffix. ESPECTRE_GIT_VERSION (CMake
# or env) still overrides it.
if((NOT DEFINED ESPECTRE_GIT_VERSION OR ESPECTRE_GIT_VERSION STREQUAL "") AND
   "$ENV{ESPECTRE_GIT_VERSION}" STREQUAL "" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/HIDECARE_VERSION")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/HIDECARE_VERSION" ESPECTRE_GIT_VERSION LIMIT_COUNT 1)
    string(STRIP "${ESPECTRE_GIT_VERSION}" ESPECTRE_GIT_VERSION)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../../espectre_git_version.cmake")
set(PROJECT_VER "${ESPECTRE_GIT_VERSION}")

include("${CMAKE_CURRENT_LIST_DIR}/../../espectre_frontend_sources.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../espectre"
    "${ESPECTRE_SDK_ROOT}/runtime/esp_idf/espectre_config"
)

project(espectre-native)
