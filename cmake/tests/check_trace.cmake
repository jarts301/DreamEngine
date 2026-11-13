# Runs a program whose project uses the timeline profiler, then checks the trace it saved: that it
# is valid JSON in the Chrome trace format, with the zones, values, threads and frames expected, and
# a summary of each expected value.
#
# Usage: cmake -DPROGRAM=<executable> -DTRACE=<json-file> [-DZONES=<a|b|...>] [-DVALUES=<a|b|...>]
#              [-DTHREADS=<a|b|...>] [-DFRAMES=<count>] -P check_trace.cmake
#
# Lists are separated with "|", because CTest would split arguments at semicolons. The program runs
# in this script's working directory, so TRACE must match the path its configuration gives. Any
# earlier trace is deleted first, so a stale file can't pass.

# A script run with -P gets no policies otherwise, and IN_LIST needs them.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS PROGRAM TRACE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check_trace.cmake needs -D${required}=...")
    endif()
endforeach()

file(REMOVE "${TRACE}")
execute_process(COMMAND "${PROGRAM}" RESULT_VARIABLE result OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} failed with ${result}:\n${output}")
endif()
if(NOT EXISTS "${TRACE}")
    message(FATAL_ERROR "${PROGRAM} saved no trace at ${TRACE}:\n${output}")
endif()

file(READ "${TRACE}" trace)
string(JSON event_count ERROR_VARIABLE error LENGTH "${trace}" traceEvents)
if(error)
    message(FATAL_ERROR "${TRACE} is not a Chrome trace: ${error}")
endif()

set(zones "")
set(values "")
set(threads "")
if(event_count GREATER 0)
    math(EXPR last "${event_count} - 1")
    foreach(index RANGE ${last})
        string(JSON phase GET "${trace}" traceEvents ${index} ph)
        string(JSON name GET "${trace}" traceEvents ${index} name)
        if(phase STREQUAL "X")
            string(JSON duration GET "${trace}" traceEvents ${index} dur)
            if(duration LESS 0)
                message(FATAL_ERROR "Zone '${name}' has a negative duration, ${duration}")
            endif()
            list(APPEND zones "${name}")
        elseif(phase STREQUAL "C")
            list(APPEND values "${name}")
        elseif(phase STREQUAL "M" AND name STREQUAL "thread_name")
            string(JSON thread GET "${trace}" traceEvents ${index} args name)
            list(APPEND threads "${thread}")
        endif()
    endforeach()
endif()

function(expect_all kind expected found)
    string(REPLACE "|" ";" expected "${expected}")
    foreach(item IN LISTS expected)
        if(NOT item IN_LIST found)
            list(REMOVE_DUPLICATES found)
            message(FATAL_ERROR "${TRACE} has no ${kind} named '${item}'. It has: ${found}")
        endif()
    endforeach()
endfunction()

expect_all(zone "${ZONES}" "${zones}")
expect_all(value "${VALUES}" "${values}")
expect_all(thread "${THREADS}" "${threads}")

# Each expected value is summed up in otherData, with as many samples as the trace holds.
string(REPLACE "|" ";" expected_values "${VALUES}")
foreach(value IN LISTS expected_values)
    string(JSON samples ERROR_VARIABLE error GET "${trace}" otherData values "${value}" samples)
    if(error)
        message(FATAL_ERROR "${TRACE} has no summary of '${value}': ${error}")
    endif()
    set(found_samples 0)
    foreach(name IN LISTS values)
        if(name STREQUAL value)
            math(EXPR found_samples "${found_samples} + 1")
        endif()
    endforeach()
    if(NOT samples EQUAL found_samples)
        message(FATAL_ERROR "The summary of '${value}' counts ${samples} samples, but the trace "
                            "has ${found_samples}")
    endif()
endforeach()

string(JSON frames GET "${trace}" otherData frames)
if(DEFINED FRAMES AND NOT frames EQUAL FRAMES)
    message(FATAL_ERROR "${TRACE} has ${frames} frames, expected ${FRAMES}")
endif()

list(LENGTH zones zone_count)
message(STATUS "${TRACE} is a valid trace: ${event_count} events, ${zone_count} zones, "
               "${frames} frames")
