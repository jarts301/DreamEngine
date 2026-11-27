# Environment lighting and bloom: shiny and rough spheres lit and reflecting an HDR sky, glowing
# lamps, and the look changing from day to dusk to night. See docs/guides/environment.md.
dream_add_feature_sample(environment
    SOURCES src/environment.cpp
    ASSETS assets ../assets ../assets_3d
    APPLICATION_ID org.dreamengine.environment
    DISPLAY_NAME "Dream Environment"
    NULL_PASS "frames=5 draws=[1-9][0-9]*"
    WARP
)
