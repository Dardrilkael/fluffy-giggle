include_guard(GLOBAL)

# First-party warning policy. NEVER apply this to third-party targets.
#
# WS_WERROR (default ON): treat warnings as errors on first-party code.
# Disable when a compiler upgrade introduces new warnings before you have
# time to fix them - the build still warns loudly, it just stops failing
# the CI over them.
#
# WS_EXTRA_WARNINGS (default OFF): extra strictness beyond /W4 / -Wall
# -Wextra -Wpedantic. Currently adds -Wshadow, -Wconversion,
# -Wsign-conversion on GCC/Clang - the warnings MSVC /W4 already ships and
# the GCC flags lack. Off by default because conversion/sign-conversion
# are noisy on numerical code; turn it on for an audit pass.
option(WS_WERROR "Treat first-party warnings as errors" ON)
option(WS_EXTRA_WARNINGS
       "Extra-strict warnings on first-party code (-Wshadow/-Wconversion/-Wsign-conversion on GCC/Clang)"
       OFF)

function(ws_set_warnings target)
  if(MSVC)
    set(_flags /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc
              /wd4324  # structure padded due to alignment specifier (common in graphics code)
              /w44062  # unhandled enumerator in a no-default switch (the MSVC counterpart
                       # of gcc/clang -Wswitch): enum switches stay exhaustive or grow a default
    )
    if(WS_WERROR)
      list(APPEND _flags /WX)
    endif()
    target_compile_options(${target} PRIVATE ${_flags})
  else()
    # -Wmissing-field-initializers is deliberately off: designated
    # initializers with defaulted tails are the intended spelling of the
    # aggregate APIs (Tran{.tstop = 5_ms}), and GCC flags exactly that.
    set(_flags -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    if(WS_WERROR)
      list(APPEND _flags -Werror)
    endif()
    if(WS_EXTRA_WARNINGS)
      # The MSVC /W4 counterparts that GCC/Clang need an explicit flag for.
      # -Wshadow: a parameter/locals/member shadowing - C4456/C4457/C4458/C4459.
      # -Wconversion: implicit narrowing - the cause of the int->uint
      #   promotion traps that bite silently on GCC.
      # -Wsign-conversion: implicit sign changes (size_t vs int loops).
      # -Wmismatched-tags: a class forward-declared with one class-key
      #   (class X) and defined with another (struct X { ... }) - MSVC C4099
      #   under /W4. tools/audit_class_struct.py is the standalone checker;
      #   this flag is the compiler-side check (Clang always, GCC 14+).
      list(APPEND _flags -Wshadow -Wconversion -Wsign-conversion -Wmismatched-tags)
    endif()
    target_compile_options(${target} PRIVATE ${_flags})
  endif()
endfunction()
