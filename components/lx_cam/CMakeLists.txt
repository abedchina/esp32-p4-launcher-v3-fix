idf_component_register(
    SRCS "lx_cam.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb esp_driver_jpeg esp_timer heap
)
