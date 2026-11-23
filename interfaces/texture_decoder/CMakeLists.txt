dream_add_interface(texture_decoder)

# A decoded texture is described as the graphics interface takes it.
target_link_libraries(dream_interface_texture_decoder INTERFACE dream::interface_rhi)
