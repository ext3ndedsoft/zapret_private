cmake_minimum_required(VERSION 3.20)
project(ZapretPrivate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8 /EHsc /permissive- /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /O2 /Oi /Ot /Gy)
endif()

include_directories(
    .
    src
    third_party
    third_party/webview2/build/native/include
)

set(SOURCES
    src/main.cpp
    src/webview_window.h
    src/webview_window.cpp
    src/native_bridge.h
    src/native_bridge.cpp
    src/zapret_manager.h
    src/zapret_manager.cpp
    src/resource.h
    src/app.rc
)

add_executable(ZapretPrivate WIN32 ${SOURCES})

target_link_libraries(ZapretPrivate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native/x64/WebView2LoaderStatic.lib"
    dwmapi
    iphlpapi
    ws2_32
    crypt32
    shlwapi
    shell32
    ole32
    advapi32
    user32
    gdi32
    version
    winhttp
)

set_target_properties(ZapretPrivate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

if (MSVC)
    target_link_options(ZapretPrivate PRIVATE 
        "/DEBUG:NONE"
        "/OPT:REF"
        "/OPT:ICF"
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()
