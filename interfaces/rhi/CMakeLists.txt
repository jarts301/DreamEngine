dream_add_interface(rhi)

# A device draws into the window that the platform layer provides.
target_link_libraries(dream_interface_rhi INTERFACE dream::interface_platform)
