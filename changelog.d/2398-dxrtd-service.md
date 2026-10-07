### Fixed

- V2M image: dx-rt is now built with `USE_SERVICE=ON` and `dx-rt-cli` installs and enables upstream's `dxrt.service`, so `dxrtd` arbitrates the DX-M1 between processes (#2398). Before, two concurrent clients hung and a client killed mid-request wedged the NPU until reboot. At most 3 distinct NPU core sets can be live on one DX-M1 at a time (kernel driver queue limit).
