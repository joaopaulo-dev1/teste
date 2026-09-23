cmake_minimum_required(VERSION 3.20)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version")
endif()

project(GameCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(gamecore_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(gamecore_assets STATIC
    GameCore/AssetSystem/ReadOnlyFile.cpp
)
target_include_directories(gamecore_assets PUBLIC ${CMAKE_SOURCE_DIR})
gamecore_warnings(gamecore_assets)

add_library(asset_inspector_lib STATIC
    Tools/AssetInspector/ExecutableProbe.cpp
    Tools/AssetInspector/FormatProbe.cpp
    Tools/AssetInspector/GraphicsScan.cpp
    Tools/AssetInspector/ZipLister.cpp
    Tools/AssetInspector/Inspector.cpp
)
target_include_directories(asset_inspector_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(asset_inspector_lib PUBLIC gamecore_assets)
gamecore_warnings(asset_inspector_lib)

add_library(dat_probe_lib STATIC
    Tools/DatProbe/DatProbe.cpp
    Tools/DatProbe/EntryProbe.cpp
    Tools/DatProbe/DatIndex.cpp
    Tools/DatProbe/DatListing.cpp
    Tools/DatProbe/NameTable.cpp
    Tools/DatProbe/ContentCheck.cpp
)
target_include_directories(dat_probe_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(dat_probe_lib PUBLIC asset_inspector_lib)
gamecore_warnings(dat_probe_lib)

if(IOS)
    enable_language(OBJCXX)

    set(LB3_VERSION "0.1.0")
    set(LB3_BUILD "1")
    set(LB3_BUNDLE_ID "com.jcolp.lb3port" CACHE STRING "Bundle identifier; sideloading tools may rewrite it")
    configure_file(Platform/iOS/Info.plist.in ${CMAKE_BINARY_DIR}/LB3Port-Info.plist @ONLY)

    add_executable(LB3Port MACOSX_BUNDLE
        Platform/iOS/main.mm
        Platform/iOS/AppDelegate.mm
        Platform/iOS/ArchiveListViewController.mm
        Platform/iOS/EntryListViewController.mm
        Platform/iOS/LBArchive.mm
    )
    target_include_directories(LB3Port PRIVATE ${CMAKE_SOURCE_DIR}/Platform/iOS)
    target_link_libraries(LB3Port PRIVATE
        dat_probe_lib
        "-framework Foundation"
        "-framework UIKit"
        "-framework UniformTypeIdentifiers"
    )
    target_compile_options(LB3Port PRIVATE -Wall -Wextra -fobjc-arc)
    set_target_properties(LB3Port PROPERTIES
        OUTPUT_NAME "LB3Port"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/LB3Port-Info.plist
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${LB3_BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_NAME "LB3Port"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET}
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    )
    return()
endif()

add_executable(asset_inspector Tools/AssetInspector/main.cpp)
target_link_libraries(asset_inspector PRIVATE asset_inspector_lib)
gamecore_warnings(asset_inspector)
if(WIN32)
    target_link_libraries(asset_inspector PRIVATE shell32)
endif()

add_executable(dat_probe Tools/DatProbe/main.cpp)
target_link_libraries(dat_probe PRIVATE dat_probe_lib)
gamecore_warnings(dat_probe)
if(WIN32)
    target_link_libraries(dat_probe PRIVATE shell32)
endif()

add_executable(dat_names Tools/DatProbe/names_main.cpp)
target_link_libraries(dat_names PRIVATE dat_probe_lib)
gamecore_warnings(dat_names)
if(WIN32)
    target_link_libraries(dat_names PRIVATE shell32)
endif()

add_library(dat_extract_lib STATIC Tools/DatExtract/Extractor.cpp)
target_include_directories(dat_extract_lib PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(dat_extract_lib PUBLIC dat_probe_lib)
gamecore_warnings(dat_extract_lib)

add_executable(dat_extract Tools/DatExtract/main.cpp)
target_link_libraries(dat_extract PRIVATE dat_extract_lib)
gamecore_warnings(dat_extract)
if(WIN32)
    target_link_libraries(dat_extract PRIVATE shell32)
endif()

enable_testing()

add_executable(inspector_tests tests/InspectorTest.cpp)
target_link_libraries(inspector_tests PRIVATE asset_inspector_lib)
gamecore_warnings(inspector_tests)
add_test(NAME inspector_tests COMMAND inspector_tests)

add_executable(extractor_tests tests/ExtractorTest.cpp)
target_link_libraries(extractor_tests PRIVATE dat_extract_lib)
gamecore_warnings(extractor_tests)
add_test(NAME extractor_tests COMMAND extractor_tests)

add_executable(dat_probe_tests tests/DatProbeTest.cpp)
target_link_libraries(dat_probe_tests PRIVATE dat_probe_lib)
gamecore_warnings(dat_probe_tests)
add_test(NAME dat_probe_tests COMMAND dat_probe_tests)
