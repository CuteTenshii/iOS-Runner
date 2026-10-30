cmake_minimum_required(VERSION 3.22.1)
project(iosrunner CXX)

add_library(iosrunner SHARED jni_bridge.cpp engine.cpp macho.cpp symbols.cpp swift_runtime.cpp type_system.cpp swiftui_shim.cpp objc_runtime.cpp foundation_shim.cpp stdlib_shim.cpp)
target_compile_options(iosrunner PRIVATE -Wall -Wextra)
target_link_options(iosrunner PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(iosrunner PRIVATE log)
