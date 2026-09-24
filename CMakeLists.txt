cmake_minimum_required(VERSION 3.20)
# Release pipeline injects -DFZWY_VERSION=<tag>; bump the default for local dev builds.
set(FZWY_VERSION "1.4.0" CACHE STRING "App version")
project(FzwyTask VERSION ${FZWY_VERSION} LANGUAGES CXX RC)
string(REPLACE "." "," FZWY_VERSION_COMMA "${FZWY_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# .rc files are UTF-8; tell windres explicitly to avoid garbled version info
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")

find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebSockets)

set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
set(APP_ICON_WIN "${APP_ICON}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
set(APP_RC "${CMAKE_CURRENT_BINARY_DIR}/app.rc")

# WMPF offset tables, embedded as Qt resources.
# Source: wmpf project's frida/config/win32/addresses.<version>.json
# To add a new version, drop the file in res/wmpf-offsets/win32/ and re-run configure.
set(WMPF_OFFSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res/wmpf-offsets/win32")
file(GLOB WMPF_OFFSET_FILES "${WMPF_OFFSET_DIR}/addresses.*.json")
list(SORT WMPF_OFFSET_FILES)
list(LENGTH WMPF_OFFSET_FILES WMPF_OFFSET_COUNT)
set(_wmpf_qrc "<RCC>\n  <qresource prefix=\"/wmpf-offsets/win32\">\n")
foreach(_f ${WMPF_OFFSET_FILES})
    get_filename_component(_fn "${_f}" NAME)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_BINARY_DIR}" "${_f}")
    string(APPEND _wmpf_qrc "    <file alias=\"${_fn}\">${_rel}</file>\n")
endforeach()
string(APPEND _wmpf_qrc "  </qresource>\n</RCC>\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wmpf_offsets.qrc" "${_wmpf_qrc}")
message(STATUS "WMPF offset tables: ${WMPF_OFFSET_COUNT} versions embedded")

# Hook DLL injected into the WeChat process.
# Two hard constraints (violating either causes LoadLibrary to fail):
#  1. Must statically link MinGW runtime: the target process (WeChat)
#     does not have libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll.
#  2. No Qt in this DLL: the WeChat process has no Qt6Core.dll.
# So it only depends on Win32 + standard library, with static runtime linking.
add_library(FzwyHook SHARED
    src/wmpf/hook/FzwyHook.cpp
    src/wmpf/x64len.cpp
)
set_target_properties(FzwyHook PROPERTIES
    OUTPUT_NAME "FzwyHook"
    PREFIX ""                 # MinGW adds a "lib" prefix by default; strip it
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(FzwyHook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/wmpf")
target_link_options(FzwyHook PRIVATE -static -static-libgcc -static-libstdc++)
target_link_libraries(FzwyHook PRIVATE bcrypt)

# Release: strip symbols to reduce binary size
# Note: add_link_options only affects targets created after this point,
# and FzwyTask is added below, so we use it here for FzwyTask stripping.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(FzwyHook PRIVATE -s)
    add_link_options(-s)
endif()

# WMPF channel sources, shared by FzwyTask and WmpfStudio.
set(WMPF_SOURCES
    src/wmpf/x64len.h
    src/wmpf/x64len.cpp
    src/wmpf/WmpfOffsets.h
    src/wmpf/WmpfOffsets.cpp
    src/wmpf/WmpfProbe.h
    src/wmpf/WmpfProbe.cpp
    src/wmpf/WmpfInject.h
    src/wmpf/WmpfInject.cpp
    src/wmpf/WarRemoteDebug.h
    src/wmpf/WarRemoteDebug.cpp
    src/wmpf/WmpfChannel.h
    src/wmpf/WmpfChannel.cpp
    src/wmpf/WmpfSession.h
    src/wmpf/WmpfSession.cpp
)

add_executable(FzwyTask WIN32
    src/main.cpp
    src/Config.h
    src/Config.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/Backend.h
    src/Backend.cpp
    src/BackendExport.cpp
    src/CdpClient.h
    src/CdpClient.cpp
    src/Crypto.h
    src/Crypto.cpp
    src/FzwyApi.h
    src/FzwyApi.cpp
    src/Tasks.h
    src/Tasks.cpp
    src/StateStore.h
    src/StateStore.cpp
    src/JobThread.h
    ${WMPF_SOURCES}
    res/app.qrc
    ${CMAKE_CURRENT_BINARY_DIR}/wmpf_offsets.qrc
    ${APP_RC}
)
add_dependencies(FzwyTask FzwyHook)

set_target_properties(FzwyTask PROPERTIES
    OUTPUT_NAME "FzwyTask"
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(FzwyTask PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    bcrypt
    ncrypt
)

# WmpfStudio: generic mini-program debug console (JS eval + storage dump).
add_executable(WmpfStudio WIN32
    src/studio/main.cpp
    src/studio/StudioWindow.h
    src/studio/StudioWindow.cpp
    src/CdpClient.h
    src/CdpClient.cpp
    src/Config.h
    src/Config.cpp
    src/JobThread.h
    ${WMPF_SOURCES}
)
add_dependencies(WmpfStudio FzwyHook)
set_target_properties(WmpfStudio PROPERTIES
    OUTPUT_NAME "WmpfStudio"
    WIN32_EXECUTABLE TRUE
)
target_link_libraries(WmpfStudio PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
)

# Optional embedded default config: config.defaults.json is a local gitignored file
# (CI writes it from the CONFIG_JSON secret). Values land in the exe as compile-time
# fallbacks; config.json next to the exe still overrides them.
set(_cfg_defaults "${CMAKE_CURRENT_SOURCE_DIR}/config.defaults.json")
if(EXISTS "${_cfg_defaults}")
    file(READ "${_cfg_defaults}" _cfg_json)
    string(JSON _aesKey GET "${_cfg_json}" aesKey)
    string(JSON _aesIv GET "${_cfg_json}" aesIv)
    string(JSON _spk GET "${_cfg_json}" serverPubKey)
    string(JSON _appId GET "${_cfg_json}" appId)
    string(JSON _mpCode GET "${_cfg_json}" mpCode)
    string(JSON _baseUrl GET "${_cfg_json}" baseUrl)
    string(JSON _mpVer GET "${_cfg_json}" mpVersion)
    target_compile_definitions(FzwyTask PRIVATE
        FZWY_CFG_AESKEY="${_aesKey}"
        FZWY_CFG_AESIV="${_aesIv}"
        FZWY_CFG_SERVERPUBKEY="${_spk}"
        FZWY_CFG_APPID="${_appId}"
        FZWY_CFG_MPCODE="${_mpCode}"
        FZWY_CFG_BASEURL="${_baseUrl}"
        FZWY_CFG_MPVERSION=${_mpVer}
    )
    message(STATUS "Embedded default config from config.defaults.json")
endif()

# Deploy: Qt6 runtime + MinGW runtime.
# MinGW's libgcc/libstdc++/libwinpthread are not in Qt's bin directory,
# windeployqt can't find them, so we copy them manually from the compiler dir.
get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake_exe}" DIRECTORY)
get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)

find_program(WINDEPLOYQT_EXE NAMES windeployqt windeployqt6
             HINTS "${_qt_bin}" NO_DEFAULT_PATH)
if(NOT WINDEPLOYQT_EXE)
    find_program(WINDEPLOYQT_EXE NAMES windeployqt windeployqt6)
endif()

set(DEPLOY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/FzwyTask")

if(WINDEPLOYQT_EXE)
    add_custom_target(deploy
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:FzwyTask>" "${DEPLOY_DIR}/FzwyTask.exe"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:WmpfStudio>" "${DEPLOY_DIR}/WmpfStudio.exe"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:FzwyHook>" "${DEPLOY_DIR}/FzwyHook.dll"
        COMMAND "${WINDEPLOYQT_EXE}"
                --release --no-compiler-runtime --no-system-d3d-compiler
                --no-opengl-sw --no-quick-import --no-translations
                --skip-plugin-types qmltooling,designer
                --dir "${DEPLOY_DIR}" "${DEPLOY_DIR}/FzwyTask.exe"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${DEPLOY_DIR}/translations"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_mingw_bin}/libgcc_s_seh-1.dll"
                "${_mingw_bin}/libstdc++-6.dll"
                "${_mingw_bin}/libwinpthread-1.dll"
                "${DEPLOY_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/config.example.json"
                "${DEPLOY_DIR}/config.example.json"
        DEPENDS FzwyTask FzwyHook WmpfStudio
        COMMENT "Deploy to ${DEPLOY_DIR} (Qt6 + MinGW runtime + hook DLL)"
        VERBATIM
    )
else()
    message(WARNING "windeployqt not found, skipping deploy target")
endif()
