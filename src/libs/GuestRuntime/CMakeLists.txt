project(EchoGuestRuntime)

# The guest-world shared runtime: one copy per process, DT_NEEDED by each face.
add_library(${PROJECT_NAME} SHARED
    Cross.c
    DLCall.c
    Publish.c
    Rethrow.cpp
    ReentryCatch.cpp
    DynCast.cpp
    ${ECHO_SOURCE_DIR}/src/libs/Arena/Arena.c
    ${ECHO_SOURCE_DIR}/src/libs/Arena/Operators.cpp
)
# PUBLIC include for the <echo/...> public form; PRIVATE the module's own header
# dir so same-target sources can include their headers by bare name ("Foo.h").
target_include_directories(${PROJECT_NAME}
    PUBLIC ${ECHO_SOURCE_DIR}/include
    PRIVATE ${ECHO_SOURCE_DIR}/include/echo/GuestRuntime)

# the C-3 rethrow helper genuinely needs C++ exceptions
set_source_files_properties(Rethrow.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)
set_source_files_properties(ReentryCatch.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)

# Rethrow.cpp resolves the guest-side __echo_exc$ descriptors via dlsym.
target_link_libraries(${PROJECT_NAME} PUBLIC ${CMAKE_DL_LIBS})
