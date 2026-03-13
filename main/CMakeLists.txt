idf_component_register(
    SRCS "app_main.c"
    REQUIRES apple2 cardputer esp_timer fatfs sdmmc esp_driver_sdspi
)

set(M5APPLE2_GENERATED_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_assets")
file(MAKE_DIRECTORY "${M5APPLE2_GENERATED_ASSET_DIR}")

# Re-run CMake when user-supplied ROM or disk images are added or removed.
file(GLOB M5APPLE2_EMBED_WATCH CONFIGURE_DEPENDS
    "${PROJECT_DIR}/roms/*.rom"
    "${PROJECT_DIR}/roms/*.do"
    "${PROJECT_DIR}/roms/*.po"
    "${PROJECT_DIR}/roms/*.dsk")

function(m5apple2_add_embedded_rom source_path generated_name define_name)
    set(source_abs "${PROJECT_DIR}/${source_path}")
    set(embed_rel "generated_assets/${generated_name}")
    set(embed_abs "${M5APPLE2_GENERATED_ASSET_DIR}/${generated_name}")

    if(EXISTS "${source_abs}")
        file(COPY_FILE "${source_abs}" "${embed_abs}" ONLY_IF_DIFFERENT)
        target_add_binary_data(${COMPONENT_LIB} "${embed_rel}" BINARY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${define_name}=1")
    else()
        file(REMOVE "${embed_abs}")
    endif()
endfunction()

m5apple2_add_embedded_rom("roms/apple2plus.rom" "apple2plus.rom" "M5APPLE2_HAS_APPLE2PLUS_ROM")
m5apple2_add_embedded_rom("roms/disk2.rom" "disk2.rom" "M5APPLE2_HAS_DISK2_ROM")
m5apple2_add_embedded_rom("roms/dos_3.3.do" "dos_3.3.do" "M5APPLE2_HAS_DOS33_DO")
m5apple2_add_embedded_rom("roms/dos_3.3.po" "dos_3.3.po" "M5APPLE2_HAS_DOS33_PO")
m5apple2_add_embedded_rom("roms/dos_3.3.dsk" "dos_3.3.dsk" "M5APPLE2_HAS_DOS33_DSK")
