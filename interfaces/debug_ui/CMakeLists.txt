dream_add_interface(debug_ui)

# Shares the draw lists and host of the game interface.
target_link_libraries(dream_interface_debug_ui INTERFACE dream::interface_ui)
