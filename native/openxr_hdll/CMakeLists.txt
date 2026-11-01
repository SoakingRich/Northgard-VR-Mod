cmake_minimum_required(VERSION 3.20)
project(openxr_hdll C)

# HashLink native libs are plain DLLs with the .hdll extension.
add_library(openxr SHARED src/openxr_hdll.c src/xr_theater.c src/xr_uiwin.c)
set_target_properties(openxr PROPERTIES SUFFIX ".hdll" PREFIX "")
# OpenXR headers only: the runtime is loaded directly (no loader DLL needed).
target_include_directories(openxr PRIVATE third_party/hashlink third_party/openxr)
target_compile_definitions(openxr PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(openxr PRIVATE advapi32 user32 gdi32 uuid)
