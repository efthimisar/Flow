cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon only")

project(SimpleHost VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(SimpleHost
    PRODUCT_NAME "Simple Host"
    COMPANY_NAME "Prisma Audio"
    BUNDLE_ID com.prismaaudio.simplehost
    VERSION 0.1.0
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Το Simple Host χρειάζεται είσοδο ήχου για plugins εφέ.")

target_sources(SimpleHost PRIVATE Source/Main.cpp)

target_compile_definitions(SimpleHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1)

target_link_libraries(SimpleHost PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
