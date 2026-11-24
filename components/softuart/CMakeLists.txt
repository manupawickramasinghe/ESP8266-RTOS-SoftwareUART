idf_component_register(SRCS "softuart.c"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
# CI builds with SOFTUART_WERROR=1 so new warnings fail the build
if(DEFINED ENV{SOFTUART_WERROR})
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
endif()
