idf_component_register(SRCS "usbdisk.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES board storage fat_image espressif__tinyusb usb)

# TinyUSB picks up its configuration from tusb_config.h on its include path;
# hand it ours (same trick esp_tinyusb uses for its own).
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
