cmake_minimum_required(VERSION 3.15)
project(diffmonger)

include(ExternalProject)
include(GNUInstallDirs)

ExternalProject_Add(libsodium
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/externals/libsodium
  CONFIGURE_COMMAND pwd && <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
  BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
  INSTALL_COMMAND make install
#  BUILD_IN_SOURCE 1
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libsodium.a <INSTALL_DIR>/include
)

add_library(sodium_static STATIC IMPORTED GLOBAL)
add_dependencies(sodium_static libsodium)

ExternalProject_Get_Property(libsodium install_dir)
file(MAKE_DIRECTORY "${install_dir}/include")
set_target_properties(sodium_static PROPERTIES
  IMPORTED_LOCATION "${install_dir}/lib/libsodium.a"  # or .a for static
  INTERFACE_INCLUDE_DIRECTORIES "${install_dir}/include"
)

add_subdirectory(libs/util)

add_library(libdiffmonger STATIC
  # find src -type f | sort
  src/BackendDriver.cpp
  src/BackendDriver.hpp
  src/CommandLineInterface.cpp
  src/CommandLineInterface.hpp
  src/Controller.cpp
  src/Controller.hpp
  src/cstdio-util.cpp
  src/cstdio-util.hpp
  src/diffmonger.hpp
  src/EncryptionUtil.cpp
  src/EncryptionUtil.hpp
  src/KeyPair.cpp
  src/KeyPair.hpp
  src/Node.hpp
  src/RepositoryParams.hpp
  src/RepositoryStructure.cpp
  src/RepositoryStructure.hpp
  src/sysutil.cpp
  src/sysutil.hpp
  src/zfs.cpp
  src/ZfsDriver.cpp
  src/ZfsDriver.hpp
  src/zfs.hpp
)

add_dependencies(libdiffmonger sodium_static)  # Must come before using sodium_static

target_compile_options(libdiffmonger PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(libdiffmonger PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(libdiffmonger PUBLIC diffmonger-util PRIVATE sodium_static)
target_compile_features(libdiffmonger PUBLIC cxx_std_23)

add_executable(diffmonger src/main.cpp)
target_link_libraries(diffmonger PUBLIC libdiffmonger)
target_compile_options(diffmonger PRIVATE -Wall -Wextra -Wpedantic -Werror)


add_executable(node-test test/node.cpp)
target_link_libraries(node-test PUBLIC libdiffmonger)
target_compile_options(node-test PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(disk-usage-analysis simulation/disk-usage-analysis.cpp)
target_link_libraries(disk-usage-analysis PUBLIC libdiffmonger)
target_compile_options(disk-usage-analysis PRIVATE -Wall -Wextra -Wpedantic -Werror)


install(TARGETS diffmonger
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
