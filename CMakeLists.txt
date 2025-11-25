cmake_minimum_required(VERSION 3.16)
project(FileSearchApp)

set(CMAKE_CXX_STANDARD 17)

# Укажи путь к SFML (он у тебя уже есть в папке СП/SFML)
set(SFML_DIR "/Users/antoninaber/Desktop/СП/SFML/build")

find_package(SFML 3 REQUIRED COMPONENTS Graphics Window System)

include_directories(include)

add_executable(filesearch src/main.cpp)

target_link_libraries(filesearch PRIVATE sfml-graphics sfml-window sfml-system)
