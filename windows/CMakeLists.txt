cmake_minimum_required(VERSION 3.16)
project(RetroSoundSender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# v0.3 deliberately uses the CONSOLE subsystem for startup reliability on
# old/odd Windows 8.1 tablets. main() creates the GUI, then hides the console.
add_executable(RetroSoundSender src/main.cpp)
target_link_libraries(RetroSoundSender PRIVATE ole32 ws2_32 avrt user32 comctl32)

if(MSVC)
  set_property(TARGET RetroSoundSender PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_definitions(RetroSoundSender PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0601 _WIN32_WINNT=0x0601)
  target_compile_options(RetroSoundSender PRIVATE /Od /EHsc /W4)
  # Explicit Windows 7 subsystem floor; Windows 8.1 is fully compatible.
  target_link_options(RetroSoundSender PRIVATE /SUBSYSTEM:CONSOLE,6.01)
endif()

add_executable(RetroSoundProbe src/probe.cpp)
target_link_libraries(RetroSoundProbe PRIVATE user32)
if(MSVC)
  set_property(TARGET RetroSoundProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_definitions(RetroSoundProbe PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
  target_compile_options(RetroSoundProbe PRIVATE /O2 /EHsc /W3)
  target_link_options(RetroSoundProbe PRIVATE /SUBSYSTEM:CONSOLE,6.01)
endif()
