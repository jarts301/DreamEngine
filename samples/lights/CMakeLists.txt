# Lights: 240 colored point lights circling over a courtyard of pillars at night, and spot lights
# sweeping across it, drawn by forward_plus. See docs/guides/lighting.md.
dream_add_feature_sample(lights
    SOURCES src/lights.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.lights
    DISPLAY_NAME "Dream Lights"
    NULL_PASS "frames=5 draws=[1-9][0-9]* lights=24[0-9] dropped=0"
    WARP
)
