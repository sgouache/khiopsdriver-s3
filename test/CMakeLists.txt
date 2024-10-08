# Fetch googletest from its Git repo
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY "https://github.com/google/googletest.git"
  GIT_TAG "v1.14.0")

set(gtest_force_shared_crt
    ON
    CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Find dependencies
find_package(Boost REQUIRED)
find_package(
  AWSSDK CONFIG
  COMPONENTS core s3
  REQUIRED)
add_executable(basic_test basic_test.cpp drivertest.cpp)

target_compile_options(
  basic_test
  PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4;/wd4101;/wd4625;/wd4710;/wd4711>
  PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-pedantic>)

# MSVC requires /bigobj to build in debug config
target_compile_options(
  basic_test PRIVATE $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:/bigobj>)

target_include_directories(
  basic_test
  PRIVATE ${${PROJECT_NAME}_SOURCE_DIR}/src
  PRIVATE ${AWSSDK_INCLUDE_DIRS}
  PRIVATE ${${PROJECT_NAME}_SOURCE_DIR}/src/contrib)
target_link_libraries(basic_test PRIVATE ${AWSSDK_LIBRARIES})

target_link_libraries(basic_test PRIVATE GTest::gtest GTest::gmock
                                         GTest::gmock_main khiopsdriver_file_s3)

if (WIN32)
  target_link_libraries(basic_test PRIVATE ws2_32)
endif()
gtest_discover_tests(basic_test)

add_executable(plugin_test plugin_test.cpp path_helper.cpp)
target_compile_options(
  plugin_test
  PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4;/wd4101;/wd4710;/wd4711>
  PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-pedantic>)
if(WIN32)
  # custom build and test execution for Windows
  target_link_libraries(plugin_test PRIVATE gtest ${CMAKE_DL_LIBS} Shlwapi
  )# Link to dl
  add_custom_command(
    TARGET plugin_test
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:khiopsdriver_file_s3> $<TARGET_FILE_DIR:plugin_test>
    COMMAND_EXPAND_LISTS)
else()
  target_link_libraries(plugin_test PRIVATE gtest ${CMAKE_DL_LIBS}) # Link to dl
endif()

gtest_discover_tests(plugin_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
