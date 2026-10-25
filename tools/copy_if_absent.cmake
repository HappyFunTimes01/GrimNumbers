# Copies SRC to DST only when DST does not exist.
#
# overlay.ini and elements.ini are meant to be edited in place next to the
# executable, so a post-build copy must not clobber them. copy_if_different
# would: it copies whenever the contents differ, which is exactly the state an
# edited config is in.
if(NOT EXISTS "${DST}")
    message(STATUS "installing default config: ${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
endif()
