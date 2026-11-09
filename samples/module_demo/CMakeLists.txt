# Builds one source file against different configurations. The behaviour of each program comes only
# from the TOML file it was built with.
function(dream_add_module_demo name config expected_output)
    add_executable(${name} src/main.cpp)
    target_link_libraries(${name} PRIVATE dream::interface_greeter)
    dream_configure_target(${name})
    dream_link_modules(${name} CONFIG ${config})

    if(DREAM_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected_output}")
    endif()
endfunction()

dream_add_module_demo(dream_module_demo dream.toml
    "selected=english greeting=Hello, Dream! linked=english,spanish")

if(DREAM_BUILD_TESTS)
    dream_add_module_demo(dream_module_demo_spanish_first tests/spanish_first.toml
        "selected=spanish greeting=Hola, Dream! linked=spanish,english")
    dream_add_module_demo(dream_module_demo_english_only tests/english_only.toml
        "selected=english greeting=Hello, Dream! linked=english\n")
    dream_add_module_demo(dream_module_demo_fallback tests/fallback.toml
        "selected=english greeting=Hello, Dream! linked=spanish,english")
endif()
