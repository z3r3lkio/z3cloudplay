if(NOT DEFINED HOST_CXX OR NOT DEFINED NATIVE_DIR OR NOT DEFINED HOST_DIR OR NOT DEFINED EXPECTED_LIBC_SHA256)
	message(FATAL_ERROR "HOST_CXX, NATIVE_DIR, HOST_DIR and EXPECTED_LIBC_SHA256 are required")
endif()
file(MAKE_DIRECTORY "${HOST_DIR}")
execute_process(COMMAND "${HOST_CXX}" -std=c++20 -O2 "${NATIVE_DIR}/libc_builder.cpp"
	-o "${HOST_DIR}/libc-builder" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${HOST_CXX}" -std=c++20 -O2
	"${NATIVE_DIR}/native_app_builder.cpp" "${NATIVE_DIR}/self_container.cpp"
	"${NATIVE_DIR}/elf_object.cpp" "${NATIVE_DIR}/sce_module_writer.cpp" -lz
	-o "${HOST_DIR}/ps5-native-tool" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${HOST_DIR}/libc-builder" "${NATIVE_DIR}/runtime/api-surface.txt"
	"${NATIVE_DIR}/runtime/imports.txt" "${HOST_DIR}/libc.raw.elf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${HOST_DIR}/ps5-native-tool" self --sign --in "${HOST_DIR}/libc.raw.elf"
	--out "${HOST_DIR}/libc.prx" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${HOST_DIR}/libc.prx" actual_libc_sha256)
if(NOT actual_libc_sha256 STREQUAL EXPECTED_LIBC_SHA256)
	message(FATAL_ERROR "Generated libc.prx hash mismatch: expected ${EXPECTED_LIBC_SHA256}, found ${actual_libc_sha256}")
endif()
