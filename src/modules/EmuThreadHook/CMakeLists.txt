# LD_PRELOAD'd into the QEMU process; inverts host-origin pthread_create.
add_library(echo_thread_hook SHARED main.cpp)
target_link_libraries(echo_thread_hook PRIVATE ${CMAKE_DL_LIBS})
