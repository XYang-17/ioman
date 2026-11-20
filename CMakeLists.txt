cmake_minimum_required(VERSION 3.16)
project(ioman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build ioman as a shared library" OFF)
set(IOMAN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib" CACHE PATH
    "Directory for built ioman libraries (relative paths use the build directory)")
get_filename_component(ioman_output_directory "${IOMAN_OUTPUT_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #include <ucontext.h>
    int main() {
        ucontext_t caller{}, context{};
        if (getcontext(&context) != 0) return 1;
        makecontext(&context, +[]() {}, 0);
        if (swapcontext(&caller, &context) != 0) return 1;
        return setcontext(&caller);
    }
]=] IOMAN_HAS_UCONTEXT)
if(NOT IOMAN_HAS_UCONTEXT)
    message(FATAL_ERROR "ioman requires a toolchain with ucontext support, such as GCC on Linux or WSL.")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(ioman
    src/context/context.cpp
    src/context/context_backend.cpp
    src/coroutine.cpp
    src/task_handler.cpp
    src/cancellation.cpp
    src/scheduler.cpp
    src/thread_pool.cpp
    src/ioman.cpp
    src/sync/mutex.cpp
    src/sync/condition_variable.cpp
    src/sync/wait_group.cpp
)
add_library(ioman::ioman ALIAS ioman)
target_compile_features(ioman PUBLIC cxx_std_17)
set_target_properties(ioman PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${ioman_output_directory}"
    LIBRARY_OUTPUT_DIRECTORY "${ioman_output_directory}"
    RUNTIME_OUTPUT_DIRECTORY "${ioman_output_directory}"
)
foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${configuration}" configuration_upper)
    set_target_properties(ioman PROPERTIES
        "ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper}" "${ioman_output_directory}"
        "LIBRARY_OUTPUT_DIRECTORY_${configuration_upper}" "${ioman_output_directory}"
        "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${ioman_output_directory}"
    )
endforeach()

include(GNUInstallDirs)
include(cmake/iomanHeaders.cmake)
ioman_prepare_headers("${CMAKE_CURRENT_SOURCE_DIR}" ioman_build_include)
target_include_directories(ioman PUBLIC
    "$<BUILD_INTERFACE:${ioman_build_include}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(ioman PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ioman PUBLIC Threads::Threads)

install(TARGETS ioman EXPORT iomanTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ioman")
install(EXPORT iomanTargets NAMESPACE ioman::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ioman")

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/iomanConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iomanConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ioman")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iomanConfig.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ioman")
