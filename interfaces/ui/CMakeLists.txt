dream_add_interface(ui)

# Interfaces receive the platform's input events.
target_link_libraries(dream_interface_ui INTERFACE dream::interface_platform)
