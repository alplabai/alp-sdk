/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * tinygsm-modem-at -- the AT command/response flow TinyGSM drives to
 * bring up a cellular modem, run here against a mock transcript
 * `Stream` (no real UART / modem) so it builds and runs on native_sim.
 *
 * TinyGSM (github.com/vshymanskyy/TinyGSM, LGPL-3.0) is header-heavy
 * C++ written against the Arduino core. Its AT-response engine
 * (`TinyGsmModem.tpp`'s `waitResponse()`, `getModemInfoImpl()`) is
 * built entirely on Arduino's `String` class (`WString.h`) plus
 * `Stream::readStringUntil()` -- reimplementing that surface is a
 * full Arduino-core-compat project, not a small shim (see
 * `src/ArduinoCompat/README.md` + the README's "Why the real library
 * isn't linked" for the exact `-DARDUINO_DASH` error chain). So this
 * example does NOT link libtinygsm; it drives the SAME AT exchange
 * TinyGsm would, directly over the `MockAtStream` (src/mock_at_stream.h),
 * which is the reusable half a hardware-in-the-loop test would keep.
 *
 * The real TinyGSM integration -- identical AT sequence, done by the
 * library -- is one Stream-backed object:
 *
 *   #define TINY_GSM_MODEM_SIM800
 *   #include <TinyGsmClient.h>
 *   TinyGsm modem(uartStream);      // uartStream = an alp/uart.h Stream adapter
 *   modem.init();                   // sends "AT", echo-off, etc.
 *   String info = modem.getModemInfo();   // ATI + AT+CGMI + AT+GMM, concatenated
 *
 * [UNTESTED]: native_sim (mock transcript). Not bench-run against a
 * real modem, and the upstream library is not linked (see above).
 *
 * What success looks like:
 *
 *   [tinygsm-modem-at] alp-sdk tinygsm-modem-at starting
 *   [tinygsm-modem-at] init(): AT -> OK
 *   [tinygsm-modem-at] getModemInfo() -> SIMCOM_SIM800 SIMCOM SIM800
 *   [tinygsm-modem-at] done
 */

#include <cstdio>
#include <cstring>

#include "mock_at_stream.h"

/* What the "modem" replies to each AT command. TinyGsm::init() opens
 * with a bare "AT" liveness probe; TinyGsm::getModemInfo() (the
 * TinyGsmModem.tpp default) sends "ATI", "AT+CGMI", "AT+GMM" in
 * sequence and concatenates the three payload lines. GSM_OK == "OK\r\n"
 * is what TinyGSM's waitResponse() matches on. */
static const AtExchange kTranscript[] = {
	{ "AT", "\r\nOK\r\n" },
	{ "ATI", "\r\nSIMCOM_SIM800\r\nOK\r\n" },
	{ "AT+CGMI", "\r\nSIMCOM\r\nOK\r\n" },
	{ "AT+GMM", "\r\nSIM800\r\nOK\r\n" },
};

/* Send one AT command line (sendAT() appends "\r\n") and collect the
 * modem's raw reply into `out`. Mirrors what TinyGSM's stream.write()
 * + waitResponse() read loop do, minus the String machinery. */
static void send_at(Stream &s, const char *cmd, char *out, size_t outsz)
{
	for (const char *p = cmd; *p != '\0'; p++) {
		s.write(static_cast<uint8_t>(*p));
	}
	s.write(static_cast<uint8_t>('\r'));
	s.write(static_cast<uint8_t>('\n'));

	size_t n = 0;
	int    c;
	while ((c = s.read()) >= 0 && n < outsz - 1) {
		out[n++] = static_cast<char>(c);
	}
	out[n] = '\0';
}

/* Extract the first non-empty payload line of an AT reply (skipping
 * the leading "\r\n" and stopping before the trailing "OK"). This is
 * the value TinyGSM would hand back as a String. */
static void first_line(const char *resp, char *out, size_t outsz)
{
	while (*resp == '\r' || *resp == '\n') {
		resp++;
	}
	size_t n = 0;
	while (*resp != '\0' && *resp != '\r' && *resp != '\n' && n < outsz - 1) {
		out[n++] = *resp++;
	}
	out[n] = '\0';
}

int main()
{
	printf("[tinygsm-modem-at] alp-sdk tinygsm-modem-at starting\n");

	/* On real hardware this Stream is a UART bound to the modem; a
	 * mock transcript stands in on native_sim (same Stream contract,
	 * table-driven instead of silicon). */
	MockAtStream stream(kTranscript, sizeof(kTranscript) / sizeof(kTranscript[0]));

	char resp[256];

	/* init(): liveness probe. A real init() then walks echo-off /
	 * error-mode / SIM-status commands this transcript doesn't model. */
	send_at(stream, "AT", resp, sizeof(resp));
	bool alive = (strstr(resp, "OK") != nullptr);
	printf("[tinygsm-modem-at] init(): AT -> %s\n", alive ? "OK" : "no response");

	/* getModemInfo(): ATI + AT+CGMI + AT+GMM, payload lines joined by
	 * spaces -- exactly what TinyGsmModem.tpp's getModemInfoImpl()
	 * assembles into its returned String. */
	char              info[128] = { 0 };
	char              line[64];
	const char *const info_cmds[] = { "ATI", "AT+CGMI", "AT+GMM" };
	for (size_t i = 0; i < sizeof(info_cmds) / sizeof(info_cmds[0]); i++) {
		send_at(stream, info_cmds[i], resp, sizeof(resp));
		first_line(resp, line, sizeof(line));
		if (i > 0) {
			strncat(info, " ", sizeof(info) - strlen(info) - 1);
		}
		strncat(info, line, sizeof(info) - strlen(info) - 1);
	}
	printf("[tinygsm-modem-at] getModemInfo() -> %s\n", info);

	if (!alive || info[0] == '\0') {
		printf("[tinygsm-modem-at] ERROR: modem did not respond as expected\n");
		return 1;
	}

	printf("[tinygsm-modem-at] done\n");
	return 0;
}
