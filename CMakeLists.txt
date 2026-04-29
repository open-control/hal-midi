cmake_minimum_required(VERSION 3.29)

project(open_control_hal_midi LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(OC_HAL_MIDI_BUILD_TESTS_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL)
    set(OC_HAL_MIDI_BUILD_TESTS_DEFAULT ON)
endif()

option(
    OC_HAL_MIDI_BUILD_TESTS
    "Build and register OpenControl HAL MIDI unit tests"
    ${OC_HAL_MIDI_BUILD_TESTS_DEFAULT})

if(OC_HAL_MIDI_BUILD_TESTS AND BUILD_TESTING)
    file(GLOB OC_HAL_MIDI_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_*.cpp")

    set(OC_HAL_MIDI_TEST_TARGETS)

    foreach(test_source IN LISTS OC_HAL_MIDI_TESTS)
        get_filename_component(test_name "${test_source}" NAME_WE)
        list(APPEND OC_HAL_MIDI_TEST_TARGETS "${test_name}")

        add_executable("${test_name}" "${test_source}")
        target_include_directories("${test_name}"
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src")

        add_test(NAME "${test_name}" COMMAND "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES LABELS open-control-hal-midi)
    endforeach()

    if(PROJECT_IS_TOP_LEVEL)
        add_custom_target(test-native
            COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}" --parallel --output-on-failure
            DEPENDS ${OC_HAL_MIDI_TEST_TARGETS}
            USES_TERMINAL)
    endif()
endif()
