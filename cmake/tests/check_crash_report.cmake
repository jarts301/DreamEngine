# Runs a program that is meant to crash, then checks the crash report it left.
#
# Usage: cmake -DPROGRAM=<executable> [-DARGUMENTS=<a|b|...>] -DDIRECTORY=<report folder>
#              -DEXPECT=<text|text|...> [-DMINIDUMP=ON] -P check_crash_report.cmake
#
# Lists are separated with "|", because CTest would split arguments at semicolons. Each EXPECT item
# is plain text that the report must contain. The program runs in this script's working directory.
# The report folder is emptied first, so a stale report can't pass.

cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS PROGRAM DIRECTORY EXPECT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check_crash_report.cmake needs -D${required}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${DIRECTORY}")
string(REPLACE "|" ";" arguments "${ARGUMENTS}")
execute_process(COMMAND "${PROGRAM}" ${arguments} RESULT_VARIABLE result OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} was meant to crash, but ended normally:\n${output}")
endif()

file(GLOB reports "${DIRECTORY}/crash-*.txt")
list(LENGTH reports report_count)
if(NOT report_count EQUAL 1)
    message(FATAL_ERROR "Expected one crash report in ${DIRECTORY}, found ${report_count}. The "
                        "program ended with '${result}':\n${output}")
endif()
file(READ "${reports}" report)

string(REPLACE "|" ";" expected "${EXPECT}")
foreach(text IN LISTS expected)
    string(FIND "${report}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "The crash report lacks '${text}':\n${report}")
    endif()
endforeach()

string(FIND "${output}" "Crash report:" position)
if(position EQUAL -1)
    message(FATAL_ERROR "The program didn't say where the crash report is:\n${output}")
endif()

if(MINIDUMP)
    file(GLOB dumps "${DIRECTORY}/crash-*.dmp")
    list(LENGTH dumps dump_count)
    if(NOT dump_count EQUAL 1)
        message(FATAL_ERROR "Expected one minidump in ${DIRECTORY}, found ${dump_count}")
    endif()
    file(SIZE "${dumps}" dump_size)
    if(dump_size EQUAL 0)
        message(FATAL_ERROR "The minidump ${dumps} is empty")
    endif()
endif()

message(STATUS "${reports} is a complete crash report (the program ended with '${result}')")
