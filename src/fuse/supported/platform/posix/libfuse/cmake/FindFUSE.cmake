find_path(FUSE_INCLUDE_DIR
          fuse_common.h
          HINTS
          $ENV{FUSE_PREFIX}
          PATH_SUFFIXES
          include/fuse3
          include/fuse
)

# First, try to locate libfuse under the same install prefix as headers.
if (FUSE_INCLUDE_DIR)
    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.20.0")
        cmake_path(GET FUSE_INCLUDE_DIR PARENT_PATH _FUSE_INCLUDE_PARENT)
        cmake_path(GET _FUSE_INCLUDE_PARENT PARENT_PATH _FUSE_HEADER_PREFIX)
    else()
        get_filename_component(_FUSE_INCLUDE_PARENT "${FUSE_INCLUDE_DIR}" DIRECTORY)
        get_filename_component(_FUSE_HEADER_PREFIX "${_FUSE_INCLUDE_PARENT}" DIRECTORY)
    endif()
endif()

find_library(FUSE_LIBRARY
             NAMES
             fuse3
             fuse.2
             fuse
             HINTS
             ${_FUSE_HEADER_PREFIX}
             $ENV{FUSE_PREFIX}
             PATH_SUFFIXES
             lib
)

if (FUSE_INCLUDE_DIR AND FUSE_LIBRARY)
    find_package(Threads)

    set(FUSE_DEFINITIONS -D_FILE_OFFSET_BITS=64)

    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.20.0")
        cmake_path(GET FUSE_INCLUDE_DIR PARENT_PATH FUSE_INCLUDE_DIRS)
    else()
        get_filename_component(FUSE_INCLUDE_DIRS "${FUSE_INCLUDE_DIR}" DIRECTORY)
    endif()

    set(FUSE_LIBRARIES ${CMAKE_THREAD_LIBS_INIT} ${FUSE_LIBRARY})

    if (NOT TARGET FUSE)
        add_library(FUSE UNKNOWN IMPORTED)

        set_target_properties(
          FUSE
          PROPERTIES
          IMPORTED_LOCATION ${FUSE_LIBRARY}
        )

        if (Threads_FOUND)
            target_link_libraries(FUSE INTERFACE Threads::Threads)
        endif()

        target_compile_definitions(FUSE INTERFACE ${FUSE_DEFINITIONS})
        target_include_directories(FUSE INTERFACE ${FUSE_INCLUDE_DIRS})
    endif()

    # libfuse version macros may live in libfuse_config.h (3.18+) or
    # fuse_common.h (3.14 and earlier). libfuse_config.h can exist without
    # version macros, so probe each header until a match is found.
    set(FUSE_VERSION_MAJOR)
    set(FUSE_VERSION_MINOR)

    foreach(_FUSE_VERSION_HEADER IN ITEMS libfuse_config.h fuse_common.h)
        set(_FUSE_VERSION_PATH "${FUSE_INCLUDE_DIR}/${_FUSE_VERSION_HEADER}")

        if (NOT EXISTS "${_FUSE_VERSION_PATH}")
            continue()
        endif()

        file(READ "${_FUSE_VERSION_PATH}" _FUSE_VERSION_CONTENT)

        string(REGEX MATCH "#define[ \t]+FUSE_MAJOR_VERSION[ \t]+([0-9]+)"
               _
               "${_FUSE_VERSION_CONTENT}"
        )

        if (NOT CMAKE_MATCH_1)
            continue()
        endif()

        set(FUSE_VERSION_MAJOR ${CMAKE_MATCH_1})

        string(REGEX MATCH "#define[ \t]+FUSE_MINOR_VERSION[ \t]+([0-9]+)"
               _
               "${_FUSE_VERSION_CONTENT}"
        )

        if (CMAKE_MATCH_1)
            set(FUSE_VERSION_MINOR ${CMAKE_MATCH_1})
        endif()

        break()
    endforeach()

    if (NOT FUSE_VERSION_MINOR)
        set(FUSE_VERSION_MINOR 0)
    endif()

    if (NOT FUSE_VERSION_MAJOR MATCHES "^[0-9]+$")
        message(FATAL_ERROR
                "Could not determine libfuse major version from ${FUSE_INCLUDE_DIR}")
    endif()

    set(FUSE_VERSION "${FUSE_VERSION_MAJOR}.${FUSE_VERSION_MINOR}")

    # Cleanup after ourselves.
    unset(_FUSE_VERSION_CONTENT)
    unset(_FUSE_VERSION_HEADER)
    unset(_FUSE_VERSION_PATH)
    unset(_FUSE_HEADER_PREFIX)
    unset(_FUSE_INCLUDE_PARENT)
endif()

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(
  FUSE
  REQUIRED_VARS
  FUSE_INCLUDE_DIR
  FUSE_LIBRARY
  VERSION_VAR
  FUSE_VERSION
)

mark_as_advanced(FUSE_INCLUDE_DIR FUSE_LIBRARY)
