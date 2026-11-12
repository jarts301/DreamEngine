find_package(Threads REQUIRED)

dream_add_library(timeline SOURCES src/timeline.cpp LIBRARIES Threads::Threads)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_library_timeline_tests
        SOURCES
            tests/timeline_test.cpp
        LIBRARIES
            dream::library_timeline
            Threads::Threads
    )
endif()
