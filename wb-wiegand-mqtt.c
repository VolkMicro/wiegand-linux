#include <errno.h>
#include <gpiod.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE_ID "wiegand"
#define DEFAULT_D0 228 /* A2 IN on WB8 */
#define DEFAULT_D1 233 /* A1 IN on WB8 */
#define DEFAULT_MQTT_HOST "localhost"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_CHIP "gpiochip0"

#define MIN_PULSE_NS 400000LL          /* 400 usec debounce (filters bounce) */
#define FRAME_TIMEOUT_NS (50LL * 1000 * 1000) /* 50 msec */

static volatile bool running = true;

struct config {
	char device_id[64];
	int d0;
	int d1;
	char mqtt_host[128];
	int mqtt_port;
	char config_path[256];
	bool skip_meta;
	bool swap_lines;   /* swap D0/D1 mapping */
	bool invert_bits;  /* invert collected bits */
	bool reverse_bits; /* reverse bit order */
};

static void handle_signal(int sig)
{
	(void)sig;
	running = false;
}

static void trim_newline(char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
		s[len - 1] = '\0';
		len--;
	}
}

static int bits_to_uint(const char *bits, size_t start, size_t count)
{
	size_t i;
	int val = 0;

	for (i = 0; i < count; ++i) {
		char b = bits[start + i];
		val <<= 1;
		if (b == '1')
			val |= 1;
	}
	return val;
}

static bool check_parity26(const char *bits)
{
	int i, even = 0, odd = 0;

	for (i = 1; i <= 12; ++i) /* bits 1..12 */
		even ^= (bits[i] == '1');
	for (i = 13; i <= 24; ++i) /* bits 13..24 */
		odd ^= (bits[i] == '1');

	return (bits[0] == (even ? '1' : '0')) &&
	       (bits[25] != (odd ? '1' : '0')); /* last is odd parity */
}

static uint64_t bits_to_u64(const char *bits, size_t len)
{
	size_t i;
	uint64_t val = 0;

	for (i = 0; i < len && i < 64; ++i) {
		val <<= 1;
		if (bits[i] == '1')
			val |= 1ULL;
	}
	return val;
}

static void invert_bits(const char *in, char *out, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		out[i] = (in[i] == '1') ? '0' : '1';
	out[len] = '\0';
}

static void reverse_bits(const char *in, char *out, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		out[i] = in[len - 1 - i];
	out[len] = '\0';
}

static int load_config(struct config *cfg, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];

	if (!f)
		return -errno;

	while (fgets(line, sizeof(line), f)) {
		char *eq, *key, *val;

		trim_newline(line);
		if (line[0] == '#' || line[0] == '\0')
			continue;

		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = line;
		val = eq + 1;

		if (strcmp(key, "D0") == 0)
			cfg->d0 = atoi(val);
		else if (strcmp(key, "D1") == 0)
			cfg->d1 = atoi(val);
		else if (strcmp(key, "DEVICE_ID") == 0)
			strncpy(cfg->device_id, val, sizeof(cfg->device_id) - 1);
		else if (strcmp(key, "MQTT_HOST") == 0)
			strncpy(cfg->mqtt_host, val, sizeof(cfg->mqtt_host) - 1);
		else if (strcmp(key, "MQTT_PORT") == 0)
			cfg->mqtt_port = atoi(val);
		else if (strcmp(key, "SKIP_META") == 0)
			cfg->skip_meta = (strcmp(val, "0") != 0);
		else if (strcmp(key, "SWAP_LINES") == 0)
			cfg->swap_lines = (strcmp(val, "0") != 0);
		else if (strcmp(key, "INVERT_BITS") == 0)
			cfg->invert_bits = (strcmp(val, "0") != 0);
		else if (strcmp(key, "REVERSE_BITS") == 0)
			cfg->reverse_bits = (strcmp(val, "0") != 0);
	}

	fclose(f);
	return 0;
}

static int publish(struct mosquitto *mosq, const char *topic, const char *payload)
{
	return mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload,
				 0, true);
}

static void publish_meta(struct mosquitto *mosq, const char *dev)
{
	char topic[128];

	snprintf(topic, sizeof(topic), "/devices/%s/meta/name", dev);
	publish(mosq, topic, "Wiegand");
	snprintf(topic, sizeof(topic), "/devices/%s/meta/driver", dev);
	publish(mosq, topic, "wb-wiegand-gpiod");

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits/meta/type", dev);
	publish(mosq, topic, "text");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Value/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Facility/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Card/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError/meta/type", dev);
	publish(mosq, topic, "text");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Format/meta/type", dev);
	publish(mosq, topic, "text");

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Value/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Facility/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Card/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Format/meta/readonly", dev);
	publish(mosq, topic, "1");
}

static void publish_frame(struct mosquitto *mosq, const struct config *cfg,
			  const char *bits, size_t len, uint64_t counter)
{
	char topic[128];
	char payload[256];
	const char *error = "";
	const char *format = "unknown";
	int facility = -1, card = -1;
	bool parity_ok = false;
	char base_bits[256];
	char cfg_tmp[256];
	uint64_t raw;
	char candidate[256];
	char tmp[256];
	char salvage[256];
	const char *input = bits;

	/* Apply user-configured transforms (reverse/invert) before autodetect */
	if (len < sizeof(base_bits)) {
		memcpy(base_bits, bits, len);
		base_bits[len] = '\0';
		input = base_bits;
		if (cfg->reverse_bits) {
			reverse_bits(input, cfg_tmp, len);
			input = cfg_tmp;
		}
		if (cfg->invert_bits) {
			invert_bits(input, base_bits, len);
			input = base_bits;
		}
		bits = input;
	}

	/*
	 * Try to salvage a noisy W26 frame that has 1-2 extra/short bits.
	 * Only accept if exactly one window passes parity; otherwise keep len_mismatch.
	 */
	if (len != 26 && len != 34 && len >= 24 && len <= 32) {
		size_t start;
		int found = 0;

		for (start = 0; start + 26 <= len; start++) {
			memcpy(salvage, bits + start, 26);
			salvage[26] = '\0';
			if (check_parity26(salvage)) {
				found++;
				input = salvage;
			}
		}
		if (found == 1) {
			bits = input;
			len = 26;
		}
	}

	raw = bits_to_u64(bits, len);

	if (len == 26) {
		const char *best = bits;
		size_t i;
		format = "w26";

		/* Try four variants: as-is, inverted, reversed, reversed+inverted */
		for (i = 0; i < 4 && !parity_ok; i++) {
			const char *cur = NULL;
			switch (i) {
			case 0:
				cur = bits;
				break;
			case 1:
				invert_bits(bits, candidate, len);
				cur = candidate;
				break;
			case 2:
				reverse_bits(bits, candidate, len);
				cur = candidate;
				break;
			case 3:
				reverse_bits(bits, tmp, len);
				invert_bits(tmp, candidate, len);
				cur = candidate;
				break;
			}
			if (check_parity26(cur)) {
				parity_ok = true;
				best = cur;
				facility = bits_to_uint(cur, 1, 8);
				card = bits_to_uint(cur, 9, 16);
				raw = bits_to_u64(cur, len);
				break;
			}
		}
		if (parity_ok) {
			bits = best; /* publish normalized bits */
		} else {
			error = "parity_fail";
		}
	} else if (len == 34) {
		const char *best = bits;
		size_t i;
		format = "w34";
		/* Wiegand-34 parity: even over bits 1..16, odd over 17..32 */
		for (i = 0; i < 4 && !parity_ok; i++) {
			const char *cur = NULL;
			int even = 0, odd = 0, j;
			switch (i) {
			case 0: cur = bits; break;
			case 1: invert_bits(bits, candidate, len); cur = candidate; break;
			case 2: reverse_bits(bits, candidate, len); cur = candidate; break;
			case 3: reverse_bits(bits, tmp, len); invert_bits(tmp, candidate, len); cur = candidate; break;
			}
			for (j = 1; j <= 16; j++) even ^= (cur[j] == '1');
			for (j = 17; j <= 32; j++) odd ^= (cur[j] == '1');
			if ((cur[0] == (even ? '1' : '0')) && (cur[33] != (odd ? '1' : '0'))) {
				parity_ok = true;
				best = cur;
				facility = bits_to_uint(cur, 1, 16);
				card = bits_to_uint(cur, 17, 16);
				raw = bits_to_u64(cur, len);
				break;
			}
		}
		if (parity_ok) {
			bits = best;
		} else {
			error = "parity_fail";
		}
	} else {
		error = "len_mismatch";
	}

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter", cfg->device_id);
	snprintf(payload, sizeof(payload), "%llu", (unsigned long long)counter);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits", cfg->device_id);
	publish(mosq, topic, bits);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len", cfg->device_id);
	snprintf(payload, sizeof(payload), "%zu", len);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Value", cfg->device_id);
	snprintf(payload, sizeof(payload), "%llu", (unsigned long long)raw);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Facility", cfg->device_id);
	snprintf(payload, sizeof(payload), "%d", facility);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Card", cfg->device_id);
	snprintf(payload, sizeof(payload), "%d", card);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError", cfg->device_id);
	publish(mosq, topic, error);
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Format", cfg->device_id);
	publish(mosq, topic, format);
}

static inline long diff_ns(struct timespec a, struct timespec b)
{
	return (a.tv_sec - b.tv_sec) * 1000000000LL + (a.tv_nsec - b.tv_nsec);
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--d0 N] [--d1 N] [--device ID] [--mqtt-host HOST] [--mqtt-port PORT]\n"
		"          [--config /etc/wb-wiegand.conf] [--skip-meta]\n",
		prog);
}

int main(int argc, char **argv)
{
	struct mosquitto *mosq = NULL;
	struct config cfg = {
		.device_id = DEFAULT_DEVICE_ID,
		.d0 = DEFAULT_D0,
		.d1 = DEFAULT_D1,
		.mqtt_host = DEFAULT_MQTT_HOST,
		.mqtt_port = DEFAULT_MQTT_PORT,
		.config_path = "/etc/wb-wiegand.conf",
		.skip_meta = false,
		.swap_lines = false,
		.invert_bits = false,
		.reverse_bits = false,
	};
	int i, ret = 1;
	struct gpiod_chip *chip = NULL;
	struct gpiod_line *line_d0 = NULL, *line_d1 = NULL;
	int epfd = -1;
	struct epoll_event ev = {0}, events[2];
	char bits[256] = {0};
	int nbits = 0;
	uint64_t counter = 0;
	struct timespec last_event = {0};

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--d0") == 0 && i + 1 < argc) {
			cfg.d0 = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--d1") == 0 && i + 1 < argc) {
			cfg.d1 = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			strncpy(cfg.device_id, argv[++i], sizeof(cfg.device_id) - 1);
		} else if (strcmp(argv[i], "--mqtt-host") == 0 && i + 1 < argc) {
			strncpy(cfg.mqtt_host, argv[++i], sizeof(cfg.mqtt_host) - 1);
		} else if (strcmp(argv[i], "--mqtt-port") == 0 && i + 1 < argc) {
			cfg.mqtt_port = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			strncpy(cfg.config_path, argv[++i], sizeof(cfg.config_path) - 1);
		} else if (strcmp(argv[i], "--skip-meta") == 0) {
			cfg.skip_meta = true;
		} else {
			print_usage(argv[0]);
			return 1;
		}
	}

	if (access(cfg.config_path, R_OK) == 0)
		load_config(&cfg, cfg.config_path);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	mosquitto_lib_init();
	mosq = mosquitto_new(NULL, true, NULL);
	if (!mosq) {
		fprintf(stderr, "mosquitto_new failed\n");
		goto out;
	}

	if (mosquitto_connect(mosq, cfg.mqtt_host, cfg.mqtt_port, 30) != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "MQTT connect failed\n");
		goto out;
	}
	mosquitto_loop_start(mosq);

	if (!cfg.skip_meta)
		publish_meta(mosq, cfg.device_id);

	chip = gpiod_chip_open_by_name(DEFAULT_CHIP);
	if (!chip) {
		perror("gpiod_chip_open_by_name");
		goto out;
	}
	line_d0 = gpiod_chip_get_line(chip, cfg.d0);
	line_d1 = gpiod_chip_get_line(chip, cfg.d1);
	if (!line_d0 || !line_d1) {
		fprintf(stderr, "failed to get lines D0/D1\n");
		goto out;
	}
	if (gpiod_line_request_falling_edge_events(line_d0, "wiegand-gpiod") ||
	    gpiod_line_request_falling_edge_events(line_d1, "wiegand-gpiod")) {
		perror("gpiod_line_request (are lines busy?)");
		goto out;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		perror("epoll_create1");
		goto out;
	}
	ev.events = EPOLLIN;
	ev.data.ptr = line_d0;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, gpiod_line_event_get_fd(line_d0), &ev) < 0) {
		perror("epoll_ctl d0");
		goto out;
	}
	ev.data.ptr = line_d1;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, gpiod_line_event_get_fd(line_d1), &ev) < 0) {
		perror("epoll_ctl d1");
		goto out;
	}

	while (running) {
		int n = epoll_wait(epfd, events, 2, 100);
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (n == 0 && nbits > 0 && last_event.tv_sec != 0 &&
		    diff_ns(now, last_event) > FRAME_TIMEOUT_NS) {
			if (nbits >= 8) {
				counter++;
				publish_frame(mosq, &cfg, bits, (size_t)nbits, counter);
			}
			nbits = 0;
			bits[0] = '\0';
		}
		if (n <= 0)
			continue;

		for (i = 0; i < n; ++i) {
			struct gpiod_line_event evl;
			struct gpiod_line *line = events[i].data.ptr;
			struct timespec ev_ts = evl.ts;
			bool bit_one;

			if (gpiod_line_event_read(line, &evl) < 0)
				continue;
			if (evl.event_type != GPIOD_LINE_EVENT_FALLING_EDGE)
				continue;

			/* If gap exceeded, close out previous frame before taking this bit */
			if (last_event.tv_sec != 0 &&
			    diff_ns(ev_ts, last_event) > FRAME_TIMEOUT_NS) {
				if (nbits >= 8) {
					counter++;
					publish_frame(mosq, &cfg, bits, (size_t)nbits, counter);
				}
				nbits = 0;
				bits[0] = '\0';
			}

			if (last_event.tv_sec != 0 &&
			    diff_ns(ev_ts, last_event) < MIN_PULSE_NS)
				continue;
			last_event = ev_ts;
			if (nbits < (int)sizeof(bits) - 1) {
				bit_one = (line == (cfg.swap_lines ? line_d0 : line_d1));
				bits[nbits++] = bit_one ? '1' : '0';
				bits[nbits] = '\0';
			}
		}
	}

	ret = 0;

out:
	if (epfd >= 0)
		close(epfd);
	if (line_d0)
		gpiod_line_release(line_d0);
	if (line_d1)
		gpiod_line_release(line_d1);
	if (chip)
		gpiod_chip_close(chip);
	if (mosq) {
		mosquitto_loop_stop(mosq, true);
		mosquitto_disconnect(mosq);
		mosquitto_destroy(mosq);
	}
	mosquitto_lib_cleanup();
	return ret;
}
