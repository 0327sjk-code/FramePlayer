cmake_minimum_required(VERSION 3.24)

project(
    ZTNativeSequencePlayer
    VERSION 0.5.0
    DESCRIPTION "Windows native PNG sequence and video player"
    LANGUAGES CXX
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "ZTNativeSequencePlayer requires Windows and the MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# CMake's MSVC defaults include /W3. Remove it so each target can select one
# warning level without producing command-line override diagnostics.
string(REGEX REPLACE "([/-])W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

include(cmake/CompilerOptions.cmake)

set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(imgui STATIC
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
)
target_include_directories(imgui PUBLIC
    "${IMGUI_ROOT}"
    "${IMGUI_ROOT}/backends"
)
zt_configure_third_party_target(imgui)

file(GLOB_RECURSE PLAYER_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(ZTSequencePlayer WIN32
    ${PLAYER_SOURCE_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/App/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/version.rc"
)

target_include_directories(ZTSequencePlayer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(ZTSequencePlayer PRIVATE
    imgui
    d3d11
    dxgi
    windowscodecs
    shell32
    advapi32
    ole32
    dwmapi
    uuid
    mfplat
    mfreadwrite
    mfuuid
    propsys
    winhttp
    bcrypt
    version
)

zt_configure_project_target(ZTSequencePlayer)
target_compile_options(ZTSequencePlayer PRIVATE /WX)

set_target_properties(ZTSequencePlayer PROPERTIES
    OUTPUT_NAME "FramePlayer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ZTSequencePlayer>"
)

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT ZTSequencePlayer)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${PLAYER_SOURCE_FILES})
source_group("Resources" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/App/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/version.rc"
)

install(TARGETS ZTSequencePlayer RUNTIME DESTINATION .)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION .
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/LICENSE.txt"
    DESTINATION .
    RENAME DEAR_IMGUI_LICENSE.txt
)

option(ZT_BUILD_TESTS "Build native validation executables" ON)
if(ZT_BUILD_TESTS)
    enable_testing()

    add_executable(ZTFrameUploadSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/FrameUploadSmoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/PngImageInfo.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Overlay/MaskOverlayTexture.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Render/FrameTexture.cpp"
    )
    target_include_directories(ZTFrameUploadSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTFrameUploadSmoke PRIVATE
        d3d11
        windowscodecs
        ole32
    )
    zt_configure_project_target(ZTFrameUploadSmoke)
    target_compile_options(ZTFrameUploadSmoke PRIVATE /WX)

    add_executable(ZTFfmpegExportSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/FfmpegExportSmoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/ExportNaming.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/ExportPolicy.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/FfconcatManifest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/FfmpegExportController.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/FfmpegProcess.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/Image2SequenceInput.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/PngImageInfo.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTFfmpegExportSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTFfmpegExportSmoke PRIVATE
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTFfmpegExportSmoke)
    target_compile_options(ZTFfmpegExportSmoke PRIVATE /WX)

    add_executable(ZTPlayerUILogicTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PlayerUILogicTests.cpp"
    )
    target_include_directories(ZTPlayerUILogicTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    zt_configure_project_target(ZTPlayerUILogicTests)
    target_compile_options(ZTPlayerUILogicTests PRIVATE /WX)
    add_test(NAME PlayerUILogic COMMAND ZTPlayerUILogicTests)

    add_executable(ZTKeyboardShuttleControllerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/KeyboardShuttleControllerTests.cpp"
    )
    target_include_directories(ZTKeyboardShuttleControllerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    zt_configure_project_target(ZTKeyboardShuttleControllerTests)
    target_compile_options(ZTKeyboardShuttleControllerTests PRIVATE /WX)
    add_test(
        NAME KeyboardShuttleController
        COMMAND ZTKeyboardShuttleControllerTests
    )

    add_executable(ZTCoreLogicTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CoreLogicTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Cache/MemoryFrameCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/ExportNaming.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTCoreLogicTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    zt_configure_project_target(ZTCoreLogicTests)
    target_compile_options(ZTCoreLogicTests PRIVATE /WX)
    add_test(NAME CoreLogic COMMAND ZTCoreLogicTests)

    add_executable(ZTUpdateLogicTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/UpdateLogicTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/SemanticVersion.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/Sha256.cpp"
    )
    target_include_directories(ZTUpdateLogicTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTUpdateLogicTests PRIVATE bcrypt)
    zt_configure_project_target(ZTUpdateLogicTests)
    target_compile_options(ZTUpdateLogicTests PRIVATE /WX)
    add_test(NAME UpdateLogic COMMAND ZTUpdateLogicTests)

    add_executable(ZTUpdateFileStoreTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/UpdateFileStoreTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/SemanticVersion.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/Sha256.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/detail/UpdateError.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/detail/UpdateFileStore.cpp"
    )
    target_include_directories(ZTUpdateFileStoreTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTUpdateFileStoreTests PRIVATE bcrypt)
    zt_configure_project_target(ZTUpdateFileStoreTests)
    target_compile_options(ZTUpdateFileStoreTests PRIVATE /WX)
    add_test(NAME UpdateFileStore COMMAND ZTUpdateFileStoreTests)

    add_executable(ZTSelfUpdateBootstrapHarness
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SelfUpdateBootstrapHarness.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/App/UpdateCommandLine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/SelfUpdateBootstrap.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/Sha256.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/detail/SelfUpdatePath.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/detail/SelfUpdateProcess.cpp"
    )
    target_include_directories(ZTSelfUpdateBootstrapHarness PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTSelfUpdateBootstrapHarness PRIVATE
        shell32
        bcrypt
    )
    zt_configure_project_target(ZTSelfUpdateBootstrapHarness)
    target_compile_options(ZTSelfUpdateBootstrapHarness PRIVATE /WX)

    add_executable(ZTSelfUpdateBootstrapTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SelfUpdateBootstrapTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Update/Sha256.cpp"
    )
    target_include_directories(ZTSelfUpdateBootstrapTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTSelfUpdateBootstrapTests PRIVATE bcrypt)
    zt_configure_project_target(ZTSelfUpdateBootstrapTests)
    target_compile_options(ZTSelfUpdateBootstrapTests PRIVATE /WX)
    add_test(
        NAME SelfUpdateBootstrap
        COMMAND ZTSelfUpdateBootstrapTests
            "$<TARGET_FILE:ZTSelfUpdateBootstrapHarness>"
    )

    add_executable(ZTExportPolicyTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExportPolicyTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/ExportPolicy.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTExportPolicyTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    zt_configure_project_target(ZTExportPolicyTests)
    target_compile_options(ZTExportPolicyTests PRIVATE /WX)
    add_test(NAME ExportPolicy COMMAND ZTExportPolicyTests)

    add_executable(ZTImage2SequenceInputTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Image2SequenceInputTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Export/Image2SequenceInput.cpp"
    )
    target_include_directories(ZTImage2SequenceInputTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    zt_configure_project_target(ZTImage2SequenceInputTests)
    target_compile_options(ZTImage2SequenceInputTests PRIVATE /WX)
    add_test(NAME Image2SequenceInput COMMAND ZTImage2SequenceInputTests)

    add_executable(ZTPlayerEngineExportSnapshotTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PlayerEngineExportSnapshotTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Cache/MemoryFrameCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/DecodeScheduler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEnginePlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineScheduling.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTPlayerEngineExportSnapshotTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTPlayerEngineExportSnapshotTests PRIVATE
        windowscodecs
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTPlayerEngineExportSnapshotTests)
    target_compile_options(ZTPlayerEngineExportSnapshotTests PRIVATE /WX)
    add_test(
        NAME PlayerEngineExportSnapshot
        COMMAND ZTPlayerEngineExportSnapshotTests
    )

    add_executable(ZTComparisonPlayerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ComparisonPlayerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Cache/MemoryFrameCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/ComparisonPlayer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/ComparisonPlayerPlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/DecodeScheduler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEnginePlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineScheduling.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTComparisonPlayerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTComparisonPlayerTests PRIVATE
        windowscodecs
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTComparisonPlayerTests)
    target_compile_options(ZTComparisonPlayerTests PRIVATE /WX)
    add_test(NAME ComparisonPlayer COMMAND ZTComparisonPlayerTests)

    add_executable(ZTDecodeSchedulerTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/DecodeSchedulerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/DecodeScheduler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
    )
    target_include_directories(ZTDecodeSchedulerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTDecodeSchedulerTests PRIVATE
        windowscodecs
        ole32
    )
    zt_configure_project_target(ZTDecodeSchedulerTests)
    target_compile_options(ZTDecodeSchedulerTests PRIVATE /WX)
    add_test(NAME DecodeScheduler COMMAND ZTDecodeSchedulerTests)

    add_executable(ZTMediaFoundationVideoSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/MediaFoundationVideoSmoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
    )
    target_include_directories(ZTMediaFoundationVideoSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTMediaFoundationVideoSmoke PRIVATE
        windowscodecs
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTMediaFoundationVideoSmoke)
    target_compile_options(ZTMediaFoundationVideoSmoke PRIVATE /WX)

    add_executable(ZTPlayerEngineVideoSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PlayerEngineVideoSmoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Cache/MemoryFrameCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/DecodeScheduler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEnginePlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineScheduling.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTPlayerEngineVideoSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTPlayerEngineVideoSmoke PRIVATE
        windowscodecs
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTPlayerEngineVideoSmoke)
    target_compile_options(ZTPlayerEngineVideoSmoke PRIVATE /WX)

    add_executable(ZTComparisonPlayerVideoSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ComparisonPlayerVideoSmoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Cache/MemoryFrameCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/ComparisonPlayer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/ComparisonPlayerPlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/DecodeScheduler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEnginePlayback.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineScheduling.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PlayerEngineSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/SequenceScanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/MediaFoundationVideoDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Imaging/WicImageDecoder.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/Utf8.cpp"
    )
    target_include_directories(ZTComparisonPlayerVideoSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ZTComparisonPlayerVideoSmoke PRIVATE
        windowscodecs
        ole32
        mfplat
        mfreadwrite
        mfuuid
        propsys
    )
    zt_configure_project_target(ZTComparisonPlayerVideoSmoke)
    target_compile_options(ZTComparisonPlayerVideoSmoke PRIVATE /WX)
endif()
