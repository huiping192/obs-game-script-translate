cmake_minimum_required(VERSION 3.16)
project(obs-game-translator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

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

# ── miniaudio (header-only, decoding only) ────────────────────────────────
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

# ── CURL ──────────────────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── Plugin target ─────────────────────────────────────────────────────────
add_library(obs-game-translator MODULE
    src/plugin-main.cpp
    src/translate-source.cpp
    src/llm-provider.cpp
    src/llm-utils.cpp
    src/claude-provider.cpp
    src/glm-provider.cpp
    src/image-encode.cpp
    src/voice-cache.cpp
    src/voice-analysis.cpp
    src/tts-replicate.cpp
    src/tts-gemini.cpp
    src/audio-output.cpp
)

target_include_directories(obs-game-translator PRIVATE
    "${OBS_HEADERS_DIR}/libobs"
    "${OBS_HEADERS_DIR}"
    "${stb_SOURCE_DIR}"
    "${miniaudio_SOURCE_DIR}"
)

target_link_libraries(obs-game-translator PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# ── Locale data ───────────────────────────────────────────────────────────
set(LOCALE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/locale")

# ── Platform-specific configuration ──────────────────────────────────────
if(APPLE)
    set(OBS_APP        "/Applications/OBS.app")
    set(OBS_FRAMEWORKS "${OBS_APP}/Contents/Frameworks")

    target_link_libraries(obs-game-translator PRIVATE
        "-F${OBS_FRAMEWORKS}"
        "-framework libobs"
    )

    set_target_properties(obs-game-translator PROPERTIES
        BUNDLE                   TRUE
        BUNDLE_EXTENSION         "plugin"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/PluginInfo.plist"
        PREFIX                   ""
        OUTPUT_NAME              "obs-game-translator"
        INSTALL_RPATH            "${OBS_FRAMEWORKS}"
        BUILD_RPATH              "${OBS_FRAMEWORKS}"
    )

    # Copy locale files into the .plugin bundle so OBS can find them
    add_custom_command(TARGET obs-game-translator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:obs-game-translator>/Contents/Resources/locale"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${LOCALE_SRC_DIR}"
                "$<TARGET_BUNDLE_DIR:obs-game-translator>/Contents/Resources/locale"
        COMMENT "Copying locale files into plugin bundle"
    )

    set(OBS_PLUGIN_DEST "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
    add_custom_target(install-plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:obs-game-translator>"
                "${OBS_PLUGIN_DEST}/obs-game-translator.plugin"
        DEPENDS obs-game-translator
        COMMENT "Installing plugin to ${OBS_PLUGIN_DEST}/obs-game-translator.plugin"
    )

elseif(WIN32)
    # Point OBS_DIR at your OBS Studio installation root.
    # Example: cmake -S obs-plugin -B obs-plugin/build -DOBS_DIR="C:/Program Files/obs-studio"
    if(NOT DEFINED OBS_DIR)
        set(OBS_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS Studio installation directory")
    endif()

    target_compile_definitions(obs-game-translator PRIVATE NOMINMAX)

    target_link_libraries(obs-game-translator PRIVATE
        "${OBS_DIR}/bin/64bit/obs.lib"
    )

    set_target_properties(obs-game-translator PROPERTIES
        PREFIX      ""
        OUTPUT_NAME "obs-game-translator"
    )

    set(OBS_PLUGIN_DEST "$ENV{APPDATA}/obs-studio/plugins/obs-game-translator/bin/64bit")
    set(OBS_DATA_DEST   "$ENV{APPDATA}/obs-studio/plugins/obs-game-translator/data/locale")
    add_custom_target(install-plugin
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_DEST}"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:obs-game-translator>"
                "${OBS_PLUGIN_DEST}/obs-game-translator.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_DATA_DEST}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${LOCALE_SRC_DIR}"
                "${OBS_DATA_DEST}"
        DEPENDS obs-game-translator
        COMMENT "Installing plugin to ${OBS_PLUGIN_DEST}"
    )

else()
    message(WARNING "Unsupported platform; only macOS and Windows are tested.")
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(obs-game-translator-tests
        tests/stubs/obs_stub.cpp
        tests/test_image_encode.cpp
        tests/test_base64.cpp
        tests/test_system_prompt.cpp
        tests/test_providers.cpp
        src/image-encode.cpp
        src/llm-utils.cpp
        src/claude-provider.cpp
        src/glm-provider.cpp
        src/llm-provider.cpp
    )
    target_include_directories(obs-game-translator-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${stb_SOURCE_DIR}"
    )
    target_link_libraries(obs-game-translator-tests
        PRIVATE Catch2::Catch2WithMain
                CURL::libcurl
                nlohmann_json::nlohmann_json
    )
    include(CTest)
    include(Catch)
    catch_discover_tests(obs-game-translator-tests)
endif()
