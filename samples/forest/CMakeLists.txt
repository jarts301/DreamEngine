# A forest of 3,000 trees and 600 rocks, placed by the game: drawn in a few instanced draws, with
# what is out of view culled and far trees drawn with simpler versions made at build time. See
# docs/adr/0031 and 0032.
dream_add_feature_sample(forest
    SOURCES src/forest.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.forest
    DISPLAY_NAME "Dream Forest"
    NULL_PASS "frames=5 objects=6600 drawn=[0-9]+ draw_calls=[1-9][0-9]? simplified=[1-9][0-9]+ culled=[1-9]"
    WARP
)
