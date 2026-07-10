/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * jsmn-json-parse -- tokenize a JSON config blob with jsmn and pull two
 * fields out of the token array by hand.
 *
 * jsmn (https://github.com/zserge/jsmn) is a minimal, allocation-free JSON
 * *tokenizer* -- not a full parser/DOM.  It walks the input once and fills a
 * caller-owned array of jsmntok_t (each token is just {type, start, end,
 * size} -- byte offsets into the ORIGINAL string, no copying, no heap).
 * That's exactly the shape embedded firmware wants: a fixed-size config
 * blob (over UART, a cloud MQTT payload, an OTA manifest) decoded into
 * plain C fields with a known worst-case RAM footprint.
 *
 * The trade-off versus a "real" JSON library (cJSON, nlohmann_json): jsmn
 * gives you tokens, not values.  Turning `tokens[i]` into an int/string is
 * on you -- see json_extract_int() / json_extract_str() below.  For a
 * handful of known fields (which is most embedded config use) that's a
 * fair trade for zero heap and a single ~500-line header.
 *
 * What success looks like:
 *
 *   [jsmn-json-parse] parsed 7 tokens
 *   [jsmn-json-parse] device  = "e1m-aen801"
 *   [jsmn-json-parse] interval_ms = 500
 *   [jsmn-json-parse] done
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* JSMN_STATIC would make the functions `static` (one-TU-only linkage);
 * we don't need it here since this is the only .c file that includes
 * jsmn.h, but it's the idiom to know if you split parsing into its own
 * translation unit later. Left at the header's default (extern). */
#include <jsmn.h>

/* Fixed-size config blob -- the kind of thing you'd receive over a
 * console command, a BLE GATT write, or an MQTT retained message.
 * Deliberately small and flat: jsmn shines on exactly this shape. */
static const char kConfigJson[] = "{\"device\":\"e1m-aen801\",\"interval_ms\":500,\"debug\":true}";

/* Worst case is roughly one token per '{', '}', ':' , ',' and value --
 * for this 3-key object that's the root object + 3 keys + 3 values = 7;
 * 16 gives headroom without dynamic growth (jsmn has no realloc path). */
#define JSON_MAX_TOKENS 16u

/* What the app actually wants out of the blob -- plain C, no jsmn types
 * leak past this point. This is the pattern to copy: parse once into a
 * typed struct, then let the rest of the firmware forget jsmn exists. */
struct device_config {
	char device[32];
	int  interval_ms;
};

/* jsmn tokens carry [start, end) byte offsets into the ORIGINAL buffer,
 * not the token's own copy -- jsmn never allocates or copies. `js` MUST
 * stay alive as long as you're reading from `tok`. */
static int jsmn_tok_streq(const char *js, const jsmntok_t *tok, const char *s)
{
	size_t tok_len = (size_t)(tok->end - tok->start);

	return tok->type == JSMN_STRING && strlen(s) == tok_len &&
	       strncmp(js + tok->start, s, tok_len) == 0;
}

/* Copies a JSMN_STRING token's bytes into a caller buffer + NUL-terminates.
 * Truncates (silently, for this teaching example) if the value doesn't
 * fit -- production code should treat that as a parse error. */
static void json_extract_str(const char *js, const jsmntok_t *tok, char *out, size_t out_sz)
{
	size_t tok_len = (size_t)(tok->end - tok->start);
	size_t n       = tok_len < out_sz - 1 ? tok_len : out_sz - 1;

	memcpy(out, js + tok->start, n);
	out[n] = '\0';
}

/* Copies a JSMN_PRIMITIVE token's bytes into a small stack buffer and
 * hands them to atoi(). jsmn doesn't distinguish int/float/bool within
 * PRIMITIVE -- the caller is expected to already know the field's type
 * from the schema, same as with json_extract_str() above. */
static int json_extract_int(const char *js, const jsmntok_t *tok)
{
	char   buf[16];
	size_t tok_len = (size_t)(tok->end - tok->start);
	size_t n       = tok_len < sizeof(buf) - 1 ? tok_len : sizeof(buf) - 1;

	memcpy(buf, js + tok->start, n);
	buf[n] = '\0';
	return atoi(buf);
}

int main(void)
{
	jsmn_parser          parser;
	jsmntok_t            tokens[JSON_MAX_TOKENS];
	struct device_config cfg = { .device = "", .interval_ms = 0 };

	/* jsmn_init resets the parser's internal cursor -- cheap enough to
	 * call fresh before every jsmn_parse(), no persistent state you need
	 * to manage across calls. */
	jsmn_init(&parser);

	int ntok = jsmn_parse(&parser, kConfigJson, strlen(kConfigJson), tokens, JSON_MAX_TOKENS);

	/* jsmn_parse returns the token count on success, or a negative
	 * jsmnerr_t (JSMN_ERROR_NOMEM / _INVAL / _PART) on failure -- always
	 * check before indexing `tokens`. A negative count here would mean
	 * either malformed JSON or JSON_MAX_TOKENS was too small. */
	if (ntok < 0) {
		printf("[jsmn-json-parse] parse failed: %d\n", ntok);
		return 1;
	}
	printf("[jsmn-json-parse] parsed %d tokens\n", ntok);

	/* tokens[0] is always the whole object (JSMN_OBJECT); its ->size is
	 * the key count. Object entries walk as [key, value] pairs from
	 * index 1 -- jsmn doesn't build a key->value map for you, so this
	 * linear key-name comparison IS the "lookup". Fine for a handful of
	 * known fields; for a big schema you'd want a switch on a hashed key
	 * or a generated field table instead. */
	for (int i = 1; i < ntok - 1; i += 2) {
		const jsmntok_t *key = &tokens[i];
		const jsmntok_t *val = &tokens[i + 1];

		if (jsmn_tok_streq(kConfigJson, key, "device")) {
			json_extract_str(kConfigJson, val, cfg.device, sizeof(cfg.device));
		} else if (jsmn_tok_streq(kConfigJson, key, "interval_ms")) {
			cfg.interval_ms = json_extract_int(kConfigJson, val);
		}
		/* "debug" is present in kConfigJson but this example doesn't
		 * need it -- unrecognised keys are simply skipped, which is
		 * how you get forward compatibility with a config blob that
		 * grows new fields over firmware versions. */
	}

	printf("[jsmn-json-parse] device  = \"%s\"\n", cfg.device);
	printf("[jsmn-json-parse] interval_ms = %d\n", cfg.interval_ms);

	printf("[jsmn-json-parse] done\n");
	return 0;
}
