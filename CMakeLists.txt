cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon only")

project(Flow VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(Flow
    PRODUCT_NAME "Flow"
    COMPANY_NAME "Prisma Audio"
    BUNDLE_ID com.prismaaudio.flow
    VERSION 0.3.0)

juce_add_binary_data(FlowResources SOURCES Resources/Carlito-Bold.ttf)

target_sources(Flow PRIVATE
    Source/Main.cpp
    Source/HobbyStyle.cpp
    Source/RecorderEngine.cpp)

target_compile_definitions(Flow PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1)

target_link_libraries(Flow PRIVATE
    FlowResources
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
