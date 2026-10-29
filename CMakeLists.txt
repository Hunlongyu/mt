cmake_minimum_required(VERSION 3.29)
project(mt VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The current native target requires Windows and MSVC.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(QT_NO_KEYWORDS)

# Discover only plugins used by this executable, including with broad static SDKs.
# This avoids importing unused PostgreSQL/TLS/Direct2D dependencies.
set(QT_SKIP_AUTO_PLUGIN_INCLUSION ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Sql Svg Network)
find_package(Qt6QSchannelBackendPlugin REQUIRED PATHS "${Qt6Network_DIR}" NO_DEFAULT_PATH)
find_package(Qt6QWindowsIntegrationPlugin REQUIRED PATHS "${Qt6Gui_DIR}" NO_DEFAULT_PATH)
find_package(Qt6QSQLiteDriverPlugin REQUIRED PATHS "${Qt6Sql_DIR}" NO_DEFAULT_PATH)
find_package(Qt6QJpegPlugin REQUIRED PATHS "${Qt6Gui_DIR}" NO_DEFAULT_PATH)
find_package(Qt6QICOPlugin REQUIRED PATHS "${Qt6Gui_DIR}" NO_DEFAULT_PATH)
get_target_property(qt_core_type Qt6::Core TYPE)
if(NOT qt_core_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "A static Qt SDK is required; Qt6::Core is ${qt_core_type}.")
endif()
qt_standard_project_setup()
include(cmake/Dependencies.cmake)
include(CTest)

add_library(mt_options INTERFACE)
target_compile_options(mt_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
target_compile_definitions(mt_options INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN QT_NO_CAST_FROM_ASCII)

qt_add_library(mt_core STATIC
    src/infrastructure/updates/ReleaseInfo.cpp
    src/infrastructure/settings/Settings.cpp
    src/infrastructure/storage/ClipboardStore.cpp
    src/infrastructure/storage/HopyImport.cpp
    src/infrastructure/storage/HLaunchImport.cpp
    src/infrastructure/i18n/I18n.cpp
    src/modules/clipboard/ClipboardRecord.cpp
    src/modules/clipboard/ClipboardText.cpp
    src/modules/launcher/LauncherCatalog.cpp
    src/modules/launcher/LauncherService.cpp
    src/modules/launcher/EdgeTrigger.cpp
    src/modules/gestures/GestureRecognizer.cpp
    src/modules/gestures/GestureCapture.cpp
    src/modules/gestures/GestureRules.cpp
    src/modules/screen_lock/UnlockPolicy.cpp
    src/modules/screen_lock/LockInput.cpp)
target_include_directories(mt_core PUBLIC src)
target_link_libraries(mt_core PUBLIC Qt6::Core Qt6::Gui Qt6::Sql PRIVATE mt_options)

qt_add_library(mt_platform STATIC src/platform/windows/DesktopServices.cpp src/platform/windows/CaretLocator.cpp src/platform/windows/ShellJobs.cpp
    src/platform/windows/GestureInput.cpp src/platform/windows/GestureActions.cpp
    src/platform/windows/MenuFrame.cpp src/platform/windows/EdgeWatcher.cpp
    src/platform/windows/LockPassword.cpp src/platform/windows/LockSession.cpp
    src/platform/windows/UpdateInstaller.cpp)
target_include_directories(mt_platform PUBLIC src)
target_link_libraries(mt_platform PUBLIC mt_core)
target_link_libraries(mt_platform PUBLIC Qt6::Core Qt6::Gui PRIVATE mt_options WIL::WIL user32 shell32 ole32 oleaut32 oleacc imm32 shcore uiautomationcore advapi32 dwmapi crypt32 wtsapi32)

qt_add_library(mt_updates STATIC src/infrastructure/updates/UpdateService.cpp)
target_include_directories(mt_updates PUBLIC src)
target_link_libraries(mt_updates PUBLIC mt_core PRIVATE mt_options Qt6::Network Qt6::QSchannelBackendPlugin)

qt_add_library(mt_ui STATIC
    src/ui/components/Theme.cpp
    src/ui/components/Panel.cpp
    src/ui/components/Placement.cpp
    src/ui/components/PopupMenuStyle.cpp
    src/ui/shell/SettingsDialog.cpp
    src/ui/shell/SettingsControls.cpp
    src/ui/shell/GestureSettings.cpp
    src/modules/gestures/GestureOverlay.cpp
    src/modules/clipboard/ClipboardPanel.cpp
    src/modules/clipboard/ClipboardPreview.cpp
    src/modules/launcher/LauncherPanel.cpp
    src/modules/screen_lock/ScreenLockService.cpp)
target_include_directories(mt_ui PUBLIC src)
target_link_libraries(mt_ui PUBLIC mt_core Qt6::Widgets PRIVATE mt_platform mt_options Qt6::Svg)

configure_file(resources/windows/version.rc.in generated/version.rc @ONLY)
configure_file(src/app/BuildInfo.h.in generated/BuildInfo.h @ONLY)
qt_add_executable(mt WIN32
    src/app/main.cpp src/app/Application.cpp src/app/StaticPlugins.cpp
    src/app/GestureController.cpp
    src/modules/clipboard/ClipboardService.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
target_include_directories(mt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mt PRIVATE mt_ui mt_platform mt_updates mt_options
    Qt6::QWindowsIntegrationPlugin Qt6::QSQLiteDriverPlugin Qt6::QJpegPlugin Qt6::QICOPlugin)
qt_add_resources(mt shared_theme PREFIX "/mt" BASE resources/theme FILES resources/theme/theme.json)
qt_add_resources(mt chinese_widgets PREFIX "/mt/i18n" BASE resources/i18n FILES resources/i18n/qtbase_zh_CN.qm)
qt_import_plugins(mt NO_DEFAULT)
target_link_options(mt PRIVATE /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/mt.manifest)
include(CheckIPOSupported)
check_ipo_supported(RESULT mt_ipo_supported OUTPUT mt_ipo_error)
if(mt_ipo_supported)
    set_property(TARGET mt mt_core mt_platform mt_ui mt_updates PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()



if(BUILD_TESTING)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-results")
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(mt_update_tests tests/unit/UpdateTests.cpp)
    target_link_libraries(mt_update_tests PRIVATE mt_updates mt_platform Qt6::Network Qt6::Test mt_options)
    set_target_properties(mt_update_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_import_plugins(mt_update_tests NO_DEFAULT)
    add_test(NAME update_contracts COMMAND mt_update_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/updates.txt,txt"
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/updates.xml,junitxml")
    set_tests_properties(update_contracts PROPERTIES TIMEOUT 30)
    qt_add_executable(mt_gesture_recognizer_tests tests/unit/GestureRecognizerTests.cpp)
    target_link_libraries(mt_gesture_recognizer_tests PRIVATE mt_core Qt6::Test mt_options)
    set_target_properties(mt_gesture_recognizer_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME gesture_recognizer_contracts COMMAND mt_gesture_recognizer_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/gesture-recognizer.txt,txt"
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/gesture-recognizer.xml,junitxml")
    set_tests_properties(gesture_recognizer_contracts PROPERTIES TIMEOUT 30)
    qt_add_executable(mt_tests tests/unit/BehaviorTests.cpp)
    target_link_libraries(mt_tests PRIVATE mt_core Qt6::Test mt_options Qt6::QSQLiteDriverPlugin)
    set_target_properties(mt_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_import_plugins(mt_tests NO_DEFAULT)
    add_test(NAME behavior_contracts COMMAND mt_tests -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/behavior.txt,txt" -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/behavior.xml,junitxml")
    qt_add_executable(mt_widget_tests tests/unit/WidgetTests.cpp)
    target_link_libraries(mt_widget_tests PRIVATE mt_ui Qt6::Test mt_options Qt6::QWindowsIntegrationPlugin)
    set_target_properties(mt_widget_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_add_resources(mt_widget_tests widget_test_theme PREFIX "/mt" BASE resources/theme FILES resources/theme/theme.json)
    qt_import_plugins(mt_widget_tests NO_DEFAULT)
    add_test(NAME widget_contracts COMMAND mt_widget_tests -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/widgets.txt,txt" -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/widgets.xml,junitxml")
    set_tests_properties(behavior_contracts widget_contracts PROPERTIES TIMEOUT 30)
    qt_add_executable(mt_gesture_tests tests/unit/GestureTests.cpp src/app/GestureController.cpp)
    target_link_libraries(mt_gesture_tests PRIVATE mt_ui mt_platform Qt6::Test mt_options Qt6::QWindowsIntegrationPlugin)
    set_target_properties(mt_gesture_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_add_resources(mt_gesture_tests gesture_test_theme PREFIX "/mt" BASE resources/theme FILES resources/theme/theme.json)
    qt_import_plugins(mt_gesture_tests NO_DEFAULT)
    add_test(NAME gesture_contracts COMMAND mt_gesture_tests -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/gestures.txt,txt" -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/gestures.xml,junitxml")
    set_tests_properties(gesture_contracts PROPERTIES TIMEOUT 30)
    # Explicit opt-in native input regression; no settings/UI dependency or ctest entry.
    qt_add_executable(mt_gesture_input_tests tests/integration/GestureInputIntegrationTests.cpp)
    target_link_libraries(mt_gesture_input_tests PRIVATE mt_platform Qt6::Widgets Qt6::Test mt_options
        Qt6::QWindowsIntegrationPlugin)
    set_target_properties(mt_gesture_input_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_import_plugins(mt_gesture_input_tests NO_DEFAULT)
    qt_add_executable(mt_clipboard_tests
        tests/integration/ClipboardIntegrationTests.cpp
        src/app/Application.cpp
        src/app/GestureController.cpp
        src/modules/clipboard/ClipboardService.cpp)
    target_include_directories(mt_clipboard_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(mt_clipboard_tests PRIVATE mt_ui mt_platform mt_updates Qt6::Test mt_options
        Qt6::QWindowsIntegrationPlugin Qt6::QSQLiteDriverPlugin Qt6::QJpegPlugin Qt6::QICOPlugin)
    set_target_properties(mt_clipboard_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_add_resources(mt_clipboard_tests clipboard_test_theme PREFIX "/mt"
        BASE resources/theme FILES resources/theme/theme.json)
    qt_import_plugins(mt_clipboard_tests NO_DEFAULT)
    add_test(NAME clipboard_regressions COMMAND mt_clipboard_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/clipboard.txt,txt"
        -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/clipboard.xml,junitxml")
    set_tests_properties(clipboard_regressions PROPERTIES TIMEOUT 45)
    qt_add_executable(mt_launcher_tests tests/unit/LauncherRegressionTests.cpp)
    target_link_libraries(mt_launcher_tests PRIVATE mt_ui mt_platform Qt6::Test mt_options Qt6::QWindowsIntegrationPlugin)
    set_target_properties(mt_launcher_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_add_resources(mt_launcher_tests launcher_test_theme PREFIX "/mt" BASE resources/theme FILES resources/theme/theme.json)
    qt_import_plugins(mt_launcher_tests NO_DEFAULT)
    add_test(NAME launcher_regressions COMMAND mt_launcher_tests -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/launcher.txt,txt" -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/launcher.xml,junitxml")
    set_tests_properties(launcher_regressions PROPERTIES TIMEOUT 45)
    # Opt-in only: captures the desktop keyboard and briefly covers every display.
    qt_add_executable(mt_lock_integration tests/integration/ScreenLockTests.cpp
        src/app/Application.cpp src/app/GestureController.cpp src/modules/clipboard/ClipboardService.cpp)
    target_include_directories(mt_lock_integration PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(mt_lock_integration PRIVATE mt_ui mt_platform mt_updates Qt6::Test mt_options
        Qt6::QWindowsIntegrationPlugin Qt6::QSQLiteDriverPlugin Qt6::QJpegPlugin Qt6::QICOPlugin)
    qt_add_resources(mt_lock_integration lock_test_theme PREFIX "/mt"
        BASE resources/theme FILES resources/theme/theme.json)
    set_target_properties(mt_lock_integration PROPERTIES WIN32_EXECUTABLE FALSE)
    qt_import_plugins(mt_lock_integration NO_DEFAULT)
endif()

foreach(consumer IN ITEMS mt mt_gesture_tests mt_widget_tests mt_clipboard_tests mt_launcher_tests mt_lock_integration)
    if(TARGET ${consumer})
        qt_add_resources(${consumer} product_logo PREFIX "/mt" BASE resources FILES resources/logo.ico)
        qt_add_resources(${consumer} legal_notices
            PREFIX "/mt/legal"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES LICENSE THIRD_PARTY_NOTICES.md
                resources/licenses/LGPL-3.0.txt resources/licenses/GPL-3.0.txt
                resources/licenses/WIL.txt)
        qt_add_resources(${consumer} lucide_icons
            PREFIX "/mt/icons"
            BASE resources/icons
            FILES
                resources/icons/lucide/menu.svg
                resources/icons/lucide/pin.svg
                resources/icons/lucide/arrow-up-to-line.svg
                resources/icons/lucide/x.svg
                resources/icons/lucide/search.svg
                resources/icons/lucide/ellipsis.svg
                resources/icons/lucide/text.svg
                resources/icons/lucide/image.svg
                resources/icons/lucide/file-text.svg
                resources/icons/lucide/star.svg
                resources/icons/lucide/settings.svg
                resources/icons/lucide/arrow-left.svg
                resources/icons/lucide/circle-x.svg
                resources/icons/lucide/pause.svg
                resources/icons/lucide/play.svg
                resources/icons/lucide/sun.svg
                resources/icons/lucide/moon.svg
                resources/icons/lucide/layers.svg
                resources/icons/lucide/copy.svg
                resources/icons/lucide/external-link.svg
                resources/icons/lucide/circle-help.svg
                resources/icons/lucide/chevron-left.svg
                resources/icons/lucide/chevron-right.svg
                resources/icons/lucide/palette.svg
                resources/icons/lucide/layout-grid.svg
                resources/icons/lucide/mouse-pointer-2.svg
                resources/icons/lucide/monitor-off.svg
                resources/icons/lucide/info.svg
                resources/icons/lucide/monitor.svg
                resources/icons/lucide/plus.svg
                resources/icons/lucide/pencil.svg
                resources/icons/lucide/trash-2.svg
                resources/icons/lucide/folder.svg
                resources/icons/lucide/keyboard.svg
                resources/icons/lucide/shield-check.svg
                resources/icons/lucide/upload.svg
                resources/icons/lucide/check.svg
                resources/icons/lucide/LICENSE.txt)
    endif()
endforeach()
