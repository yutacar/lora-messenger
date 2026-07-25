# SPDX-License-Identifier: MIT
# CPack post-build hook: validate every generated Debian archive.

if(NOT CPACK_PACKAGE_FILES)
    message(FATAL_ERROR "CPack produced no package files to validate")
endif()
if(NOT CPACK_PACKAGE_VALIDATOR OR
   NOT EXISTS "${CPACK_PACKAGE_VALIDATOR}")
    message(FATAL_ERROR
        "Debian package validator is missing: ${CPACK_PACKAGE_VALIDATOR}"
    )
endif()

find_program(_package_python NAMES python3 REQUIRED)
set(_validated_debs 0)
foreach(_package_file IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _package_file MATCHES "\\.deb$")
        continue()
    endif()
    math(EXPR _validated_debs "${_validated_debs} + 1")
    set(_validator_command
        "${_package_python}"
        "${CPACK_PACKAGE_VALIDATOR}"
        "--expected-package" "${CPACK_DEBIAN_PACKAGE_NAME}"
        "--expected-architecture" "${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}"
    )
    if(CPACK_PACKAGE_TARGET_SYSROOT)
        list(APPEND _validator_command
            "--sysroot" "${CPACK_PACKAGE_TARGET_SYSROOT}")
    endif()
    list(APPEND _validator_command "${_package_file}")
    execute_process(
        COMMAND ${_validator_command}
        RESULT_VARIABLE _validator_result
        OUTPUT_VARIABLE _validator_stdout
        ERROR_VARIABLE _validator_stderr
    )
    if(NOT _validator_result EQUAL 0)
        message(FATAL_ERROR
            "Debian package validation failed for ${_package_file}\n"
            "${_validator_stdout}${_validator_stderr}"
        )
    endif()
    string(STRIP "${_validator_stdout}" _validator_stdout)
    string(STRIP "${_validator_stderr}" _validator_stderr)
    message(STATUS
        "Validated Debian package: ${_package_file}\n"
        "${_validator_stdout}\n${_validator_stderr}"
    )
endforeach()

if(_validated_debs EQUAL 0)
    message(FATAL_ERROR "CPack post-build hook found no .deb package")
endif()
