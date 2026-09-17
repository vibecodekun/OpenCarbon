cmake_minimum_required(VERSION 3.25)
project(carbon-pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# PDBs for Release too, so crash offsets can be resolved
add_compile_options($<$<CONFIG:Release>:/Zi>)
add_link_options($<$<CONFIG:Release>:/MAP>
                 $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

set(EXTERN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../extern)
set(GB_SND_EMU_DIR ${EXTERN_DIR}/gb_snd_emu/Gb_Snd_Emu-0.1.4)

# --- third-party (upstream sources, not decompiled) -----------------------------------------------
add_library(glad STATIC ${EXTERN_DIR}/glad/src/gl.c)
target_include_directories(glad PUBLIC ${EXTERN_DIR}/glad/include)

add_library(tinyxml2 STATIC ${EXTERN_DIR}/tinyxml2/tinyxml2.cpp)
target_include_directories(tinyxml2 PUBLIC ${EXTERN_DIR}/tinyxml2)

add_library(stb STATIC src/third_party/stb_impl.cpp)
target_include_directories(stb PUBLIC ${EXTERN_DIR}/stb)
target_compile_definitions(stb PRIVATE _CRT_SECURE_NO_WARNINGS)

# Microsoft Edge WebView2 SDK (NuGet Microsoft.Web.WebView2 1.0.4191.47): stands in for the Switch offline web applet
set(WEBVIEW2_DIR ${EXTERN_DIR}/webview2/pkg/build/native)
add_library(webview2 STATIC IMPORTED)
set_target_properties(webview2 PROPERTIES
    IMPORTED_LOCATION ${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib
    INTERFACE_INCLUDE_DIRECTORIES ${WEBVIEW2_DIR}/include)

# blargg's Gb_Snd_Emu 0.1.4 + Blip_Buffer 0.3.4, compiled unmodified
add_library(gb_snd_emu STATIC
    ${GB_SND_EMU_DIR}/gb_apu/Blip_Buffer.cpp
    ${GB_SND_EMU_DIR}/gb_apu/Gb_Apu.cpp
    ${GB_SND_EMU_DIR}/gb_apu/Gb_Oscs.cpp
    ${GB_SND_EMU_DIR}/gb_apu/Multi_Buffer.cpp
)
target_include_directories(gb_snd_emu PUBLIC src/third_party/gb_snd_emu_compat ${GB_SND_EMU_DIR}/gb_apu)
target_compile_options(gb_snd_emu PRIVATE /W0)

# --- Carbon: decompiled GB/GBC core -----------------------------------------------------------------
# Needs Paths, Platform::CreateDirectory/Commit and AudioStream from whichever target links it.
add_library(carbon_gb STATIC
    src/gb/Audio.cpp
    src/gb/Cartridge.cpp
    src/gb/Cpu.cpp
    src/gb/Memory.cpp
    src/gb/State.cpp
    src/gb/System.cpp
    src/gb/Video.cpp
    src/platform/AudioOut.cpp
)
target_include_directories(carbon_gb PUBLIC src)
target_compile_definitions(carbon_gb PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(carbon_gb PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(carbon_gb PUBLIC gb_snd_emu stb)

# --- Carbon: app ------------------------------------------------------------------------------------
set(CARBON_APP_SOURCES
    src/app/App.cpp
    src/app/Artwork.cpp
    src/app/GameDef.cpp
    src/app/Globals.cpp
    src/app/Input.cpp
    src/app/MenuAudio.cpp
    src/app/Renderer.cpp
    src/gfx/ResourceManager.cpp
    src/gfx/Shader.cpp
    src/gfx/SpriteRenderer.cpp
    src/gfx/TextRenderer.cpp
    src/gfx/Texture2D.cpp
)

set(CARBON_PLATFORM_SOURCES
    src/platform/Paths.cpp
    src/platform/win32/AudioStream_wasapi.cpp
    src/platform/win32/MoviePlayer_win32.cpp
    src/platform/win32/OfflineHtml_win32.cpp
    src/platform/win32/Platform_win32.cpp
    src/platform/win32/Script.cpp
    src/platform/win32/main.cpp
)

add_executable(carbon ${CARBON_APP_SOURCES} ${CARBON_PLATFORM_SOURCES}
    src/stub/GbaStubs.cpp  # temporary, until VBA-Next is integrated
)
target_include_directories(carbon PRIVATE src)
target_compile_definitions(carbon PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(carbon PRIVATE /W4 /permissive- /utf-8 /MP)
target_link_libraries(carbon PRIVATE carbon_gb glad tinyxml2 stb opengl32 xinput webview2 version shlwapi
    mfplat mfreadwrite mfuuid)  # Media Foundation for wf_logo.mp4

# --- audio_out_test: AudioOut buffer/volume timing against the Ryujinx trace --------------------------------
add_executable(audio_out_test tests/audio_out_test.cpp src/platform/AudioOut.cpp)
target_include_directories(audio_out_test PRIVATE src)
target_compile_options(audio_out_test PRIVATE /W4 /permissive- /utf-8)

# --- gbdiff.dll: the GB core behind a C API for tools/gbdiff -------------------------------------------
add_library(gbdiff SHARED src/diff/GbDiffApi.cpp src/diff/HeadlessPlatform.cpp src/platform/Paths.cpp)
target_include_directories(gbdiff PRIVATE src)
target_compile_options(gbdiff PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(gbdiff PRIVATE carbon_gb)
