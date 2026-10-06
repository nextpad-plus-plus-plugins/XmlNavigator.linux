cmake_minimum_required(VERSION 3.20)
project(XmlNavigator VERSION 1.0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

set(NPP_LINUX_DIR "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")

add_library(XmlNavigator SHARED src/XmlNavigator.cpp)
target_include_directories(XmlNavigator PRIVATE
    "${NPP_LINUX_DIR}/src" "${NPP_LINUX_DIR}/scintilla/include" src)
target_link_libraries(XmlNavigator PRIVATE PkgConfig::GTK4)
target_compile_options(XmlNavigator PRIVATE -Wno-deprecated-declarations)
set_target_properties(XmlNavigator PROPERTIES
    PREFIX "" SUFFIX ".so" OUTPUT_NAME "XmlNavigator")

set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/XmlNavigator")
install(TARGETS XmlNavigator DESTINATION "${PLUGIN_INSTALL_DIR}")
install(DIRECTORY resources/ DESTINATION "${PLUGIN_INSTALL_DIR}/resources")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/pkg/XmlNavigator")
add_custom_target(package_zip
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/pkg/XmlNavigator"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:XmlNavigator>"
            "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/pkg/XmlNavigator/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources"
            "${CMAKE_BINARY_DIR}/pkg/XmlNavigator/resources"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/XmlNavigator.zip"
            --format=zip XmlNavigator
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/pkg"
    DEPENDS XmlNavigator
    COMMENT "Packaging XmlNavigator.zip")
