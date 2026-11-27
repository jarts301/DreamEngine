# Shadows: a low sun turning over a long avenue of pillars, arches and a cut-out fence, shadowed
# to 60 m in four cascades. See docs/adr/0029 and 0041.
dream_add_feature_sample(shadows
    SOURCES src/shadows.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.shadows
    DISPLAY_NAME "Dream Shadows"
    NULL_PASS "frames=5 draws=[1-9][0-9]* casters=[1-9][0-9]*"
    WARP
)
