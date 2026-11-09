cmake_minimum_required(VERSION 3.20)
project(fb_present_fit_regression C)
enable_testing()
# Windows only: the display aspect and size live in the GDI window
# (src/video/fb_present.c); the SDL window shows the frame at its own size.
add_executable(fb-present-fit-test fb_present_fit_test.c ../../src/video/fb_present.c)
target_include_directories(fb-present-fit-test PRIVATE ../../src/input ../../src)
target_compile_definitions(fb-present-fit-test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(fb-present-fit-test PRIVATE user32 gdi32)
add_test(NAME fb-present-fit COMMAND fb-present-fit-test)
