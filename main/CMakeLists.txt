idf_component_register(
    SRCS "app_main.c"
    REQUIRES apple2 cardputer esp_timer
)

set(M5APPLE2_GENERATED_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_assets")
file(MAKE_DIRECTORY "${M5APPLE2_GENERATED_ASSET_DIR}")

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
m5apple2_add_embedded_rom("roms/dos_3.3.dsk" "dos_3.3.dsk" "M5APPLE2_HAS_DOS33_DSK")
