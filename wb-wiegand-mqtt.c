#include <errno.h>
#include <fcntl.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SYSFS_PATH "/sys/kernel/wiegand/read"
#define DEFAULT_DEVICE_ID "wiegand"
#define DEFAULT_D0 228
#define DEFAULT_D1 233
#define DEFAULT_MQTT_HOST "localhost"
#define DEFAULT_MQTT_PORT 1883

static volatile bool running = true;

struct config {
	char device_id[64];
	int d0;
	int d1;
	char mqtt_host[128];
	int mqtt_port;
	char config_path[256];
	bool autoload_module;
	bool skip_meta;
};

struct wiegand_frame {
	char bits[256];
	size_t len;
	uint64_t counter;
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
		else if (strcmp(key, "AUTOLOAD_MODULE") == 0)
			cfg->autoload_module = (strcmp(val, "0") != 0);
		else if (strcmp(key, "SKIP_META") == 0)
			cfg->skip_meta = (strcmp(val, "0") != 0);
	}

	fclose(f);
	return 0;
}

static int ensure_module_loaded(const struct config *cfg)
{
	char cmd[256];
	int ret;

	if (!cfg->autoload_module)
		return 0;

	snprintf(cmd, sizeof(cmd),
		 "modprobe -q wiegand-gpio D0=%d D1=%d", cfg->d0, cfg->d1);
	ret = system(cmd);
	return ret;
}

static int read_frame(struct wiegand_frame *out)
{
	char buf[512];
	ssize_t n;
	int fd;
	char *colon;

	fd = open(SYSFS_PATH, O_RDONLY);
	if (fd < 0)
		return -errno;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;

	buf[n] = '\0';
	trim_newline(buf);

	colon = strchr(buf, ':');
	if (!colon)
		return -EINVAL;

	*colon = '\0';
	out->counter = strtoull(buf, NULL, 10);
	strncpy(out->bits, colon + 1, sizeof(out->bits) - 1);
	out->bits[sizeof(out->bits) - 1] = '\0';
	out->len = strlen(out->bits);

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
	publish(mosq, topic, "wb-mqtt-wiegand");

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits/meta/type", dev);
	publish(mosq, topic, "text");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/FacilityCode/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/CardNumber/meta/type", dev);
	publish(mosq, topic, "value");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError/meta/type", dev);
	publish(mosq, topic, "text");

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/FacilityCode/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/CardNumber/meta/readonly", dev);
	publish(mosq, topic, "1");
	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError/meta/readonly", dev);
	publish(mosq, topic, "1");
}

static void publish_frame(struct mosquitto *mosq, const struct config *cfg,
			  const struct wiegand_frame *frame)
{
	char topic[128];
	char payload[256];
	const char *error = "";
	int facility = -1, card = -1;
	bool parity_ok = false;

	if (frame->len == 26) {
		parity_ok = check_parity26(frame->bits);
		if (parity_ok) {
			facility = bits_to_uint(frame->bits, 1, 8);
			card = bits_to_uint(frame->bits, 9, 16);
		} else {
			error = "parity_fail";
		}
	} else {
		error = "len_mismatch";
	}

	snprintf(topic, sizeof(topic), "/devices/%s/controls/ReadCounter", cfg->device_id);
	snprintf(payload, sizeof(payload), "%llu", (unsigned long long)frame->counter);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Bits", cfg->device_id);
	publish(mosq, topic, frame->bits);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/Len", cfg->device_id);
	snprintf(payload, sizeof(payload), "%zu", frame->len);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/FacilityCode", cfg->device_id);
	snprintf(payload, sizeof(payload), "%d", facility);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/CardNumber", cfg->device_id);
	snprintf(payload, sizeof(payload), "%d", card);
	publish(mosq, topic, payload);

	snprintf(topic, sizeof(topic), "/devices/%s/controls/LastError", cfg->device_id);
	publish(mosq, topic, error);
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--d0 N] [--d1 N] [--device ID] [--mqtt-host HOST] [--mqtt-port PORT]\n"
		"          [--config /etc/wb-wiegand.conf] [--no-modprobe] [--skip-meta]\n",
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
		.autoload_module = true,
		.skip_meta = false,
	};
	int i, ret = 1;
	int in_fd = -1;
	int wd;
	struct pollfd fds[1];

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
		} else if (strcmp(argv[i], "--no-modprobe") == 0) {
			cfg.autoload_module = false;
		} else if (strcmp(argv[i], "--skip-meta") == 0) {
			cfg.skip_meta = true;
		} else {
			print_usage(argv[0]);
			return 1;
		}
	}

	if (access(cfg.config_path, R_OK) == 0)
		load_config(&cfg, cfg.config_path);

	if (ensure_module_loaded(&cfg) != 0) {
		fprintf(stderr, "Failed to modprobe wiegand-gpio\n");
		return 1;
	}

	if (access(SYSFS_PATH, R_OK) != 0) {
		fprintf(stderr, "Sysfs path %s not available\n", SYSFS_PATH);
		return 1;
	}

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

	in_fd = inotify_init1(IN_NONBLOCK);
	if (in_fd < 0) {
		perror("inotify_init1");
		goto out;
	}

	wd = inotify_add_watch(in_fd, SYSFS_PATH, IN_MODIFY);
	if (wd < 0) {
		perror("inotify_add_watch");
		goto out;
	}

	fds[0].fd = in_fd;
	fds[0].events = POLLIN;

	while (running) {
		struct wiegand_frame frame;
		char buf[sizeof(struct inotify_event) + 32];
		int poll_ret;

		poll_ret = poll(fds, 1, 1000);
		if (poll_ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		} else if (poll_ret == 0) {
			continue;
		}

		if (fds[0].revents & POLLIN) {
			/* Drain inotify */
			read(in_fd, buf, sizeof(buf));

			if (read_frame(&frame) == 0)
				publish_frame(mosq, &cfg, &frame);
		}
	}

	ret = 0;

out:
	if (in_fd >= 0)
		close(in_fd);
	if (mosq) {
		mosquitto_loop_stop(mosq, true);
		mosquitto_disconnect(mosq);
		mosquitto_destroy(mosq);
	}
	mosquitto_lib_cleanup();
	return ret;
}
