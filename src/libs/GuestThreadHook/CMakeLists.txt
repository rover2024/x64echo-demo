project(EchoGuestThreadHook)

add_library(${PROJECT_NAME} SHARED GuestThreadHook.c)
target_include_directories(${PROJECT_NAME} PRIVATE ${ECHO_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PRIVATE EchoGuestRuntime ${CMAKE_DL_LIBS})
