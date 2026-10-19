cmake_minimum_required(VERSION 3.12)
project(aphelion LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(APHELION_SOURCES
    src/ast.c
    src/bytecode.c
    src/compiler.c
    src/file.c
    src/lexer.c
    src/main.c
    src/module.c
    src/parser.c
    src/semantic.c
    src/token.c
    src/value.c
    src/vm.c
)

add_executable(aphelion ${APHELION_SOURCES})
target_compile_features(aphelion PRIVATE c_std_11)
target_include_directories(aphelion PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
set_target_properties(aphelion PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(aphelion PRIVATE /W4 /WX)
else()
    target_compile_options(aphelion PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

if(WIN32)
    target_link_libraries(aphelion PRIVATE shell32)
else()
    target_link_libraries(aphelion PRIVATE m)
endif()

option(APHELION_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(APHELION_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
            "APHELION_ENABLE_SANITIZERS requires a GCC or Clang toolchain "
            "with sanitizer runtime libraries (compiler: ${CMAKE_C_COMPILER_ID})"
        )
    endif()

    message(STATUS
        "Sanitizers enabled; the build will fail if the compiler's "
        "AddressSanitizer/UBSan runtime libraries are unavailable."
    )
    target_compile_options(aphelion PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
    )
    if(CMAKE_VERSION VERSION_LESS 3.13)
        # target_link_options() was introduced in CMake 3.13.
        set_property(TARGET aphelion APPEND_STRING PROPERTY
                     LINK_FLAGS " -fsanitize=address,undefined")
    else()
        target_link_options(aphelion PRIVATE -fsanitize=address,undefined)
    endif()
    add_custom_target(sanitize DEPENDS aphelion)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pytest"
        RESULT_VARIABLE APHELION_PYTEST_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT APHELION_PYTEST_RESULT STREQUAL "0")
        message(FATAL_ERROR
            "BUILD_TESTING=ON requires pytest in ${Python3_EXECUTABLE}. "
            "Install it with that interpreter's -m pip install pytest, or "
            "configure with -DBUILD_TESTING=OFF for a compiler-only build."
        )
    endif()

    # Quote the compiler path for the Python C-driver helper and use forward
    # slashes so both native Windows and POSIX argument parsing preserve it.
    file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" APHELION_TEST_COMPILER)
    set(APHELION_TEST_CC "\"${APHELION_TEST_COMPILER}\"")
    if(CMAKE_C_COMPILER_ARG1)
        string(APPEND APHELION_TEST_CC " ${CMAKE_C_COMPILER_ARG1}")
    endif()
    set(APHELION_TEST_ENVIRONMENT
        "APHELION_TEST_EXECUTABLE=$<TARGET_FILE:aphelion>"
        "CC=${APHELION_TEST_CC}"
    )
    if(APHELION_ENABLE_SANITIZERS)
        list(APPEND APHELION_TEST_ENVIRONMENT
            "APHELION_TEST_CFLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g"
            "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1"
            "UBSAN_OPTIONS=halt_on_error=1"
        )
    endif()
    add_test(
        NAME aphelion_pytest
        COMMAND "${CMAKE_COMMAND}" -E env ${APHELION_TEST_ENVIRONMENT}
                "${Python3_EXECUTABLE}" -m pytest -q
    )
    set_tests_properties(aphelion_pytest PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
