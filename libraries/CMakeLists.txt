# In-house libraries that backends build on, for jobs no MIT or Apache 2.0 library does. They are
# always built, so their own tests run even when no project selects a backend that uses them.
add_subdirectory(impulse)
add_subdirectory(markup)
add_subdirectory(mixer)
