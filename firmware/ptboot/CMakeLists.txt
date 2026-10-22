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

pico_set_linker_script(ptboot ${CMAKE_CURRENT_SOURCE_DIR}/memmap_ptboot.ld)

pico_enable_stdio_usb(ptboot 0)
pico_enable_stdio_uart(ptboot 0)

pico_add_extra_outputs(ptboot)
