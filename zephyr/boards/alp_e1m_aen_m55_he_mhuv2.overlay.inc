/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared E1M-AEN M55-HE mailbox fragment.
 *
 * This is the default dual-M55 RPMsg shape: M55-HE is the OpenAMP remote and
 * receives from M55-HP through the non-secure MHU-1 pair.  The register windows
 * match the Alif Ensemble common e1.dtsi MHU-1 nodes and the AEN801
 * bench-proven aen-rpc-pingpong path (#225, 2026-06-19).  A32<->M55 Linux
 * remoteproc overlays are endpoint-specific and should override ipc0 when that
 * topology is selected.
 */

#include "../../build/generated/dts-reservations.dtsi"

/ {
	chosen {
		zephyr,ipc = &ipc0;
		zephyr,ipc-shm = &alp_default_rpmsg;
	};

	soc {
		mhu_he_tx: mhu@400b0000 {
			compatible = "alif,mhuv2-mbox";
			reg = <0x400B0000 0x1000>;
			alif,direction = "tx";
			#mbox-cells = <1>;
			status = "okay";
		};

		mhu_he_rx: mhu@400a0000 {
			compatible = "alif,mhuv2-mbox";
			reg = <0x400A0000 0x1000>;
			interrupts = <43 3>;
			alif,direction = "rx";
			#mbox-cells = <1>;
			status = "okay";
		};
	};

	ipc0: ipc0 {
		compatible = "zephyr,ipc-openamp-static-vrings";
		memory-region = <&alp_default_rpmsg>;
		mboxes = <&mhu_he_tx 0>, <&mhu_he_rx 0>;
		mbox-names = "tx", "rx";
		role = "remote";
		status = "okay";
	};
};
