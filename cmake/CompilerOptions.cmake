function(zt_configure_project_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_options(${target_name} PRIVATE
        /W4
        /utf-8
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc
        /MP
    )
    target_compile_definitions(${target_name} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
endfunction()

function(zt_configure_third_party_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_options(${target_name} PRIVATE
        /W3
        /utf-8
        /permissive-
        /EHsc
        /MP
    )
    target_compile_definitions(${target_name} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
endfunction()
