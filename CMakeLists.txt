cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(percentframewindow VERSION 1.0.0)

# Only src/*.cpp is compiled into the mod. The pure-logic headers in src/ are
# also used by tests/ (see tests/run_tests.sh), which is NOT part of this build.
file(GLOB SOURCES src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Uncomment if the build says createCheckpoint / loadFromCheckpoint do not exist (disables checkpoint fast mode):
# target_compile_definitions(${PROJECT_NAME} PRIVATE FW_NO_CHECKPOINTS)

setup_geode_mod(${PROJECT_NAME})
