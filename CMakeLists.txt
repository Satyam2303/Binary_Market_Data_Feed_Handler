cmake_minimum_required(VERSION 3.20)

project(binary_market_data_feed_handler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(bmdfh_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    bmdfh_warnings
    INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
  )
elseif(MSVC)
  target_compile_options(bmdfh_warnings INTERFACE /W4 /permissive-)
endif()

add_library(
  bmdfh_core
  src/common/order_book.cpp
  src/common/protocol.cpp
  src/common/recovery.cpp
  src/common/udp_multicast.cpp
)

target_include_directories(bmdfh_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bmdfh_core PUBLIC bmdfh_warnings)
target_compile_features(bmdfh_core PUBLIC cxx_std_20)

add_executable(exchange_sim src/exchange_sim/main.cpp)
target_link_libraries(exchange_sim PRIVATE bmdfh_core)

add_executable(feed_handler src/feed_handler/main.cpp)
target_link_libraries(feed_handler PRIVATE bmdfh_core)
