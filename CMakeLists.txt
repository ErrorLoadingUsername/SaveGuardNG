cmake_minimum_required(VERSION 3.21)
project(SaveGuardNG VERSION 1.1.6.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "ErrorLoadingUsername"
    SOURCES
        src/main.cpp
        src/win.cpp
        src/ui.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
set_source_files_properties(src/win.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_include_directories(${PROJECT_NAME} PRIVATE include src)
target_compile_options(${PROJECT_NAME} PRIVATE /EHa /W4 /permissive-)

# Keep build paths out of the DLL: __FILE__ / std::source_location (log lines, asserts)
# would otherwise embed the full path of every source and header file.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" SOURCE_DIR_NATIVE)
target_compile_options(${PROJECT_NAME} PRIVATE "/d1trimfile:${SOURCE_DIR_NATIVE}")
