# A platform whose window is a web page, served by the program itself, for machines with no
# display (ADR 0039). It pairs with the software rhi module, whose frames it shows.
dream_add_module(platform web
    SOURCES src/web_platform.cpp
    LIBRARIES dream::library_webserve
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_platform_web_tests
        SOURCES
            tests/web_platform_test.cpp
        LIBRARIES
            dream::module_platform_web
    )
endif()
