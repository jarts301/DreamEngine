dream_add_interface(crash_upload)

# Uploads go through the platform's HTTP requests.
target_link_libraries(dream_interface_crash_upload INTERFACE dream::interface_platform)
