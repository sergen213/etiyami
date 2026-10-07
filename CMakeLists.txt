cmake_minimum_required(VERSION 3.20)
set(YAMI_RELEASE_VERSION "1.1.0" CACHE STRING "Native release version (MAJOR.MINOR.PATCH)")
if(NOT YAMI_RELEASE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "YAMI_RELEASE_VERSION must be MAJOR.MINOR.PATCH")
endif()
project(YamiNative VERSION "${YAMI_RELEASE_VERSION}" LANGUAGES CXX)
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
        vulkan_final.frag effects.frag temporal.frag)
    set(YAMI_SHADER_SOURCES)
    foreach(shader IN LISTS YAMI_SHADER_NAMES)
        list(APPEND YAMI_SHADER_SOURCES "${YAMI_SHADER_DIR}/${shader}")
    endforeach()
    # Variants share source; RT-off SPIR-V contains no ray-query operations.
    list(APPEND YAMI_SHADER_NAMES effects_rt.frag vulkan_raster_ui.frag)
    set(YAMI_SPIRV_FILES)
    foreach(shader IN LISTS YAMI_SHADER_NAMES)
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/spirv/${shader}.spv")
        set(shader_source "${YAMI_SHADER_DIR}/${shader}")
        set(shader_defines)
        if(shader STREQUAL "effects_rt.frag")
            set(shader_source "${YAMI_SHADER_DIR}/effects.frag")
            set(shader_defines -DYAMI_RAY_QUERY=1)
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
endif()

add_library(yami_engine STATIC
    native/assets.cpp native/script_vm.cpp native/gameplay.cpp native/enemy_ai.cpp
    native/entity.cpp native/combat.cpp native/media.cpp native/menu.cpp
    native/ending.cpp native/renderer.cpp native/scene.cpp native/game.cpp
    native/launcher.cpp native/update.cpp native/update_install.cpp)
target_compile_features(yami_engine PUBLIC cxx_std_20)
target_compile_definitions(yami_engine PUBLIC
    YAMI_VERSION="${PROJECT_VERSION}" YAMI_UPDATE_REPO="${YAMI_UPDATE_REPO}")
target_link_libraries(yami_engine PUBLIC PkgConfig::YAMI_DEPS Threads::Threads)
target_compile_definitions(yami_engine PUBLIC YAMI_HAS_VULKAN=$<BOOL:${YAMI_ENABLE_VULKAN}>)
if(YAMI_ENABLE_VULKAN)
    target_sources(yami_engine PRIVATE native/vulkan_renderer.cpp native/vulkan_effects.cpp "${YAMI_SHADER_HEADER}")
    target_include_directories(yami_engine PRIVATE "${YAMI_VULKAN_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
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
    foreach(component assets script_vm gameplay enemy_ai entity combat media menu ending renderer update update_install)
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
