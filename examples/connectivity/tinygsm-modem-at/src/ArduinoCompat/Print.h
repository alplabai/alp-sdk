/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Print.h -- the small Arduino `Print` base class, reimplemented
 * just far enough to let `Stream.h` (and this example's own mock
 * modem Stream) compile against TinyGSM's `ARDUINO_DASH` non-Arduino
 * integration point.  Real Arduino's `Print` also emits numeric
 * formatting (`print(int, int base)`, floating point, `Printable`
 * dispatch, ...); this shim only carries the byte-oriented subset a
 * bare AT-command transport actually needs.  See
 * `../../README.md` "Why this doesn't build" for why TinyGSM itself
 * needs far more than this to compile.
 */

#ifndef ALP_TINYGSM_ARDUINOCOMPAT_PRINT_H_
#define ALP_TINYGSM_ARDUINOCOMPAT_PRINT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

class Print
{
  public:
	virtual ~Print() = default;

	/* The one pure-virtual primitive every Arduino Print backend
	 * implements -- everything else in this class funnels through
	 * it, one byte at a time. */
	virtual size_t write(uint8_t b) = 0;

	/* Default multi-byte write: byte-at-a-time via write(uint8_t).
	 * A real UART backend would override this with a bulk DMA
	 * write; that optimisation is out of scope for a mock. */
	virtual size_t write(const uint8_t *buf, size_t size)
	{
		size_t n = 0;
		while (n < size) {
			if (write(buf[n]) == 0) {
				break;
			}
			n++;
		}
		return n;
	}

	size_t write(const char *s)
	{
		return s ? write(reinterpret_cast<const uint8_t *>(s), strlen(s)) : 0;
	}

	size_t print(const char *s)
	{
		return write(s);
	}

	size_t println(const char *s)
	{
		size_t n = write(s);
		n += write(reinterpret_cast<const uint8_t *>("\r\n"), 2);
		return n;
	}

	size_t println()
	{
		return write(reinterpret_cast<const uint8_t *>("\r\n"), 2);
	}
};

#endif /* ALP_TINYGSM_ARDUINOCOMPAT_PRINT_H_ */
