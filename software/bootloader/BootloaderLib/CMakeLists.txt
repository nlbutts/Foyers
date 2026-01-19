add_library(bootloader_lib STATIC
    bootloader.c
)

target_include_directories(bootloader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# We need to link against the main project includes (e.g. main.h) 
# or ensure they are available.
# Since this lib uses HAL, it needs HAL headers. 
# Usually 'stm32cubemx' target has them usage requirements.
target_link_libraries(bootloader_lib
    stm32cubemx
)
