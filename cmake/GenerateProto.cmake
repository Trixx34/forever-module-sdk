# SKELETON ONLY. Wires protoc codegen for the SDK's .proto sources into the generated/ directory
# under the build tree. See doc/module-system/11-sdk-scope.md ("Generated protobuf C++ code:
# generated at SDK build time, not committed").
#
# TODO: pin an exact minimum protoc/Protobuf version once that's decided during implementation
# (deferred in doc/module-system/11-sdk-scope.md "Open items").

function(forever_module_generate_proto)
    set(options "")
    set(oneValueArgs OUT_SOURCES_VAR OUT_HEADERS_VAR)
    set(multiValueArgs PROTO_FILES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # protoc writes <cpp_out>/<file>.pb.{h,cc} relative to the -I root, so cpp_out is the directory
    # the generated files land in directly. Headers are included as "forever_module/*.pb.h" via the
    # generated/ include root that CMakeLists.txt exposes, so the files must sit under generated/forever_module/.
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/forever_module")
    file(MAKE_DIRECTORY "${generated_dir}")

    set(sources "")
    set(headers "")

    foreach(proto_file ${ARG_PROTO_FILES})
        get_filename_component(proto_name ${proto_file} NAME_WE)
        set(gen_src "${generated_dir}/${proto_name}.pb.cc")
        set(gen_hdr "${generated_dir}/${proto_name}.pb.h")

        add_custom_command(
            OUTPUT "${gen_src}" "${gen_hdr}"
            COMMAND protobuf::protoc
                --cpp_out "${generated_dir}"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
                "${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}"
            COMMENT "Generating protobuf sources for ${proto_file}"
            VERBATIM
        )

        list(APPEND sources "${gen_src}")
        list(APPEND headers "${gen_hdr}")
    endforeach()

    set(${ARG_OUT_SOURCES_VAR} "${sources}" PARENT_SCOPE)
    set(${ARG_OUT_HEADERS_VAR} "${headers}" PARENT_SCOPE)
endfunction()
