# Checks DreamAssetDatabase.cmake (ADR 0047) on a small set of assets: run with
# -DOUTPUT=<an empty folder to use>. It writes the assets, runs the step, and checks what each file
# needs, the bundles, and that a missing file fails the step with its name.

if(NOT OUTPUT)
    message(FATAL_ERROR "Pass -DOUTPUT=<folder>")
endif()
set(script "${CMAKE_CURRENT_LIST_DIR}/../DreamAssetDatabase.cmake")
set(assets "${OUTPUT}/assets")
file(REMOVE_RECURSE "${OUTPUT}")

# A model whose JSON names a texture beside it, which the build cooked, and an embedded image.
file(WRITE "${assets}/models/crate.glb"
    "glTF\\0\\0{\"images\":[{\"uri\":\"textures/crate.png\"},{\"uri\":\"data:image/png;base64,AA\"}]}")
file(WRITE "${assets}/models/crate.lods" "lods")
file(WRITE "${assets}/models/textures/crate.ktx2" "ktx2")
file(WRITE "${assets}/models/ball.glb" "glTF {\"meshes\":[]}")
file(WRITE "${assets}/scenes/forest.toml" "[entities.crate.mesh]\nmodel = \"assets:/models/crate.glb\"\n\n[entities.music.audio_source]\nsound = \"assets:/sounds/wind.wav\"\npipeline = \"assets:/shaders/solid.vert\"\n")
file(WRITE "${assets}/scenes/beach.toml" "[entities.crate.mesh]\nmodel = \"assets:/models/crate.glb\"\n[entities.ball.mesh]\nmodel = \"assets:/models/ball.glb\"\n")
file(WRITE "${assets}/sounds/wind.wav" "RIFF")
file(WRITE "${assets}/shaders/solid.vert.spv" "spv")
file(WRITE "${assets}/shaders/solid.vert.dxbc" "dxbc")
file(WRITE "${assets}/ui/menu.rml" "<rml><head><link type=\"text/rcss\" href=\"style.rcss\"/></head><body><img src=\"../models/textures/crate.png\"/><a href=\"#top\">top</a></body></rml>\n")
file(WRITE "${assets}/ui/style.rcss" "body { decorator: image(url(\"frame.png\")); }\n")
file(WRITE "${assets}/ui/frame.png" "png")
file(WRITE "${assets}/notes.txt" "not an asset anything names")
# A Tiled map whose image layer and tileset are beside it, and a tileset whose image was cooked.
file(WRITE "${assets}/maps/level.tmj" "{\"type\": \"map\", \"layers\": [{\"type\": \"imagelayer\",\n \"image\": \"../ui/frame.png\"}],\n \"tilesets\": [{\"firstgid\": 1, \"source\": \"props.tsj\"}]}\n")
file(WRITE "${assets}/maps/props.tsj" "{\"type\": \"tileset\", \"image\": \"../models/textures/crate.png\"}\n")
# A sprite atlas the build packed, and its image beside it.
file(WRITE "${assets}/atlases/things.atlas" "image = \"things.png\"\nwidth = 16\nheight = 16\n\n[regions]\n\"coin\" = [0, 0, 8, 8]\n")
file(WRITE "${assets}/atlases/things.png" "png")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DASSETS=${assets}" -DPLATFORM=windows
        -DTEXTURE_COOKER=ktx2_uastc
        "-DBUNDLES=forest=assets:/scenes/forest.toml|beach=assets:/scenes/beach.toml"
        -P "${script}"
    RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The asset database failed:\n${errors}")
endif()
file(READ "${assets}/asset_database.toml" text)

# The database as lists, read back the way the runtime does: one entry per file.
string(FIND "${text}" "[assets]" table_start)
string(SUBSTRING "${text}" ${table_start} -1 assets_table)
function(read_list key out)
    if(NOT assets_table MATCHES "\n${key} = \\[\n([^]]*)\\]")
        message(FATAL_ERROR "No list '${key}' in:\n${text}")
    endif()
    string(REGEX MATCHALL "\"[^\"]*\"" items "${CMAKE_MATCH_1}")
    string(REPLACE "\"" "" items "${items}")
    set(${out} "${items}" PARENT_SCOPE)
endfunction()
read_list(paths paths)
read_list(kinds kinds)
read_list(needs needs)
read_list(bundles bundles)
list(LENGTH paths count)
list(LENGTH needs needs_count)
if(NOT count EQUAL 17 OR NOT needs_count EQUAL 17)
    message(FATAL_ERROR "Expected 17 files and 17 needs, not ${count} and ${needs_count}:\n${text}")
endif()

function(expect path kind needed bundle)
    list(FIND paths "${path}" index)
    if(index LESS 0)
        message(FATAL_ERROR "${path} isn't in the database")
    endif()
    list(GET kinds ${index} got_kind)
    list(GET needs ${index} got_needs)
    list(GET bundles ${index} got_bundle)
    if(NOT got_kind STREQUAL kind OR NOT got_needs STREQUAL needed OR NOT got_bundle STREQUAL bundle)
        message(FATAL_ERROR "${path}: expected ${kind}, needs '${needed}', in ${bundle}; got "
                            "${got_kind}, needs '${got_needs}', in ${got_bundle}")
    endif()
endfunction()
# A PNG the build cooked is needed as its .ktx2, and a path without its ending names each shader.
expect(models/crate.glb model "models/textures/crate.ktx2|models/crate.lods" base)
expect(scenes/forest.toml scene
    "models/crate.glb|sounds/wind.wav|shaders/solid.vert.dxbc|shaders/solid.vert.spv" forest)
expect(scenes/beach.toml scene "models/crate.glb|models/ball.glb" beach)
# Only the forest needs the wind and the shaders; both need the crate, which stays in the base.
expect(sounds/wind.wav sound "" forest)
expect(shaders/solid.vert.spv shader "" forest)
expect(models/ball.glb model "" beach)
expect(models/textures/crate.ktx2 texture "" base)
expect(ui/menu.rml ui "ui/style.rcss|models/textures/crate.ktx2" base)
expect(ui/style.rcss style "ui/frame.png" base)
expect(notes.txt file "" base)
expect(maps/level.tmj map "ui/frame.png|maps/props.tsj" base)
expect(maps/props.tsj tileset "models/textures/crate.ktx2" base)
expect(atlases/things.atlas atlas "atlases/things.png" base)
if(NOT text MATCHES "bundles = \\[\n    \"forest\",\n    \"beach\",\n\\]")
    message(FATAL_ERROR "The bundles aren't listed:\n${text}")
endif()
if(NOT text MATCHES "texture_cooker = \"ktx2_uastc\"" OR NOT text MATCHES "platform = \"windows\"")
    message(FATAL_ERROR "The platform and cooker aren't recorded:\n${text}")
endif()

# A scene that names a file the assets don't have fails the step, naming both.
file(WRITE "${assets}/scenes/broken.toml" "[entities.a.mesh]\nmodel = \"assets:/models/missing.glb\"\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DASSETS=${assets}" -DPLATFORM=windows
        -DTEXTURE_COOKER=ktx2_uastc -P "${script}"
    RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "assets:/scenes/broken.toml names assets:/models/missing.glb")
    message(FATAL_ERROR "A missing file should fail the step, naming it:\n${errors}")
endif()
# And a bundle whose root isn't there.
file(REMOVE "${assets}/scenes/broken.toml")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DASSETS=${assets}" -DPLATFORM=windows
        -DTEXTURE_COOKER=ktx2_uastc "-DBUNDLES=cave=assets:/scenes/cave.toml" -P "${script}"
    RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "Bundle 'cave' starts at assets:/scenes/cave.toml")
    message(FATAL_ERROR "A bundle without its root should fail the step:\n${errors}")
endif()

message(STATUS "The asset database knows what each file needs and where it is shipped")
