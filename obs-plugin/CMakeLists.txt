cmake_minimum_required(VERSION 3.16)
project(obs-game-translator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

include(FetchContent)

# ── OBS headers (sparse clone, only libobs/) ──────────────────────────────
set(OBS_HEADERS_DIR "${CMAKE_BINARY_DIR}/obs-headers")

if(NOT EXISTS "${OBS_HEADERS_DIR}/libobs/obs.h")
    message(STATUS "Fetching OBS 29.1.3 headers via sparse checkout (first time only)...")
    execute_process(
        COMMAND git clone
                --depth 1
                --filter=blob:none
                --sparse
                --branch 29.1.3
                https://github.com/obsproject/obs-studio.git
                "${OBS_HEADERS_DIR}"
        RESULT_VARIABLE _GIT_CLONE_RET
    )
    if(NOT _GIT_CLONE_RET EQUAL 0)
        message(FATAL_ERROR "Failed to clone OBS headers. Check network and try again.")
    endif()
    execute_process(
        COMMAND git sparse-checkout set libobs
        WORKING_DIRECTORY "${OBS_HEADERS_DIR}"
        RESULT_VARIABLE _GIT_SPARSE_RET
    )
    if(NOT _GIT_SPARSE_RET EQUAL 0)
        message(FATAL_ERROR "Failed to configure git sparse-checkout for libobs.")
    endif()
    message(STATUS "OBS headers ready.")
endif()

# ── nlohmann/json ─────────────────────────────────────────────────────────
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# ── stb (image write + resize) ────────────────────────────────────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stb)

# ── CURL ──────────────────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── OBS.app paths ─────────────────────────────────────────────────────────
set(OBS_APP        "/Applications/OBS.app")
set(OBS_FRAMEWORKS "${OBS_APP}/Contents/Frameworks")

# ── Plugin target ─────────────────────────────────────────────────────────
add_library(obs-game-translator MODULE
    src/plugin-main.cpp
    src/translate-source.cpp
    src/claude-api.cpp
    src/image-encode.cpp
)

target_include_directories(obs-game-translator PRIVATE
    "${OBS_HEADERS_DIR}/libobs"
    "${OBS_HEADERS_DIR}"
    "${stb_SOURCE_DIR}"
)

target_link_libraries(obs-game-translator PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    "-F${OBS_FRAMEWORKS}"
    "-framework libobs"
)

# macOS .plugin bundle
set_target_properties(obs-game-translator PROPERTIES
    BUNDLE                  TRUE
    BUNDLE_EXTENSION        "plugin"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/PluginInfo.plist"
    PREFIX                  ""
    OUTPUT_NAME             "obs-game-translator"
    INSTALL_RPATH           "${OBS_FRAMEWORKS}"
    BUILD_RPATH             "${OBS_FRAMEWORKS}"
)

# ── Install helper (run: cmake --build build --target install-plugin) ─────
# OBS 32 on macOS expects: ~/Library/Application Support/obs-studio/plugins/<name>.plugin/
set(OBS_PLUGIN_DEST "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
add_custom_target(install-plugin
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:obs-game-translator>"
            "${OBS_PLUGIN_DEST}/obs-game-translator.plugin"
    DEPENDS obs-game-translator
    COMMENT "Installing plugin to ${OBS_PLUGIN_DEST}/obs-game-translator.plugin"
)
