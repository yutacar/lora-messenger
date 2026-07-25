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
set(image "${TEST_WORKING_DIRECTORY}/screenshot/phase3-recovery.png")
set(local_leaves
    "${settings_file}"
    "${settings_file}.tmp"
    "${database_file}"
    "${database_file}-wal"
    "${database_file}-shm"
    "${database_file}-journal"
)
file(MAKE_DIRECTORY
    "${TEST_WORKING_DIRECTORY}/screenshot"
    "${config_dir}"
    "${data_dir}"
)
file(REMOVE ${local_leaves} "${image}")
file(WRITE "${settings_file}" "{broken")
file(WRITE "${settings_file}.tmp" "settings-temp-sentinel")
file(WRITE "${database_file}" "database-sentinel")
file(WRITE "${database_file}-wal" "wal-sentinel")
file(WRITE "${database_file}-shm" "shm-sentinel")
file(WRITE "${database_file}-journal" "journal-sentinel")

set(leaf_index 0)
foreach(leaf IN LISTS local_leaves)
    file(READ "${leaf}" "before_${leaf_index}" HEX)
    math(EXPR leaf_index "${leaf_index} + 1")
endforeach()

set(common_environment
    "${CMAKE_COMMAND}" -E env
    "SDL_VIDEODRIVER=dummy"
    "APP_SCRIPT_INTERVAL_MS=20"
    "LC_ALL=C"
    "TZ=UTC"
    "NO_COLOR=1"
    "XDG_CONFIG_HOME=${config_home}"
    "XDG_DATA_HOME=${data_home}"
    "LORA_MESSENGER_SEED_DEMO=0"
)

execute_process(
    COMMAND ${common_environment}
        "APP_SCRIPT=AWAIT=modal:recovery,EXPECT=persistence:recovery,EXPECT=focus:modal.cancel,SHOT=phase3-recovery,ENTER"
        "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE cancel_result
    OUTPUT_VARIABLE cancel_output
    ERROR_VARIABLE cancel_error
    TIMEOUT 12
)
if(NOT cancel_result EQUAL 0)
    message(FATAL_ERROR
        "recovery cancel exited ${cancel_result}\n${cancel_output}\n${cancel_error}")
endif()
set(cancel_combined_output "${cancel_output}\n${cancel_error}")
foreach(diagnostic IN ITEMS
        "common UI teardown complete"
        "APP_SCRIPT completed successfully")
    string(FIND "${cancel_combined_output}" "${diagnostic}"
        diagnostic_position)
    if(diagnostic_position EQUAL -1)
        message(FATAL_ERROR
            "recovery cancel omitted diagnostic '${diagnostic}'\n"
            "${cancel_combined_output}")
    endif()
endforeach()
set(leaf_index 0)
foreach(leaf IN LISTS local_leaves)
    if(NOT EXISTS "${leaf}")
        message(FATAL_ERROR "recovery cancel removed local leaf: ${leaf}")
    endif()
    file(READ "${leaf}" after HEX)
    if(NOT after STREQUAL "${before_${leaf_index}}")
        message(FATAL_ERROR "recovery cancel modified local leaf: ${leaf}")
    endif()
    math(EXPR leaf_index "${leaf_index} + 1")
endforeach()

execute_process(
    COMMAND ${common_environment}
        "APP_SCRIPT=AWAIT=modal:recovery,EXPECT=focus:modal.cancel,RIGHT,EXPECT=focus:modal.confirm,ENTER"
        "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE delete_result
    OUTPUT_VARIABLE delete_output
    ERROR_VARIABLE delete_error
    TIMEOUT 12
)
if(NOT delete_result EQUAL 0)
    message(FATAL_ERROR
        "recovery delete exited ${delete_result}\n${delete_output}\n${delete_error}")
endif()
set(delete_combined_output "${delete_output}\n${delete_error}")
foreach(diagnostic IN ITEMS
        "common UI teardown complete"
        "APP_SCRIPT completed successfully")
    string(FIND "${delete_combined_output}" "${diagnostic}"
        diagnostic_position)
    if(diagnostic_position EQUAL -1)
        message(FATAL_ERROR
            "recovery deletion omitted diagnostic '${diagnostic}'\n"
            "${delete_combined_output}")
    endif()
endforeach()
foreach(leaf IN LISTS local_leaves)
    if(EXISTS "${leaf}")
        message(FATAL_ERROR
            "confirmed recovery deletion left local leaf: ${leaf}")
    endif()
endforeach()

if(NOT EXISTS "${image}")
    message(FATAL_ERROR "recovery screenshot was not created")
endif()
file(READ "${image}" dimensions OFFSET 16 LIMIT 8 HEX)
string(TOLOWER "${dimensions}" dimensions)
if(NOT dimensions STREQUAL "00000140000000aa")
    message(FATAL_ERROR "recovery screenshot is not 320x170")
endif()

message(STATUS
    "all six local leaves were preserved on cancel and removed on confirmation")
