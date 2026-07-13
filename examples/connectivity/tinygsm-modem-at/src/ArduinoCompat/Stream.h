/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stream.h -- the small Arduino `Stream` base class this example's
 * `MockAtStream` (src/mock_at_stream.h) derives from.  This is the
 * "small Arduino-Stream shim" the parent plan asks for: enough of
 * the real `Stream` contract (`available()`/`read()`/`peek()` plus
 * `Print`'s `write()`) for a UART-shaped mock to implement, matching
 * the exact interface a real `HardwareSerial` on an Arduino target,
 * or this SDK's own UART wrapper wrapped in a `Stream` adapter, would
 * present to TinyGSM's `Stream&` constructor parameter.
 *
 * What real Arduino's `Stream` adds beyond this (`parseInt()`,
 * `readStringUntil()` -> `String`, `readBytesUntil()`, a `setTimeout`
 * / `find()` family) is exactly the part TinyGSM's own AT-parsing
 * engine (`TinyGsmModem.tpp`'s `waitResponse()`) calls -- and why
 * this shim alone does not get TinyGSM itself to compile. See
 * `../../README.md` "Why this doesn't build".
 */

#ifndef ALP_TINYGSM_ARDUINOCOMPAT_STREAM_H_
#define ALP_TINYGSM_ARDUINOCOMPAT_STREAM_H_

#include "Print.h"

class Stream : public Print
{
  public:
	/* Bytes ready to read without blocking -- what a UART driver's
	 * RX-FIFO depth / ring-buffer fill level reports. */
	virtual int available() = 0;

	/* One byte, or -1 if none available (never blocks). */
	virtual int read() = 0;

	/* Like read(), but doesn't consume the byte. */
	virtual int peek() = 0;

	/* Block until every buffered TX byte has left the wire. A mock
	 * has nothing to drain, so implementations may no-op. */
	virtual void flush() = 0;
};

#endif /* ALP_TINYGSM_ARDUINOCOMPAT_STREAM_H_ */
