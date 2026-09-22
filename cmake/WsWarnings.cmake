include_guard(GLOBAL)

# Applies the workspace warning policy to a first-party target.
# NEVER apply this to third-party targets.
function(ws_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc
      /wd4324  # structure padded due to alignment specifier (common in graphics code)
    )
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
