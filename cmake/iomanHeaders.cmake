function(ioman_prepare_headers source_root output_variable)
    set(build_include "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE "${source_root}/include"
        "${source_root}/include/*.h")
    foreach(header IN LISTS headers)
        configure_file("${source_root}/include/${header}"
            "${build_include}/ioman/${header}" COPYONLY)
    endforeach()
    set("${output_variable}" "${build_include}" PARENT_SCOPE)
endfunction()
