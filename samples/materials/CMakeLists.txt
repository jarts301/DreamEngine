# Materials: plastic and metal from smooth to rough, a normal-mapped wall, glass, a cut-out fence
# and a glowing lamp, under the sky. See docs/guides/materials.md.
dream_add_feature_sample(materials
    SOURCES src/materials.cpp
    ASSETS assets ../assets ../assets_3d
    APPLICATION_ID org.dreamengine.materials
    DISPLAY_NAME "Dream Materials"
    NULL_PASS "frames=5 draws=[1-9][0-9]*"
    WARP
)
