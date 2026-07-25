# SPDX-License-Identifier: MIT
# APPLaunch/CPack layout for the standalone CardputerZero application.

include(GNUInstallDirs)

set(APP_PACKAGE_NAME "lora-messenger" CACHE STRING "Debian package identifier")
set(APP_EXECUTABLE_NAME "lora-messenger" CACHE STRING "Installed executable name")
set(APP_DISPLAY_NAME "LoRa Messenger" CACHE STRING "Human-readable application name")
set(APP_DEBIAN_REVISION "1" CACHE STRING "Debian revision")
set(APP_DEBIAN_ARCHITECTURE "arm64" CACHE STRING "Debian architecture")
set(APP_MAINTAINER "LoRa Messenger contributors <noreply@example.invalid>" CACHE STRING
    "Debian maintainer; replace before publication")
set(APP_PACKAGE_DESCRIPTION "Offline LoRa messenger for CardputerZero with opt-in Cap LoRa-1262 transport" CACHE STRING
    "Debian package summary")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate-package-metadata.cmake"
    RESULT_VARIABLE APP_PACKAGE_METADATA_RESULT
    OUTPUT_VARIABLE APP_PACKAGE_METADATA_STDOUT
    ERROR_VARIABLE APP_PACKAGE_METADATA_STDERR
)
if(NOT APP_PACKAGE_METADATA_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Package metadata validation failed during configure:\n"
        "${APP_PACKAGE_METADATA_STDOUT}${APP_PACKAGE_METADATA_STDERR}"
    )
endif()
message(STATUS "Package metadata preflight passed")

set(APPLAUNCH_DATADIR "${CMAKE_INSTALL_DATADIR}/APPLaunch")
set(APP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/package")
file(MAKE_DIRECTORY "${APP_GENERATED_DIR}")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/app.desktop.in"
    "${APP_GENERATED_DIR}/${APP_PACKAGE_NAME}.desktop"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/app-launch.in"
    "${APP_GENERATED_DIR}/${APP_PACKAGE_NAME}-launch"
    @ONLY
)

install(TARGETS lora_messenger
    RUNTIME DESTINATION "${APPLAUNCH_DATADIR}/bin"
)
install(PROGRAMS "${APP_GENERATED_DIR}/${APP_PACKAGE_NAME}-launch"
    DESTINATION "${APPLAUNCH_DATADIR}/bin"
)
install(FILES "${APP_GENERATED_DIR}/${APP_PACKAGE_NAME}.desktop"
    DESTINATION "${APPLAUNCH_DATADIR}/applications"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/lora-messenger.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/lora-messenger_100.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/lora-messenger_80.png"
    DESTINATION "${APPLAUNCH_DATADIR}/share/images"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/inter-medium.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/inter-regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/lora-ui-ja.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/lora-ui-zh-hans.otf"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/fonts"
)
# App's own runtime asset (AssetManager's /usr/share/<app_name> lookup
# root -- see asset_manager.cpp's add_root_if_valid calls): the title
# screen's generated wordmark (tools/generate_title_logo.py), resolved at
# runtime as "images/title_logo.png" (src/app/app.cpp).
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/title_logo.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/images"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/licenses"
    PATTERN ".DS_Store" EXCLUDE
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${APP_PACKAGE_NAME}"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/phase6-cap-lora-1262.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${APP_PACKAGE_NAME}/docs"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${APP_PACKAGE_NAME}"
    RENAME "copyright"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${APP_PACKAGE_NAME}/assets/fonts"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${APP_PACKAGE_NAME}/assets/licenses"
    PATTERN ".DS_Store" EXCLUDE
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_NAME "${APP_PACKAGE_NAME}")
set(CPACK_PACKAGE_VENDOR "LoRa Messenger contributors")
set(CPACK_PACKAGE_CONTACT "${APP_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${APP_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${APP_PACKAGE_NAME}_${PROJECT_VERSION}-${APP_DEBIAN_REVISION}_${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

set(CPACK_DEBIAN_PACKAGE_NAME "${APP_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}-${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${APP_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CM0_PACKAGE_SHLIBDEPS_DEFAULT OFF)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CROSSCOMPILING)
    set(CM0_PACKAGE_SHLIBDEPS_DEFAULT ON)
endif()
option(CM0_PACKAGE_SHLIBDEPS
    "Generate Debian dependencies with dpkg-shlibdeps on a Debian cross-build host"
    "${CM0_PACKAGE_SHLIBDEPS_DEFAULT}"
)
if(CM0_PACKAGE_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
else()
    # macOS has no target dpkg database, so local inspection packages use the
    # current Debian trixie runtime names. Release packages are generated in
    # the Linux cross-build gate above, where dpkg-shlibdeps adds ABI minima.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6 (>= 2.34), libfreetype6 (>= 2.2.1), libgcc-s1 (>= 4.5), libpng16-16t64 (>= 1.6.46), libstdc++6 (>= 14), libjpeg62-turbo (>= 1:2.1.5), zlib1g (>= 1:1.2.0)")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
endif()
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
set(CPACK_PACKAGE_TARGET_SYSROOT "${CMAKE_SYSROOT}")
set(CPACK_PACKAGE_VALIDATOR
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/package/validate_deb.py")
set(CPACK_POST_BUILD_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate-deb-package.cmake")

# This project is user-launched. It intentionally generates and installs no
# systemd unit or background service.
include(CPack)
