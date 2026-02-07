add_library(cortos_config INTERFACE)

# Default configuration (used when not overridden with 'BEFORE')
target_include_directories(cortos_config INTERFACE
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

add_library(cortos::config ALIAS cortos_config)