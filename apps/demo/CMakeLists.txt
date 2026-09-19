cmake_minimum_required(VERSION 3.16)

# Shared board components and build settings live at the top of the repo
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../common/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(es3c35p_demo)
