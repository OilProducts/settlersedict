cmake_minimum_required(VERSION 3.30)
project(settlersedict C)

set(CMAKE_C_STANDARD 23)

add_executable(settlersedict main.c src/poll_loop.c
        src/tcp_to_stdout.c
        src/tcp_to_stdout.h
)


