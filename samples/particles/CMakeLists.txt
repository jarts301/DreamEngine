# Particles: a campfire, a fountain, magic sparkles, falling snow, and explosions set off from code.
# See docs/guides/particles.md.
dream_add_feature_sample(particles
    SOURCES src/particles.cpp
    ASSETS assets ../assets ../assets_3d
    APPLICATION_ID org.dreamengine.particles
    DISPLAY_NAME "Dream Particles"
    NULL_PASS "frames=5 draws=[1-9][0-9]* particles=[1-9][0-9]+"
    WARP
)
