cmake_minimum_required(VERSION 3.31)

project(lora_messenger VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)

option(BUILD_UI "Build the LVGL application runtime" ON)
option(USE_DESKTOP "Build the SDL desktop simulator" ON)
option(BUILD_TESTING "Build core, protocol, simulation, and persistence tests" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(APP_USE_DRM "Use LVGL DRM/KMS instead of framebuffer on embedded Linux" OFF)
option(CM0_ALLOW_FETCH_DEPS "Allow downloading missing CM0 dependencies" ON)

set(APP_NAME "lora-messenger" CACHE STRING "Package and runtime asset identifier")
set(APP_ASSETS_ROOT "" CACHE PATH "Optional runtime assets root")
set(APP_KEY_INPUT_DEVICE "" CACHE STRING "Optional Linux evdev keyboard path")
set(APP_FRAMEBUFFER_DEVICE "/dev/fb0" CACHE STRING "Linux framebuffer device")
set(APP_DRM_DEVICE "/dev/dri/card0" CACHE STRING "Linux DRM device")
set(APP_DRM_CONNECTOR_ID "-1" CACHE STRING "DRM connector id; -1 auto-selects")

# Keep sanitizer instrumentation dependency-free and transitive so every core,
# application, and unit-test translation unit uses the same runtime contract.
add_library(lora_messenger_sanitizers INTERFACE)
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        target_compile_options(lora_messenger_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer
        )
        target_link_options(lora_messenger_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
        )
    else()
        message(FATAL_ERROR
            "ENABLE_SANITIZERS requires AppleClang, Clang, or GNU C++"
        )
    endif()
endif()

# This target is deliberately standard C++ only. Keep LVGL, SDL, Linux, radio,
# storage, and network dependencies out of its public and private link graph.
add_library(lora_messenger_core STATIC
    src/core/app_metadata.cpp
    src/core/uuid.cpp
    src/core/text.cpp
    src/model/identity.cpp
    src/model/post.cpp
    src/model/timeline.cpp
)
target_include_directories(lora_messenger_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_core PUBLIC lora_messenger_sanitizers)

add_library(lora_messenger_protocol STATIC
    src/protocol/dedupe_window.cpp
    src/protocol/fragmenter.cpp
    src/protocol/frame_codec.cpp
    src/protocol/post_codec.cpp
    src/protocol/reassembler.cpp
)
target_include_directories(lora_messenger_protocol PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_protocol PUBLIC
    lora_messenger_core
    lora_messenger_sanitizers
)

add_library(lora_messenger_application STATIC
    src/application/messenger_state.cpp
    src/application/transmission_scheduler.cpp
)
target_include_directories(lora_messenger_application PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_application PUBLIC
    lora_messenger_core
    lora_messenger_protocol
    lora_messenger_sanitizers
)

add_library(lora_messenger_simulated_transport STATIC
    src/adapters/transport/simulated_radio_bus.cpp
)
target_include_directories(lora_messenger_simulated_transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_simulated_transport PUBLIC
    lora_messenger_sanitizers
)

add_library(lora_messenger_lan_transport STATIC
    src/adapters/network/lan_broadcast_policy.cpp
    src/adapters/network/udp_broadcast_transport.cpp
)
if(UNIX)
    target_sources(lora_messenger_lan_transport PRIVATE
        src/adapters/network/posix_udp_broadcast_socket.cpp
    )
endif()
target_include_directories(lora_messenger_lan_transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_lan_transport PUBLIC
    lora_messenger_sanitizers
)

add_library(lora_messenger_cap_lora STATIC
    src/adapters/radio/cap_lora_1262_radio.cpp
    src/adapters/radio/japan_920_radio_policy.cpp
)
target_include_directories(lora_messenger_cap_lora PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_cap_lora PUBLIC
    lora_messenger_sanitizers
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(lora_messenger_cap_lora_linux STATIC
        src/adapters/radio/linux_cap_lora_1262_radio.cpp
    )
    target_include_directories(lora_messenger_cap_lora_linux PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(lora_messenger_cap_lora_linux PUBLIC
        lora_messenger_cap_lora
        lora_messenger_sanitizers
    )
endif()

# Phase 2 navigation, editing, localization, and UI snapshots remain pure C++.
# Keep this target before the UI boundary so the same Debug/Release/sanitizer
# presets exercise it without LVGL, SDL, FreeType, OS, storage, or transport.
add_library(lora_messenger_viewmodel STATIC
    src/viewmodel/i18n.cpp
    src/viewmodel/text_editor.cpp
    src/viewmodel/messenger_view_model.cpp
)
target_include_directories(lora_messenger_viewmodel PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_viewmodel PUBLIC
    lora_messenger_application
    lora_messenger_sanitizers
)

add_library(lora_messenger_app_script_parser STATIC
    src/platform/app_script_parser.cpp
)
target_include_directories(lora_messenger_app_script_parser PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
)
target_link_libraries(lora_messenger_app_script_parser PUBLIC
    lora_messenger_sanitizers
)

# Phase 3 pins a repaired SQLite release rather than inheriting the host SDK's
# database library. The amalgamation is isolated from core/application/viewmodel
# and from product strict-warning counts.
find_package(Threads REQUIRED)
add_library(lora_messenger_sqlite STATIC
    third_party/sqlite/sqlite3.c
)
set_target_properties(lora_messenger_sqlite PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_include_directories(lora_messenger_sqlite PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
)
target_compile_definitions(lora_messenger_sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_MMAP_SIZE=0
    SQLITE_DEFAULT_SYNCHRONOUS=2
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=2
    SQLITE_ENABLE_API_ARMOR
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_MMAP_SIZE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_TEMP_STORE=2
    SQLITE_THREADSAFE=1
)
target_link_libraries(lora_messenger_sqlite PUBLIC
    Threads::Threads
    lora_messenger_sanitizers
)

add_library(lora_messenger_storage STATIC
    src/adapters/storage/atomic_settings_store.cpp
    src/adapters/storage/settings_json.cpp
    src/adapters/storage/sqlite_history_store.cpp
    src/platform/xdg_paths.cpp
)
target_include_directories(lora_messenger_storage PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(lora_messenger_storage PUBLIC
    lora_messenger_core
    lora_messenger_protocol
    lora_messenger_sqlite
    lora_messenger_sanitizers
)

add_library(lora_messenger_persistence STATIC
    src/app/persistent_session.cpp
)
target_include_directories(lora_messenger_persistence PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
)
target_link_libraries(lora_messenger_persistence PUBLIC
    lora_messenger_application
    lora_messenger_storage
    lora_messenger_sanitizers
)

add_library(lora_messenger_radio_runtime STATIC
    src/app/radio_runtime.cpp
)
target_include_directories(lora_messenger_radio_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
)
target_link_libraries(lora_messenger_radio_runtime PUBLIC
    lora_messenger_application
    lora_messenger_persistence
    lora_messenger_protocol
    lora_messenger_sanitizers
)

if(BUILD_TESTING)
    include(CTest)

    function(lora_messenger_add_unit_test test_name source_file link_target)
        set(test_target "lora_messenger_${test_name}_test")
        add_executable(${test_target} "${source_file}")
        target_link_libraries(${test_target} PRIVATE
            ${link_target}
            lora_messenger_sanitizers
        )
        add_test(NAME ${test_name} COMMAND ${test_target})
        set_tests_properties(${test_name} PROPERTIES
            LABELS "core;unit"
            TIMEOUT 10
        )
    endfunction()

    lora_messenger_add_unit_test(core_smoke
        tests/unit/core_smoke_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(limits_contract
        tests/unit/limits_contract_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(uuid
        tests/unit/uuid_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(utf8
        tests/unit/utf8_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(key_codec
        tests/unit/key_codec_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(identity
        tests/unit/identity_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(post
        tests/unit/post_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(timeline
        tests/unit/timeline_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(timeline_restore
        tests/unit/timeline_restore_test.cpp
        lora_messenger_core
    )
    lora_messenger_add_unit_test(post_codec
        tests/unit/post_codec_test.cpp
        lora_messenger_protocol
    )
    lora_messenger_add_unit_test(frame_codec
        tests/unit/frame_codec_test.cpp
        lora_messenger_protocol
    )
    lora_messenger_add_unit_test(fragmenter
        tests/unit/fragmenter_test.cpp
        lora_messenger_protocol
    )
    lora_messenger_add_unit_test(dedupe_window
        tests/unit/dedupe_window_test.cpp
        lora_messenger_protocol
    )
    lora_messenger_add_unit_test(reassembler
        tests/unit/reassembler_test.cpp
        lora_messenger_protocol
    )
    lora_messenger_add_unit_test(messenger_state
        tests/unit/messenger_state_test.cpp
        lora_messenger_application
    )
    lora_messenger_add_unit_test(transmission_scheduler
        tests/unit/transmission_scheduler_test.cpp
        lora_messenger_application
    )
    lora_messenger_add_unit_test(simulated_radio_bus
        tests/unit/simulated_radio_bus_test.cpp
        lora_messenger_simulated_transport
    )
    lora_messenger_add_unit_test(udp_broadcast_transport
        tests/unit/udp_broadcast_transport_test.cpp
        lora_messenger_lan_transport
    )
    lora_messenger_add_unit_test(lan_broadcast_policy
        tests/unit/lan_broadcast_policy_test.cpp
        lora_messenger_lan_transport
    )
    if(UNIX)
        lora_messenger_add_unit_test(posix_udp_broadcast_socket
            tests/unit/posix_udp_broadcast_socket_test.cpp
            lora_messenger_lan_transport
        )
    endif()
    lora_messenger_add_unit_test(cap_lora_1262_radio
        tests/unit/cap_lora_1262_radio_test.cpp
        lora_messenger_cap_lora
    )
    lora_messenger_add_unit_test(japan_920_radio_policy
        tests/unit/japan_920_radio_policy_test.cpp
        lora_messenger_cap_lora
    )
    add_executable(lora_messenger_phase4_two_process_node
        tests/integration/phase4_two_process_node.cpp
    )
    target_link_libraries(
        lora_messenger_phase4_two_process_node PRIVATE
        lora_messenger_protocol
        lora_messenger_sanitizers
    )
    add_executable(lora_messenger_phase4_simulation_test
        tests/integration/phase4_simulation_test.cpp
    )
    target_link_libraries(
        lora_messenger_phase4_simulation_test PRIVATE
        lora_messenger_application
        lora_messenger_simulated_transport
        lora_messenger_sanitizers
    )
    add_dependencies(
        lora_messenger_phase4_simulation_test
        lora_messenger_phase4_two_process_node
    )
    add_test(
        NAME phase4_simulation
        COMMAND lora_messenger_phase4_simulation_test
                $<TARGET_FILE:lora_messenger_phase4_two_process_node>
    )
    set_tests_properties(phase4_simulation PROPERTIES
        LABELS "core;integration;stress"
        TIMEOUT 60
    )
    add_executable(lora_messenger_radio_runtime_test
        tests/integration/radio_runtime_test.cpp
    )
    target_link_libraries(
        lora_messenger_radio_runtime_test PRIVATE
        lora_messenger_radio_runtime
        lora_messenger_simulated_transport
        lora_messenger_sanitizers
    )
    add_test(
        NAME radio_runtime
        COMMAND lora_messenger_radio_runtime_test
    )
    set_tests_properties(radio_runtime PROPERTIES
        LABELS "core;integration;radio"
        TIMEOUT 20
    )
    lora_messenger_add_unit_test(state_persistence
        tests/unit/state_persistence_test.cpp
        lora_messenger_application
    )
    lora_messenger_add_unit_test(i18n
        tests/unit/i18n_test.cpp
        lora_messenger_viewmodel
    )
    lora_messenger_add_unit_test(text_editor
        tests/unit/text_editor_test.cpp
        lora_messenger_viewmodel
    )
    lora_messenger_add_unit_test(messenger_view_model
        tests/unit/messenger_view_model_test.cpp
        lora_messenger_viewmodel
    )
    lora_messenger_add_unit_test(app_script_parser
        tests/unit/app_script_parser_test.cpp
        lora_messenger_app_script_parser
    )
    lora_messenger_add_unit_test(settings_json
        tests/unit/settings_json_test.cpp
        lora_messenger_storage
    )
    lora_messenger_add_unit_test(atomic_settings_store
        tests/integration/atomic_settings_store_test.cpp
        lora_messenger_storage
    )
    lora_messenger_add_unit_test(sqlite_history_store
        tests/integration/sqlite_history_store_test.cpp
        lora_messenger_storage
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sqlite_migration_test.cpp")
        lora_messenger_add_unit_test(sqlite_migration
            tests/integration/sqlite_migration_test.cpp
            lora_messenger_storage
        )
    endif()
    lora_messenger_add_unit_test(persistent_session
        tests/integration/persistent_session_test.cpp
        lora_messenger_persistence
    )
    add_test(
        NAME package_metadata
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate-package-metadata.cmake"
    )
    set_tests_properties(package_metadata PROPERTIES
        LABELS "core;packaging;metadata"
        TIMEOUT 10
    )
endif()

# A core-only configure returns before FetchContent and platform discovery. This
# is the offline, hardware-independent Phase 0-4 gate.
if(NOT BUILD_UI)
    return()
endif()

include(FetchContent)

set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/src/config/lv_conf.h" CACHE PATH "" FORCE)

FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    # v9.5.0, resolved and recorded in NOTES.md.
    GIT_TAG 85aa60d18b3d5e5588d7b247abf90198f07c8a63
)
FetchContent_GetProperties(lvgl)
if(NOT lvgl_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(lvgl)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()
add_subdirectory("${lvgl_SOURCE_DIR}" "${lvgl_BINARY_DIR}" EXCLUDE_FROM_ALL)
set(LVGL_SOURCE_DIR "${lvgl_SOURCE_DIR}")

if(USE_DESKTOP)
    message(STATUS "Configuring desktop simulator")
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(SDL2 CONFIG REQUIRED)
    find_package(fmt CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)

    if(TARGET libjpeg-turbo::libjpeg-turbo)
        set(JPEG_TARGET libjpeg-turbo::libjpeg-turbo)
    elseif(TARGET JPEG::JPEG)
        set(JPEG_TARGET JPEG::JPEG)
    else()
        message(FATAL_ERROR "No JPEG target was provided")
    endif()

    set(SDL_TARGET SDL2::SDL2)
    if(NOT TARGET SDL2::SDL2)
        add_library(lora_messenger_sdl INTERFACE)
        target_include_directories(lora_messenger_sdl INTERFACE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(lora_messenger_sdl INTERFACE ${SDL2_LIBRARIES})
        set(SDL_TARGET lora_messenger_sdl)
    endif()

    target_link_libraries(lvgl PUBLIC
        ZLIB::ZLIB
        PNG::PNG
        ${JPEG_TARGET}
        Freetype::Freetype
        ${SDL_TARGET}
    )
    target_compile_definitions(lvgl PUBLIC USE_DESKTOP=1 APP_USE_DRM=0)
else()
    message(STATUS "Configuring CardputerZero aarch64 Linux build")
    if(NOT CM0_SDK_ROOT OR NOT EXISTS "${CM0_SDK_ROOT}")
        message(FATAL_ERROR "CM0_SDK_ROOT must point to the CardputerZero sysroot")
    endif()

    set(CM0_SYSROOT "${CM0_SDK_ROOT}")
    if(CMAKE_SYSROOT)
        set(CM0_SYSROOT "${CMAKE_SYSROOT}")
    endif()
    set(CM0_MULTIARCH "aarch64-linux-gnu" CACHE STRING "CM0 multiarch tuple")
    set(CM0_INCLUDE_HINTS
        "${CM0_SYSROOT}/usr/include/freetype2"
        "${CM0_SYSROOT}/usr/include/libpng16"
        "${CM0_SYSROOT}/usr/include/${CM0_MULTIARCH}"
        "${CM0_SYSROOT}/usr/include"
        "${CM0_SYSROOT}/include/${CM0_MULTIARCH}"
        "${CM0_SYSROOT}/include"
    )
    set(CM0_LIBRARY_HINTS
        "${CM0_SYSROOT}/usr/lib/${CM0_MULTIARCH}"
        "${CM0_SYSROOT}/usr/lib"
        "${CM0_SYSROOT}/lib/${CM0_MULTIARCH}"
        "${CM0_SYSROOT}/lib"
    )

    function(cm0_import_library target header library)
        find_path(_include_dir NAMES "${header}" HINTS ${CM0_INCLUDE_HINTS} NO_DEFAULT_PATH)
        find_library(_library NAMES ${library} HINTS ${CM0_LIBRARY_HINTS} NO_DEFAULT_PATH)
        if(NOT _include_dir OR NOT _library)
            message(FATAL_ERROR "Missing ${target} in ${CM0_SYSROOT}")
        endif()
        add_library(${target} UNKNOWN IMPORTED)
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
        )
        unset(_include_dir CACHE)
        unset(_library CACHE)
    endfunction()

    cm0_import_library(CM0_Freetype ft2build.h freetype)
    cm0_import_library(CM0_PNG png.h "png16;png")
    cm0_import_library(CM0_JPEG jpeglib.h jpeg)
    cm0_import_library(CM0_ZLIB zlib.h z)
    find_library(CM0_PTHREAD NAMES pthread HINTS ${CM0_LIBRARY_HINTS} NO_DEFAULT_PATH REQUIRED)
    find_library(CM0_M NAMES m HINTS ${CM0_LIBRARY_HINTS} NO_DEFAULT_PATH REQUIRED)

    set(CM0_MULTIARCH_INCLUDE_DIRS)
    foreach(CM0_INCLUDE_DIR IN ITEMS
            "${CM0_SYSROOT}/usr/include/${CM0_MULTIARCH}"
            "${CM0_SYSROOT}/include/${CM0_MULTIARCH}")
        if(EXISTS "${CM0_INCLUDE_DIR}/bits/timesize.h" OR
           EXISTS "${CM0_INCLUDE_DIR}/bits/wordsize.h")
            list(APPEND CM0_MULTIARCH_INCLUDE_DIRS "${CM0_INCLUDE_DIR}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES CM0_MULTIARCH_INCLUDE_DIRS)

    find_path(CM0_FMT_INCLUDE_DIR
        NAMES fmt/format.h
        HINTS ${CM0_INCLUDE_HINTS}
        NO_DEFAULT_PATH
    )
    find_library(CM0_FMT_LIBRARY
        NAMES fmt libfmt.so.10 libfmt.so.10.1.0
        HINTS ${CM0_LIBRARY_HINTS}
        NO_DEFAULT_PATH
    )
    if(CM0_FMT_INCLUDE_DIR AND CM0_FMT_LIBRARY)
        add_library(CM0_FMT UNKNOWN IMPORTED)
        set_target_properties(CM0_FMT PROPERTIES
            IMPORTED_LOCATION "${CM0_FMT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${CM0_FMT_INCLUDE_DIR}"
        )
        add_library(fmt::fmt ALIAS CM0_FMT)
    elseif(CM0_ALLOW_FETCH_DEPS)
        message(STATUS "Missing fmt headers in CM0 sysroot; fetching fmt 10.1.1")
        FetchContent_Declare(
            fmt
            GIT_REPOSITORY https://github.com/fmtlib/fmt.git
            # fmt 10.1.1 release commit, resolved and recorded in NOTES.md.
            GIT_TAG f5e54359df4c26b6230fc61d38aa294581393084
        )
        set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules for app packages" FORCE)
        set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
        set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
        FetchContent_MakeAvailable(fmt)
        if(TARGET fmt AND CM0_MULTIARCH_INCLUDE_DIRS)
            target_include_directories(fmt SYSTEM BEFORE PRIVATE ${CM0_MULTIARCH_INCLUDE_DIRS})
        endif()
        if(TARGET fmt AND NOT TARGET fmt::fmt)
            add_library(fmt::fmt ALIAS fmt)
        endif()
    else()
        message(FATAL_ERROR
            "Missing fmt headers/library in CM0 sysroot: ${CM0_SYSROOT}\n"
            "Provide fmt in the sysroot or reconfigure with -DCM0_ALLOW_FETCH_DEPS=ON."
        )
    endif()

    target_link_libraries(lvgl PUBLIC
        CM0_Freetype CM0_PNG CM0_JPEG CM0_ZLIB ${CM0_PTHREAD} ${CM0_M}
    )
    target_include_directories(lvgl PUBLIC ${CM0_INCLUDE_HINTS})
    target_compile_definitions(lvgl PUBLIC
        USE_DESKTOP=0
        APP_USE_DRM=$<BOOL:${APP_USE_DRM}>
    )
endif()

add_library(logger STATIC
    src/logger/logger.cpp
)
target_include_directories(logger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/logger")
target_link_libraries(logger PUBLIC fmt::fmt)

add_library(lora_messenger_platform STATIC
    src/platform/app_script.cpp
    src/platform/linux_input.cpp
    src/platform/screenshot.cpp
)
target_include_directories(lora_messenger_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${LVGL_SOURCE_DIR}"
)
target_compile_definitions(lora_messenger_platform PUBLIC
    APP_KEY_INPUT_DEVICE="${APP_KEY_INPUT_DEVICE}"
)
target_link_libraries(lora_messenger_platform PUBLIC lvgl logger)
target_link_libraries(lora_messenger_platform PRIVATE
    lora_messenger_app_script_parser
)

add_library(lora_messenger_view STATIC
    src/view/screens/messenger_screen.cpp
    src/view/theme.cpp
)
target_include_directories(lora_messenger_view PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/view"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/view/screens"
    "${LVGL_SOURCE_DIR}"
)
target_link_libraries(lora_messenger_view PUBLIC
    lvgl
    lora_messenger_viewmodel
)

set(APP_SOURCE_ASSETS_ROOT_VALUE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(NOT USE_DESKTOP)
    # Device packages must not embed a developer-machine source path.
    set(APP_SOURCE_ASSETS_ROOT_VALUE "")
endif()

add_library(lora_messenger_runtime STATIC
    src/app/app.cpp
    src/app/asset_manager.cpp
    src/app/demo_scenario.cpp
)
target_include_directories(lora_messenger_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logger"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/view"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/view/screens"
    "${LVGL_SOURCE_DIR}"
)
target_compile_definitions(lora_messenger_runtime PUBLIC
    APP_NAME="${APP_NAME}"
    APP_CMAKE_ASSETS_ROOT="${APP_ASSETS_ROOT}"
    APP_SOURCE_ASSETS_ROOT="${APP_SOURCE_ASSETS_ROOT_VALUE}"
    APP_FRAMEBUFFER_DEVICE="${APP_FRAMEBUFFER_DEVICE}"
    APP_DRM_DEVICE="${APP_DRM_DEVICE}"
    APP_DRM_CONNECTOR_ID=${APP_DRM_CONNECTOR_ID}
)
target_link_libraries(lora_messenger_runtime PUBLIC
    lvgl
    logger
    lora_messenger_application
    lora_messenger_platform
    lora_messenger_persistence
    lora_messenger_lan_transport
    lora_messenger_radio_runtime
    lora_messenger_view
    lora_messenger_viewmodel
)
if(TARGET lora_messenger_cap_lora_linux)
    target_link_libraries(lora_messenger_runtime PUBLIC
        lora_messenger_cap_lora_linux
    )
endif()

add_executable(lora_messenger src/main.cpp)
set_target_properties(lora_messenger PROPERTIES OUTPUT_NAME "lora-messenger")
target_link_libraries(lora_messenger PRIVATE lora_messenger_runtime)

if(BUILD_TESTING AND USE_DESKTOP)
    add_executable(lora_messenger_font_coverage_test
        tests/ui/font_coverage_test.cpp
    )
    target_link_libraries(lora_messenger_font_coverage_test PRIVATE
        lora_messenger_viewmodel
        Freetype::Freetype
    )
    add_test(NAME font_coverage
        COMMAND lora_messenger_font_coverage_test
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
    )
    set_tests_properties(font_coverage PROPERTIES
        LABELS "ui;i18n"
        TIMEOUT 10
    )

    set(SCREENSHOT_IO_FAILURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-work/screenshot-io-failure")
    file(MAKE_DIRECTORY "${SCREENSHOT_IO_FAILURE_DIR}")
    file(WRITE "${SCREENSHOT_IO_FAILURE_DIR}/screenshot" "blocks screenshot directory creation\n")

    set(EXPECTED_FAILURE_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-expected-script-failure.cmake")

    add_test(NAME script_rejects_empty_screenshot
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=SHOT="
            "-DEXPECTED_DIAGNOSTIC=screenshot name is empty after sanitizing"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_rejects_empty_screenshot PROPERTIES
        LABELS "ui;script;negative"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME script_reports_screenshot_io_failure
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${SCREENSHOT_IO_FAILURE_DIR}"
            "-DAPP_SCRIPT_VALUE=SHOT=blocked"
            "-DEXPECTED_DIAGNOSTIC=failed to save screenshot: screenshot/blocked.png"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_reports_screenshot_io_failure PROPERTIES
        LABELS "ui;script;negative"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME script_rejects_unknown_token
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=NOPE"
            "-DEXPECTED_DIAGNOSTIC=APP_SCRIPT parse error at token 1: unknown script token"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_rejects_unknown_token PROPERTIES
        LABELS "ui;script;negative"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME script_rejects_expectation_mismatch
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=EXPECT=screen:compose"
            "-DEXPECTED_DIAGNOSTIC=APP_SCRIPT EXPECT failed for screen: expected compose"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_rejects_expectation_mismatch PROPERTIES
        LABELS "ui;script;negative"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME script_rejects_await_timeout
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=AWAIT=screen:compose"
            "-DEXPECTED_DIAGNOSTIC=APP_SCRIPT AWAIT timed out for screen: expected compose"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_rejects_await_timeout PROPERTIES
        LABELS "ui;script;negative"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME script_rejects_incomplete_sequence
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=HOME,RIGHT,ENTER,WAIT"
            "-DEXPECTED_DIAGNOSTIC=APP_SCRIPT stopped before completing all actions"
            -P "${EXPECTED_FAILURE_RUNNER}"
    )
    set_tests_properties(script_rejects_incomplete_sequence PROPERTIES
        LABELS "ui;script;negative;lifecycle"
        TIMEOUT 8
        RESOURCE_LOCK lora_messenger_ui
    )

    # Four visually distinct 13-glyph rows exercise the conservative detail
    # wrapper and make the top/bottom scroll evidence unambiguous.
    set(PHASE2_WIDE_DETAIL
        "OOOOOOOOOOOOOWWWWWWWWWWWWWMMMMMMMMMMMMMQQQQQQQQQQQQQ")
    string(REPEAT "0123456789" 16 PHASE2_LONG_DRAFT)
    string(CONCAT PHASE2_UI_SCRIPT
        "AWAIT=screen:menu,EXPECT=locale:en,EXPECT=focus:menu.talk,"
        "SHOT=title-en-menu,ENTER,"
        "AWAIT=screen:timeline,EXPECT=focus:timeline.row,"
        "SHOT=phase2-en-timeline,ENTER,EXPECT=screen:detail,"
        "EXPECT=focus:detail.body,SHOT=phase2-en-detail,DOWN,"
        "SHOT=phase2-en-detail-scrolled,UP,N,EXPECT=screen:compose,ESC,"
        "EXPECT=screen:detail,S,EXPECT=screen:settings,ESC,"
        "EXPECT=screen:detail,ESC,N,EXPECT=screen:compose,"
        "EXPECT=focus:compose.body,SHOT=phase2-en-compose-empty,"
        "ENTER,EXPECT=modal:error,EXPECT=focus:modal.dismiss,"
        "SHOT=phase2-en-error,ENTER,"
        "EXPECT=modal:none,TEXT=Hello%20from%20Mika,TAB,"
        "EXPECT=screen:mentions,EXPECT=focus:mentions.option,"
        "SHOT=phase2-en-mentions,ESC,EXPECT=screen:compose,"
        "EXPECT=focus:compose.body,SHOT=phase2-en-compose,ENTER,"
        "EXPECT=modal:status,EXPECT=status:queued,EXPECT=focus:modal.dismiss,"
        "SHOT=phase2-en-status,ENTER,EXPECT=screen:timeline,N,"
        "TEXT=${PHASE2_WIDE_DETAIL},ENTER,EXPECT=modal:status,"
        "EXPECT=status:queued,ENTER,EXPECT=screen:timeline,ENTER,"
        "EXPECT=screen:detail,SHOT=phase2-en-detail-wide,DOWN,DOWN,"
        "SHOT=phase2-en-detail-wide-bottom,ESC,EXPECT=screen:timeline,DOWN,"
        "S,EXPECT=screen:settings,"
        "EXPECT=focus:settings.language,SHOT=phase2-en-settings,ESC,"
        "EXPECT=screen:timeline,N,TEXT=${PHASE2_LONG_DRAFT},"
        "EXPECT=focus:compose.body,SHOT=phase2-en-compose-long,HOME,"
        "EXPECT=modal:exit,EXPECT=focus:modal.cancel,SHOT=phase2-en-exit,"
        "ESC,ESC,EXPECT=modal:discard,EXPECT=focus:modal.cancel,"
        "SHOT=phase2-en-discard,RIGHT,EXPECT=focus:modal.confirm,ENTER,"
        "EXPECT=screen:timeline,S,RIGHT,"
        "EXPECT=locale:ja,EXPECT=screen:settings,EXPECT=focus:settings.language,"
        "SHOT=phase2-ja-settings,"
        "ESC,EXPECT=screen:timeline,SHOT=phase2-ja-timeline,S,RIGHT,"
        "EXPECT=locale:zh-hans,EXPECT=screen:settings,"
        "SHOT=phase2-zh-hans-settings,ESC,EXPECT=screen:timeline,"
        "SHOT=phase2-zh-hans-timeline,HOME,EXPECT=modal:exit,"
        "EXPECT=focus:modal.cancel,RIGHT,EXPECT=focus:modal.confirm,ENTER"
    )
    set(PHASE2_UI_CAPTURES
        title-en-menu
        phase2-en-timeline
        phase2-en-detail
        phase2-en-detail-scrolled
        phase2-en-compose-empty
        phase2-en-error
        phase2-en-mentions
        phase2-en-compose
        phase2-en-compose-long
        phase2-en-status
        phase2-en-detail-wide
        phase2-en-detail-wide-bottom
        phase2-en-settings
        phase2-en-exit
        phase2-en-discard
        phase2-ja-settings
        phase2-ja-timeline
        phase2-zh-hans-settings
        phase2-zh-hans-timeline
    )
    add_test(NAME ui_phase2_keyboard_flow
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-work/ui-phase2-$<CONFIG>"
            "-DALLOWED_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-work"
            "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=${PHASE2_UI_SCRIPT}"
            "-DEXPECTED_SCREENSHOTS=${PHASE2_UI_CAPTURES}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ui-script.cmake"
    )
    set_tests_properties(ui_phase2_keyboard_flow PROPERTIES
        LABELS "ui;script;e2e"
        TIMEOUT 15
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME ui_window_close_teardown
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-work/ui-close-$<CONFIG>"
            "-DALLOWED_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-work"
            "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=AWAIT=screen:menu,EXPECT=focus:menu.talk,SHOT=phase2-window-close,CLOSE"
            "-DEXPECTED_SCREENSHOTS=phase2-window-close"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ui-script.cmake"
    )
    set_tests_properties(ui_window_close_teardown PROPERTIES
        LABELS "ui;script;lifecycle"
        TIMEOUT 15
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME ui_phase3_restart_persistence
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-work/ui-phase3-restart-$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ui-restart.cmake"
    )
    set_tests_properties(ui_phase3_restart_persistence PROPERTIES
        LABELS "ui;script;e2e;persistence"
        TIMEOUT 30
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME ui_phase3_recovery
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-work/ui-phase3-recovery-$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ui-recovery.cmake"
    )
    set_tests_properties(ui_phase3_recovery PROPERTIES
        LABELS "ui;script;e2e;persistence;recovery"
        TIMEOUT 30
        RESOURCE_LOCK lora_messenger_ui
    )

    add_test(NAME ui_phase3_delete_confirmation
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:lora_messenger>"
            "-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-work/ui-phase3-delete-$<CONFIG>"
            "-DALLOWED_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-work"
            "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAPP_SCRIPT_VALUE=AWAIT=screen:menu,ENTER,AWAIT=screen:timeline,S,EXPECT=screen:settings,D,EXPECT=modal:delete-data,EXPECT=focus:modal.cancel,SHOT=phase3-delete-confirmation,ENTER,EXPECT=modal:none,D,EXPECT=modal:delete-data,EXPECT=focus:modal.cancel,RIGHT,EXPECT=focus:modal.confirm,ENTER"
            "-DEXPECTED_SCREENSHOTS=phase3-delete-confirmation"
            "-DEXPECT_LOCAL_DATA_REMOVED=ON"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ui-script.cmake"
    )
    set_tests_properties(ui_phase3_delete_confirmation PROPERTIES
        LABELS "ui;script;e2e;persistence"
        TIMEOUT 15
        RESOURCE_LOCK lora_messenger_ui
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND
   NOT USE_DESKTOP)
    include(cmake/cm0-package.cmake)
endif()
