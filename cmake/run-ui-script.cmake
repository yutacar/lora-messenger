if(NOT DEFINED TEST_EXECUTABLE OR TEST_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED TEST_WORKING_DIRECTORY OR TEST_WORKING_DIRECTORY STREQUAL "")
    message(FATAL_ERROR "TEST_WORKING_DIRECTORY is required")
endif()
if(NOT DEFINED ALLOWED_TEST_ROOT OR ALLOWED_TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "ALLOWED_TEST_ROOT is required")
endif()
if(NOT DEFINED BINARY_ROOT OR BINARY_ROOT STREQUAL "")
    message(FATAL_ERROR "BINARY_ROOT is required")
endif()
if(NOT DEFINED APP_SCRIPT_VALUE OR APP_SCRIPT_VALUE STREQUAL "")
    message(FATAL_ERROR "APP_SCRIPT_VALUE is required")
endif()
if(NOT DEFINED EXPECTED_SCREENSHOTS OR EXPECTED_SCREENSHOTS STREQUAL "")
    message(FATAL_ERROR "EXPECTED_SCREENSHOTS is required")
endif()

foreach(path_variable IN ITEMS TEST_WORKING_DIRECTORY ALLOWED_TEST_ROOT BINARY_ROOT)
    cmake_path(IS_ABSOLUTE ${path_variable} path_is_absolute)
    if(NOT path_is_absolute)
        message(FATAL_ERROR "${path_variable} must be absolute")
    endif()
    cmake_path(NORMAL_PATH ${path_variable}
        OUTPUT_VARIABLE normalized_${path_variable})
endforeach()

set(test_working_directory "${normalized_TEST_WORKING_DIRECTORY}")
set(allowed_test_root "${normalized_ALLOWED_TEST_ROOT}")
set(binary_root "${normalized_BINARY_ROOT}")
cmake_path(IS_PREFIX binary_root "${allowed_test_root}" NORMALIZE allowed_in_binary)
cmake_path(IS_PREFIX allowed_test_root "${test_working_directory}" NORMALIZE work_in_allowed)
if(NOT allowed_in_binary OR allowed_test_root STREQUAL binary_root)
    message(FATAL_ERROR "ALLOWED_TEST_ROOT must be a strict child of BINARY_ROOT")
endif()
if(NOT work_in_allowed OR test_working_directory STREQUAL allowed_test_root OR
   test_working_directory STREQUAL binary_root)
    message(FATAL_ERROR
        "TEST_WORKING_DIRECTORY must be a strict child of ALLOWED_TEST_ROOT")
endif()

file(MAKE_DIRECTORY "${test_working_directory}/screenshot")
set(test_config_home "${test_working_directory}/xdg-config")
set(test_data_home "${test_working_directory}/xdg-data")
file(MAKE_DIRECTORY
    "${test_config_home}/lora-messenger"
    "${test_data_home}/lora-messenger"
)
if(NOT DEFINED RESET_PERSISTENCE OR RESET_PERSISTENCE)
    file(REMOVE
        "${test_config_home}/lora-messenger/settings.json"
        "${test_config_home}/lora-messenger/settings.json.tmp"
        "${test_data_home}/lora-messenger/history.sqlite3"
        "${test_data_home}/lora-messenger/history.sqlite3-wal"
        "${test_data_home}/lora-messenger/history.sqlite3-shm"
        "${test_data_home}/lora-messenger/history.sqlite3-journal"
    )
endif()
if(NOT DEFINED SEED_DEMO)
    set(SEED_DEMO "1")
endif()
foreach(stem IN LISTS EXPECTED_SCREENSHOTS)
    string(LENGTH "${stem}" stem_length)
    if(stem_length GREATER 64 OR
       NOT stem MATCHES "^([a-z0-9]|[a-z0-9][a-z0-9._-]*[a-z0-9])$")
        message(FATAL_ERROR "unsafe expected screenshot stem: ${stem}")
    endif()
    file(REMOVE "${test_working_directory}/screenshot/${stem}.png")
endforeach()
file(GLOB old_temporary_files
    "${test_working_directory}/screenshot/*.tmp-*"
    "${test_working_directory}/screenshot/.*.tmp-*"
)
if(old_temporary_files)
    file(REMOVE ${old_temporary_files})
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SDL_VIDEODRIVER=dummy"
        "APP_SCRIPT=${APP_SCRIPT_VALUE}"
        "APP_SCRIPT_INTERVAL_MS=20"
        "LC_ALL=C"
        "TZ=UTC"
        "NO_COLOR=1"
        "XDG_CONFIG_HOME=${test_config_home}"
        "XDG_DATA_HOME=${test_data_home}"
        "LORA_MESSENGER_SEED_DEMO=${SEED_DEMO}"
        "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${test_working_directory}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
    TIMEOUT 12
)

set(combined_output "${standard_output}\n${standard_error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "UI script exited ${result}\n--- output ---\n${combined_output}")
endif()
string(FIND "${combined_output}" "common UI teardown complete" teardown_position)
if(teardown_position EQUAL -1)
    message(FATAL_ERROR
        "the common UI teardown diagnostic was not observed\n${combined_output}")
endif()
string(FIND "${combined_output}" "APP_SCRIPT completed successfully" completion_position)
if(completion_position EQUAL -1)
    message(FATAL_ERROR
        "the APP_SCRIPT completion diagnostic was not observed\n${combined_output}")
endif()

if(DEFINED EXPECT_LOCAL_DATA_REMOVED AND EXPECT_LOCAL_DATA_REMOVED)
    foreach(leaf IN ITEMS
            "${test_config_home}/lora-messenger/settings.json"
            "${test_config_home}/lora-messenger/settings.json.tmp"
            "${test_data_home}/lora-messenger/history.sqlite3"
            "${test_data_home}/lora-messenger/history.sqlite3-wal"
            "${test_data_home}/lora-messenger/history.sqlite3-shm"
            "${test_data_home}/lora-messenger/history.sqlite3-journal"
            "${test_data_home}/lora-messenger/history.sqlite3.probe")
        if(EXISTS "${leaf}")
            message(FATAL_ERROR
                "confirmed deletion left app-owned local leaf: ${leaf}")
        endif()
    endforeach()
endif()

foreach(stem IN LISTS EXPECTED_SCREENSHOTS)
    set(image "${test_working_directory}/screenshot/${stem}.png")
    if(NOT EXISTS "${image}")
        message(FATAL_ERROR "expected screenshot was not created: ${image}")
    endif()
    file(READ "${image}" png_dimensions OFFSET 16 LIMIT 8 HEX)
    string(TOLOWER "${png_dimensions}" png_dimensions)
    if(NOT png_dimensions STREQUAL "00000140000000aa")
        message(FATAL_ERROR
            "${image} is not a 320x170 PNG; IHDR dimensions=${png_dimensions}")
    endif()
endforeach()

file(GLOB temporary_files
    "${test_working_directory}/screenshot/*.tmp-*"
    "${test_working_directory}/screenshot/.*.tmp-*"
)
if(temporary_files)
    message(FATAL_ERROR "temporary screenshot files remain: ${temporary_files}")
endif()

message(STATUS
    "UI script passed with 320x170 captures: ${EXPECTED_SCREENSHOTS}")
