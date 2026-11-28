# Board-independent BSP contract. Each board implements it in boards/<id>/board/.
# bsp_io_weak.c: defaults for the optional expansion-header calls (boards override them).
idf_component_register(SRCS "bsp_io_weak.c" INCLUDE_DIRS "include" REQUIRES esp_common)
