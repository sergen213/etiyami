cmake_minimum_required(VERSION 3.20)
set(YAMI_RELEASE_VERSION "1.0.1" CACHE STRING "Native release version (MAJOR.MINOR.PATCH)")
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

add_library(yami_engine STATIC
    native/assets.cpp native/script_vm.cpp native/gameplay.cpp native/enemy_ai.cpp
    native/entity.cpp native/combat.cpp native/media.cpp native/menu.cpp
    native/ending.cpp native/renderer.cpp native/scene.cpp native/game.cpp
    native/launcher.cpp native/update.cpp native/update_install.cpp)
target_compile_features(yami_engine PUBLIC cxx_std_20)
target_compile_definitions(yami_engine PUBLIC
    YAMI_VERSION="${PROJECT_VERSION}" YAMI_UPDATE_REPO="${YAMI_UPDATE_REPO}")
target_link_libraries(yami_engine PUBLIC PkgConfig::YAMI_DEPS Threads::Threads)
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
endif()
if(APPLE)
    target_link_options(yami-native PRIVATE -Wl,-headerpad_max_install_names)
    target_link_options(yami-launcher PRIVATE -Wl,-headerpad_max_install_names)
    target_link_options(yami-updater PRIVATE -Wl,-headerpad_max_install_names)
endif()

install(TARGETS yami-native yami-launcher yami-updater RUNTIME DESTINATION "." BUNDLE DESTINATION ".")
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
endif()
