### Fixed

- V2M image: dx-rt is now built with `USE_SERVICE=ON` and `dx-rt-cli` installs and enables upstream's `dxrt.service`, so `dxrtd` arbitrates the DX-M1 between processes (#2398). Before, two concurrent clients hung and a client killed mid-request wedged the NPU until reboot. On dx-rt 3.2.0, processes sharing one DX-M1 must still bind the same NPU core set.
