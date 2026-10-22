cmake_minimum_required(VERSION 3.24)
project(BedrockZoom LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "Do not build SDK tools in this sample" FORCE)
FetchContent_Declare(
    bedrock_sdk
    GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(bedrock_sdk)

add_library(zoom_core STATIC src/config.cpp src/controller.cpp)
target_include_directories(zoom_core PUBLIC include)
target_link_libraries(zoom_core PUBLIC bedrock::symbols)
target_compile_features(zoom_core PUBLIC cxx_std_20)

if(WIN32)
    add_library(BedrockZoom SHARED src/minecraft_client.cpp)
    target_link_libraries(BedrockZoom PRIVATE zoom_core bedrock::symbols
        bedrock::logging bedrock::hook_runtime shell32 ole32)
    set_target_properties(BedrockZoom PROPERTIES OUTPUT_NAME "BedrockZoom")
    bedrock_enable_hook_runtime(BedrockZoom)

    if(MSVC)
        foreach(target zoom_core BedrockZoom)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(zoom_test tests/zoom_test.cpp)
    target_link_libraries(zoom_test PRIVATE zoom_core)
    if(MSVC)
        target_compile_options(zoom_test PRIVATE /UNDEBUG)
        set_property(TARGET zoom_test PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(zoom_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME zoom_test COMMAND zoom_test)
endif()
