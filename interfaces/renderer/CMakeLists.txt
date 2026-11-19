dream_add_interface(renderer)

# A renderer draws through the graphics interface, and draws the game interface's draw lists.
target_link_libraries(dream_interface_renderer INTERFACE dream::interface_rhi dream::interface_ui)
