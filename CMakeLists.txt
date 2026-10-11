cmake_minimum_required(VERSION 3.14)
project(BitcoindModulePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME bitcoind_module
    SOURCES
        src/bitcoind_module_impl.h
        src/bitcoind_module_impl.cpp
        src/rpc_allowlist.h
    EXTERNAL_LIBS
        bitcoind_c
    INCLUDE_DIRS
        lib
)
