### Fixed — backends: refuse unknown tflm dtypes, honour the CPU pin, fix power sleep accounting (#2925, #2978)

tflm: tensors whose type has no alp dtype (int64, bool, ...) are refused with
ALP_ERR_NOSUPPORT at open instead of being reported as F32; an explicit
ALP_INFERENCE_BACKEND_CPU pin no longer registers the Ethos-U op.

yocto_drv: slept_ms is timed with CLOCK_BOOTTIME so suspend time counts.

zephyr_pm_policy: the wake timeout no longer wraps for large wake_after_ms; a
wake timer that never fired returns ALP_ERR_TIMEOUT with realised_mode RUN.

som_power: prior_active() reads the ENABLE_LOW pad only after the pinctrl
state is applied.
