# "main" depends on every component in the project, including softuart.
idf_component_register(SRCS "softuart_example_main.c"
                       INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
if(DEFINED ENV{SOFTUART_WERROR})
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
endif()
