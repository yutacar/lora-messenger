# SPDX-License-Identifier: MIT

foreach(required_variable IN ITEMS
        TEST_EXECUTABLE
        TEST_WORKING_DIRECTORY
        APP_SCRIPT_VALUE
        EXPECTED_DIAGNOSTIC)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "Missing required test variable: ${required_variable}")
    endif()
endforeach()

set(ENV{SDL_VIDEODRIVER} "dummy")
set(ENV{APP_SCRIPT} "${APP_SCRIPT_VALUE}")
set(ENV{APP_SCRIPT_INTERVAL_MS} "20")
set(ENV{NO_COLOR} "1")
set(test_config_home "${TEST_WORKING_DIRECTORY}/xdg-config")
set(test_data_home "${TEST_WORKING_DIRECTORY}/xdg-data")
file(MAKE_DIRECTORY
    "${test_config_home}/lora-messenger"
    "${test_data_home}/lora-messenger"
)
file(REMOVE
    "${test_config_home}/lora-messenger/settings.json"
    "${test_config_home}/lora-messenger/settings.json.tmp"
    "${test_data_home}/lora-messenger/history.sqlite3"
    "${test_data_home}/lora-messenger/history.sqlite3-wal"
    "${test_data_home}/lora-messenger/history.sqlite3-shm"
    "${test_data_home}/lora-messenger/history.sqlite3-journal"
)
set(ENV{XDG_CONFIG_HOME} "${test_config_home}")
set(ENV{XDG_DATA_HOME} "${test_data_home}")
set(ENV{LORA_MESSENGER_SEED_DEMO} "1")

execute_process(
    COMMAND "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE application_status
    OUTPUT_VARIABLE application_stdout
    ERROR_VARIABLE application_stderr
    TIMEOUT 5
)

set(application_output "${application_stdout}\n${application_stderr}")
message("${application_output}")

if(NOT application_status MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Application did not exit normally: ${application_status}")
endif()
if(NOT application_status EQUAL 1)
    message(FATAL_ERROR "Expected application exit 1, got ${application_status}")
endif()

string(FIND "${application_output}" "${EXPECTED_DIAGNOSTIC}" expected_index)
if(expected_index EQUAL -1)
    message(FATAL_ERROR
        "Missing expected failure diagnostic: ${EXPECTED_DIAGNOSTIC}"
    )
endif()

set(propagation_diagnostic "APP_SCRIPT failed; stopping with a non-zero status")
string(FIND "${application_output}" "${propagation_diagnostic}" propagation_index)
if(propagation_index EQUAL -1)
    message(FATAL_ERROR
        "Missing APP_SCRIPT failure propagation diagnostic: ${propagation_diagnostic}"
    )
endif()

set(teardown_diagnostic "common UI teardown complete")
string(FIND "${application_output}" "${teardown_diagnostic}" teardown_index)
if(teardown_index EQUAL -1)
    message(FATAL_ERROR
        "Missing common teardown diagnostic: ${teardown_diagnostic}"
    )
endif()
