cmake_minimum_required(VERSION 3.25)

project(XtremeASIO VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "XtremeASIO is Windows-only")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure an x64 generator/toolchain")
endif()

option(XTREMEASIO_BUILD_DRIVER "Build the x64 ASIO COM driver" ON)

add_library(xtreme_wasapi STATIC
  src/wasapi.cpp
)
target_include_directories(xtreme_wasapi PUBLIC include)
target_compile_features(xtreme_wasapi PUBLIC cxx_std_20)
target_compile_definitions(xtreme_wasapi PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(xtreme_wasapi PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(xtreme_wasapi PUBLIC ole32 uuid avrt)

add_executable(xtremeasio_diag src/diagnostic_main.cpp)
target_link_libraries(xtremeasio_diag PRIVATE xtreme_wasapi)
target_compile_options(xtremeasio_diag PRIVATE /W4 /permissive- /EHsc /utf-8)

add_executable(wasapi_test src/test_tone_main.cpp)
target_link_libraries(wasapi_test PRIVATE xtreme_wasapi)
target_compile_options(wasapi_test PRIVATE /W4 /permissive- /EHsc /utf-8)

add_executable(ks_test src/ks_test_main.cpp)
target_compile_features(ks_test PRIVATE cxx_std_20)
target_compile_definitions(ks_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ks_test PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ks_test PRIVATE setupapi ksuser uuid avrt)

add_library(xtreme_ks STATIC src/ks.cpp)
target_include_directories(xtreme_ks PUBLIC include)
target_compile_features(xtreme_ks PUBLIC cxx_std_20)
target_compile_definitions(xtreme_ks PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(xtreme_ks PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(xtreme_ks PUBLIC setupapi ksuser uuid avrt)

if(XTREMEASIO_BUILD_DRIVER)
  add_library(xtremeasio_driver SHARED
    src/aggregate_asio_driver.cpp
    src/asio_driver.def
  )
  target_include_directories(xtremeasio_driver PRIVATE
    include
    third_party/asio-sdk/common
  )
  target_compile_features(xtremeasio_driver PRIVATE cxx_std_20)
  target_compile_definitions(xtremeasio_driver PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(xtremeasio_driver PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(xtremeasio_driver PRIVATE xtreme_wasapi xtreme_ks ole32 uuid)
  set_target_properties(xtremeasio_driver PROPERTIES OUTPUT_NAME "XtremeASIO")

  add_library(xtremeasio_jbl_driver SHARED
    src/asio_driver.cpp
    src/asio_jbl_driver.def
  )
  target_include_directories(xtremeasio_jbl_driver PRIVATE
    include
    third_party/asio-sdk/common
  )
  target_compile_features(xtremeasio_jbl_driver PRIVATE cxx_std_20)
  target_compile_definitions(xtremeasio_jbl_driver PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(xtremeasio_jbl_driver PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(xtremeasio_jbl_driver PRIVATE xtreme_wasapi xtreme_ks ole32 uuid)
  set_target_properties(xtremeasio_jbl_driver PROPERTIES OUTPUT_NAME "XtremeASIO_JBL")

  add_executable(jbl_asio_smoke src/jbl_asio_smoke_main.cpp)
  target_include_directories(jbl_asio_smoke PRIVATE third_party/asio-sdk/common include)
  target_compile_features(jbl_asio_smoke PRIVATE cxx_std_20)
  target_compile_definitions(jbl_asio_smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(jbl_asio_smoke PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(jbl_asio_smoke PRIVATE xtreme_wasapi ole32)
  add_executable(asio_smoke src/asio_smoke_main.cpp)
  target_include_directories(asio_smoke PRIVATE third_party/asio-sdk/common include)
  target_compile_features(asio_smoke PRIVATE cxx_std_20)
  target_compile_definitions(asio_smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(asio_smoke PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(asio_smoke PRIVATE xtreme_wasapi ole32)

  add_executable(asio_probe src/asio_probe_main.cpp)
  target_include_directories(asio_probe PRIVATE third_party/asio-sdk/common)
  target_compile_features(asio_probe PRIVATE cxx_std_20)
  target_compile_definitions(asio_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(asio_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(asio_probe PRIVATE ole32)
endif()

