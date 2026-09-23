cmake_minimum_required(VERSION 3.16)
project(grabercam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EASYCAP_ROOT "/opt/magewell/libeasycap" CACHE PATH "CEasyCapS sources")

add_library(easycap STATIC
	${EASYCAP_ROOT}/src/CEasyCapS.cpp
	${EASYCAP_ROOT}/src/CShm.cpp
	${EASYCAP_ROOT}/src/gentl_capture.cpp
	${EASYCAP_ROOT}/src/load_gentl_cti.cpp
	${EASYCAP_ROOT}/src/load_gentl_cti_ex.cpp
)
target_include_directories(easycap
	PUBLIC ${EASYCAP_ROOT}/include
	PRIVATE ${EASYCAP_ROOT}/include_private
)
target_compile_options(easycap PRIVATE -O2 -w -DNDEBUG)
target_link_libraries(easycap PUBLIC dl pthread rt)

add_executable(grabercam
	src/main.cpp
	src/session.cpp
)
target_link_libraries(grabercam PRIVATE easycap)
target_compile_options(grabercam PRIVATE -O2 -Wall -Wextra)
