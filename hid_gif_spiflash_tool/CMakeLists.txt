cmake_minimum_required(VERSION 3.15)

project(hid_gif_spiflash_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build a console application.
add_executable(hid_gif_spiflash_tool
    HIDTransferTest.cpp
    stdafx.cpp
)

target_include_directories(hid_gif_spiflash_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Allow the classic CRT functions (fopen, fread, strtoul, ...) used by the tool.
# UNICODE/_UNICODE match the original Visual Studio project (TCHAR == wchar_t).
target_compile_definitions(hid_gif_spiflash_tool PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# Windows HID / SetupAPI libraries.
target_link_libraries(hid_gif_spiflash_tool PRIVATE setupapi hid)
