find_package(SWIG REQUIRED)
set(UseSWIG_MODULE_VERSION 2)
include(UseSWIG)

if(DEFINED PYTHON3_EXECUTABLE AND NOT DEFINED Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "${PYTHON3_EXECUTABLE}" CACHE FILEPATH "Python 3 interpreter" FORCE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

# Set the output directory for generated Python files
set(PYTHON_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bindings/python)

# Define the interface file and its properties
set(SWIG_INTERFACE_FILE ${CMAKE_CURRENT_LIST_DIR}/../megaapi.i)
set_source_files_properties(${SWIG_INTERFACE_FILE} PROPERTIES
    CPLUSPLUS ON
)
set_property(SOURCE ${SWIG_INTERFACE_FILE} PROPERTY SWIG_USE_TARGET_INCLUDE_DIRECTORIES TRUE)

set(SWIG_COMPILE_OPTIONS -c++;-py3;-threads;-USWIGJAVA)
if(ENABLE_SYNC)
    list(APPEND SWIG_COMPILE_OPTIONS -DENABLE_SYNC)
endif()

if(USE_LIBUV)
    list(APPEND SWIG_COMPILE_OPTIONS -DHAVE_LIBUV)
endif()

set_property(SOURCE ${SWIG_INTERFACE_FILE} PROPERTY SWIG_COMPILE_OPTIONS ${SWIG_COMPILE_OPTIONS})

# Generate Python bindings
swig_add_library(SDKPythonBindings
    TYPE MODULE
    LANGUAGE python
    SOURCES ${SWIG_INTERFACE_FILE}
    OUTPUT_DIR ${PYTHON_OUTPUT_DIR}
)

set_target_properties(SDKPythonBindings PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${PYTHON_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PYTHON_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${PYTHON_OUTPUT_DIR}
    SWIG_USE_SWIG_DEPENDENCIES ON
)

set(SDKPYTHON_SWIG_DEFINES)
if(ENABLE_SYNC)
    list(APPEND SDKPYTHON_SWIG_DEFINES ENABLE_SYNC)
endif()

if(USE_LIBUV)
    list(APPEND SDKPYTHON_SWIG_DEFINES HAVE_LIBUV)
endif()

set_property(TARGET SDKPythonBindings PROPERTY SWIG_COMPILE_DEFINITIONS ${SDKPYTHON_SWIG_DEFINES})

set_property(TARGET SDKPythonBindings PROPERTY SWIG_INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

target_include_directories(SDKPythonBindings
    PRIVATE
    ${Python3_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

target_link_libraries(SDKPythonBindings
    PRIVATE
    Python3::Module
    MEGA::SDKlib
)

# GIL is handled inside megaapi.i (SWIG director:before/after + director:except
# under SWIGPYTHON, plus the -threads flag above). The old post-build regex patch
# of megaapiPYTHON_wrap.cxx (patch_director_gil.py) was fragile and segfaulted; it
# has been removed.

# Generate Python wheel
set(SETUP_PYTHON_FILE ${CMAKE_CURRENT_LIST_DIR}/setup.py)

# Create a dist directory for wheels  
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/dist)

# Copy setup files to the output directory for wheel building
configure_file(${CMAKE_CURRENT_LIST_DIR}/setup.py ${PYTHON_OUTPUT_DIR}/setup.py COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/DESCRIPTION.rst ${PYTHON_OUTPUT_DIR}/DESCRIPTION.rst COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/__init__.py ${PYTHON_OUTPUT_DIR}/__init__.py COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/MANIFEST.in ${PYTHON_OUTPUT_DIR}/MANIFEST.in COPYONLY)
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/pyproject.toml)
    configure_file(${CMAKE_CURRENT_LIST_DIR}/pyproject.toml ${PYTHON_OUTPUT_DIR}/pyproject.toml COPYONLY)
endif()

# Copy version header for setup.py to read
file(MAKE_DIRECTORY ${PYTHON_OUTPUT_DIR}/../../include/mega)
configure_file(${CMAKE_CURRENT_LIST_DIR}/../../include/mega/version.h ${PYTHON_OUTPUT_DIR}/../../include/mega/version.h COPYONLY)

add_custom_target(build_wheel ALL
    COMMAND ${Python3_EXECUTABLE} setup.py bdist_wheel --dist-dir ${CMAKE_BINARY_DIR}/dist
    WORKING_DIRECTORY ${PYTHON_OUTPUT_DIR}
    DEPENDS SDKPythonBindings
    COMMENT "Building Python wheel..."
)