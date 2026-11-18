# Runs on the build machine, so it is only built when the build is for that machine. PC games use it
# for their packages; an Android game's APK is its package.
add_executable(dream_pack pack.cpp)
target_link_libraries(dream_pack PRIVATE dream::core)
dream_configure_target(dream_pack)
