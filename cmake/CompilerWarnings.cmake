function(viewer_enable_warnings target)
  get_target_property(target_type "${target}" TYPE)
  if(target_type STREQUAL "INTERFACE_LIBRARY")
    set(warning_scope INTERFACE)
  else()
    set(warning_scope PRIVATE)
  endif()

  if(MSVC)
    target_compile_options("${target}" ${warning_scope}
      /W4
      /permissive-
      /Zc:__cplusplus
    )
  else()
    target_compile_options("${target}" ${warning_scope}
      -Wall
      -Wextra
      -Wpedantic
    )
  endif()
endfunction()
