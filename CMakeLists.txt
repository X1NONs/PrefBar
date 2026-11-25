# Copyright (C) 2026 X1NONs
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(PrefBar VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PREFBAR_BUILD_PS5 "Build PrefBar for the PS5 using the Payload SDK" OFF)
option(PREFBAR_DEBUG_NOTIFY "Compile optional system-toast notifications" ON)
option(PREFBAR_ENABLE_WEB "Compile the embedded Control Center HTTP server" ON)

if(PREFBAR_BUILD_PS5)
    set(PREFBAR_SDK "$ENV{PS5_PAYLOAD_SDK}")
    if(NOT PREFBAR_SDK OR NOT EXISTS "${PREFBAR_SDK}/bin/prospero-cmake")
        message(FATAL_ERROR
            "PS5_PAYLOAD_SDK must point to a PS5 Payload SDK installation")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(PREFBAR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

    # ShellUI renderer: built as an intermediate ELF and embedded in controller.
    add_library(prefbar_shellui SHARED
        src/ps5/shellui_payload/prefbar_shellui.cpp
        src/ps5/shellui_payload/prefbar_shellui_entry.cpp
    )
    set_target_properties(prefbar_shellui PROPERTIES
        OUTPUT_NAME "PrefBar_ShellUI_v0.3.1"
        PREFIX ""
        SUFFIX ".elf"
        NO_SONAME TRUE
    )
    target_include_directories(prefbar_shellui PRIVATE
        include
        src/ps5/shellui_payload
        "${PREFBAR_SDK}"
        "${PREFBAR_SDK}/include"
    )
    target_compile_options(prefbar_shellui PRIVATE
        --target=x86_64-sie-ps5
        -DPS5
        -fPIC
        -fvisibility=hidden
        -march=znver2
        -O2
        -Wall
        -Wextra
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(prefbar_shellui PRIVATE
        -Wl,--gc-sections
        -Wl,-Bsymbolic
        -Wl,-e,elf_main
        -Wl,-soname,libPrefBar_ShellUI.so
        -Wl,-z,now
        -Wl,--hash-style=sysv
    )
    target_link_libraries(prefbar_shellui PRIVATE
        kernel_sys
        SceLibcInternal
        SceNet
    )

    # Generate the renderer byte array after the renderer ELF has been linked.
    set(PREFBAR_SHELLUI_BLOB_CPP
        "${PREFBAR_GENERATED_DIR}/prefbar_shellui_blob.cpp")
    add_custom_command(
        OUTPUT "${PREFBAR_SHELLUI_BLOB_CPP}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PREFBAR_GENERATED_DIR}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/embed_binary.py"
            "$<TARGET_FILE:prefbar_shellui>"
            "${PREFBAR_SHELLUI_BLOB_CPP}"
        DEPENDS prefbar_shellui scripts/tools/embed_binary.py
        VERBATIM
    )

    # Embed the Control Center and its PS5 Media shortcut assets.
    if(PREFBAR_ENABLE_WEB)
        set(PREFBAR_LAUNCHER_TITLE_ID "PREF00001")
        set(PREFBAR_LAUNCHER_PORT 8998)
        file(MAKE_DIRECTORY "${PREFBAR_GENERATED_DIR}")

        configure_file(
            web/backend/launcher/param.json.in
            "${PREFBAR_GENERATED_DIR}/prefbar_launcher_param.json"
            @ONLY
        )

        set(PREFBAR_WEB_BLOB_CPP
            "${PREFBAR_GENERATED_DIR}/prefbar_web_blob.cpp")
        add_custom_command(
            OUTPUT "${PREFBAR_WEB_BLOB_CPP}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/embed_binary.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
                "${PREFBAR_WEB_BLOB_CPP}"
                prefbar_web_index_html
            DEPENDS web/index.html scripts/tools/embed_binary.py
            VERBATIM
        )

        set(PREFBAR_LAUNCHER_PARAM_CPP
            "${PREFBAR_GENERATED_DIR}/prefbar_launcher_param_blob.cpp")
        add_custom_command(
            OUTPUT "${PREFBAR_LAUNCHER_PARAM_CPP}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/embed_binary.py"
                "${PREFBAR_GENERATED_DIR}/prefbar_launcher_param.json"
                "${PREFBAR_LAUNCHER_PARAM_CPP}"
                prefbar_launcher_param_json
            DEPENDS
                "${PREFBAR_GENERATED_DIR}/prefbar_launcher_param.json"
                scripts/tools/embed_binary.py
            VERBATIM
        )

        set(PREFBAR_LAUNCHER_ICON_PNG
            "${PREFBAR_GENERATED_DIR}/prefbar_launcher_icon0.png")
        add_custom_command(
            OUTPUT "${PREFBAR_LAUNCHER_ICON_PNG}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/prepare_launcher_icon.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/Docs/logo.png"
                "${PREFBAR_LAUNCHER_ICON_PNG}"
            DEPENDS Docs/logo.png scripts/tools/prepare_launcher_icon.py
            VERBATIM
        )

        set(PREFBAR_LAUNCHER_ICON_CPP
            "${PREFBAR_GENERATED_DIR}/prefbar_launcher_icon_blob.cpp")
        add_custom_command(
            OUTPUT "${PREFBAR_LAUNCHER_ICON_CPP}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/embed_binary.py"
                "${PREFBAR_LAUNCHER_ICON_PNG}"
                "${PREFBAR_LAUNCHER_ICON_CPP}"
                prefbar_launcher_icon0_png
            DEPENDS "${PREFBAR_LAUNCHER_ICON_PNG}" scripts/tools/embed_binary.py
            VERBATIM
        )

        set(PREFBAR_WEB_SOURCES
            web/backend/web_server.cpp
            web/backend/app_launcher.cpp
            "${PREFBAR_WEB_BLOB_CPP}"
            "${PREFBAR_LAUNCHER_PARAM_CPP}"
            "${PREFBAR_LAUNCHER_ICON_CPP}")
    else()
        set(PREFBAR_WEB_SOURCES)
    endif()

    # Controller, PrefBar telemetry worker, and the generated renderer blob.
    add_executable(prefbar_controller
        src/ps5/prefbar_controller.cpp
        src/ps5/firmware.cpp
        src/ps5/game_process.cpp
        src/ps5/shellui_injector.cpp
        src/ps5/shellui_hook_controller.cpp
        src/ps5/stable_shellui_injector.cpp
        src/ps5/remote_call.cpp
        src/ps5/stable_elfldr_bridge.c
        src/ps5/compat/pt.c
        src/ps5/telemetry/prefbar_telemetry.c
        "${PREFBAR_SHELLUI_BLOB_CPP}"
        ${PREFBAR_WEB_SOURCES}
    )
    if(NOT PREFBAR_ENABLE_WEB)
        target_compile_definitions(prefbar_controller PRIVATE PREFBAR_NO_WEB=1)
    else()
        target_compile_definitions(prefbar_controller PRIVATE
            PREFBAR_LAUNCHER_TITLE_ID=\"${PREFBAR_LAUNCHER_TITLE_ID}\"
            PREFBAR_LAUNCHER_PORT=${PREFBAR_LAUNCHER_PORT}
        )
    endif()
    set_target_properties(prefbar_controller PROPERTIES
        OUTPUT_NAME "PrefBar_controller_v0.3.1.elf"
    )
    target_include_directories(prefbar_controller PRIVATE
        include
        src/ps5
        src/ps5/compat
        web/backend
        "${PREFBAR_SDK}"
        "${PREFBAR_SDK}/include"
    )

    set(PREFBAR_TELEMETRY_DEFINITIONS PREFBAR_EMBED=1)
    if(PREFBAR_DEBUG_NOTIFY)
        list(APPEND PREFBAR_TELEMETRY_DEFINITIONS PREFBAR_NOTIFY=1)
    endif()
    set_source_files_properties(src/ps5/telemetry/prefbar_telemetry.c
        PROPERTIES COMPILE_DEFINITIONS "${PREFBAR_TELEMETRY_DEFINITIONS}")

    target_compile_options(prefbar_controller PRIVATE
        --target=x86_64-sie-ps5
        -DPS5
        -fPIC
        -fPIE
        -ffunction-sections
        -fdata-sections
        -march=znver2
        -O2
        -Wall
        -Wextra
    )
    target_link_options(prefbar_controller PRIVATE -Wl,--gc-sections)
    set(PREFBAR_CONTROLLER_LIBRARIES
        kernel_sys
        SceLibcInternal
        SceSystemService
        SceNet
        SceSysmodule
        SceUserService
        SceNetCtl
    )
    if(PREFBAR_ENABLE_WEB)
        list(APPEND PREFBAR_CONTROLLER_LIBRARIES SceAppInstUtil)
    endif()
    target_link_libraries(prefbar_controller PRIVATE
        ${PREFBAR_CONTROLLER_LIBRARIES}
    )

else()
    # Host-only checks use the same root CMake entry point as the PS5 build.
    include(CTest)
    if(BUILD_TESTING)
        find_package(Threads REQUIRED)

        add_executable(prefbar_wire_tests
            scripts/tests/prefbar_wire_selftest.c)
        target_include_directories(prefbar_wire_tests PRIVATE include)
        target_compile_options(prefbar_wire_tests PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME prefbar_wire_tests COMMAND prefbar_wire_tests)

        add_executable(prefbar_debug_tests
            scripts/tests/prefbar_debug_selftest.c)
        target_include_directories(prefbar_debug_tests PRIVATE include)
        target_compile_definitions(prefbar_debug_tests PRIVATE
            PREFBAR_SIM=1 PREFBAR_EMBED=1)
        target_compile_options(prefbar_debug_tests PRIVATE -Wall -Wextra -UNDEBUG)
        target_link_libraries(prefbar_debug_tests PRIVATE Threads::Threads)
        add_test(NAME prefbar_debug_tests COMMAND prefbar_debug_tests)

        add_executable(prefbar_pool_tests
            scripts/tests/prefbar_pool_selftest.c)
        target_include_directories(prefbar_pool_tests PRIVATE include)
        target_compile_definitions(prefbar_pool_tests PRIVATE
            PREFBAR_SIM=1 PREFBAR_EMBED=1)
        target_compile_options(prefbar_pool_tests PRIVATE -Wall -Wextra -UNDEBUG)
        target_link_libraries(prefbar_pool_tests PRIVATE Threads::Threads)
        add_test(NAME prefbar_pool_tests COMMAND prefbar_pool_tests)

        add_executable(prefbar_dce_tests
            scripts/tests/prefbar_dce_selftest.c)
        target_include_directories(prefbar_dce_tests PRIVATE include)
        target_compile_definitions(prefbar_dce_tests PRIVATE
            PREFBAR_SIM=1 PREFBAR_EMBED=1)
        target_compile_options(prefbar_dce_tests PRIVATE -Wall -Wextra -UNDEBUG)
        target_link_libraries(prefbar_dce_tests PRIVATE Threads::Threads)
        add_test(NAME prefbar_dce_tests COMMAND prefbar_dce_tests)

        add_executable(prefbar_touchpad_tests
            scripts/tests/test_touchpad_toggle.cpp)
        target_include_directories(prefbar_touchpad_tests PRIVATE include)
        target_compile_options(prefbar_touchpad_tests PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME prefbar_touchpad_tests COMMAND prefbar_touchpad_tests)

        add_executable(prefbar_hook_protocol_tests
            scripts/tests/test_shellui_hook_protocol.cpp)
        target_include_directories(prefbar_hook_protocol_tests PRIVATE include)
        target_compile_options(prefbar_hook_protocol_tests PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME prefbar_hook_protocol_tests COMMAND prefbar_hook_protocol_tests)
        add_executable(prefbar_crash_strike_tests
            scripts/tests/test_crash_strikes.cpp)
        target_include_directories(prefbar_crash_strike_tests PRIVATE include)
        target_compile_options(prefbar_crash_strike_tests PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME prefbar_crash_strike_tests COMMAND prefbar_crash_strike_tests)
    endif()
endif()
