# vip_infiniteammo — Plugify C++ plugin: port of the CookiesVip module "[VIP] Infinite Ammo".
#
#   plugify-vip_infiniteammo  the plugin shared library (entry "bin/vip_infiniteammo")

set(VIP_INFINITEAMMO_VERSION "1.0.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_* / __hudmenu_* (s2sdk_bindings, hudmenu_bindings - both come in through
#   pisexcompat's PUBLIC link) and __vip_* (vip_bindings, the generated consumer bindings of
#   plugins/vip).
set(VIP_INFINITEAMMO_BINDING_OBJECTS "")
if(TARGET vip_bindings)
    list(APPEND VIP_INFINITEAMMO_BINDING_OBJECTS $<TARGET_OBJECTS:vip_bindings>)
else()
    message(WARNING "vip_infiniteammo: vip_bindings is missing (build plugins/vip first)")
endif()

add_library(plugify-vip_infiniteammo SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${VIP_INFINITEAMMO_BINDING_OBJECTS}
)
target_include_directories(plugify-vip_infiniteammo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg/pisexcompat headers must not fail -Werror.
target_include_directories(plugify-vip_infiniteammo SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_link_libraries(plugify-vip_infiniteammo PRIVATE
        pisexcompat
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

# --- tests --------------------------------------------------------------------
# Engine-free logic tests; run by ./test.sh (build/tests).
add_executable(vip_infiniteammo_tests tests/vip_infiniteammo_tests.cpp)
target_include_directories(vip_infiniteammo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_infiniteammo_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_infiniteammo_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_infiniteammo_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_infiniteammo_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
