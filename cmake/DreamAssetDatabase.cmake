# Writes a game's asset database (ADR 0047): every file of its staged assets, what kind it is, the
# files it needs, and the bundle it is shipped in. Run with cmake -P after staging:
#
#   cmake -DASSETS=<staged folder> -DPLATFORM=<name> -DTEXTURE_COOKER=<name>
#         [-DBUNDLES=<name>=<root>,<root>|<name>=...] [-DMISSING=error|warning]
#         -P DreamAssetDatabase.cmake
#
# It writes <staged folder>/asset_database.toml, which the runtime reads as core::AssetDatabase, and
# fails when a file refers to one the assets don't have, naming both, so a typo in a scene breaks
# the build rather than the game. What a file needs is what it names:
#
#   .toml         Every "assets:/..." string, such as a scene's models. A path without its ending,
#                 such as a shader's, names every file that starts with it and a dot.
#   .glb          Each image its model refers to by file, beside it, and its cooked .lods.
#   .rml, .rcss   The files their href, src and url() name, beside them.
#   .tmj, .tsj    Tiled maps and tilesets: the images and tilesets they name, beside them.
#
# A PNG the build cooked is needed as its .ktx2, the file the game reads. Paths are relative to the
# assets folder, with forward slashes.

if(NOT DEFINED ASSETS)
    message(FATAL_ERROR "DreamAssetDatabase.cmake needs ASSETS")
endif()
get_filename_component(ASSETS "${ASSETS}" ABSOLUTE)
if(NOT DEFINED MISSING)
    set(MISSING error)
endif()
set(database_name "asset_database.toml")

file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${ASSETS}" "${ASSETS}/*")
list(REMOVE_ITEM files "${database_name}")
list(SORT files)

# The path of a file a reference names, or empty when the assets don't have it: as named, as the
# cooked .ktx2 of a PNG, or, for a name without an ending, every file it starts.
function(_dream_resolve reference out)
    set(found "")
    # The cooked name is worked out first: CMake expands a condition's variables before it runs.
    set(cooked "")
    if(reference MATCHES "^(.*)\\.[Pp][Nn][Gg]$")
        set(cooked "${CMAKE_MATCH_1}.ktx2")
    endif()
    if(EXISTS "${ASSETS}/${reference}" AND NOT IS_DIRECTORY "${ASSETS}/${reference}")
        set(found "${reference}")
    elseif(cooked AND EXISTS "${ASSETS}/${cooked}")
        set(found "${cooked}")
    else()
        file(GLOB variants RELATIVE "${ASSETS}" "${ASSETS}/${reference}.*")
        list(SORT variants)
        set(found "${variants}")
    endif()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# The folder of a path, with a trailing slash, or empty at the top.
function(_dream_folder path out)
    if(path MATCHES "^(.*/)[^/]*$")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# "a/b/../c" as "a/c", for references with ".." in them.
function(_dream_normal path out)
    cmake_path(SET normal NORMALIZE "${path}")
    set(${out} "${normal}" PARENT_SCOPE)
endfunction()

set(problems "")
set(kinds "")
set(sizes "")
foreach(file IN LISTS files)
    string(MAKE_C_IDENTIFIER "${file}" id)
    set(needs_${id} "")
    set(references "")
    set(relative_to "")
    string(TOLOWER "${file}" lower)
    if(lower MATCHES "\\.toml$")
        set(kind data)
        file(STRINGS "${ASSETS}/${file}" lines REGEX "assets:/")
        foreach(line IN LISTS lines)
            string(REGEX MATCHALL "\"assets:/[^\"]+\"" quoted "${line}")
            foreach(reference IN LISTS quoted)
                string(REGEX REPLACE "^\"assets:/(.*)\"$" "\\1" reference "${reference}")
                list(APPEND references "${reference}")
            endforeach()
        endforeach()
        file(STRINGS "${ASSETS}/${file}" entities REGEX "^\\[entities\\.")
        if(entities)
            set(kind scene)
        endif()
    elseif(lower MATCHES "\\.glb$")
        set(kind model)
        _dream_folder("${file}" relative_to)
        # The model's JSON is text inside the binary file, which STRINGS finds.
        file(STRINGS "${ASSETS}/${file}" lines REGEX "\"uri\"")
        foreach(line IN LISTS lines)
            # Images inside the file, as data: URIs, name no file, and their ; would split a list.
            string(REGEX REPLACE "\"uri\"[ \t]*:[ \t]*\"data:[^\"]*\"" "" line "${line}")
            string(REGEX MATCHALL "\"uri\"[ \t]*:[ \t]*\"[^\"]+\"" uris "${line}")
            foreach(uri IN LISTS uris)
                string(REGEX REPLACE "^\"uri\"[ \t]*:[ \t]*\"(.*)\"$" "\\1" uri "${uri}")
                if(NOT uri MATCHES "^data:")
                    list(APPEND references "${relative_to}${uri}")
                endif()
            endforeach()
        endforeach()
        string(REGEX REPLACE "\\.[Gg][Ll][Bb]$" ".lods" lods "${file}")
        if(EXISTS "${ASSETS}/${lods}")
            list(APPEND references "${lods}")
        endif()
    elseif(lower MATCHES "\\.(rml|rcss)$")
        set(kind ui)
        if(lower MATCHES "\\.rcss$")
            set(kind style)
        endif()
        _dream_folder("${file}" relative_to)
        file(STRINGS "${ASSETS}/${file}" lines REGEX "(href|src)=\"|url\\(")
        foreach(line IN LISTS lines)
            string(REGEX MATCHALL "(href|src)=\"[^\"]+\"|url\\([^)]+\\)" named "${line}")
            foreach(reference IN LISTS named)
                string(REGEX REPLACE "^(href|src)=\"(.*)\"$" "\\2" reference "${reference}")
                string(REGEX REPLACE "^url\\([\"']?([^\"')]*)[\"']?\\)$" "\\1" reference
                    "${reference}")
                if(reference MATCHES "^assets:/(.*)$")
                    list(APPEND references "${CMAKE_MATCH_1}")
                elseif(NOT reference MATCHES "^[a-z]+:" AND NOT reference MATCHES "^#")
                    list(APPEND references "${relative_to}${reference}")
                endif()
            endforeach()
        endforeach()
    elseif(lower MATCHES "\\.(tmj|tsj)$")
        set(kind map)
        if(lower MATCHES "\\.tsj$")
            set(kind tileset)
        endif()
        # Tiled's JSON: tilesets' and image layers' "image", and tilesets kept apart by "source",
        # beside the file.
        _dream_folder("${file}" relative_to)
        file(STRINGS "${ASSETS}/${file}" lines REGEX "\"(image|source)\"")
        foreach(line IN LISTS lines)
            string(REGEX MATCHALL "\"(image|source)\"[ \t]*:[ \t]*\"[^\"]+\"" named "${line}")
            foreach(reference IN LISTS named)
                string(REGEX REPLACE "^\"(image|source)\"[ \t]*:[ \t]*\"(.*)\"$" "\\2" reference
                    "${reference}")
                list(APPEND references "${relative_to}${reference}")
            endforeach()
        endforeach()
    elseif(lower MATCHES "\\.(png|ktx2|jpg|jpeg)$")
        set(kind texture)
    elseif(lower MATCHES "\\.lods$")
        set(kind lods)
    elseif(lower MATCHES "\\.hdr$")
        set(kind environment)
    elseif(lower MATCHES "\\.(wav|ogg|mp3|flac)$")
        set(kind sound)
    elseif(lower MATCHES "\\.(ttf|otf)$")
        set(kind font)
    elseif(lower MATCHES "\\.(spv|dxbc|dxil|glsl|essl)$")
        set(kind shader)
    else()
        set(kind file)
    endif()

    list(REMOVE_DUPLICATES references)
    foreach(reference IN LISTS references)
        # A folder, such as one the code lists, isn't a file to need.
        if(reference MATCHES "/$")
            continue()
        endif()
        _dream_normal("${reference}" reference)
        _dream_resolve("${reference}" resolved)
        if(resolved STREQUAL "")
            list(APPEND problems
                "assets:/${file} names assets:/${reference}, which the assets don't have")
        else()
            list(APPEND needs_${id} ${resolved})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES needs_${id})
    list(REMOVE_ITEM needs_${id} "${file}")
    file(SIZE "${ASSETS}/${file}" size)
    list(APPEND kinds "${kind}")
    list(APPEND sizes "${size}")
endforeach()

if(problems)
    list(JOIN problems "\n  " joined)
    if(MISSING STREQUAL "error")
        message(FATAL_ERROR "Assets that name files the game doesn't have:\n  ${joined}\n"
                            "Fix the names, or add the files to the game's asset folders.")
    else()
        message(WARNING "Assets that name files the game doesn't have:\n  ${joined}")
    endif()
endif()

# Each bundle holds its roots and everything they need, unless another bundle needs it too: what
# two bundles share stays in the base, which is always there.
set(bundle_names "")
foreach(file IN LISTS files)
    string(MAKE_C_IDENTIFIER "${file}" id)
    set(bundle_${id} "base")
endforeach()
if(DEFINED BUNDLES AND NOT BUNDLES STREQUAL "")
    string(REPLACE "|" ";" BUNDLES "${BUNDLES}")
    foreach(bundle IN LISTS BUNDLES)
        if(NOT bundle MATCHES "^([a-z0-9_]+)=(.+)$")
            message(FATAL_ERROR "Bundle '${bundle}' isn't <name>=<root>,<root>")
        endif()
        set(name "${CMAKE_MATCH_1}")
        string(REPLACE "," ";" roots "${CMAKE_MATCH_2}")
        list(APPEND bundle_names "${name}")
        set(reached "")
        set(pending "")
        foreach(root IN LISTS roots)
            string(REGEX REPLACE "^assets:/" "" root "${root}")
            _dream_resolve("${root}" resolved)
            if(resolved STREQUAL "")
                message(FATAL_ERROR "Bundle '${name}' starts at assets:/${root}, which the assets "
                                    "don't have")
            endif()
            list(APPEND pending ${resolved})
        endforeach()
        while(pending)
            list(POP_FRONT pending next)
            if(next IN_LIST reached)
                continue()
            endif()
            list(APPEND reached "${next}")
            string(MAKE_C_IDENTIFIER "${next}" id)
            list(APPEND pending ${needs_${id}})
        endwhile()
        foreach(file IN LISTS reached)
            string(MAKE_C_IDENTIFIER "${file}" id)
            if(bundle_${id} STREQUAL "base" AND NOT DEFINED claimed_${id})
                set(bundle_${id} "${name}")
                set(claimed_${id} TRUE)
            elseif(NOT bundle_${id} STREQUAL "${name}")
                set(bundle_${id} "base")
            endif()
        endforeach()
    endforeach()
endif()

# Lists of strings, one entry per file, as core::Config reads them.
function(_dream_toml_list out)
    set(items "")
    foreach(item IN LISTS ARGN)
        if(item STREQUAL "<none>")
            set(item "")
        endif()
        string(REPLACE "\\" "\\\\" item "${item}")
        string(REPLACE "\"" "\\\"" item "${item}")
        list(APPEND items "\"${item}\"")
    endforeach()
    list(JOIN items ",\n    " joined)
    if(joined STREQUAL "")
        set(${out} "[]" PARENT_SCOPE)
    else()
        set(${out} "[\n    ${joined},\n]" PARENT_SCOPE)
    endif()
endfunction()

set(needs_column "")
set(bundle_column "")
foreach(file IN LISTS files)
    string(MAKE_C_IDENTIFIER "${file}" id)
    list(JOIN needs_${id} "|" joined)
    # A CMake list can't hold an empty first entry, so "none" stands in until it is written.
    if(joined STREQUAL "")
        set(joined "<none>")
    endif()
    list(APPEND needs_column "${joined}")
    list(APPEND bundle_column "${bundle_${id}}")
endforeach()
_dream_toml_list(paths_text ${files})
_dream_toml_list(kinds_text ${kinds})
_dream_toml_list(needs_text ${needs_column})
_dream_toml_list(bundles_text ${bundle_column})
_dream_toml_list(bundle_names_text ${bundle_names})
list(JOIN sizes ", " sizes_text)

set(text "# Made by the build: this game's assets for ${PLATFORM} (ADR 0047). Each list has one entry
# per file, in the order of `paths`; `needs` joins the files each needs with |.

platform = \"${PLATFORM}\"
texture_cooker = \"${TEXTURE_COOKER}\"
bundles = ${bundle_names_text}

[assets]
paths = ${paths_text}
kinds = ${kinds_text}
bytes = [${sizes_text}]
needs = ${needs_text}
bundles = ${bundles_text}
")

# Written only when it changed, so hot reload doesn't see a change that isn't one.
set(output "${ASSETS}/${database_name}")
set(previous "")
if(EXISTS "${output}")
    file(READ "${output}" previous)
endif()
if(NOT previous STREQUAL text)
    file(WRITE "${output}" "${text}")
endif()
