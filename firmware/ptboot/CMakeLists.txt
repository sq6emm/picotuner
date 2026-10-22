# ptboot: bootloader for PicoTuneWH updates over Ethernet (see ../common/ptupdate.h)

add_executable(ptboot ptboot.c)

target_link_libraries(ptboot PRIVATE
        pico_stdlib
        hardware_flash
        hardware_irq
        )

if(PTBOOT_DEBUG_LED)
	target_compile_definitions(ptboot PRIVATE PTBOOT_DEBUG_LED=1)
endif()

if(PTBOOT_START_DELAY_MS)
	target_compile_definitions(ptboot PRIVATE PTBOOT_START_DELAY_MS=${PTBOOT_START_DELAY_MS})
endif()

# The W5100S-EVB-Pico tested here only starts from power-on with a longer crystal start-up
# (with 1, G4EWJ's own 0v3e stays dark too; warm resets always worked)
target_compile_definitions(ptboot PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

pico_set_linker_script(ptboot ${CMAKE_CURRENT_SOURCE_DIR}/memmap_ptboot.ld)

pico_enable_stdio_usb(ptboot 0)
pico_enable_stdio_uart(ptboot 0)

pico_add_extra_outputs(ptboot)
