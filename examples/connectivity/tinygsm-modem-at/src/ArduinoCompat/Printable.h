/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Printable.h -- the tiny slice of Arduino's Printable interface
 * TinyGSM's ArduinoCompat/IPAddress.h `#include`s.  IPAddress isn't
 * actually exercised by this example (we never open a socket -- see
 * README.md), so this only needs to satisfy the compiler, not carry
 * real behaviour.
 */

#ifndef ALP_TINYGSM_ARDUINOCOMPAT_PRINTABLE_H_
#define ALP_TINYGSM_ARDUINOCOMPAT_PRINTABLE_H_

#include <cstddef>

class Print;

class Printable
{
  public:
	virtual size_t printTo(Print &p) const = 0;
	virtual ~Printable()                   = default;
};

#endif /* ALP_TINYGSM_ARDUINOCOMPAT_PRINTABLE_H_ */
