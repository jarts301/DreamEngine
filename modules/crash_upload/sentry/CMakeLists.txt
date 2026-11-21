# Sends reports over the network, so an Android game that lists it asks for INTERNET (ADR 0043).
dream_add_module(crash_upload sentry
    SOURCES src/sentry_crash_uploader.cpp
    ANDROID_PERMISSIONS INTERNET
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_crash_upload_sentry_tests
        SOURCES
            tests/test_main.cpp
            tests/sentry_crash_uploader_test.cpp
        LIBRARIES
            dream::module_crash_upload_sentry
    )
endif()
