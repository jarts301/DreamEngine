# Imported targets for ozz-animation's libraries: ozz::base, ozz::animation and
# ozz::animation_offline, each linking the ones it needs.
get_filename_component(_ozz_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

foreach(_ozz_name IN ITEMS base animation animation_offline)
    if(TARGET ozz::${_ozz_name})
        continue()
    endif()
    add_library(ozz::${_ozz_name} STATIC IMPORTED)
    find_library(_ozz_release NAMES ozz_${_ozz_name} PATHS "${_ozz_prefix}/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    find_library(_ozz_debug NAMES ozz_${_ozz_name} PATHS "${_ozz_prefix}/debug/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    set_target_properties(ozz::${_ozz_name} PROPERTIES
        IMPORTED_LOCATION "${_ozz_release}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ozz_prefix}/include")
    if(_ozz_debug)
        set_target_properties(ozz::${_ozz_name} PROPERTIES
            IMPORTED_LOCATION_DEBUG "${_ozz_debug}"
            IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
            MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE)
    endif()
    unset(_ozz_release CACHE)
    unset(_ozz_debug CACHE)
endforeach()

set_property(TARGET ozz::animation APPEND PROPERTY INTERFACE_LINK_LIBRARIES ozz::base)
set_property(TARGET ozz::animation_offline APPEND PROPERTY INTERFACE_LINK_LIBRARIES ozz::animation)
unset(_ozz_name)
unset(_ozz_prefix)
