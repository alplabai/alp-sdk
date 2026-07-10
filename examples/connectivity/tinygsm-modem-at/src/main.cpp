/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * tinygsm-modem-at -- drive TinyGSM's `TinyGsm` modem class against a
 * mock AT-transcript `Stream` (no real UART / cellular modem
 * required) and print what `init()` / `getModemInfo()` report.
 *
 * **[UNTESTED] -- does not build in this workspace.** TinyGSM
 * (github.com/vshymanskyy/TinyGSM, LGPL-3.0) is header-heavy C++
 * written against the Arduino core: its `TinyGsm modem(stream)`
 * constructor wants a `Stream&`, and its AT-response parser
 * (`waitResponse()`) is built entirely on Arduino's `String` class.
 * This example provides the small `Stream` shim the parent plan asks
 * for (`src/ArduinoCompat/` -- `Print.h`/`Stream.h`/`Printable.h`,
 * matching the filenames TinyGSM's own `-DARDUINO_DASH` non-Arduino
 * integration point expects), but that shim does not reach far
 * enough: TinyGSM's own `ArduinoCompat/IPAddress.h` next asks for
 * `WString.h` -- Arduino's `String` class, ~40 methods, used
 * pervasively by every per-modem header this SDK would need to pull
 * in (SIM800's battery/calling/GPRS/GPS/SMS/SSL/TCP/time/NTP `.tpp`
 * files). Reimplementing `String` is a full Arduino-core-compat
 * project, not "a small Arduino-Stream shim" -- see
 * `src/ArduinoCompat/README.md` and this file's companion
 * `../README.md` "Why this doesn't build" for the exact,
 * empirically-confirmed `g++ -DARDUINO_DASH` error chain that proves
 * this (`Printable.h` -> `WString.h`, in that order).
 *
 * What follows is nonetheless the CORRECT TinyGSM integration
 * pattern: construct `TinyGsm` over a `Stream&`, call `init()`, call
 * `getModemInfo()`. On real hardware `stream` would be a UART; here
 * it's `MockAtStream` (src/mock_at_stream.h) replaying a canned
 * AT/response transcript, standing in for the modem exactly the way
 * a hardware-in-the-loop test double would.
 *
 * What success would look like once buildable:
 *
 *   [tinygsm-modem-at] alp-sdk tinygsm-modem-at starting
 *   [tinygsm-modem-at] init() -> true
 *   [tinygsm-modem-at] getModemInfo() -> SIMCOM_SIM800 SIMCOM SIM800
 *   [tinygsm-modem-at] done
 */

#include <cstdio>

/* TINY_GSM_MODEM_SIM800 picks the SIMCOM SIM800 command set (matches
 * this example's canned "SIMCOM" AT+CGMI transcript); ARDUINO_DASH is
 * TinyGSM's own switch for non-Arduino platforms -- both are passed
 * via target_compile_definitions() in CMakeLists.txt rather than
 * #defined here, so they're visible to every translation unit
 * TinyGsmClient.h's modem-family dispatch macro touches. */
#include <TinyGsmClient.h>

#include "mock_at_stream.h"

/* The canned modem transcript: what the "modem" replies to each AT
 * command TinyGSM sends. TinyGsm::init() -> initImpl() (SIM800's
 * override) opens with a bare "AT" liveness probe, then walks through
 * echo-off / error-mode / clock / battery-check / SIM-status commands
 * this table does NOT model (MockAtStream just logs "no canned
 * response" and lets waitResponse() time out for those -- harmless
 * for this teaching example, but not what a real integration test
 * would want); TinyGsm::getModemInfo() (the TinyGsmModem.tpp default
 * -- SIM800 doesn't override it) sends "ATI", "AT+CGMI", and "AT+GMM"
 * in sequence and concatenates the three replies -- those three ARE
 * modeled below. GSM_OK == "OK\r\n" (TinyGsmClientSIM800.h) is what
 * waitResponse() actually matches on; the "\r\n" is what a real
 * modem's own line-ending would send but is not itself matched. */
static const AtExchange kTranscript[] = {
	{ "AT", "\r\nOK\r\n" },
	{ "ATI", "\r\nSIMCOM_SIM800\r\nOK\r\n" },
	{ "AT+CGMI", "\r\nSIMCOM\r\nOK\r\n" },
	{ "AT+GMM", "\r\nSIM800\r\nOK\r\n" },
};

int main()
{
	printf("[tinygsm-modem-at] alp-sdk tinygsm-modem-at starting\n");

	MockAtStream stream(kTranscript, sizeof(kTranscript) / sizeof(kTranscript[0]));

	/* TinyGsm is a `typedef` alias picked by the TINY_GSM_MODEM_*
	 * macro (TinyGsmClient.h) -- here, TinyGsmSim800. It always
	 * takes the transport Stream by reference, never owns it: the
	 * caller (us) owns `stream`'s lifetime, matching every other
	 * peripheral-handle convention in this SDK ("borrowed, must
	 * outlive"). */
	TinyGsm modem(stream);

	/* init() -> initImpl(): opens with a bare "AT" liveness probe
	 * (modeled below), then several more commands this transcript
	 * doesn't model -- see the kTranscript comment above. */
	bool ok = modem.init();
	printf("[tinygsm-modem-at] init() -> %s\n", ok ? "true" : "false");

	/* getModemInfo() returns TinyGSM's own String type -- on real
	 * Arduino/ARDUINO_DASH builds this concatenates the ATI /
	 * AT+CGMI / AT+GMM replies. */
	String info = modem.getModemInfo();
	printf("[tinygsm-modem-at] getModemInfo() -> %s\n", info.c_str());

	printf("[tinygsm-modem-at] done\n");
	return 0;
}
