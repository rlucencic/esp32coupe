idf_component_register(SRCS "coupe.c" "floppy.c" "sound.c"
                       INCLUDE_DIRS "include"
                       REQUIRES z80
                       PRIV_REQUIRES freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
