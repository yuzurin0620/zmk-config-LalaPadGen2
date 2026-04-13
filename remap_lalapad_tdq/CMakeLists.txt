zephyr_library()

set(_zmk_app_include_candidates
  ${ZEPHYR_BASE}/../app/include
  ${ZEPHYR_BASE}/../zmk/app/include
)
foreach(_inc ${_zmk_app_include_candidates})
  if(EXISTS ${_inc})
    zephyr_library_include_directories(${_inc})
    break()
  endif()
endforeach()

zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_TAP_DANCE_TRIPLE
  src/behaviors/behavior_tap_dance_triple.c
)
