include(FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wil/include/wil/resource.h")
    set(FETCHCONTENT_SOURCE_DIR_WIL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wil")
endif()
# MIT; Windows handle/COM RAII only. No code from the four reference applications.
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG a1c0cbe1d7065aef6333399d3e25608eacdffc0f
    SYSTEM EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(wil)
