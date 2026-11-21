cmake_minimum_required(VERSION 3.16)
project(wifi_popup_xlib C)

set(CMAKE_C_STANDARD 11)

# Forzar 32 bits
add_compile_options(-m32)
add_link_options(-m32)

# Enlazar explícitamente contra las bibliotecas de 32 bits
add_link_options(-L/usr/lib32)

# Buscar D-Bus (opcional, si lo usas)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(wifi-popup wifi_popup.c)

target_include_directories(wifi-popup PRIVATE
    /usr/include/X11
    ${DBUS_INCLUDE_DIRS}
)

# Enlazar X11 y D-Bus manualmente
target_link_libraries(wifi-popup PRIVATE
    X11
    SM
    ICE
    ${DBUS_LIBRARIES}
)