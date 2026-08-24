cmake_minimum_required(VERSION 3.10)
project(quantumsync-local VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

add_executable(quantumsync-local
    src/main.cpp
    src/http_server.cpp
)

target_include_directories(quantumsync-local PRIVATE
    ${Boost_INCLUDE_DIRS}
    src
)

target_link_libraries(quantumsync-local PRIVATE
    ${Boost_LIBRARIES}
    Threads::Threads
)

install(TARGETS quantumsync-local DESTINATION bin)
