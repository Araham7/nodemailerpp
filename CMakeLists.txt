cmake_minimum_required(VERSION 3.16)
project(nodemailerpp VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(NODEMAILERPP_TOP ON)
else()
  set(NODEMAILERPP_TOP OFF)
endif()
option(NODEMAILERPP_BUILD_TESTS "Build nodemailerpp tests" ${NODEMAILERPP_TOP})
option(NODEMAILERPP_BUILD_EXAMPLES "Build nodemailerpp examples" ${NODEMAILERPP_TOP})

find_package(CURL REQUIRED)
find_package(Drogon CONFIG REQUIRED)   # for trantor (event loops) and jsoncpp

add_library(nodemailerpp src/nodemailer.cc)
add_library(nodemailerpp::nodemailerpp ALIAS nodemailerpp)
target_include_directories(nodemailerpp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(nodemailerpp PUBLIC Drogon::Drogon PRIVATE CURL::libcurl)
target_compile_options(nodemailerpp PRIVATE -Wall -Wextra)

if(NODEMAILERPP_BUILD_TESTS)
  enable_testing()
  add_executable(nodemailerpp_tests tests/test_nodemailerpp.cc)
  target_link_libraries(nodemailerpp_tests PRIVATE nodemailerpp)
  add_test(NAME nodemailerpp_tests COMMAND nodemailerpp_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(NODEMAILERPP_BUILD_EXAMPLES)
  add_executable(send_mail examples/send_mail.cc)
  target_link_libraries(send_mail PRIVATE nodemailerpp)
endif()

include(GNUInstallDirs)
install(TARGETS nodemailerpp EXPORT nodemailerppTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/nodemailer DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nodemailerppTargets NAMESPACE nodemailerpp:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodemailerpp)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nodemailerppConfig.cmake
"include(CMakeFindDependencyMacro)\nfind_dependency(CURL)\nif(NOT TARGET Drogon::Drogon)\n  find_dependency(Drogon)\nendif()\ninclude(\${CMAKE_CURRENT_LIST_DIR}/nodemailerppTargets.cmake)\n")
include(CMakePackageConfigHelpers)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/nodemailerppConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nodemailerppConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/nodemailerppConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodemailerpp)
