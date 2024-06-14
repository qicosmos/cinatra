include(FindPackageHandleStandardArgs)

find_path(BROTLI_INCLUDE_DIR "brotli/decode.h")

find_library(BROTLICOMMON_LIBRARY NAMES brotlicommon)
find_library(BROTLIDEC_LIBRARY NAMES brotlidec)
find_library(BROTLIENC_LIBRARY NAMES brotlienc)

find_package_handle_standard_args(Brotli
    FOUND_VAR
    BROTLI_FOUND
    REQUIRED_VARS
    BROTLIDEC_LIBRARY
    BROTLICOMMON_LIBRARY
    BROTLI_INCLUDE_DIR
    FAIL_MESSAGE
    "Could NOT find Brotli"
)

set(BROTLI_INCLUDE_DIRS ${BROTLI_INCLUDE_DIR})
set(BROTLI_LIBRARIES ${BROTLIDEC_LIBRARY} ${BROTLIENC_LIBRARY} ${BROTLICOMMON_LIBRARY})