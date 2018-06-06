cmake_minimum_required(VERSION 3.10)
project(radio)

find_package (Threads REQUIRED)

set(CMAKE_CXX_STANDARD 14)
#set (CMAKE_CXX_FLAGS "-Wall -Wextra")


# http://stackoverflow.com/questions/10555706/
macro (add_executable _name)
    # invoke built-in add_executable
    _add_executable(${ARGV})
    if (TARGET ${_name})
        target_link_libraries(${_name} helper ${CMAKE_THREAD_LIBS_INIT})
    endif()
endmacro()

add_library(helper helper.cpp)
add_executable(sender sender.cpp)
add_executable(receiver receiver.cpp)