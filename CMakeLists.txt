cmake_minimum_required(VERSION 3.20)

# Host-test target uses the system g++; the DLL target cross-compiles with MinGW.
option(DECODER_TESTS_ONLY "Build only the host unit-test target" OFF)

if(NOT DECODER_TESTS_ONLY)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
endif()

project(DecoderRing CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/public
                    ${CMAKE_SOURCE_DIR}/src)

# Pure core sources shared by BOTH the DLL and the host tests (no Nexus, no Win32).
set(CORE_SOURCES
    src/chat/ChatLinks.cpp
    src/chat/WaypointNames.cpp
    src/resolve/ItemResolver.cpp
    src/resolve/RecipeResolver.cpp
    src/resolve/SkinResolver.cpp
    src/resolve/SkillResolver.cpp
    src/resolve/PriceCache.cpp
    src/resolve/OfflineResolve.cpp
    src/resolve/RecordFill.cpp
    src/resolve/DecoderService.cpp)

if(DECODER_TESTS_ONLY)
    add_executable(decoder_tests tests/test_main.cpp ${CORE_SOURCES})
    target_compile_definitions(decoder_tests PRIVATE DECODER_HOSTTEST)

    add_executable(chatlinks_isolation tests/isolation_main.cpp src/chat/ChatLinks.cpp)
    target_include_directories(chatlinks_isolation PRIVATE src)
    return()
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_SHARED_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++ -DWIN32_LEAN_AND_MEAN")

add_library(DecoderRing SHARED
    src/dllmain.cpp
    src/resolve/Http_WinINet.cpp
    ${CORE_SOURCES})
set_target_properties(DecoderRing PROPERTIES SUFFIX ".dll" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(DecoderRing wininet)
target_link_options(DecoderRing PRIVATE -Wl,--strip-all)
target_sources(DecoderRing PRIVATE ${CMAKE_SOURCE_DIR}/DecoderRing.def)
