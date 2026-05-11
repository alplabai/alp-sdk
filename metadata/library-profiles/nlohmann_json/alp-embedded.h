/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * nlohmann/json configuration header for the ALP SDK's embedded
 * targets.
 *
 * nlohmann/json (https://github.com/nlohmann/json) is heavier than
 * fmt / ETL but works on embedded with the right macros set.  This
 * profile makes it compatible with the SDK's invariants.
 *
 *   - JSON_NOEXCEPTION : the SDK is exception-free.  json routes
 *                        parse / type failures through abort() on
 *                        Cortex-M when this is set; safe paths use
 *                        nlohmann::json::parse(..., callback, false)
 *                        with allow_exceptions=false to get the
 *                        is_discarded() error path instead.
 *
 *   - JSON_USE_IMPLICIT_CONVERSIONS=0 :
 *                        avoids surprise implicit conversions to
 *                        std::string / numeric types when assigning
 *                        from a json value.  Marginally faster
 *                        compile, less foot-gun.
 *
 * nlohmann/json is large (~30k LOC of single-header), so on the
 * smallest targets prefer a different JSON path (Zephyr's
 * data/json subsys; jsmn; nanopb for binary).  Use nlohmann on
 * V2N-class and Yocto where the cost is acceptable.
 *
 * Consumers wanting different json settings supply their own
 * config header before #include <nlohmann/json.hpp>.
 */

#ifndef ALP_NLOHMANN_JSON_CONFIG_H_
#define ALP_NLOHMANN_JSON_CONFIG_H_

/* No exceptions on the hot path.  json's parse errors surface
 * via the discarded() sentinel or the parser-callback `false`
 * return when this is set. */
#define JSON_NOEXCEPTION 1

/* Disable implicit conversions to/from json values.  Makes
 * accidental serialisation/deserialisation surprises a compile
 * error instead of a runtime bug. */
#define JSON_USE_IMPLICIT_CONVERSIONS 0

#endif /* ALP_NLOHMANN_JSON_CONFIG_H_ */
