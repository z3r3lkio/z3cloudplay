cmake_minimum_required(VERSION 3.24)
project(Z3CloudPlayLauncher VERSION 0.2.0 LANGUAGES C CXX)

option(Z3CLOUDPLAY_UI_SIMULATION "Package selected PS5 UI with developer simulation fixtures" OFF)
option(Z3CLOUDPLAY_PLATFORM_PS5 "Build a PS5 native title" OFF)
set(Z3CLOUDPLAY_PS5_TITLE_ID "" CACHE STRING "Explicit PS5 title id (required for PS5 packaging)")

file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" z3cloudplay_dependency_lock)
include(CTest)
include(cmake/PlaydenCore.cmake)
include(cmake/SteamBackend.cmake)
z3cloudplay_register_steam_backend_tests()
include(cmake/ProsperoRuntime.cmake)
include(cmake/HomebrewUI.cmake)
