include_guard(GLOBAL)

# Applies the workspace warning policy to a first-party target.
# NEVER apply this to third-party targets.
function(ws_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc
      /wd4324  # structure padded due to alignment specifier (common in graphics code)
      /w44062  # unhandled enumerator in a no-default switch (the MSVC counterpart
               # of gcc/clang -Wswitch): enum switches stay exhaustive or grow a default
    )
  else()
    # -Wmissing-field-initializers is deliberately off: designated
    # initializers with defaulted tails are the intended spelling of the
    # aggregate APIs (Tran{.tstop = 5_ms}), and GCC flags exactly that.
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
  endif()
endfunction()
