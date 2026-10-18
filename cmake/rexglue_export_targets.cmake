# EXPORT_NAME only for targets that exist in this configuration (host-only
# targets like rexcodegen are not built on Switch).
set(_rex_export_names
    rexcore=core rexfilesystem=filesystem rexui=ui rexinput=input
    rexaudio=audio rexruntime=runtime rexcodegen=codegen)
foreach(_pair IN LISTS _rex_export_names)
    string(REPLACE "=" ";" _kv "${_pair}")
    list(GET _kv 0 _tgt)
    list(GET _kv 1 _name)
    if(TARGET ${_tgt})
        set_target_properties(${_tgt} PROPERTIES EXPORT_NAME ${_name})
    endif()
endforeach()

set(REXGLUE_INSTALL_TARGETS
    rexruntime
    rexgpu-xenos
    disruptorplus renderdoc simde tomlplusplus
    aes128 mspack o1heap disasm xxhash
    libavcodec libavutil
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGLUE_INSTALL_TARGETS
        SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler  # glslang
        spirv-tools-headers
    )
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGLUE_INSTALL_TARGETS dxc-headers)
endif()

if(REXGLUE_ENABLE_TRACY)
    list(APPEND REXGLUE_INSTALL_TARGETS TracyClient)
endif()

# Keep only targets that exist in this configuration.
set(_rex_filtered)
foreach(_t IN LISTS REXGLUE_INSTALL_TARGETS)
    if(TARGET ${_t})
        list(APPEND _rex_filtered ${_t})
    endif()
endforeach()
set(REXGLUE_INSTALL_TARGETS ${_rex_filtered})

set(REXGLUE_INSTALL_FIDELITYFX_TARGETS)
if(TARGET amd_fidelityfx_vk)
    list(APPEND REXGLUE_INSTALL_FIDELITYFX_TARGETS amd_fidelityfx_vk)
endif()

if(TARGET amd_fidelityfx_dx12)
    list(APPEND REXGLUE_INSTALL_FIDELITYFX_TARGETS amd_fidelityfx_dx12)
endif()
