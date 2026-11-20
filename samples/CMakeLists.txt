# Adds a test that runs a game and passes when its output matches `expected`.
function(dream_add_sample_test name expected)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
endfunction()

# The cube, render_targets and scene_demo samples drew with their own GPU shaders, which this
# branch can't run, so they are not part of it (ADR 0039).
add_subdirectory(arcade_demo)
add_subdirectory(hello_game)
add_subdirectory(module_demo)
add_subdirectory(physics_demo)
add_subdirectory(render_demo)
add_subdirectory(toppler)
