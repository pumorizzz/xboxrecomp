cmake_minimum_required(VERSION 3.20)
project(usb_pad_record_regression C)
# The pad recorder (src/usb/usb_gamepad.c) with the window's keys, the flip
# count and the host pad stubbed: no window, no executor.
if(NOT WIN32)
    message(FATAL_ERROR "usb_pad_record: Windows only (temp paths, _putenv_s)")
endif()
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(usb-pad-record-test usb_pad_record_test.c ../../src/usb/usb_gamepad.c)
target_include_directories(usb-pad-record-test PRIVATE ../../src/usb ../../src/input ../../src)
target_compile_definitions(usb-pad-record-test PRIVATE _CRT_SECURE_NO_WARNINGS)
# One process per mode: RECOMP_PB_EXEC is read once.
add_test(NAME usb-pad-record-exec COMMAND usb-pad-record-test exec)
add_test(NAME usb-pad-record-no-exec COMMAND usb-pad-record-test no-exec)
