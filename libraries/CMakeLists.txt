# In-house libraries that backends build on, frozen by ADR 0022: bug fixes only. They are always
# built, so their own tests run even when no project selects a backend that uses them.
add_subdirectory(impulse)
add_subdirectory(markup)
add_subdirectory(mixer)
add_subdirectory(timeline)
