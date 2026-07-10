/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mock_at_stream.h -- a canned-transcript AT-modem `Stream` stand-in.
 *
 * On real hardware, TinyGSM's `TinyGsm modem(stream)` constructor
 * takes a `Stream&` bound to the UART the modem is wired to (an
 * `alp/uart.h` handle wrapped in a `Stream` adapter, on this SDK's
 * portable API -- see README.md "Swapping in a real UART"). Here on
 * native_sim there's no modem, so `MockAtStream` plays back a
 * pre-recorded AT command/response transcript instead: every
 * `write()` of a command line is matched against the next expected
 * command, and the corresponding response is queued up for
 * subsequent `read()`/`available()` calls -- functionally the same
 * contract a real modem's UART presents, just driven by a table
 * instead of silicon.
 */

#ifndef ALP_TINYGSM_MOCK_AT_STREAM_H_
#define ALP_TINYGSM_MOCK_AT_STREAM_H_

#include <cstdio>
#include <cstring>

#include "ArduinoCompat/Stream.h"

/* One request/response exchange in the canned transcript.  `command`
 * excludes the trailing "\r\n" TinyGSM's sendAT() appends; `response`
 * is the raw bytes a real modem would write back, "\r\n" included. */
struct AtExchange {
	const char *command;
	const char *response;
};

class MockAtStream : public Stream
{
  public:
	MockAtStream(const AtExchange *transcript, size_t count)
	    : transcript_(transcript), count_(count)
	{
	}

	/* --- Print: the modem's TX path (what the app sends TO the "modem") --- */
	size_t write(uint8_t b) override
	{
		/* Buffer the outgoing command line byte-by-byte; on '\n'
		 * (sendAT() always terminates with "\r\n") match it
		 * against the transcript and queue the matching response. */
		if (tx_len_ < sizeof(tx_buf_) - 1) {
			tx_buf_[tx_len_++] = static_cast<char>(b);
		}
		if (b == '\n') {
			tx_buf_[tx_len_] = '\0';
			match_and_queue_response();
			tx_len_ = 0;
		}
		return 1;
	}

	/* --- Stream: the modem's RX path (what the "modem" sends back) --- */
	int available() override
	{
		return static_cast<int>(rx_len_ - rx_pos_);
	}

	int read() override
	{
		if (rx_pos_ >= rx_len_) {
			return -1;
		}
		return static_cast<unsigned char>(rx_buf_[rx_pos_++]);
	}

	int peek() override
	{
		if (rx_pos_ >= rx_len_) {
			return -1;
		}
		return static_cast<unsigned char>(rx_buf_[rx_pos_]);
	}

	void flush() override
	{
		/* Nothing buffered to drain in a mock -- a real UART
		 * backend would block here until the TX FIFO empties. */
	}

  private:
	void match_and_queue_response()
	{
		/* tx_buf_ holds "<command>\r\n"; strip the CRLF before
		 * comparing against the table's bare command text. */
		size_t cmd_len = tx_len_;
		while (cmd_len > 0 && (tx_buf_[cmd_len - 1] == '\n' || tx_buf_[cmd_len - 1] == '\r')) {
			cmd_len--;
		}

		for (size_t i = 0; i < count_; i++) {
			if (strlen(transcript_[i].command) == cmd_len &&
			    strncmp(transcript_[i].command, tx_buf_, cmd_len) == 0) {
				const char *resp = transcript_[i].response;
				rx_len_          = strlen(resp);
				if (rx_len_ >= sizeof(rx_buf_)) {
					rx_len_ = sizeof(rx_buf_) - 1;
				}
				memcpy(rx_buf_, resp, rx_len_);
				rx_pos_ = 0;
				return;
			}
		}
		/* Unrecognised command -- leave the RX buffer empty; a
		 * real modem would eventually time out the same way. */
		printf("[tinygsm-modem-at] mock: no canned response for \"%.*s\"\n", (int)cmd_len, tx_buf_);
	}

	const AtExchange *transcript_;
	size_t            count_;

	char   tx_buf_[128];
	size_t tx_len_ = 0;

	char   rx_buf_[256];
	size_t rx_len_ = 0;
	size_t rx_pos_ = 0;
};

#endif /* ALP_TINYGSM_MOCK_AT_STREAM_H_ */
