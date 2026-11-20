# A small HTTP server on POSIX sockets, which the web platform and the hub build on to show
# programs and test results in a web browser (ADR 0039).
find_package(Threads REQUIRED)

dream_add_library(webserve SOURCES src/server.cpp LIBRARIES Threads::Threads)
target_link_libraries(dream_library_webserve PUBLIC Threads::Threads)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_library_webserve_tests
        SOURCES
            tests/server_test.cpp
        LIBRARIES
            dream::library_webserve
    )
endif()
