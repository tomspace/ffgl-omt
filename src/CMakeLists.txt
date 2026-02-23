cmake_minimum_required(VERSION 3.15)
project(ffgl-omt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(FFGL_ROOT "" CACHE PATH "Root of the resolume/ffgl repository")
set(LIBOMT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/libomt"
    CACHE PATH "Directory containing bin/ include/ lib/ from the libomt release")

if(NOT FFGL_ROOT)
    message(FATAL_ERROR "Please set FFGL_ROOT to the root of your resolume/ffgl clone")
endif()

# ---------------------------------------------------------------------------
# Collect FFGL SDK source files
# We compile these DIRECTLY into each plugin DLL (not as a static lib)
# so that __declspec(dllexport) on plugMain flows through correctly.
# ---------------------------------------------------------------------------
file(GLOB FFGL_LIB_SOURCES
    "${FFGL_ROOT}/source/lib/ffgl/*.cpp"
    "${FFGL_ROOT}/source/lib/ffglex/*.cpp"
)

# ---------------------------------------------------------------------------
# libomt imported shared library
# ---------------------------------------------------------------------------
add_library(libomt SHARED IMPORTED)

if(WIN32)
    set_target_properties(libomt PROPERTIES
        IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.dll"
        IMPORTED_IMPLIB               "${LIBOMT_ROOT}/lib/libomt.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
    )
elseif(APPLE)
    set_target_properties(libomt PROPERTIES
        IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
    )
else()
    set_target_properties(libomt PROPERTIES
        IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.so"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
    )
endif()

# ---------------------------------------------------------------------------
# Helper macro – builds one FFGL plugin DLL
# ---------------------------------------------------------------------------
macro(ffgl_plugin TARGET_NAME)
    cmake_parse_arguments(ARG "" "OUTPUT" "SOURCES" ${ARGN})

    # Include FFGL SDK sources directly so dllexport on plugMain works
    add_library(${TARGET_NAME} SHARED
        ${ARG_SOURCES}
        ${FFGL_LIB_SOURCES}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${ARG_OUTPUT}"
        PREFIX      ""
    )

    if(WIN32)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".dll")
    elseif(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUNDLE           TRUE
            BUNDLE_EXTENSION "bundle"
        )
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source/shared"
        "${FFGL_ROOT}/source/lib"
        "${FFGL_ROOT}/source/lib/ffglex"
        "${FFGL_ROOT}/deps/glew/include"
    )

    # GLEW_STATIC: we're using the static glew that ships in the ffgl repo
    target_compile_definitions(${TARGET_NAME} PRIVATE GLEW_STATIC)

    target_link_libraries(${TARGET_NAME} PRIVATE
        libomt
    )

    if(WIN32)
        target_link_libraries(${TARGET_NAME} PRIVATE
            "${FFGL_ROOT}/deps/glew/lib/Release/x64/glew32s.lib"
            opengl32.lib
        )

        # Copy libomt.dll and libvmx.dll next to the plugin after build
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBOMT_ROOT}/bin/libomt.dll"
                "${LIBOMT_ROOT}/bin/libvmx.dll"
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying OMT runtime DLLs next to ${TARGET_NAME}"
        )
    endif()
endmacro()

# ---------------------------------------------------------------------------
# Plugins
# ---------------------------------------------------------------------------
ffgl_plugin(OMTSend
    SOURCES
        source/plugins/OMTSend/OMTSend.cpp
        source/plugins/OMTSend/OMTSend.h
    OUTPUT OMTSend
)

ffgl_plugin(OMTReceive
    SOURCES
        source/plugins/OMTReceive/OMTReceive.cpp
        source/plugins/OMTReceive/OMTReceive.h
    OUTPUT OMTReceive
)

ffgl_plugin(MinTest
    SOURCES
        source/plugins/MinTest/MinTest.cpp
        source/plugins/MinTest/MinTest.h
    OUTPUT MinTest
)
