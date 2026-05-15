cmake_minimum_required(VERSION 3.13)
project(diffmonger-util)
set(CMAKE_CXX_STANDARD 23)

add_library(diffmonger-util STATIC
  # find include/ src/ -type f  | sort
  include/diffmonger/util/array.hpp
  include/diffmonger/util/FdOwner.hpp
  include/diffmonger/util/finally.hpp
  include/diffmonger/util/ioutil.hpp
  include/diffmonger/util/misc.hpp
  include/diffmonger/util/parser/Parser.hpp
  include/diffmonger/util/PasswordBuffer.hpp
  include/diffmonger/util/PidOwner.hpp
  include/diffmonger/util/Serialisation.hpp
  include/diffmonger/util/transform_iterator.hpp
  include/diffmonger/util/Uuid.hpp
  src/FdOwner.cpp
  src/ioutil.cpp
  src/PasswordBuffer.cpp
  src/Uuid.cpp
)

target_include_directories(diffmonger-util
  PUBLIC include
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/diffmonger/util")
set_target_properties(diffmonger-util PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
