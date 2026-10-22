set(TARGET_NAME picotunewh)

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPICO_STDIO_DEFAULT_CRLF=0")

#add_definitions(-DPICO_STDIO_DEFAULT_CRLF=0)

add_executable(${TARGET_NAME}
        ${TARGET_NAME}.c
		nim.c
		rpi2c.c
		stv0910.c
		stv0910_utils.c
		stv6120.c
		stv6120_utils.c
		stvvglna.c
		stvvglna_utils.c    
		ota.c
        )

# key for updates over Ethernet: cmake -DPTUP_KEY=...
if(DEFINED PTUP_KEY)
	target_compile_definitions(${TARGET_NAME} PRIVATE PTUP_KEY="${PTUP_KEY}")
endif()

pico_set_linker_script(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_custom-2MB.ld)

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/${TARGET_NAME}.pio)

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_exception
        hardware_flash
#        pico_flash
        pico_multicore
        hardware_irq
        ETHERNET_FILES
        IOLIBRARY_FILES
        SNTP_FILES
        TIMER_FILES
        DHCP_FILES
        DNS_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})

