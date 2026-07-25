cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}")
    message(FATAL_ERROR "SOURCE_ROOT must name the project directory")
endif()

function(assert_json_equal json path expected)
    string(JSON actual GET "${json}" ${path})
    if(NOT "${actual}" STREQUAL "${expected}")
        string(JOIN "." dotted_path ${path})
        message(FATAL_ERROR
            "${dotted_path}: expected '${expected}', got '${actual}'"
        )
    endif()
endfunction()

function(assert_png_dimensions path expected_width expected_height)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing PNG: ${path}")
    endif()
    file(READ "${path}" png_header OFFSET 0 LIMIT 24 HEX)
    string(TOLOWER "${png_header}" png_header)
    string(SUBSTRING "${png_header}" 0 16 signature)
    if(NOT signature STREQUAL "89504e470d0a1a0a")
        message(FATAL_ERROR "Not a PNG: ${path}")
    endif()
    string(SUBSTRING "${png_header}" 32 8 width_hex)
    string(SUBSTRING "${png_header}" 40 8 height_hex)
    math(EXPR width "0x${width_hex}")
    math(EXPR height "0x${height_hex}")
    if(NOT width EQUAL expected_width OR NOT height EQUAL expected_height)
        message(FATAL_ERROR
            "${path}: expected ${expected_width}x${expected_height}, "
            "got ${width}x${height}"
        )
    endif()
endfunction()

file(READ "${SOURCE_ROOT}/app-builder.json" manifest)
assert_json_equal("${manifest}" "package_name" "lora-messenger")
assert_json_equal("${manifest}" "version" "0.1.0")
assert_json_equal("${manifest}" "bin_name" "lora-messenger")
assert_json_equal("${manifest}" "runtime" "legacy-deb-only")
assert_json_equal("${manifest}" "lvgl_version" "9.5")

string(JSON caps_length LENGTH "${manifest}" caps)
if(NOT caps_length EQUAL 2)
    message(FATAL_ERROR "caps must contain exactly keyboard and filesystem")
endif()
assert_json_equal("${manifest}" "caps;0" "keyboard")
assert_json_equal("${manifest}" "caps;1" "filesystem")

assert_json_equal("${manifest}" "store;license" "MIT")
foreach(locale IN ITEMS en ja zh-CN)
    string(JSON locale_type TYPE "${manifest}" store locales "${locale}")
    if(NOT locale_type STREQUAL "OBJECT")
        message(FATAL_ERROR "Missing store locale: ${locale}")
    endif()
    string(JSON locale_title GET "${manifest}" store locales "${locale}" title)
    string(JSON locale_summary GET "${manifest}" store locales "${locale}" summary)
    if(locale_title STREQUAL "" OR locale_summary STREQUAL "")
        message(FATAL_ERROR "Incomplete store locale: ${locale}")
    endif()
endforeach()

string(JSON screenshot_count LENGTH "${manifest}" store screenshots)
if(screenshot_count LESS 1)
    message(FATAL_ERROR "At least one store screenshot is required")
endif()
math(EXPR screenshot_last "${screenshot_count} - 1")
foreach(index RANGE 0 ${screenshot_last})
    string(JSON screenshot GET "${manifest}" store screenshots ${index})
    assert_png_dimensions("${SOURCE_ROOT}/${screenshot}" 320 170)
endforeach()

string(JSON icon GET "${manifest}" store icon)
assert_png_dimensions("${SOURCE_ROOT}/${icon}" 512 512)

string(JSON permissions_length LENGTH "${manifest}" store permissions)
if(NOT permissions_length EQUAL 12)
    message(FATAL_ERROR
        "store.permissions must contain exactly the 12 approved keys"
    )
endif()
assert_json_equal("${manifest}" "store;permissions;keyboard_input" "ON")
assert_json_equal("${manifest}" "store;permissions;network" "ON")
assert_json_equal("${manifest}" "store;permissions;filesystem" "app-data-only")
assert_json_equal("${manifest}" "store;permissions;external_hardware" "ON")
assert_json_equal("${manifest}" "store;permissions;hdmi_output" "OFF")
assert_json_equal("${manifest}" "store;permissions;background_service" "OFF")
assert_json_equal("${manifest}" "store;permissions;audio_output" "OFF")
assert_json_equal("${manifest}" "store;permissions;microphone" "OFF")
assert_json_equal("${manifest}" "store;permissions;camera" "OFF")
assert_json_equal("${manifest}" "store;permissions;sensors" "OFF")
assert_json_equal("${manifest}" "store;permissions;gps" "OFF")
assert_json_equal("${manifest}" "store;permissions;device_id" "OFF")

file(READ "${SOURCE_ROOT}/cmake/templates/app.desktop.in" desktop_template)
foreach(required_line IN ITEMS
        "Type=Application"
        "Exec=/usr/share/APPLaunch/bin/@APP_PACKAGE_NAME@-launch"
        "Icon=share/images/lora-messenger.png"
        "Terminal=false")
    string(FIND "${desktop_template}" "${required_line}" line_offset)
    if(line_offset EQUAL -1)
        message(FATAL_ERROR "Desktop template lacks: ${required_line}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/cmake/cm0-package.cmake" package_rules)
foreach(forbidden IN ITEMS
        ".service"
        "CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA"
        "postinst"
        "preinst"
        "prerm"
        "postrm")
    string(FIND "${package_rules}" "${forbidden}" forbidden_offset)
    if(NOT forbidden_offset EQUAL -1)
        message(FATAL_ERROR
            "Package rules unexpectedly mention forbidden '${forbidden}'"
        )
    endif()
endforeach()

message(STATUS
    "Package metadata valid: ${screenshot_count} screenshots, "
    "three locales, explicit permissions, no service/control scripts"
)
