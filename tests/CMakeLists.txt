find_package(GTest QUIET)

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
if(MSVC)
  add_compile_options(/wd4996)
endif()

target_link_libraries(slick-queue-tests PRIVATE slick::queue GTest::gtest_main)

# No compile definitions: feature configuration is a template parameter now, so a single
# binary instantiates every configuration it needs (see test_traits.h).

include(GoogleTest)
gtest_discover_tests(slick-queue-tests)
