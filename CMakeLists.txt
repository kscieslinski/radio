cmake_minimum_required(VERSION 3.10)
project(radio)

find_package (Threads REQUIRED)
find_package (Boost 1.40 REQUIRED COMPONENTS program_options)

set(CMAKE_CXX_STANDARD 14)
set (CMAKE_CXX_FLAGS "-Wall -Wextra")


# http://stackoverflow.com/questions/10555706/
macro (add_executable _name)
    # invoke built-in add_executable
    _add_executable(${ARGV})
    if (TARGET ${_name})
        target_link_libraries(${_name} helper err ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
    endif()
endmacro()

add_library(helper helper.cpp)
add_library(err err.cpp)
add_executable(sender sender.cpp)
add_executable(receiver receiver.cpp)