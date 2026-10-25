cmake_minimum_required(VERSION 3.20)
set(YAMI_RELEASE_VERSION "1.6.0" CACHE STRING "Native release version (MAJOR.MINOR.PATCH)")
if(NOT YAMI_RELEASE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "YAMI_RELEASE_VERSION must be MAJOR.MINOR.PATCH")
endif()
project(YamiNative VERSION "${YAMI_RELEASE_VERSION}" LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()
set(YAMI_UPDATE_REPO "sergen213/etiyami" CACHE STRING "GitHub owner/repository for release updates")
if(NOT YAMI_UPDATE_REPO MATCHES "^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$")
    message(FATAL_ERROR "YAMI_UPDATE_REPO must be a GitHub owner/repository")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(YAMI_DEPS REQUIRED IMPORTED_TARGET
    sdl3 epoxy libavformat libavcodec libavutil libswscale libswresample libxml-2.0
    "libcurl>=7.85" libarchive json-c)

# Vulkan is a build-time SDK dependency only. SDL loads the user's loader/driver
# when Vulkan is selected; merely starting the OpenGL launcher must not need it.
find_path(YAMI_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/include")
find_program(YAMI_GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")
find_program(YAMI_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")
set(YAMI_VULKAN_DEFAULT OFF)
if(YAMI_VULKAN_INCLUDE_DIR AND (YAMI_GLSLANG_VALIDATOR OR YAMI_GLSLC))
    set(YAMI_VULKAN_DEFAULT ON)
endif()
option(YAMI_ENABLE_VULKAN "Build the selectable Vulkan renderer with embedded shaders" ${YAMI_VULKAN_DEFAULT})
if(YAMI_ENABLE_VULKAN)
    if(NOT YAMI_VULKAN_INCLUDE_DIR OR NOT (YAMI_GLSLANG_VALIDATOR OR YAMI_GLSLC))
        message(FATAL_ERROR "Vulkan requires SDK headers and glslangValidator or glslc; use -DYAMI_ENABLE_VULKAN=OFF for OpenGL/GLES only")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(YAMI_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/native/shaders")
    set(YAMI_SHADER_NAMES vulkan_raster.vert vulkan_raster.frag vulkan_final.vert
        vulkan_final.frag effects.frag temporal.frag display.frag fsr_inputs.frag
        effects_rt.comp effects_resolve.vert effects_resolve.frag sun_midpoint.comp sun_taa.comp)
    # display.frag, fsr_inputs.frag and sun_midpoint.comp #include the shared lit-radiance composition;
    # effects.frag, effects_rt.comp and effects_resolve.vert the shared RT sampling code.
    set(YAMI_SHADER_SOURCES "${YAMI_SHADER_DIR}/lit_radiance.glsl" "${YAMI_SHADER_DIR}/effects_rt.glsl")
    foreach(shader IN LISTS YAMI_SHADER_NAMES)
        list(APPEND YAMI_SHADER_SOURCES "${YAMI_SHADER_DIR}/${shader}")
    endforeach()
    # Variants share source; RT-off SPIR-V contains no ray-query operations.
    list(APPEND YAMI_SHADER_NAMES effects_rt.frag effects_ms.frag effects_rt_ms.frag effects_sun.frag effects_classify.frag effects_classify_ms.frag vulkan_raster_ui.frag sun_midpoint_select.comp)
    set(YAMI_SPIRV_FILES)
    foreach(shader IN LISTS YAMI_SHADER_NAMES)
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/spirv/${shader}.spv")
        set(shader_source "${YAMI_SHADER_DIR}/${shader}")
        set(shader_defines)
        if(shader STREQUAL "effects_rt.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1)
        elseif(shader STREQUAL "effects_ms.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_MSAA_METADATA=1)
        elseif(shader STREQUAL "effects_rt_ms.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1 -DYAMI_MSAA_METADATA=1)
        elseif(shader STREQUAL "effects_sun.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1 -DYAMI_SUN_PASS=1)
        elseif(shader STREQUAL "effects_classify.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1 -DYAMI_SOURCE_CLASSIFY=1)
        elseif(shader STREQUAL "effects_classify_ms.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1 -DYAMI_SOURCE_CLASSIFY=1 -DYAMI_MSAA_METADATA=1)
        elseif(shader STREQUAL "sun_midpoint_select.comp")
            set(shader_source "${YAMI_SHADER_DIR}/sun_midpoint.comp")
            set(shader_defines -DYAMI_MIDPOINT_SELECT=1)
        elseif(shader STREQUAL "vulkan_raster_ui.frag")
            set(shader_source "${YAMI_SHADER_DIR}/vulkan_raster.frag")
            set(shader_defines -DYAMI_INTERFACE_ONLY=1)
        endif()
        if(YAMI_GLSLANG_VALIDATOR)
            set(shader_command "${YAMI_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
                "-I${YAMI_SHADER_DIR}" ${shader_defines} -o "${spirv}" "${shader_source}")
        else()
            set(shader_command "${YAMI_GLSLC}" --target-env=vulkan1.3
                "-I${YAMI_SHADER_DIR}" ${shader_defines} -o "${spirv}" "${shader_source}")
        endif()
        add_custom_command(OUTPUT "${spirv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spirv"
            COMMAND ${shader_command}
            DEPENDS ${YAMI_SHADER_SOURCES}
            VERBATIM)
        list(APPEND YAMI_SPIRV_FILES "${spirv}")
    endforeach()
    set(YAMI_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/yami_vulkan_shaders.hpp")
    add_custom_command(OUTPUT "${YAMI_SHADER_HEADER}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
            --output "${YAMI_SHADER_HEADER}" ${YAMI_SPIRV_FILES}
        DEPENDS ${YAMI_SPIRV_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
        VERBATIM)

    # AMD FidelityFX SDK v1.1.4 (third_party/fidelityfx-sdk): FSR 3.1.4 upscaler, frame interpolation and optical flow
    # with embedded GLSL permutations. Only what yami_ffx_clamp_capabilities lets the SDK request is built (FP32, wave32,
    # render-resolution unjittered motion vectors, standard depth); tools/embed_fsr_shaders.py checks the set.
    set(YAMI_FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-sdk")
    set(YAMI_FFX_GPU "${YAMI_FFX_DIR}/include/FidelityFX/gpu")
    file(GLOB_RECURSE YAMI_FFX_GPU_HEADERS "${YAMI_FFX_GPU}/*.h")
    set(YAMI_FFX_SPIRV_FILES)
    # Compiles src/backends/vk/shaders/<effect>/<stem>.glsl to ffx/<stem><suffix>.spv with the extra defines.
    function(yami_ffx_shader effect stem suffix)
        set(source "${YAMI_FFX_DIR}/src/backends/vk/shaders/${effect}/${stem}.glsl")
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/ffx/${stem}${suffix}.spv")
        set(defines -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0 ${ARGN} "-I${YAMI_FFX_GPU}" "-I${YAMI_FFX_GPU}/${effect}")
        if(YAMI_GLSLANG_VALIDATOR)
            set(command "${YAMI_GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp -Os ${defines} -o "${spirv}" "${source}")
        else()
            set(command "${YAMI_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute -Os ${defines} -o "${spirv}" "${source}")
        endif()
        add_custom_command(OUTPUT "${spirv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/ffx"
            COMMAND ${command}
            DEPENDS "${source}" ${YAMI_FFX_GPU_HEADERS}
            VERBATIM)
        set(YAMI_FFX_SPIRV_FILES ${YAMI_FFX_SPIRV_FILES} "${spirv}" PARENT_SCOPE)
    endfunction()
    # Base defines from the SDK's CMakeCompile{FSR3Upscaler,Frameinterpolation,Opticalflow}Shaders.txt.
    set(YAMI_FSR3_DEFINES
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
        -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1 -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
        -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
    foreach(pass IN ITEMS prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity
            luma_instability accumulate rcas debug_view autogen_reactive)
        foreach(sharpen 0 1)
            foreach(hdr 0 1)
                yami_ffx_shader(fsr3upscaler ffx_fsr3upscaler_${pass}_pass .h${hdr}s${sharpen} ${YAMI_FSR3_DEFINES}
                    -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=${hdr} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen})
            endforeach()
        endforeach()
    endforeach()
    foreach(pass IN ITEMS reconstruct_and_dilate_pass setup_pass reconstruct_previous_depth_pass game_motion_vector_field_pass
            optical_flow_vector_field_pass disocclusion_mask_pass pass compute_inpainting_pyramid_pass inpainting_pass
            compute_game_vector_field_inpainting_pyramid_pass debug_view_pass)
        yami_ffx_shader(frameinterpolation ffx_frameinterpolation_${pass} ""
            -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
            -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
            -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1
            -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0)
    endforeach()
    foreach(pass IN ITEMS prepare_luma_pass compute_luminance_pyramid_pass generate_scd_histogram_pass compute_scd_divergence_pass
            compute_optical_flow_advanced_pass_v5 filter_optical_flow_pass_v5 scale_optical_flow_advanced_pass_v5)
        yami_ffx_shader(opticalflow ffx_opticalflow_${pass} "" -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0)
    endforeach()
    set(YAMI_FSR_BLOBS "${CMAKE_CURRENT_BINARY_DIR}/ffx/ffx_shader_blobs.cpp")
    add_custom_command(OUTPUT "${YAMI_FSR_BLOBS}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_fsr_shaders.py"
            --output "${YAMI_FSR_BLOBS}" ${YAMI_FFX_SPIRV_FILES}
        DEPENDS ${YAMI_FFX_SPIRV_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_fsr_shaders.py"
        VERBATIM)
    set(YAMI_FFX_UPSTREAM_SOURCES
        "${YAMI_FFX_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
        "${YAMI_FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
        "${YAMI_FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp" "${YAMI_FFX_DIR}/src/backends/vk/ffx_vk.cpp"
        "${YAMI_FFX_DIR}/src/shared/ffx_assert.cpp" "${YAMI_FFX_DIR}/src/shared/ffx_message.cpp"
        "${YAMI_FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp" "${YAMI_FFX_DIR}/src/shared/ffx_object_management.cpp")
    add_library(yami_fsr3 STATIC ${YAMI_FFX_UPSTREAM_SOURCES} "${YAMI_FFX_DIR}/yami/yami_ffx_vulkan.cpp" "${YAMI_FSR_BLOBS}")
    target_compile_features(yami_fsr3 PRIVATE cxx_std_17)
    # Vulkan is resolved at runtime (yami_ffx_load_vulkan); the library must not reference vk* symbols.
    target_compile_definitions(yami_fsr3 PRIVATE VK_NO_PROTOTYPES)
    # yami/ first: it shadows FidelityFX/host/ffx_types.h to size the opaque context for 4-byte wchar_t.
    target_include_directories(yami_fsr3 SYSTEM PUBLIC "${YAMI_FFX_DIR}/yami" "${YAMI_FFX_DIR}/include" "${YAMI_VULKAN_INCLUDE_DIR}")
    target_include_directories(yami_fsr3 PRIVATE
        "${YAMI_FFX_DIR}/src/shared" "${YAMI_FFX_DIR}/src/backends/shared" "${YAMI_FFX_DIR}/src/components")
    if(MSVC)
        target_compile_options(yami_fsr3 PRIVATE "/FI${YAMI_FFX_DIR}/yami/ffx_compat.h")
        set_source_files_properties(${YAMI_FFX_UPSTREAM_SOURCES} PROPERTIES COMPILE_OPTIONS /w)
    else()
        target_compile_options(yami_fsr3 PRIVATE "-include${YAMI_FFX_DIR}/yami/ffx_compat.h")
        set_source_files_properties(${YAMI_FFX_UPSTREAM_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
    endif()
endif()

add_library(yami_engine STATIC
    native/assets.cpp native/script_vm.cpp native/gameplay.cpp native/enemy_ai.cpp
    native/entity.cpp native/combat.cpp native/media.cpp native/menu.cpp
    native/ending.cpp native/renderer.cpp native/scene.cpp native/game.cpp native/parallax.cpp
    native/launcher.cpp native/update.cpp native/update_install.cpp)
target_compile_features(yami_engine PUBLIC cxx_std_20)
target_compile_definitions(yami_engine PUBLIC
    YAMI_VERSION="${PROJECT_VERSION}" YAMI_UPDATE_REPO="${YAMI_UPDATE_REPO}")
target_link_libraries(yami_engine PUBLIC PkgConfig::YAMI_DEPS Threads::Threads)
target_compile_definitions(yami_engine PUBLIC YAMI_HAS_VULKAN=$<BOOL:${YAMI_ENABLE_VULKAN}>)
if(YAMI_ENABLE_VULKAN)
    target_sources(yami_engine PRIVATE native/vulkan_renderer.cpp native/vulkan_effects.cpp "${YAMI_SHADER_HEADER}")
    target_include_directories(yami_engine PRIVATE "${YAMI_VULKAN_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(yami_engine PRIVATE yami_fsr3)
endif()
if(WIN32)
    target_link_libraries(yami_engine PUBLIC advapi32)
endif()
if(MSVC)
    target_compile_options(yami_engine PRIVATE /W4 PUBLIC /fp:strict /utf-8)
else()
    target_compile_options(yami_engine PRIVATE -Wall -Wextra -Wpedantic PUBLIC -ffp-contract=off)
endif()

add_executable(yami-native native/main.cpp)
target_link_libraries(yami-native PRIVATE yami_engine)
if(WIN32)
    target_sources(yami-native PRIVATE native/fidelityfx_license.rc)
endif()

add_executable(yami-launcher WIN32 MACOSX_BUNDLE native/main.cpp)
target_compile_definitions(yami-launcher PRIVATE YAMI_LAUNCHER_DEFAULT=1)
target_link_libraries(yami-launcher PRIVATE yami_engine)
if(APPLE)
    set_target_properties(yami-launcher PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "ETI Yami"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.eti.yaminative"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

add_executable(yami-updater WIN32 native/updater_main.cpp)
target_link_libraries(yami-updater PRIVATE yami_engine)

if(UNIX AND NOT APPLE)
    set_target_properties(yami-native yami-launcher yami-updater
        PROPERTIES INSTALL_RPATH "$ORIGIN")
    add_library(yami_setup STATIC native/setup_assets.cpp native/setup_install.cpp native/setup_artwork.cpp)
    target_link_libraries(yami_setup PUBLIC yami_engine)
    target_compile_options(yami_setup PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(yami-setup native/setup_main.cpp)
    target_link_libraries(yami-setup PRIVATE yami_setup)
    set_target_properties(yami-setup PROPERTIES INSTALL_RPATH "$ORIGIN")
    add_executable(yami-remove native/uninstall_main.cpp)
    target_compile_features(yami-remove PRIVATE cxx_std_20)
    target_compile_options(yami-remove PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(yami-remove PRIVATE YAMI_VERSION="${PROJECT_VERSION}")
    set_target_properties(yami-remove PROPERTIES INSTALL_RPATH "$ORIGIN")
    add_dependencies(yami-setup yami-remove)
    configure_file(native/uninstall.sh "${CMAKE_CURRENT_BINARY_DIR}/uninstall.sh" COPYONLY)
    add_custom_command(TARGET yami-setup POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/native/uninstall.sh" "$<TARGET_FILE_DIR:yami-setup>/uninstall.sh")
    install(TARGETS yami-setup yami-remove RUNTIME DESTINATION "." COMPONENT setup)
    install(PROGRAMS native/uninstall.sh DESTINATION "." COMPONENT setup)
elseif(WIN32)
    add_library(yami_setup STATIC native/setup_assets_windows.cpp native/setup_install_windows.cpp native/setup_artwork.cpp)
    target_link_libraries(yami_setup PUBLIC yami_engine shell32 ole32)
    add_executable(yami-setup WIN32 native/setup_main.cpp)
    target_link_libraries(yami-setup PRIVATE yami_setup)
    add_executable(yami-remove native/uninstall_windows.cpp)
    target_compile_features(yami-remove PRIVATE cxx_std_20)
    target_compile_definitions(yami-remove PRIVATE YAMI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(yami-remove PRIVATE advapi32 shell32 ole32)
    if(MSVC)
        target_compile_options(yami_setup PRIVATE /W4)
        target_compile_options(yami-remove PRIVATE /W4)
        set_property(TARGET yami-remove PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(yami_setup PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(yami-remove PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(yami-remove PRIVATE -static)
    endif()
    add_dependencies(yami-setup yami-remove)
    install(TARGETS yami-setup yami-remove RUNTIME DESTINATION "." COMPONENT setup)
endif()
if(APPLE)
    target_link_options(yami-native PRIVATE -Wl,-headerpad_max_install_names)
    target_link_options(yami-launcher PRIVATE -Wl,-headerpad_max_install_names)
    target_link_options(yami-updater PRIVATE -Wl,-headerpad_max_install_names)
endif()

install(TARGETS yami-native yami-launcher yami-updater
    RUNTIME DESTINATION "." COMPONENT runtime BUNDLE DESTINATION "." COMPONENT runtime)
option(YAMI_INSTALL_ORIGINAL_ASSETS "Install the original game assets alongside native executables" OFF)
if(YAMI_INSTALL_ORIGINAL_ASSETS)
    install(DIRECTORY game/data DESTINATION "game")
    install(FILES game/eula.rtf game/yami.ico DESTINATION "game")
    if(APPLE)
        # SDL_GetBasePath resolves to the app bundle's Resources directory.
        install(DIRECTORY game/data DESTINATION "yami-launcher.app/Contents/Resources/game")
        install(FILES game/eula.rtf game/yami.ico DESTINATION "yami-launcher.app/Contents/Resources/game")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(component assets script_vm gameplay enemy_ai entity combat media menu ending renderer update update_install parallax)
        add_executable(check_${component} native/check_${component}.cpp)
        target_link_libraries(check_${component} PRIVATE yami_engine)
        if(MSVC)
            target_compile_options(check_${component} PRIVATE /UNDEBUG /fp:strict)
        else()
            target_compile_options(check_${component} PRIVATE -UNDEBUG -ffp-contract=off)
        endif()
        if(component STREQUAL "script_vm")
            add_test(NAME ${component} COMMAND check_${component} "${CMAKE_CURRENT_SOURCE_DIR}/game/data/scripts")
        elseif(component STREQUAL "renderer")
            add_test(NAME ${component} COMMAND check_${component} "${CMAKE_CURRENT_SOURCE_DIR}/game")
            set_tests_properties(${component} PROPERTIES LABELS "native-display")
        elseif(component STREQUAL "update" OR component STREQUAL "update_install")
            add_test(NAME ${component} COMMAND check_${component})
        else()
            add_test(NAME ${component} COMMAND check_${component} "${CMAKE_CURRENT_SOURCE_DIR}/game")
        endif()
    endforeach()
    if(YAMI_ENABLE_VULKAN)
        add_test(NAME renderer_vulkan COMMAND check_renderer --vulkan)
        set_tests_properties(renderer_vulkan PROPERTIES LABELS "native-display;vulkan")
        add_executable(check_vulkan native/check_vulkan.cpp)
        target_link_libraries(check_vulkan PRIVATE yami_engine)
        if(MSVC)
            target_compile_options(check_vulkan PRIVATE /UNDEBUG /fp:strict)
        else()
            target_compile_options(check_vulkan PRIVATE -UNDEBUG -ffp-contract=off)
        endif()
        add_test(NAME vulkan COMMAND check_vulkan)
        set_tests_properties(vulkan PROPERTIES LABELS "native-display;vulkan")
    endif()
    if(WIN32)
        add_executable(check_update_install_windows native/check_update_install_windows.cpp)
        target_link_libraries(check_update_install_windows PRIVATE yami_engine)
        add_test(NAME windows_update_install COMMAND check_update_install_windows)
        add_executable(check_setup_install_windows native/check_setup_install_windows.cpp)
        target_link_libraries(check_setup_install_windows PRIVATE yami_setup)
        add_dependencies(check_setup_install_windows yami-remove)
        set(YAMI_TEST_ISO "" CACHE FILEPATH "Optional original ISO for the real Windows installer regression")
        set(YAMI_TEST_ARCHIVER "" CACHE FILEPATH "Optional trusted 7zz.exe for the real Windows installer regression")
        if(YAMI_TEST_ISO AND YAMI_TEST_ARCHIVER)
            add_test(NAME windows_setup COMMAND check_setup_install_windows "${YAMI_TEST_ISO}"
                "${CMAKE_CURRENT_BINARY_DIR}" "${YAMI_TEST_ARCHIVER}" "$<TARGET_FILE:yami-remove>"
                "${CMAKE_CURRENT_BINARY_DIR}/windows-setup-check")
        endif()
        add_executable(check_uninstall_windows native/check_uninstall_windows.cpp)
        target_compile_features(check_uninstall_windows PRIVATE cxx_std_20)
        target_link_libraries(check_uninstall_windows PRIVATE advapi32 shell32 ole32)
        add_dependencies(check_uninstall_windows yami-remove)
        add_test(NAME windows_uninstall COMMAND check_uninstall_windows "$<TARGET_FILE:yami-remove>"
            "${CMAKE_CURRENT_BINARY_DIR}/windows-uninstall-check")
    endif()
    if(UNIX AND NOT APPLE)
        add_executable(check_setup_assets native/check_setup_assets.cpp)
        target_compile_features(check_setup_assets PRIVATE cxx_std_20)
        target_compile_options(check_setup_assets PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        target_link_libraries(check_setup_assets PRIVATE PkgConfig::YAMI_DEPS)
        add_test(NAME setup_assets COMMAND check_setup_assets)
        add_executable(check_setup_gui native/check_setup_gui.cpp)
        target_link_libraries(check_setup_gui PRIVATE yami_setup)
        target_compile_options(check_setup_gui PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        add_test(NAME setup_gui COMMAND check_setup_gui)
        set_tests_properties(setup_gui PROPERTIES TIMEOUT 30
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software"
            LABELS "gui-inline-dialog-reentry")
        add_executable(check_setup_install native/check_setup_install.cpp)
        target_link_libraries(check_setup_install PRIVATE yami_setup)
        add_dependencies(check_setup_install yami-remove)
        add_executable(check_uninstall native/check_uninstall.cpp)
        target_compile_features(check_uninstall PRIVATE cxx_std_20)
        target_compile_options(check_uninstall PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        add_dependencies(check_uninstall yami-remove)
        add_test(NAME uninstall COMMAND check_uninstall "$<TARGET_FILE:yami-remove>"
            "${CMAKE_CURRENT_SOURCE_DIR}/native/uninstall.sh")
    endif()
endif()
