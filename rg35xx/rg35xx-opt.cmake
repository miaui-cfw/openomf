# Included after project() (CMAKE_PROJECT_INCLUDE): the optimisation level comes after upstream's "-O2" for Release
# on the compiler's command line, so it is the one that counts.
if(RG35XX_OPT)
    add_compile_options(${RG35XX_OPT})
endif()
