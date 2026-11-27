# A game's interface: a title menu, a settings screen with sliders and switches, and a HUD with a
# health bar, score and timer, written in RML and RCSS. See docs/adr/0012.
dream_add_feature_sample(ui
    SOURCES src/ui.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.ui
    DISPLAY_NAME "Dream Interface"
    NULL_PASS "frames=5 screen=settings documents=3"
    WARP
)
