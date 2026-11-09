# Tests dream_read_module_config. Run with: cmake -P module_config_test.cmake
#
# A rejected file stops CMake, so each invalid case runs this script again in a child process with
# DREAM_TEST_INVALID_FILE set.

include("${CMAKE_CURRENT_LIST_DIR}/../DreamModuleConfig.cmake")

if(DEFINED DREAM_TEST_INVALID_FILE)
    dream_read_module_config("${DREAM_TEST_INVALID_FILE}" windows unused)
    return()
endif()

set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/dream_module_config_test")
file(MAKE_DIRECTORY "${work_dir}")

function(expect_equal description actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${description}: got '${actual}', expected '${expected}'")
    endif()
endfunction()

function(expect_rejected name toml)
    set(file "${work_dir}/${name}.toml")
    file(WRITE "${file}" "${toml}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DDREAM_TEST_INVALID_FILE=${file}" -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
        message(FATAL_ERROR "Invalid case '${name}' was accepted")
    endif()
endfunction()

# A valid file, including text a CMake list would mangle.
set(valid_toml [==[
title = "semi;colon [not a table]"
# [modules.windows]

[game]
modules = "not the engine's table"

[modules.windows]  # Trailing comment.
rhi = ["d3d12", "vulkan"]   # Preference order.
physics = []
audio = ["miniaudio", "miniaudio"]

[modules.android]
rhi = ["vulkan"]
]==])

set(valid_file "${work_dir}/valid.toml")
file(WRITE "${valid_file}" "${valid_toml}")
dream_read_module_config("${valid_file}" windows windows)
expect_equal("found" "${windows_FOUND}" "TRUE")
expect_equal("interfaces" "${windows_INTERFACES}" "rhi;physics;audio")
expect_equal("rhi" "${windows_BACKENDS_rhi}" "d3d12;vulkan")
expect_equal("empty list" "${windows_BACKENDS_physics}" "")
expect_equal("duplicates" "${windows_BACKENDS_audio}" "miniaudio")

dream_read_module_config("${valid_file}" android android)
expect_equal("other platform" "${android_BACKENDS_rhi}" "vulkan")
expect_equal("other platform interfaces" "${android_INTERFACES}" "rhi")

dream_read_module_config("${valid_file}" switch switch)
expect_equal("missing platform" "${switch_FOUND}" "FALSE")

# Windows line endings.
string(REPLACE "\n" "\r\n" crlf_toml "${valid_toml}")
set(crlf_file "${work_dir}/crlf.toml")
file(WRITE "${crlf_file}" "${crlf_toml}")
dream_read_module_config("${crlf_file}" windows crlf)
expect_equal("CRLF" "${crlf_BACKENDS_rhi}" "d3d12;vulkan")

# No newline at the end of the file.
set(no_newline_file "${work_dir}/no_newline.toml")
file(WRITE "${no_newline_file}" "[modules.windows]\nrhi = [\"vulkan\"]")
dream_read_module_config("${no_newline_file}" windows last)
expect_equal("no final newline" "${last_BACKENDS_rhi}" "vulkan")

# Forms the real TOML parser accepts but this reader must refuse, rather than miss.
expect_rejected(multi_line "[modules.windows]\nrhi = [\n  \"vulkan\"\n]\n")
expect_rejected(plain_table "[modules]\nwindows.rhi = [\"vulkan\"]\n")
expect_rejected(nested_table "[modules.windows.extra]\nrhi = [\"vulkan\"]\n")
expect_rejected(quoted_table "[\"modules\".windows]\nrhi = [\"vulkan\"]\n")
expect_rejected(table_array "[[modules.windows]]\nrhi = [\"vulkan\"]\n")
expect_rejected(dotted_key "modules.windows.rhi = [\"vulkan\"]\n")
expect_rejected(not_a_list "[modules.windows]\nrhi = \"vulkan\"\n")
expect_rejected(unquoted "[modules.windows]\nrhi = [vulkan]\n")
expect_rejected(upper_case "[modules.windows]\nrhi = [\"Vulkan\"]\n")
expect_rejected(other_platform "[modules.android]\nrhi = \"vulkan\"\n")
