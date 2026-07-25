foreach(required IN ITEMS TEST_EXECUTABLE TEST_WORKING_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(config_home "${TEST_WORKING_DIRECTORY}/xdg-config")
set(data_home "${TEST_WORKING_DIRECTORY}/xdg-data")
set(config_dir "${config_home}/lora-messenger")
set(data_dir "${data_home}/lora-messenger")
set(settings_file "${config_dir}/settings.json")
set(database_file "${data_dir}/history.sqlite3")
file(MAKE_DIRECTORY
    "${TEST_WORKING_DIRECTORY}/screenshot"
    "${config_dir}"
    "${data_dir}"
)
file(REMOVE
    "${settings_file}"
    "${settings_file}.tmp"
    "${database_file}"
    "${database_file}-wal"
    "${database_file}-shm"
    "${database_file}-journal"
    "${TEST_WORKING_DIRECTORY}/screenshot/phase3-restart-ja-timeline.png"
)

set(common_environment
    "${CMAKE_COMMAND}" -E env
    "SDL_VIDEODRIVER=dummy"
    "APP_SCRIPT_INTERVAL_MS=20"
    "LC_ALL=C"
    "TZ=UTC"
    "NO_COLOR=1"
    "XDG_CONFIG_HOME=${config_home}"
    "XDG_DATA_HOME=${data_home}"
)

string(CONCAT first_script
    "AWAIT=screen:menu,EXPECT=focus:menu.talk,ENTER,"
    "AWAIT=screen:timeline,EXPECT=persistence:ready,EXPECT=count:3,"
    "N,TEXT=Persistent%20post,ENTER,EXPECT=modal:status,ENTER,"
    "EXPECT=count:4,S,RIGHT,EXPECT=locale:ja,"
    "DOWN,EXPECT=focus:settings.skip-title,ENTER,ESC,"
    "HOME,RIGHT,ENTER"
)
execute_process(
    COMMAND ${common_environment}
        "LORA_MESSENGER_SEED_DEMO=1"
        "APP_SCRIPT=${first_script}"
        "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_output
    ERROR_VARIABLE first_error
    TIMEOUT 12
)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR
        "first restart run exited ${first_result}\n${first_output}\n${first_error}")
endif()
if(NOT EXISTS "${settings_file}" OR NOT EXISTS "${database_file}")
    message(FATAL_ERROR "first restart run did not create settings and history")
endif()
file(READ "${settings_file}" first_settings)
string(REGEX MATCH
    "\"install_uuid\":\"([0-9a-f-]+)\""
    first_uuid_match "${first_settings}")
if(first_uuid_match STREQUAL "")
    message(FATAL_ERROR "cannot read canonical install_uuid after first run")
endif()
set(first_uuid "${CMAKE_MATCH_1}")

string(CONCAT second_script
    "AWAIT=screen:timeline,EXPECT=persistence:ready,EXPECT=locale:ja,"
    "EXPECT=count:4,EXPECT=newest-state:unknown,"
    "SHOT=phase3-restart-ja-timeline,HOME,RIGHT,ENTER"
)
execute_process(
    COMMAND ${common_environment}
        "LORA_MESSENGER_SEED_DEMO=0"
        "APP_SCRIPT=${second_script}"
        "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second_output
    ERROR_VARIABLE second_error
    TIMEOUT 12
)
if(NOT second_result EQUAL 0)
    message(FATAL_ERROR
        "second restart run exited ${second_result}\n${second_output}\n${second_error}")
endif()
file(READ "${settings_file}" second_settings)
string(REGEX MATCH
    "\"install_uuid\":\"([0-9a-f-]+)\""
    second_uuid_match "${second_settings}")
set(second_uuid "${CMAKE_MATCH_1}")
if(second_uuid_match STREQUAL "" OR
   NOT second_uuid STREQUAL first_uuid)
    message(FATAL_ERROR "install_uuid changed across restart")
endif()

set(image
    "${TEST_WORKING_DIRECTORY}/screenshot/phase3-restart-ja-timeline.png")
if(NOT EXISTS "${image}")
    message(FATAL_ERROR "restart screenshot was not created")
endif()
file(READ "${image}" dimensions OFFSET 16 LIMIT 8 HEX)
string(TOLOWER "${dimensions}" dimensions)
if(NOT dimensions STREQUAL "00000140000000aa")
    message(FATAL_ERROR "restart screenshot is not 320x170")
endif()

set(first_combined "${first_output}\n${first_error}")
set(second_combined "${second_output}\n${second_error}")
foreach(output IN ITEMS first_combined second_combined)
    string(FIND "${${output}}" "common UI teardown complete" teardown)
    if(teardown EQUAL -1)
        message(FATAL_ERROR "missing common teardown in ${output}")
    endif()
endforeach()

message(STATUS
    "UI restart preserved UUID ${first_uuid}, locale ja, four posts, and unknown queued state")
