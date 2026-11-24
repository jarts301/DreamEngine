dream_add_interface(editor_panel)

# Panels draw with the debug interface's widgets.
target_link_libraries(dream_interface_editor_panel INTERFACE dream::interface_debug_ui)
