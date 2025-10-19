set(AXERA_MSP_OUT "${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc")

add_executable(sample_vin_raw
    src/sample_vin_raw.cc
)

target_include_directories(sample_vin_raw SYSTEM PRIVATE
    "${AXERA_MSP_OUT}/include"
)

target_link_directories(sample_vin_raw PRIVATE "${AXERA_MSP_OUT}/lib")

target_link_libraries(sample_vin_raw PRIVATE
    ax_sys
    ax_ae
    ax_awb
    ax_af
    ax_proton
    ax_engine
    ax_interpreter
    ax_mipi
    ax_ivps
    ax_venc
    m
    pthread
    dl
)

llm630_enable_contribution_checks(sample_vin_raw
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sample_vin_raw.cc"
)
