idf_component_register(SRCS "main.c" "jarvis_crypto.c"
                       INCLUDE_DIRS "."
                       REQUIRES cc1101 mbedtls)
