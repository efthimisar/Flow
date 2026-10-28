cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon only")

project(Flow VERSION 0.51.3)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Ableton Link (GPL). Μπαίνει σε ξεχωριστή βιβλιοθήκη ώστε οι κεφαλίδες του να μένουν έξω από τον κώδικα JUCE.
option(FLOW_LINK "Build with Ableton Link" ON)
if(FLOW_LINK)
    FetchContent_Declare(
        AbletonLink
        GIT_REPOSITORY https://github.com/Ableton/link.git
        GIT_TAG Link-4.1.1
        GIT_SUBMODULES "modules/asio-standalone"
        SOURCE_SUBDIR does-not-exist)
    FetchContent_MakeAvailable(AbletonLink)
    include(${abletonlink_SOURCE_DIR}/AbletonLinkConfig.cmake)
endif()

add_library(FlowLink STATIC Source/LinkBridge.cpp)
target_include_directories(FlowLink PUBLIC Source)
if(FLOW_LINK)
    target_compile_definitions(FlowLink PUBLIC FLOW_LINK=1)
    target_link_libraries(FlowLink PUBLIC Ableton::Link)
else()
    target_compile_definitions(FlowLink PUBLIC FLOW_LINK=0)
endif()

juce_add_gui_app(Flow
    PRODUCT_NAME "Flow"
    COMPANY_NAME "Prisma Audio"
    BUNDLE_ID com.prismaaudio.flow
    VERSION 0.51.3
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png")

juce_add_binary_data(FlowResources SOURCES Resources/Carlito-Bold.ttf)

target_sources(Flow PRIVATE
    Source/Main.cpp
    Source/HobbyStyle.cpp
    Source/RecorderEngine.cpp
    Source/KeyLights.cpp)

target_compile_definitions(Flow PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1)

target_link_libraries(Flow PRIVATE
    FlowResources
    FlowLink
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

if(APPLE)
    target_link_libraries(Flow PRIVATE "-framework IOKit")
endif()
