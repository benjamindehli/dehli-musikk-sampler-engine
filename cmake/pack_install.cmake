# Copies a plugin's memory-mapped sample pack (samples.pak + samples.pak.json) from the
# generated assets into a shared support folder the plugin reads at runtime. Used as a
# POST_BUILD step for libraries that ship samples as a disk pack instead of embedding them
# in the binary (see --pack-samples in ds-plugin-converter).
#
# Invoked with: cmake -DSRCDIR=<assets/samples> -DDSTDIR=<support folder> -P pack_install.cmake
# Copies only when the source is newer (or the destination is missing) — a plain copy of a
# multi-GB pack on every build would be wasteful, and content-comparing it is worse.

if(NOT EXISTS "${SRCDIR}/samples.pak")
    # No pack can be legitimate: older conversions EMBED their FLACs in the binary
    # (loose .flac files in SRCDIR) and the engine falls back to the embedded path.
    # Only warn when there are no samples at all — that plugin really will be silent.
    file(GLOB _embedded_flacs "${SRCDIR}/*.flac")
    if(_embedded_flacs)
        message(STATUS "pack_install: no samples.pak in ${SRCDIR} — samples are embedded "
                       "in the binary (reconvert to switch to the disk pack).")
    else()
        message(WARNING "pack_install: no samples.pak and no .flac files in ${SRCDIR} — "
                        "run the converter first, or the plugin will be silent.")
    endif()
    return()
endif()

file(MAKE_DIRECTORY "${DSTDIR}")

foreach(f samples.pak samples.pak.json)
    set(src "${SRCDIR}/${f}")
    set(dst "${DSTDIR}/${f}")
    if(EXISTS "${src}" AND (NOT EXISTS "${dst}" OR "${src}" IS_NEWER_THAN "${dst}"))
        message(STATUS "pack_install: installing ${f} -> ${DSTDIR}")
        file(COPY "${src}" DESTINATION "${DSTDIR}")
    else()
        message(STATUS "pack_install: up to date: ${dst}")
    endif()
endforeach()
