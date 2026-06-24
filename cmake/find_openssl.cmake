if(NOT CINATRA_ENABLE_SSL)
    message(STATUS "OpenSSL support disabled")
    set(ENABLE_SSL OFF)
    return()
endif()

if(NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_openssl_candidate
            "C:/Program Files/OpenSSL-Win64"
            "C:/Program Files/OpenSSL-Win32"
            "C:/OpenSSL-Win64"
            "C:/OpenSSL-Win32")
        if(EXISTS "${_openssl_candidate}")
            set(OPENSSL_ROOT_DIR "${_openssl_candidate}" CACHE PATH "OpenSSL root directory")
            break()
        endif()
    endforeach()
endif()

find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    message(STATUS "Found OpenSSL libraries")
    set(ENABLE_SSL ON)
else()
    message(STATUS "OpenSSL libraries not found")
endif()
