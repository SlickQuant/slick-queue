cmake_minimum_required(VERSION 3.10)

project(slick-queue
        VERSION 1.4.1
        DESCRIPTION "A C++ Lock-Free MPMC queue"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# Options:
option(BUILD_SLICK_QUEUE_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

find_package(slick-shm CONFIG QUIET)

if (NOT slick-shm_FOUND)
    include(FetchContent)

    # Disable slick-shm tests and examples, but allow installation
    set(SLICK_SHM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLICK_SHM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-shm
        GIT_REPOSITORY https://github.com/SlickQuant/slick-shm.git
        GIT_TAG v0.1.3
    )
    FetchContent_MakeAvailable(slick-shm)
else()
    message(STATUS "slick-shm: ${slick-shm_VERSION}")
endif()

add_library(slick-queue INTERFACE)
add_library(slick::queue ALIAS slick-queue)
# For backward compatibility with older versions
add_library(slick_queue ALIAS slick-queue)
add_library(slick::slick_queue ALIAS slick-queue)

target_include_directories(slick-queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(slick-queue PROPERTIES EXPORT_NAME queue)

target_link_libraries(slick-queue INTERFACE slick::shm)
if(UNIX AND NOT APPLE)
    target_link_libraries(slick-queue INTERFACE rt atomic)
endif()

if(BUILD_SLICK_QUEUE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation rules
install(DIRECTORY include/ DESTINATION include)

# Generate and install CMake config files for vcpkg compatibility
include(CMakePackageConfigHelpers)

# Create the config file
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/slick-queueConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-queueConfig.cmake"
    INSTALL_DESTINATION lib/cmake/slick-queue
)

# Create the version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slick-queueConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install the config files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slick-queueConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-queueConfigVersion.cmake"
    DESTINATION lib/cmake/slick-queue
)

# Install the target
install(TARGETS slick-queue
    EXPORT slick-queueTargets
)

# Install the export set
install(EXPORT slick-queueTargets
    FILE slick-queueTargets.cmake
    NAMESPACE slick::
    DESTINATION lib/cmake/slick-queue
)

message(STATUS "${PROJECT_NAME}: ${PROJECT_VERSION}")
