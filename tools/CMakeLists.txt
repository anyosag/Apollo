cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp utils.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# [fork] Installs over-the-air updates (src/fork/updater.cpp starts it)
add_executable(apollo-updater apollo-updater.cpp)
set_target_properties(apollo-updater PROPERTIES CXX_STANDARD 23)
target_link_libraries(apollo-updater
        ${CMAKE_THREAD_LIBS_INIT}
        advapi32
        bcrypt
        shell32
        ws2_32
        ${PLATFORM_LIBRARIES})
target_compile_options(apollo-updater PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# [fork] Opens the touch keyboard for stock Moonlight for iOS's taps in text fields (src/fork/touch_keyboard.cpp starts it)
add_executable(apollo-touch-keyboard apollo-touch-keyboard.cpp)
set_target_properties(apollo-touch-keyboard PROPERTIES CXX_STANDARD 23)
target_link_libraries(apollo-touch-keyboard
        ${CMAKE_THREAD_LIBS_INIT}
        dwmapi
        ole32
        oleaut32
        shell32
        user32
        ${PLATFORM_LIBRARIES})
target_compile_options(apollo-touch-keyboard PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# [fork] Maps LuzLunar's Mac as a network drive for the signed-in user (src/fork/mac_drive.cpp starts it)
add_executable(apollo-mac-drive apollo-mac-drive.cpp)
set_target_properties(apollo-mac-drive PROPERTIES CXX_STANDARD 23)
target_link_libraries(apollo-mac-drive
        ${CMAKE_THREAD_LIBS_INIT}
        advapi32
        mpr
        nlohmann_json::nlohmann_json
        shell32
        ${PLATFORM_LIBRARIES})
target_compile_options(apollo-mac-drive PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
