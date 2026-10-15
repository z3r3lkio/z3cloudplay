# Renderer-independent Steam protocol and verified HTTP transport targets.

add_library(z3cloudplay_steam_protocol STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/proto_wire.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/auth_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/json.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/token_claims.cpp")
target_include_directories(z3cloudplay_steam_protocol PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(z3cloudplay_steam_protocol PUBLIC cxx_std_17)
add_library(z3cloudplay_steam_protobuf ALIAS z3cloudplay_steam_protocol)

if(Z3CLOUDPLAY_PLATFORM_PS5)
    set(z3cloudplay_steam_dependency_platform ps5)
else()
    set(z3cloudplay_steam_dependency_platform host)
endif()

add_library(z3cloudplay_steam_auth STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/auth_client.cpp")
target_include_directories(z3cloudplay_steam_auth PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(z3cloudplay_steam_auth PUBLIC cxx_std_17)
target_link_libraries(z3cloudplay_steam_auth PUBLIC
    z3cloudplay_steam_http z3cloudplay_steam_protocol z3cloudplay_steam_core)
if(Z3CLOUDPLAY_PLATFORM_PS5)
    target_compile_options(z3cloudplay_steam_auth PRIVATE -fno-exceptions -fno-rtti)
endif()
add_library(z3cloudplay_steam_library STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/library_client.cpp")
target_include_directories(z3cloudplay_steam_library PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(z3cloudplay_steam_library PUBLIC cxx_std_17)
target_link_libraries(z3cloudplay_steam_library PUBLIC
    z3cloudplay_steam_http z3cloudplay_steam_protocol z3cloudplay_library_service)
if(Z3CLOUDPLAY_PLATFORM_PS5)
    target_compile_options(z3cloudplay_steam_library PRIVATE -fno-exceptions -fno-rtti)
endif()
set(Z3CLOUDPLAY_STEAM_PREFIX
    "${CMAKE_SOURCE_DIR}/scripts/network-deps/.deps/${z3cloudplay_steam_dependency_platform}"
    CACHE PATH "Pinned curl/Mbed TLS prefix prepared by scripts/network-deps")

foreach(required_path IN ITEMS
        "${Z3CLOUDPLAY_STEAM_PREFIX}/include/curl/curl.h"
        "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libcurl.a"
        "${Z3CLOUDPLAY_STEAM_PREFIX}/cacert.pem")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Pinned Steam dependency is missing: ${required_path}. "
            "Run scripts/network-deps/prepare-${z3cloudplay_steam_dependency_platform}-deps.sh")
    endif()
endforeach()

# Threading changes public context layouts, so headers and all linked archives must be rebuilt together.
set(z3cloudplay_mbedtls_config "${Z3CLOUDPLAY_STEAM_PREFIX}/include/mbedtls/mbedtls_config.h")
string(JSON expected_mbedtls_config_hash GET "${z3cloudplay_dependency_lock}"
    steam_network_probe mbedtls config_sha256)
file(SHA256 "${z3cloudplay_mbedtls_config}" actual_mbedtls_config_hash)
if(NOT actual_mbedtls_config_hash STREQUAL expected_mbedtls_config_hash)
    message(FATAL_ERROR "Pinned Mbed TLS pthread configuration mismatch. "
        "Rebuild with scripts/network-deps/prepare-${z3cloudplay_steam_dependency_platform}-deps.sh")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${z3cloudplay_mbedtls_config}")

file(GLOB z3cloudplay_curl_headers CONFIGURE_DEPENDS
    "${Z3CLOUDPLAY_STEAM_PREFIX}/include/curl/*.h")
string(JSON expected_header_count LENGTH "${z3cloudplay_dependency_lock}"
    steam_network_probe public_headers)
list(LENGTH z3cloudplay_curl_headers actual_header_count)
if(NOT actual_header_count EQUAL expected_header_count)
    message(FATAL_ERROR "Pinned curl public-header set mismatch")
endif()
foreach(header_path IN LISTS z3cloudplay_curl_headers)
    file(RELATIVE_PATH header_key "${Z3CLOUDPLAY_STEAM_PREFIX}/include" "${header_path}")
    string(JSON expected_hash GET "${z3cloudplay_dependency_lock}"
        steam_network_probe public_headers "${header_key}")
    file(SHA256 "${header_path}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Pinned curl header hash mismatch: ${header_key}")
    endif()
endforeach()
set(z3cloudplay_steam_archives)
foreach(archive libcurl.a libeverest.a libmbedcrypto.a libmbedtls.a
                libmbedx509.a libp256m.a)
    set(archive_path "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/${archive}")
    # PS5 archives come from the pinned SDK and LLVM 18 with relative source paths, so they are
    # byte-reproducible and pinned. Host archives depend on the local Xcode toolchain; their
    # pinned sources are checked when prepare-host-deps.sh builds them.
    if(Z3CLOUDPLAY_PLATFORM_PS5)
        string(JSON expected_hash GET "${z3cloudplay_dependency_lock}"
            steam_network_probe ps5_archives "${archive}")
        file(SHA256 "${archive_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Pinned ps5 archive hash mismatch: ${archive}. "
                "Rebuild with scripts/network-deps/prepare-ps5-deps.sh")
        endif()
    endif()
    list(APPEND z3cloudplay_steam_archives "${archive_path}")
endforeach()
string(JSON expected_ca_hash GET "${z3cloudplay_dependency_lock}"
    steam_network_probe ca_bundle sha256)
file(SHA256 "${Z3CLOUDPLAY_STEAM_PREFIX}/cacert.pem" actual_ca_hash)
if(NOT actual_ca_hash STREQUAL expected_ca_hash)
    message(FATAL_ERROR "Pinned Steam CA bundle hash mismatch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/dependencies.lock.json" ${z3cloudplay_curl_headers}
    ${z3cloudplay_steam_archives} "${Z3CLOUDPLAY_STEAM_PREFIX}/cacert.pem")

add_library(z3cloudplay_curl_8_22 STATIC IMPORTED GLOBAL)
set_target_properties(z3cloudplay_curl_8_22 PROPERTIES
    IMPORTED_LOCATION "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libcurl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${Z3CLOUDPLAY_STEAM_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
    INTERFACE_LINK_LIBRARIES
        "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedtls.a;${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedx509.a;${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedcrypto.a;${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libeverest.a;${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libp256m.a;Threads::Threads")
if(APPLE AND NOT Z3CLOUDPLAY_PLATFORM_PS5)
    set_property(TARGET z3cloudplay_curl_8_22 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        "-framework SystemConfiguration" "-framework CoreFoundation"
        "-framework CoreServices")
endif()

# Require the threading implementation to link, catching new headers with old host archives.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES z3cloudplay_curl_8_22)
unset(z3cloudplay_mbedtls_threading_links CACHE)
check_cxx_source_compiles("#include <mbedtls/threading.h>
int main() { mbedtls_threading_mutex_t mutex; mbedtls_mutex_init(&mutex);
  int result = mbedtls_mutex_lock(&mutex); mbedtls_mutex_unlock(&mutex);
  mbedtls_mutex_free(&mutex); return result; }" z3cloudplay_mbedtls_threading_links)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT z3cloudplay_mbedtls_threading_links)
    message(FATAL_ERROR "Mbed TLS pthread headers/libraries mismatch. Rebuild network dependencies together.")
endif()

add_library(z3cloudplay_steam_http STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/http_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/ws_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/native_network.cpp")
target_include_directories(z3cloudplay_steam_http PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(z3cloudplay_steam_http PUBLIC cxx_std_17)
target_link_libraries(z3cloudplay_steam_http PUBLIC z3cloudplay_curl_8_22 Threads::Threads)
if(Z3CLOUDPLAY_PLATFORM_PS5)
    target_compile_definitions(z3cloudplay_steam_http PRIVATE Z3CLOUDPLAY_PLATFORM_PS5=1)
    target_compile_options(z3cloudplay_steam_http PRIVATE -fno-exceptions -fno-rtti)
endif()

# Authenticated CM session for the CM-only library facts (acquisition dates).
add_library(z3cloudplay_steam_cm STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/entitled_library.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_app_details.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cdn_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/depot_installer.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/steam_install_runner.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_mapping.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/steam_cloud_runner.cpp")
target_include_directories(z3cloudplay_steam_cm PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(z3cloudplay_steam_cm PUBLIC cxx_std_17)
target_link_libraries(z3cloudplay_steam_cm PUBLIC
    z3cloudplay_steam_http z3cloudplay_steam_protocol z3cloudplay_steam_cm_codec z3cloudplay_depot_codec z3cloudplay_steam_core
    z3cloudplay_runtime_steamworks)
if(Z3CLOUDPLAY_PLATFORM_PS5)
    target_compile_definitions(z3cloudplay_steam_cm PRIVATE Z3CLOUDPLAY_PLATFORM_PS5=1)
    target_compile_options(z3cloudplay_steam_cm PRIVATE -fno-exceptions -fno-rtti)
endif()

set(Z3CLOUDPLAY_STEAM_CA_BUNDLE "${Z3CLOUDPLAY_STEAM_PREFIX}/cacert.pem"
    CACHE INTERNAL "Verified Steam HTTPS trust bundle")
set(Z3CLOUDPLAY_STEAM_LINK_ARCHIVES
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libcurl.a"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedtls.a"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedx509.a"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedcrypto.a"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libeverest.a"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libp256m.a"
    CACHE INTERNAL "Verified native Steam transport archives")

set(z3cloudplay_staged_ca "${CMAKE_BINARY_DIR}/trust/cacert.pem")
add_custom_command(OUTPUT "${z3cloudplay_staged_ca}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/trust"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${Z3CLOUDPLAY_STEAM_CA_BUNDLE}" "${z3cloudplay_staged_ca}"
    DEPENDS "${Z3CLOUDPLAY_STEAM_CA_BUNDLE}" VERBATIM)
add_custom_target(z3cloudplay_steam_trust ALL DEPENDS "${z3cloudplay_staged_ca}")

# The secure vault is isolated until the PS5 passphrase UI is available.
add_library(z3cloudplay_secure_session_vault STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/secure_session_vault.cpp")
target_compile_features(z3cloudplay_secure_session_vault PUBLIC cxx_std_17)
target_include_directories(z3cloudplay_secure_session_vault PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${Z3CLOUDPLAY_STEAM_PREFIX}/include")
target_link_libraries(z3cloudplay_secure_session_vault PUBLIC
    "${Z3CLOUDPLAY_STEAM_PREFIX}/lib/libmbedcrypto.a")

function(z3cloudplay_register_steam_backend_tests)
    if(NOT BUILD_TESTING OR Z3CLOUDPLAY_PLATFORM_PS5)
        return()
    endif()

    foreach(test_name proto_wire auth_messages)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE z3cloudplay_steam_protocol)
    endforeach()
    add_test(NAME proto_wire COMMAND proto_wire_test)
    add_test(NAME auth_messages COMMAND auth_messages_test
        "${CMAKE_SOURCE_DIR}/fixtures/steam/auth")

    foreach(test_name steam_json token_claims)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE z3cloudplay_steam_protocol)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()

    add_executable(secure_session_vault_test "${CMAKE_SOURCE_DIR}/tests/secure_session_vault_test.cpp")
    target_link_libraries(secure_session_vault_test PRIVATE z3cloudplay_secure_session_vault)
    add_test(NAME secure_session_vault COMMAND secure_session_vault_test)

    foreach(test_name session_store steam_auth)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()

    if(TARGET z3cloudplay_steam_http)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(native_resolution_cache_test "${CMAKE_SOURCE_DIR}/tests/native_resolution_cache_test.cpp")
        target_link_libraries(native_resolution_cache_test PRIVATE z3cloudplay_steam_http)
        add_test(NAME native_resolution_cache COMMAND native_resolution_cache_test)

        add_executable(mbedtls_threading_test "${CMAKE_SOURCE_DIR}/tests/mbedtls_threading_test.cpp")
        target_link_libraries(mbedtls_threading_test PRIVATE z3cloudplay_steam_http)
        add_test(NAME mbedtls_threading COMMAND mbedtls_threading_test)

        add_executable(http_client_test "${CMAKE_SOURCE_DIR}/tests/http_client_test.cpp")
        target_link_libraries(http_client_test PRIVATE z3cloudplay_steam_http)
        add_test(NAME http_client
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/http_fixture_server.py"
                "$<TARGET_FILE:http_client_test>" "${CMAKE_SOURCE_DIR}/tests/fixtures/http")
        set_tests_properties(http_client PROPERTIES TIMEOUT 15)

        add_executable(auth_client_test "${CMAKE_SOURCE_DIR}/tests/auth_client_test.cpp")
        target_link_libraries(auth_client_test PRIVATE z3cloudplay_steam_auth)
        add_test(NAME auth_client COMMAND auth_client_test)

        foreach(test_name cm_messages cm_client)
            add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
            target_link_libraries(${test_name}_test PRIVATE z3cloudplay_steam_cm)
            add_test(NAME ${test_name} COMMAND ${test_name}_test "${CMAKE_SOURCE_DIR}/fixtures/steam/cm")
        endforeach()
        add_executable(download_rate_test "${CMAKE_SOURCE_DIR}/tests/download_rate_test.cpp")
        target_link_libraries(download_rate_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME download_rate COMMAND download_rate_test)
        add_executable(install_service_test "${CMAKE_SOURCE_DIR}/tests/install_service_test.cpp")
        target_link_libraries(install_service_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME install_service COMMAND install_service_test)
        add_executable(cloud_service_test "${CMAKE_SOURCE_DIR}/tests/cloud_service_test.cpp")
        target_link_libraries(cloud_service_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME cloud_service COMMAND cloud_service_test)
        add_executable(install_volumes_test "${CMAKE_SOURCE_DIR}/tests/install_volumes_test.cpp")
        target_link_libraries(install_volumes_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME install_volumes COMMAND install_volumes_test)
        add_executable(depot_installer_test "${CMAKE_SOURCE_DIR}/tests/depot_installer_test.cpp")
        target_link_libraries(depot_installer_test PRIVATE z3cloudplay_steam_cm)
        add_test(NAME depot_installer COMMAND depot_installer_test)
        add_executable(steam_cloud_test "${CMAKE_SOURCE_DIR}/tests/steam_cloud_test.cpp")
        target_link_libraries(steam_cloud_test PRIVATE z3cloudplay_steam_cm)
        target_compile_features(steam_cloud_test PRIVATE cxx_std_20)
        add_test(NAME steam_cloud COMMAND steam_cloud_test)
        add_executable(cdn_client_test "${CMAKE_SOURCE_DIR}/tests/cdn_client_test.cpp")
        target_link_libraries(cdn_client_test PRIVATE z3cloudplay_steam_cm)
        add_test(NAME cdn_client COMMAND cdn_client_test)
        add_test(NAME download_speed_report
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_download_speed_report.py")
        add_executable(store_details_test "${CMAKE_SOURCE_DIR}/tests/store_details_test.cpp")
        target_link_libraries(store_details_test PRIVATE z3cloudplay_steam_core)
        add_test(NAME store_details COMMAND store_details_test)
        add_executable(app_details_test "${CMAKE_SOURCE_DIR}/tests/app_details_test.cpp")
        target_link_libraries(app_details_test PRIVATE z3cloudplay_steam_cm)
        add_test(NAME app_details COMMAND app_details_test)
        add_executable(entitled_library_test "${CMAKE_SOURCE_DIR}/tests/entitled_library_test.cpp")
        target_link_libraries(entitled_library_test PRIVATE z3cloudplay_steam_cm)
        add_test(NAME entitled_library COMMAND entitled_library_test)

        add_executable(library_client_test "${CMAKE_SOURCE_DIR}/tests/library_client_test.cpp")
        target_link_libraries(library_client_test PRIVATE z3cloudplay_steam_library)
        add_test(NAME library_client COMMAND library_client_test
            "${CMAKE_SOURCE_DIR}/fixtures/steam/library")
    endif()
endfunction()
