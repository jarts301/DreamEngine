dream_add_interface(asset_source)

# A source reads the game's files through the platform.
target_link_libraries(dream_interface_asset_source INTERFACE dream::interface_platform)
