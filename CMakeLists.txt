cmake_minimum_required(VERSION 3.20)
project(ue_mcp_for_all_versions LANGUAGES CXX)

# ----------------------------------------------------------------------------
# ue-mcp-for-all-versions
# A single, version-agnostic C++ MCP server that drives ANY Unreal Engine
# version (4.25 -> 5.8) through the engine's built-in RemoteControl HTTP API.
# It links NO engine headers, so it has no UE ABI / BuildId dependency: one
# binary works everywhere, and unsupported APIs degrade at runtime.
# ----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static runtime on MSVC so the resulting .exe is truly copy-and-run.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(uemcp_core STATIC
  src/rc_client.cpp
  src/capability_registry.cpp
  src/project_setup.cpp
  src/tool_registry.cpp
  src/mcp_server.cpp
)
target_include_directories(uemcp_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
if(WIN32)
  target_link_libraries(uemcp_core PUBLIC ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(uemcp_core PUBLIC Threads::Threads)

add_executable(ue-mcp-for-all-versions src/main.cpp)
target_link_libraries(ue-mcp-for-all-versions PRIVATE uemcp_core)

# ---- Tests ----------------------------------------------------------------
enable_testing()
add_executable(uemcp_tests tests/test_main.cpp)
target_link_libraries(uemcp_tests PRIVATE uemcp_core)
add_test(NAME uemcp_tests COMMAND uemcp_tests)
