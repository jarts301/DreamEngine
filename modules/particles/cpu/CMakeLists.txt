dream_add_module(particles cpu SOURCES src/cpu_particle_system.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_particles_cpu_tests
        SOURCES
            tests/test_main.cpp
            tests/cpu_particle_system_test.cpp
        LIBRARIES
            dream::module_particles_cpu
    )
endif()
