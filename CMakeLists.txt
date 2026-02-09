cmake_minimum_required(VERSION 3.10.0)
project(emcc VERSION 0.1.0 LANGUAGES C)

if (MSVC)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

add_executable(emcc wrapper.c)
if (MSVC)
    target_compile_options(emcc PRIVATE /GS-)
    target_link_options(emcc PRIVATE
        "/NODEFAULTLIB:libcmt"
        "/NODEFAULTLIB:msvcrt"
        "/NODEFAULTLIB:libcmtd"
        "/NODEFAULTLIB:msvcrtd"
        "/ENTRY:wmainCRTStartup"
    )
else()
    target_compile_options(emcc PRIVATE -Os -s -Wl,--gc-sections)
    target_link_options(emcc PRIVATE -nostdlib)
endif()
