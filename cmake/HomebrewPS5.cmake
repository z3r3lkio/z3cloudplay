# Direct Hui native title packaging; no Rml archives in the link graph.
function(z3cloudplay_add_homebrew_ps5_title)
	if(NOT DEFINED ENV{PS5_PAYLOAD_SDK})
		message(FATAL_ERROR "PS5_PAYLOAD_SDK is unset; use scripts/build-homebrew-ps5.sh")
	endif()
	if(NOT Z3CLOUDPLAY_PS5_TITLE_ID MATCHES "^FAKE[0-9][0-9][0-9][0-9][0-9]$")
		message(FATAL_ERROR "Z3CLOUDPLAY_PS5_TITLE_ID is required and must match FAKE12345")
	endif()

	set(sdk "$ENV{PS5_PAYLOAD_SDK}")
	set(sdk_deps "${Z3CLOUDPLAY_PS5_DEPS_DIR}")
	# Native tooling, linker script and runtime shim come from the pinned boilerplate submodule.
	set(boilerplate "${CMAKE_SOURCE_DIR}/third_party/ps5-native-app-boilerplate")
	if(NOT EXISTS "${boilerplate}/tooling/native/sce_module_writer.cpp")
		message(FATAL_ERROR "Missing boilerplate submodule; run: git submodule update --init third_party/ps5-native-app-boilerplate")
	endif()
	set(gl_source "${CMAKE_SOURCE_DIR}/third_party/ps5-opengl")
	set(gl_sdk "${sdk_deps}/ps5-opengl/ps5-opengl-sdk-1.0.0/sdk")
	set(out "${CMAKE_BINARY_DIR}")
	set(app "${out}/dist/${Z3CLOUDPLAY_PS5_TITLE_ID}")
	set(host "${out}/host-tools")
	set(native "${host}/native-src")
	file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" dependency_lock)
	string(JSON expected_gl_sha GET "${dependency_lock}" ps5_opengl libPS5OpenGL_sha256)
	string(JSON expected_builtins_sha GET "${dependency_lock}" compiler_rt_builtins sha256)
	string(JSON expected_libc_sha GET "${dependency_lock}" ps5_native_app_boilerplate libc_prx_sha256)
	file(SHA256 "${gl_sdk}/lib/libPS5OpenGL.a" actual_gl_sha)
	file(SHA256 "${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a" actual_builtins_sha)
	if(NOT actual_gl_sha STREQUAL expected_gl_sha)
		message(FATAL_ERROR "libPS5OpenGL.a hash mismatch: expected ${expected_gl_sha}, found ${actual_gl_sha}")
	endif()
	if(NOT actual_builtins_sha STREQUAL expected_builtins_sha)
		message(FATAL_ERROR "compiler-rt builtins hash mismatch: expected ${expected_builtins_sha}, found ${actual_builtins_sha}")
	endif()

	file(MAKE_DIRECTORY "${out}/link/tooling/native" "${out}/stubs")
	file(COPY "${boilerplate}/tooling/native/" DESTINATION "${native}")
	set(writer "${native}/sce_module_writer.cpp")
	file(READ "${writer}" writer_contents)
	set(heap_default "write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());")
	string(FIND "${writer_contents}" "${heap_default}" heap_at)
	if(heap_at EQUAL -1)
		message(FATAL_ERROR "Could not apply the pinned 256 MiB PS5 heap patch")
	endif()
	string(REPLACE "${heap_default}" "write_u64(result.data, result.heap_size, 0x10000000ULL);"
		writer_contents "${writer_contents}")
	file(WRITE "${writer}" "${writer_contents}")
	file(COPY "${boilerplate}/tooling/native/ps5-pie.ld" DESTINATION "${out}/link/tooling/native")

	file(GLOB sdk_stubs CONFIGURE_DEPENDS "${sdk}/target/lib/*.so")
	set(link_stubs)
	foreach(stub IN LISTS sdk_stubs)
		get_filename_component(name "${stub}" NAME)
		file(CREATE_LINK "${stub}" "${out}/stubs/${name}" SYMBOLIC COPY_ON_ERROR)
		list(APPEND link_stubs "${out}/stubs/${name}")
	endforeach()
	foreach(name libSceAgc.so libSceAgcDriver.so)
		file(COPY_FILE "${gl_sdk}/lib/${name}" "${out}/stubs/${name}" ONLY_IF_DIFFERENT)
		list(APPEND link_stubs "${out}/stubs/${name}")
	endforeach()

	add_library(z3cloudplay_ps5_runtime OBJECT
		"${boilerplate}/tooling/native/app_crt.cpp"
		"${gl_source}/native-app/runtime_shims.c"
		"${gl_source}/native-app/app_heap.c")
	target_compile_features(z3cloudplay_ps5_runtime PRIVATE cxx_std_20)
	target_compile_options(z3cloudplay_ps5_runtime PRIVATE -O2 -ffunction-sections -fdata-sections
		$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)

	add_library(z3cloudplay_ps5_shims OBJECT src/platform/ps5/title_shims.c src/platform/ps5/file_shims.c)
	target_include_directories(z3cloudplay_ps5_shims PRIVATE "${gl_sdk}/include")
	target_compile_options(z3cloudplay_ps5_shims PRIVATE -O2 -Wall -ffunction-sections -fdata-sections)
	set(shim_wraps --wrap=glDrawArrays --wrap=glDrawArraysInstanced --wrap=glDrawArraysInstancedBaseInstance
		--wrap=glDrawElements --wrap=glDrawElementsInstanced --wrap=glBindFramebuffer --wrap=glBlitFramebuffer
		--wrap=glTexImage2D --wrap=glTexSubImage2D --wrap=glClear --wrap=sceSystemServiceHideSplashScreen
        --wrap=openat --wrap=mkdirat)

	add_library(z3cloudplay_ps5_app OBJECT src/homebrew/ps5.cpp src/homebrew/usb_keyboard.cpp src/platform/ps5/title_runtime.cpp
        src/platform/ps5/native_startup.cpp src/platform/ps5/console_services.cpp
        "${Z3CLOUDPLAY_HUI_SOURCE_DIR}/src/platform/ps5/display_egl.cpp"
        "${Z3CLOUDPLAY_HUI_SOURCE_DIR}/src/platform/ps5/pad.cpp"
        "${Z3CLOUDPLAY_HUI_SOURCE_DIR}/src/platform/ps5/system.cpp")
    # The runtime host (per-game bottles and titles) is part of the normal app.
    # prospero-lld receives archives explicitly; CMake PUBLIC links do not reach it.
    set(title_database_targets z3cloudplay_homebrew_runtime_host z3cloudplay_native_coordinator z3cloudplay_publication_transaction
        z3cloudplay_title_database z3cloudplay_sqlite_reader z3cloudplay_title_activity
        z3cloudplay_runtime_settings_publication z3cloudplay_runtime_settings_publication_journal z3cloudplay_runtime_publication_backup
        z3cloudplay_runtime_publication_contract z3cloudplay_runtime_publication_verifier z3cloudplay_runtime_package_installer
        z3cloudplay_runtime_kit_trust z3cloudplay_runtime_kit_admission z3cloudplay_runtime_kit_manifest)
    set(title_database_archives)
    foreach(target IN LISTS title_database_targets)
        list(APPEND title_database_archives $<TARGET_FILE:${target}>)
    endforeach()
    option(Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS "Require the native runtime client and exact title helper" ON)
    set(prospero_default_source "${CMAKE_SOURCE_DIR}/third_party/prospero-win")
    set(Z3CLOUDPLAY_PROSPERO_SOURCE_DIR "${prospero_default_source}" CACHE PATH
        "prospero-win source for the native helper client")
    set(native_access_source OFF)
    if(EXISTS "${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.h")
        file(READ "${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.h" lapy_header)
        if(Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS AND lapy_header MATCHES "pw_lapy_elevation_request_at_detailed")
            set(native_access_source ON)
            target_sources(z3cloudplay_ps5_app PRIVATE
                "${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.c"
                "${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR}/native/ps5log/ps5log_ps5_net.c")
            target_include_directories(z3cloudplay_ps5_app PRIVATE "${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR}/native")
        endif()
    endif()
    if(Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS AND NOT native_access_source)
        message(FATAL_ERROR "Native runtime access requires the patched prospero-win client; initialize the pinned source or supply Z3CLOUDPLAY_PROSPERO_SOURCE_DIR. Launcher-only builds must explicitly set Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS=OFF.")
    endif()
    target_include_directories(z3cloudplay_ps5_app PRIVATE "${gl_sdk}/include" "${Z3CLOUDPLAY_HUI_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(z3cloudplay_ps5_app PRIVATE GL_GLEXT_PROTOTYPES=1
        Z3CLOUDPLAY_PS5_TITLE_ID="${Z3CLOUDPLAY_PS5_TITLE_ID}"
        Z3CLOUDPLAY_NATIVE_ACCESS_SOURCE=$<BOOL:${native_access_source}>
        Z3CLOUDPLAY_UI_SIMULATION=$<BOOL:${Z3CLOUDPLAY_UI_SIMULATION}>)
    target_compile_options(z3cloudplay_ps5_app PRIVATE -O2 -Wall -ffunction-sections -fdata-sections
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
    target_compile_features(z3cloudplay_ps5_app PRIVATE cxx_std_20)
    target_link_libraries(z3cloudplay_ps5_app PRIVATE z3cloudplay_hui z3cloudplay_library z3cloudplay_steam_auth
        z3cloudplay_homebrew_controller z3cloudplay_local_session_repository z3cloudplay_secure_session_repository z3cloudplay_steam_library z3cloudplay_steam_cm z3cloudplay_native_access)

	set(host_tool "${host}/ps5-native-tool")
	set(libc_prx "${host}/libc.prx")
	add_custom_command(OUTPUT "${host_tool}" "${libc_prx}"
		COMMAND "${CMAKE_COMMAND}" -DHOST_CXX=/usr/bin/c++ -DNATIVE_DIR=${native} -DHOST_DIR=${host}
			-DEXPECTED_LIBC_SHA256=${expected_libc_sha}
			-P "${CMAKE_SOURCE_DIR}/cmake/PreparePS5HostTools.cmake"
		DEPENDS "${native}/native_app_builder.cpp" "${native}/sce_module_writer.cpp" VERBATIM)

	set(pie "${out}/llvm-pie.elf")
	add_custom_command(OUTPUT "${pie}"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${boilerplate}/tooling/native/ps5-pie.ld"
			"${out}/link/tooling/native/ps5-pie-base.ld"
		COMMAND "${sdk}/bin/prospero-lld" -T "${gl_source}/native-app/ps5-pie.ld" --eh-frame-hdr
			--wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size
			--version-script "${gl_source}/native-app/app-symbols.map" -e _start --gc-sections --build-id=sha1
			-L "${out}/stubs" -L "${sdk}/target/lib" -u ps5_agc_gate2_run ${shim_wraps} -o "${pie}"
			$<TARGET_OBJECTS:z3cloudplay_ps5_runtime> $<TARGET_OBJECTS:z3cloudplay_ps5_shims>
			$<TARGET_OBJECTS:z3cloudplay_ps5_app>
			--start-group "${gl_sdk}/lib/libPS5OpenGL.a" "${sdk}/target/lib/libunwind.a"
			"${sdk}/target/lib/libc++abi.a" "${sdk}/target/lib/libc++.a"
			"${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a" $<TARGET_FILE:z3cloudplay_hui> $<TARGET_FILE:z3cloudplay_homebrew_ui> $<TARGET_FILE:z3cloudplay_homebrew_model>
			$<TARGET_FILE:z3cloudplay_homebrew_qr> $<TARGET_FILE:z3cloudplay_homebrew_controller>
			$<TARGET_FILE:z3cloudplay_homebrew_artwork_cache> $<TARGET_FILE:z3cloudplay_homebrew_artwork> $<TARGET_FILE:z3cloudplay_persistence_controller> $<TARGET_FILE:z3cloudplay_persistence_service> $<TARGET_FILE:z3cloudplay_preferences> $<TARGET_FILE:z3cloudplay_library_controller> $<TARGET_FILE:z3cloudplay_library_service> $<TARGET_FILE:z3cloudplay_steam_library> $<TARGET_FILE:z3cloudplay_steam_cm> $<TARGET_FILE:z3cloudplay_steam_cm_codec> $<TARGET_FILE:z3cloudplay_depot_codec>
			$<TARGET_FILE:z3cloudplay_local_session_repository> $<TARGET_FILE:z3cloudplay_secure_session_repository> $<TARGET_FILE:z3cloudplay_secure_session_vault> $<TARGET_FILE:z3cloudplay_auth_diagnostics>
			$<TARGET_FILE:z3cloudplay_steam_http> $<TARGET_FILE:z3cloudplay_steam_protocol>
			$<TARGET_FILE:z3cloudplay_steam_auth>
			$<TARGET_FILE:z3cloudplay_steam_core> $<TARGET_FILE:z3cloudplay_diagnostic_log> $<TARGET_FILE:z3cloudplay_storage>
			$<TARGET_FILE:z3cloudplay_library> $<TARGET_FILE:z3cloudplay_native_access> $<TARGET_FILE:z3cloudplay_managed_directory> $<TARGET_FILE:z3cloudplay_runtime_steamworks> $<TARGET_FILE:z3cloudplay_runtime_artifact_hash> $<TARGET_FILE:z3cloudplay_runtime_settings_storage> $<TARGET_FILE:z3cloudplay_runtime_private_directories> $<TARGET_FILE:z3cloudplay_runtime_session_reconciler> $<TARGET_FILE:z3cloudplay_runtime_launch_service> $<TARGET_FILE:z3cloudplay_runtime_install_service> $<TARGET_FILE:z3cloudplay_runtime_installs> $<TARGET_FILE:z3cloudplay_game_paths>
			${Z3CLOUDPLAY_STEAM_LINK_ARCHIVES} ${title_database_archives}
			--end-group --as-needed ${link_stubs}
		WORKING_DIRECTORY "${out}/link"
		DEPENDS z3cloudplay_ps5_runtime z3cloudplay_ps5_shims z3cloudplay_ps5_app z3cloudplay_hui z3cloudplay_homebrew_ui z3cloudplay_homebrew_model ${title_database_targets}
			z3cloudplay_homebrew_qr z3cloudplay_homebrew_controller z3cloudplay_local_session_repository z3cloudplay_secure_session_repository z3cloudplay_secure_session_vault z3cloudplay_auth_diagnostics
			z3cloudplay_homebrew_artwork_cache z3cloudplay_homebrew_artwork z3cloudplay_persistence_controller z3cloudplay_persistence_service z3cloudplay_preferences z3cloudplay_library_controller z3cloudplay_library_service z3cloudplay_steam_library z3cloudplay_steam_cm z3cloudplay_steam_cm_codec z3cloudplay_depot_codec
			z3cloudplay_library z3cloudplay_steam_auth z3cloudplay_steam_http z3cloudplay_steam_protocol z3cloudplay_steam_core z3cloudplay_diagnostic_log z3cloudplay_storage z3cloudplay_native_access z3cloudplay_managed_directory z3cloudplay_runtime_steamworks z3cloudplay_runtime_artifact_hash z3cloudplay_runtime_settings_storage z3cloudplay_runtime_private_directories z3cloudplay_runtime_session_reconciler z3cloudplay_runtime_launch_service z3cloudplay_runtime_install_service z3cloudplay_runtime_installs z3cloudplay_game_paths
			$<TARGET_OBJECTS:z3cloudplay_ps5_runtime> $<TARGET_OBJECTS:z3cloudplay_ps5_shims>
			$<TARGET_OBJECTS:z3cloudplay_ps5_app>
			$<TARGET_FILE:z3cloudplay_steam_http> $<TARGET_FILE:z3cloudplay_steam_protocol>
			$<TARGET_FILE:z3cloudplay_steam_auth>
			$<TARGET_FILE:z3cloudplay_steam_core> $<TARGET_FILE:z3cloudplay_diagnostic_log> $<TARGET_FILE:z3cloudplay_storage>
			$<TARGET_FILE:z3cloudplay_library> $<TARGET_FILE:z3cloudplay_native_access> $<TARGET_FILE:z3cloudplay_managed_directory> $<TARGET_FILE:z3cloudplay_runtime_steamworks> $<TARGET_FILE:z3cloudplay_runtime_artifact_hash> $<TARGET_FILE:z3cloudplay_runtime_settings_storage> $<TARGET_FILE:z3cloudplay_runtime_private_directories> $<TARGET_FILE:z3cloudplay_runtime_session_reconciler> $<TARGET_FILE:z3cloudplay_runtime_launch_service> $<TARGET_FILE:z3cloudplay_runtime_install_service> $<TARGET_FILE:z3cloudplay_runtime_installs> $<TARGET_FILE:z3cloudplay_game_paths>
			$<TARGET_FILE:z3cloudplay_homebrew_qr> $<TARGET_FILE:z3cloudplay_homebrew_controller>
			$<TARGET_FILE:z3cloudplay_homebrew_artwork_cache> $<TARGET_FILE:z3cloudplay_homebrew_artwork> $<TARGET_FILE:z3cloudplay_persistence_controller> $<TARGET_FILE:z3cloudplay_persistence_service> $<TARGET_FILE:z3cloudplay_preferences> $<TARGET_FILE:z3cloudplay_library_controller> $<TARGET_FILE:z3cloudplay_library_service> $<TARGET_FILE:z3cloudplay_steam_library> $<TARGET_FILE:z3cloudplay_steam_cm> $<TARGET_FILE:z3cloudplay_steam_cm_codec> $<TARGET_FILE:z3cloudplay_depot_codec>
			$<TARGET_FILE:z3cloudplay_local_session_repository> $<TARGET_FILE:z3cloudplay_auth_diagnostics>
			"${gl_source}/native-app/ps5-pie.ld" "${gl_source}/native-app/app-symbols.map"
			"${gl_sdk}/lib/libPS5OpenGL.a" "${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a"
			${Z3CLOUDPLAY_STEAM_LINK_ARCHIVES}
		COMMAND_EXPAND_LISTS VERBATIM)

	set(stamp "${app}/.packaged")
	file(GLOB_RECURSE package_inputs CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/*"
		"${CMAKE_SOURCE_DIR}/build/homebrew-fonts/*"
		"${CMAKE_SOURCE_DIR}/runtime/*")
	set(simulation_package_commands)
	set(native_helper_package_commands)
	set(Z3CLOUDPLAY_LAPY_HELPER_ELF "" CACHE FILEPATH "Explicit locally built title-scoped Lapy helper")
	set(Z3CLOUDPLAY_LAPY_HELPER_MANIFEST "" CACHE FILEPATH "Manifest for the exact Lapy helper")
	if((Z3CLOUDPLAY_LAPY_HELPER_MANIFEST AND NOT Z3CLOUDPLAY_LAPY_HELPER_ELF) OR
		(Z3CLOUDPLAY_LAPY_HELPER_ELF AND NOT Z3CLOUDPLAY_LAPY_HELPER_MANIFEST) OR
		(Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS AND NOT Z3CLOUDPLAY_LAPY_HELPER_ELF))
		message(FATAL_ERROR "Native runtime packaging requires both the exact local Lapy helper and its manifest; launcher-only builds must explicitly set Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS=OFF")
	endif()
	if(Z3CLOUDPLAY_LAPY_HELPER_ELF)
		if(NOT EXISTS "${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST}" OR IS_SYMLINK "${Z3CLOUDPLAY_LAPY_HELPER_ELF}"
			OR IS_DIRECTORY "${Z3CLOUDPLAY_LAPY_HELPER_ELF}")
			message(FATAL_ERROR "An explicit regular Lapy helper and manifest are required")
		endif()
		string(JSON expected_helper_sha ERROR_VARIABLE helper_pin_error GET "${dependency_lock}"
			prospero_launcher_helper variants "${Z3CLOUDPLAY_PS5_TITLE_ID}")
		if(helper_pin_error)
			message(FATAL_ERROR "No reviewed Lapy helper pin for ${Z3CLOUDPLAY_PS5_TITLE_ID}")
		endif()
		file(SHA256 "${Z3CLOUDPLAY_LAPY_HELPER_ELF}" actual_helper_sha)
		file(READ "${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST}" helper_manifest)
		string(JSON helper_title GET "${helper_manifest}" target_title)
		string(JSON helper_manifest_sha GET "${helper_manifest}" elf_sha256)
		string(JSON helper_protocol_sha GET "${helper_manifest}" protocol_sha256)
		string(JSON expected_protocol_sha GET "${dependency_lock}" prospero_launcher_helper protocol_sha256)
		if(NOT actual_helper_sha STREQUAL expected_helper_sha OR
			NOT helper_manifest_sha STREQUAL expected_helper_sha OR
			NOT helper_title STREQUAL Z3CLOUDPLAY_PS5_TITLE_ID OR
			NOT helper_protocol_sha STREQUAL expected_protocol_sha)
			message(FATAL_ERROR "Lapy helper title, artifact or protocol pin mismatch")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			"${Z3CLOUDPLAY_LAPY_HELPER_ELF}" "${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST}")
		list(APPEND package_inputs "${Z3CLOUDPLAY_LAPY_HELPER_ELF}" "${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST}")
		set(native_helper_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${Z3CLOUDPLAY_LAPY_HELPER_ELF}" "${app}/lapy.elf"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST}" "${app}/lapy-manifest.json")
	endif()
	# The one-shot title remover payload (tools/title-remover), pinned like the Lapy helper.
	set(Z3CLOUDPLAY_TITLE_REMOVER_ELF "" CACHE FILEPATH "Title remover payload built by scripts/build-title-remover.sh")
	if(Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS)
		if(NOT Z3CLOUDPLAY_TITLE_REMOVER_ELF OR NOT EXISTS "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}" OR IS_SYMLINK "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}")
			message(FATAL_ERROR "Native runtime packaging requires the title remover: run scripts/build-title-remover.sh")
		endif()
		string(JSON expected_remover_sha GET "${dependency_lock}" title_remover sha256)
		file(SHA256 "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}" actual_remover_sha)
		if(NOT actual_remover_sha STREQUAL expected_remover_sha)
            if(Z3CLOUDPLAY_EXPERIMENTAL_ARCHIVE_REBUILD)
                message(WARNING "EXPERIMENTAL TEST ONLY: locally compiled title remover digest differs from pinned artifact")
            else()
                message(FATAL_ERROR "Title remover pin mismatch: ${actual_remover_sha}")
            endif()
        endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}")
		list(APPEND package_inputs "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}")
		list(APPEND native_helper_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${Z3CLOUDPLAY_TITLE_REMOVER_ELF}" "${app}/title-remover.elf")
	endif()
	set(z3cloudplay_title_name "Z3CloudPlay")
	if(Z3CLOUDPLAY_UI_SIMULATION)
		set(z3cloudplay_title_name "Z3CloudPlay UI Simulation")
		file(GLOB_RECURSE simulation_inputs CONFIGURE_DEPENDS
			"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/*"
			"${CMAKE_SOURCE_DIR}/fixtures/library.json")
		list(APPEND package_inputs ${simulation_inputs})
		set(simulation_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison" "${app}/fixtures/ui-comparison"
			COMMAND "${CMAKE_COMMAND}" -E copy
				"${CMAKE_SOURCE_DIR}/fixtures/library.json" "${app}/fixtures/library.json")
	endif()
	add_custom_command(OUTPUT "${stamp}"
		COMMAND "${host_tool}" link --in "${pie}" --out "${out}/eboot.elf" --stub-dir "${out}/stubs"
			--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
		COMMAND "${CMAKE_COMMAND}" -DAPP_DIR=${app} -DBOILERPLATE=${boilerplate}
			-DASSETS_DIR=${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets -DFONTS_DIR=${CMAKE_SOURCE_DIR}/build/homebrew-fonts
			-DPARAM_SOURCE=${gl_source}/native-app/param.json -DPARAM_DEST=${app}/sce_sys/param.json
			-DTITLE_ID=${Z3CLOUDPLAY_PS5_TITLE_ID} "-DTITLE_NAME=${z3cloudplay_title_name}" -DAPP_VERSION=${PROJECT_VERSION}
			-P "${CMAKE_SOURCE_DIR}/cmake/PackagePS5.cmake"
		${simulation_package_commands}
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/runtime" "${app}/runtime"
		${native_helper_package_commands}
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${app}/trust"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${Z3CLOUDPLAY_STEAM_CA_BUNDLE}" "${app}/trust/cacert.pem"
		COMMAND "${host_tool}" self --sign --in "${out}/eboot.elf" --out "${app}/eboot.bin" --magic 0x1D3D154F
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${libc_prx}" "${app}/sce_module/libc.prx"
		COMMAND "${CMAKE_COMMAND}" -DAPP_DIR=${app} -DTITLE_ID=${Z3CLOUDPLAY_PS5_TITLE_ID}
			-P "${CMAKE_SOURCE_DIR}/cmake/WriteNativePackageIdentity.cmake"
		COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
		DEPENDS "${pie}" "${host_tool}" "${libc_prx}" "${CMAKE_SOURCE_DIR}/cmake/PackagePS5.cmake"
			"${CMAKE_SOURCE_DIR}/cmake/WriteNativePackageIdentity.cmake"
			"${CMAKE_SOURCE_DIR}/sce_sys/icon0.png" "${CMAKE_SOURCE_DIR}/assets/branding/generic-title-icon.png"
			"${CMAKE_SOURCE_DIR}/sce_sys/pic0.dds" "${CMAKE_SOURCE_DIR}/sce_sys/pic1.dds"
			"${Z3CLOUDPLAY_STEAM_CA_BUNDLE}" ${package_inputs}
		VERBATIM)
	add_custom_target(z3cloudplay-homebrew-ps5-package ALL DEPENDS "${stamp}")
endfunction()
