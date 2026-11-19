cmake_minimum_required(VERSION 3.16)
project(FilmRender VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library (cross-platform, no wxWidgets UI dependency)
add_library(filmrender_core STATIC
    src/MediaUtils.cpp
    src/ProcessRunner.cpp
    src/RenderEngine.cpp
    src/ToolManager.cpp
    src/YtDlpUtils.cpp
)

find_package(Threads REQUIRED)

target_include_directories(filmrender_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(filmrender_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(filmrender_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(filmrender_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# wxWidgets GUI executable
find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

set(GUI_SOURCES
    src/main.cpp
    src/MainFrame.cpp
    src/Mp3DownloaderDialog.cpp
)

if(WIN32)
    add_executable(FilmRender WIN32 ${GUI_SOURCES})
elseif(APPLE)
    add_executable(FilmRender MACOSX_BUNDLE ${GUI_SOURCES})
    set_target_properties(FilmRender PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "FilmRender"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.filmrender.app"
    )
else()
    add_executable(FilmRender ${GUI_SOURCES})
endif()

target_link_libraries(FilmRender PRIVATE
    filmrender_core
    ${wxWidgets_LIBRARIES}
)

if(MSVC)
    target_compile_options(FilmRender PRIVATE /W4 /utf-8)
else()
    target_compile_options(FilmRender PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Unit tests for core logic
enable_testing()
add_executable(filmrender_tests
    tests/test_core.cpp
)
target_link_libraries(filmrender_tests PRIVATE filmrender_core)
if(MSVC)
    target_compile_options(filmrender_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(filmrender_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME FilmRenderCoreTests COMMAND filmrender_tests)
