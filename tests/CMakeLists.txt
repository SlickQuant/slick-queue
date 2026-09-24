if(SLICK_QUEUE_ENABLE_ASAN)
  # Directory-scoped and set before GoogleTest is added below, so it is instrumented with
  # the test binary. That matters on MSVC in particular, whose STL refuses to link
  # instrumented and uninstrumented objects together.
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")
    # The runtime checks CMake enables in Debug (/RTC1) cannot be combined with ASan.
    foreach(flags_var CMAKE_CXX_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG)
      string(REGEX REPLACE "/RTC[1csu]+" "" ${flags_var} "${${flags_var}}")
    endforeach()
    # Nor can incremental linking (LNK4300), which CMake enables per configuration.
    foreach(flags_var CMAKE_EXE_LINKER_FLAGS_DEBUG CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO)
      string(REGEX REPLACE "/INCREMENTAL(:YES)?( |$)" "/INCREMENTAL:NO\\2"
             ${flags_var} "${${flags_var}}")
    endforeach()
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
  endif()
endif()

# A prebuilt GoogleTest is not instrumented, so an ASan build always builds its own.
if(NOT SLICK_QUEUE_ENABLE_ASAN)
  find_package(GTest QUIET)
endif()

if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(slick-queue-tests tests.cpp shm_tests.cpp)

target_link_libraries(slick-queue-tests PRIVATE slick::queue GTest::gtest_main)

# No compile definitions: feature configuration is a template parameter now, so a single
# binary instantiates every configuration it needs (see test_traits.h).

if(SLICK_QUEUE_ENABLE_ASAN AND MSVC)
  # MSVC links the ASan runtime as a DLL that lives beside cl.exe and is not on PATH.
  # gtest_discover_tests runs the test binary at build time to list its tests, so the DLL
  # has to be next to it by then; this POST_BUILD step is added first, so it runs first.
  get_filename_component(msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  file(GLOB asan_runtime_dlls "${msvc_bin_dir}/clang_rt.asan*_dynamic-*.dll")
  if(asan_runtime_dlls)
    add_custom_command(TARGET slick-queue-tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${asan_runtime_dlls}
              "$<TARGET_FILE_DIR:slick-queue-tests>")
  else()
    message(WARNING "ASan runtime DLL not found in ${msvc_bin_dir}; the tests will not "
                    "start unless it is on PATH")
  endif()
endif()

include(GoogleTest)
gtest_discover_tests(slick-queue-tests)
