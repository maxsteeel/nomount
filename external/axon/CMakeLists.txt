cmake_minimum_required(VERSION 3.22.1)
include(ndk.cmake)
project(axon C)

# LTO
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_supported_output)
if (ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else ()
    message(WARNING "IPO is not supported: ${ipo_supported_output}")
endif ()

# Global options
set(CMAKE_C_VISIBILITY_PRESET hidden)
add_compile_options($<$<CONFIG:RELEASE>:-Os>)
add_link_options($<$<CONFIG:RELEASE>:-s>)

# plthook
set(PLTHOOK_PATH external/plthook)
set(PLTHOOK_SRC ${PLTHOOK_PATH}/plthook_elf.c)
add_library(plthook STATIC ${PLTHOOK_SRC})
target_include_directories(plthook SYSTEM PUBLIC ${PLTHOOK_PATH})

# axon
add_executable(inject inject.c ptrace.c utils.c)
target_link_libraries(inject)

add_library(axon_init SHARED axon_init.c utils.c)
target_link_libraries(axon_init plthook)

add_library(axon_adbd SHARED axon_adbd.c utils.c)
target_link_libraries(axon_adbd)
