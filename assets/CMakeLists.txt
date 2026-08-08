# Linker options for Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_options(-fPIC)
endif()

# Only the built-in cartridge collection survives in the CLI build; every UI
# image and font was dropped along with the editor.
juce_add_binary_data(DexedResources SOURCES
    builtin_pgm.zip
)
