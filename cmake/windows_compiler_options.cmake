function(system_compiler_options target)
    target_compile_definitions(${target}
        PRIVATE
            _CRT_SECURE_NO_DEPRECATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNING
            _SCL_SECURE_NO_WARNINGS
            NOMINMAX
            JSON_DLL
    )
    target_compile_options(${target}
        PRIVATE
            /wd4267
            /wd4244
            /wd4275
            /wd4251
            /wd4250
            /wd4800
    )
endfunction()
