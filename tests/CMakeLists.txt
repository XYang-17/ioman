cmake_minimum_required(VERSION 3.16)
project(ioman_tests LANGUAGES CXX)

get_filename_component(ioman_source_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${ioman_source_root}/cmake/iomanHeaders.cmake")
ioman_prepare_headers("${ioman_source_root}" ioman_build_include)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
set(IOMAN_TEST_MODULES "" CACHE STRING "Semicolon-separated directories under tests/")
set(IOMAN_TESTS "" CACHE STRING "Semicolon-separated test names or paths under tests/")

if(BUILD_TESTING)
    set(test_root "${CMAKE_CURRENT_SOURCE_DIR}")
    set(test_sources)

    foreach(module IN LISTS IOMAN_TEST_MODULES)
        if("${module}" STREQUAL "obj"
           OR "${module}" STREQUAL "report"
           OR NOT "${module}" MATCHES "^[A-Za-z0-9_-]+$"
           OR NOT IS_DIRECTORY "${test_root}/${module}")
            message(FATAL_ERROR "Unknown test module '${module}' under tests/.")
        endif()

        file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS RELATIVE "${test_root}"
            "${test_root}/${module}/*.cpp")
        list(APPEND test_sources ${module_sources})
    endforeach()

    if(NOT "" STREQUAL "${IOMAN_TESTS}" OR "" STREQUAL "${IOMAN_TEST_MODULES}")
        file(GLOB_RECURSE discovered_tests CONFIGURE_DEPENDS RELATIVE "${test_root}"
            "${test_root}/*.cpp")
        list(FILTER discovered_tests EXCLUDE REGEX "^(obj|report)/")
    endif()

    if("" STREQUAL "${IOMAN_TEST_MODULES}" AND "" STREQUAL "${IOMAN_TESTS}")
        set(test_sources ${discovered_tests})
    endif()

    foreach(requested_test IN LISTS IOMAN_TESTS)
        string(REGEX REPLACE "^tests/" "" requested_path "${requested_test}")
        set(test_matches)
        foreach(source IN LISTS discovered_tests)
            get_filename_component(filename "${source}" NAME)
            get_filename_component(stem "${source}" NAME_WLE)
            if("${requested_path}" STREQUAL "${source}"
               OR "${requested_test}" STREQUAL "${filename}"
               OR "${requested_test}" STREQUAL "${stem}")
                list(APPEND test_matches "${source}")
            endif()
        endforeach()

        list(LENGTH test_matches match_count)
        if(0 EQUAL match_count)
            message(FATAL_ERROR "Unknown test file '${requested_test}' under tests/.")
        elseif(1 LESS match_count)
            message(FATAL_ERROR
                "Test file '${requested_test}' is ambiguous. Use a path relative to tests/. Matches: ${test_matches}")
        endif()
        list(APPEND test_sources ${test_matches})
    endforeach()

    list(REMOVE_DUPLICATES test_sources)
    list(SORT test_sources)
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #include <ucontext.h>

    int main() {
        ucontext_t caller{};
        ucontext_t context{};
        if (0 != getcontext(&context)) {
            return 1;
        }
        makecontext(&context, +[]() {}, 0);
        if (0 != swapcontext(&caller, &context)) {
            return 1;
        }
        return setcontext(&caller);
    }
]=] IOMAN_HAS_UCONTEXT)
if(NOT IOMAN_HAS_UCONTEXT)
    message(FATAL_ERROR
        "The ioman Context backend requires ucontext.h and linkable getcontext, "
        "makecontext, swapcontext, and setcontext functions. "
        "Use a toolchain with ucontext support, such as GCC on Linux or WSL.")
endif()

# Keep component dependencies explicit. Each component has one matching implementation.
set(ioman_components context coroutine scheduler thread_pool ioman)
set(library_build_mode)
if(BUILD_TESTING AND
   (NOT "" STREQUAL "${IOMAN_TEST_MODULES}" OR NOT "" STREQUAL "${IOMAN_TESTS}"))
    set(library_build_mode EXCLUDE_FROM_ALL)
endif()

foreach(component IN LISTS ioman_components)
    file(GLOB_RECURSE component_sources CONFIGURE_DEPENDS
        "${ioman_source_root}/src/${component}.cpp")
    list(LENGTH component_sources component_source_count)
    if(NOT 1 EQUAL component_source_count)
        message(FATAL_ERROR "Expected one implementation for component '${component}' under src/.")
    endif()
    add_library(ioman_${component} STATIC ${library_build_mode} ${component_sources})
    target_include_directories(ioman_${component}
        PUBLIC "${ioman_build_include}"
        PRIVATE "${ioman_source_root}/include")
    target_compile_features(ioman_${component} PUBLIC cxx_std_17)
endforeach()
target_sources(ioman_context PRIVATE "${ioman_source_root}/src/context/context_backend.cpp")
target_link_libraries(ioman_coroutine PUBLIC ioman_context)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_sources(ioman_scheduler PRIVATE "${ioman_source_root}/src/task_handler.cpp" "${ioman_source_root}/src/cancellation.cpp")
target_link_libraries(ioman_scheduler PUBLIC ioman_coroutine Threads::Threads)
target_link_libraries(ioman_thread_pool PUBLIC ioman_scheduler Threads::Threads)
file(GLOB sync_sources CONFIGURE_DEPENDS "${ioman_source_root}/src/sync/*.cpp")
add_library(ioman_sync STATIC ${library_build_mode} ${sync_sources})
target_include_directories(ioman_sync PRIVATE "${ioman_source_root}/include")
target_link_libraries(ioman_sync PUBLIC ioman_scheduler)
target_link_libraries(ioman_ioman PUBLIC ioman_thread_pool ioman_sync)
add_library(ioman ALIAS ioman_ioman)

# The sync suite exercises cancellation through the public runtime.
set(ioman_test_extra_libraries_sync ioman_ioman)

if(BUILD_TESTING AND test_sources)
    foreach(source IN LISTS test_sources)
        get_filename_component(test_name "${source}" NAME_WLE)
        set(test_target "${test_name}")
        if(TARGET "${test_target}")
            string(SHA256 path_hash "${source}")
            set(test_target "${test_name}_${path_hash}")
        endif()

        # A component test links its own library. Other tests can use both.
        string(REGEX REPLACE "^test_" "" component "${test_name}")
        set(test_libraries ioman_ioman)
        if(TARGET "ioman_${component}")
            set(test_libraries "ioman_${component}")
        endif()

        add_executable("${test_target}" "${test_root}/${source}")
        get_filename_component(test_relative_directory "${source}" DIRECTORY)
        set(test_directory "${test_root}/obj/${test_relative_directory}")
        set_target_properties("${test_target}" PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${test_directory}")
        foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${configuration}" configuration_upper)
            set_target_properties("${test_target}" PROPERTIES
                "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${test_directory}")
        endforeach()
        target_include_directories("${test_target}" PRIVATE "${test_root}/support")
        target_compile_definitions("${test_target}" PRIVATE
            IOMAN_TEST_REPORT_FILE="${test_root}/report/${test_relative_directory}/${test_target}.report.txt")
        target_link_libraries("${test_target}" PRIVATE
            ${test_libraries}
            ${ioman_test_extra_libraries_${component}}
            Threads::Threads
        )
        add_test(NAME "${test_target}" COMMAND "${test_target}")
        set_tests_properties("${test_target}" PROPERTIES
            WORKING_DIRECTORY "${test_directory}" TIMEOUT 60)

        if("${source}" MATCHES "^([^/]+)/")
            set_tests_properties("${test_target}" PROPERTIES LABELS "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()
