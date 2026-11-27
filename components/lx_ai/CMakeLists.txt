idf_component_register(
    SRCS "lx_ai.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer heap
)
# esp-dl: compile only the ops listed in lx_ai.compile_req.yml (the whole library is ~1.9 MB)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST build_components)
    idf_component_get_property(_dl espressif__esp-dl COMPONENT_DIR)
elseif("esp-dl" IN_LIST build_components)
    idf_component_get_property(_dl esp-dl COMPONENT_DIR)
endif()
if(_dl)
    include(${_dl}/cmake/compile_register.cmake)
    esp_dl_compile_add_yml(${CMAKE_CURRENT_LIST_DIR}/lx_ai.compile_req.yml)
endif()
