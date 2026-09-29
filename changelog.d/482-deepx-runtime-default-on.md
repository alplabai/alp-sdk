### Changed — V2M images install the DEEPX DX-M1 runtime by default when `meta-deepx-m1` is present (#482)

`ALP_ENABLE_DEEPX_DXM1` in `meta-alp-sdk/conf/machine/include/e1m-v2m-deepx.inc`
was an explicit `?= "0"` opt-in, justified as a licence gate. It is not
one: the DX-M1 is DEEPX's own chip on every V2M SoM, and DEEPX publishes
`dx_rt`, `dx_rt_npu_linux_driver` and `meta-deepx-m1` on GitHub for the
users of that chip, which every V2M customer is. The flag now defaults to
`"1"` whenever `meta-deepx-m1` is in `BBFILE_COLLECTIONS`, so a V2M
image that carries DEEPX's layer ships `dx-driver dx-rt dx-rt-cli`, the
`dx_dma`/`dxrt_driver` autoload, and the `deepx-dxm1` alp-sdk
PACKAGECONFIG without a `local.conf` line. A build without the layer
still builds minus the DX-M1 path; `ALP_ENABLE_DEEPX_DXM1 = "0"` forces
it off. The stale "license-gated" wording in the image, alp-sdk,
alp-perception and `inference_deepx.cpp` comments and in
`meta-alp-sdk/README.md` step 8 is corrected.
