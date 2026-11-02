# vip_infiniteammo — Plugify C++ plugin: port of the CookiesVip module "[VIP] Infinite Ammo".
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The map gate, the
# client/handle checks, the map-gated schema offsets and the KV1 config reader live in the plugin's
# own src/, and the engine-free logic (ammo plan, pack table, offset cache policy) is shared with
# the test.
#
#   plugify-vip_infiniteammo  the plugin shared library (entry "bin/vip_infiniteammo")
#   vip_infiniteammo_tests    engine-free unit tests: the ammo plan, the KV1 reader and the pack
#                            table on the production-shaped config in tests/data, the config path,
#                            and the map gate / IsAlive / handle filter / schema cache / field
#                            access driven through fake s2sdk bindings
#                            (build/tests/vip_infiniteammo_tests)

set(VIP_INFINITEAMMO_VERSION "1.1.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_infiniteammo"), so vip_bindings already
# exists; a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_infiniteammo SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_infiniteammo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_infiniteammo SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_infiniteammo PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_infiniteammo PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_infiniteammo PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_infiniteammo"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_infiniteammo PROPERTIES OUTPUT_NAME vip_infiniteammo)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_infiniteammo PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_infiniteammo.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_infiniteammo.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# vip_infiniteammo_tests: no engine and no pisexcompat. The pure half (ammo plan, KV1 reader, pack
# table, config path) touches no binding; the guard half installs fake __s2sdk_* bindings and
# checks that no client, entity or schema query leaves the plugin outside a live map (B1/B2), that
# IsAlive applies the PITFALLS C2 chain and the handle filter the C1 rules, and that the schema
# offsets are map-gated and cache only successful lookups (C8).
add_executable(vip_infiniteammo_tests
        tests/vip_infiniteammo_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(vip_infiniteammo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_infiniteammo_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_infiniteammo_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_infiniteammo_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_infiniteammo_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        VIP_INFINITEAMMO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(vip_infiniteammo_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
