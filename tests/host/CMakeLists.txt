cmake_minimum_required(VERSION 3.16)
project(charmera_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED ENV{IDF_PATH})
    set(IDF_DIR $ENV{IDF_PATH})
else()
    set(IDF_DIR $ENV{HOME}/esp/esp-idf)
endif()
set(UNITY_DIR ${IDF_DIR}/components/unity/unity/src)
if(NOT EXISTS ${UNITY_DIR}/unity.c)
    message(FATAL_ERROR "Unity not found at ${UNITY_DIR}; set IDF_PATH")
endif()

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})

enable_testing()

# add_host_test(<name> <extra sources...>) builds test_<name>.c plus the given
# pure-C sources (no ESP-IDF includes) and registers it with CTest.
function(add_host_test name)
    add_executable(test_${name} test_${name}.c ${ARGN})
    target_include_directories(test_${name} PRIVATE
        ${REPO}/components/looks/src
        ${REPO}/components/storage/include
        ${REPO}/components/input/include
        ${REPO}/components/remote/include
        ${REPO}/components/pipeline/include
        ${REPO}/components/orient/include
        ${REPO}/components/looks/include
        ${REPO}/components/video/include)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_${name} PRIVATE unity m)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_host_test(smoke)
add_host_test(input_logic ${REPO}/components/input/src/input_logic.c)
add_host_test(storage_naming ${REPO}/components/storage/src/storage_naming.c)
add_host_test(looks ${REPO}/components/looks/src/looks.c ${REPO}/components/looks/src/frames.c ${REPO}/components/orient/orient.c)
add_host_test(power_logic ${REPO}/main/logic/power_logic.c)
target_include_directories(test_power_logic PRIVATE ${REPO}/main/logic)
add_host_test(lapse_logic ${REPO}/main/logic/lapse_logic.c)
target_include_directories(test_lapse_logic PRIVATE ${REPO}/main/logic)
add_host_test(remote_proto ${REPO}/components/remote/src/remote_proto.c ${REPO}/components/orient/orient.c)
add_host_test(exif ${REPO}/components/pipeline/src/exif.c)
add_host_test(orient ${REPO}/components/orient/orient.c)
add_host_test(preview_fit ${REPO}/main/logic/preview_fit.c)
target_include_directories(test_preview_fit PRIVATE ${REPO}/main/logic)
add_host_test(avi_writer ${REPO}/components/video/avi_writer.c)
add_host_test(fat_image ${REPO}/components/fat_image/fat_image.c)
target_include_directories(test_fat_image PRIVATE ${REPO}/components/fat_image/include)
add_host_test(fat_walk ${REPO}/components/fat_image/fat_walk.c)
target_include_directories(test_fat_walk PRIVATE ${REPO}/components/fat_image/include)
