"""Execute production bitrate boundaries without starting Android resources."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run_c(code, sources=()):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'test.c'
        source.write_text(code)
        binary = Path(directory) / 'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'src'), str(source),
                        *(str(ROOT / p) for p in sources), '-o', str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True, timeout=8)


class VideoBitrateTest(unittest.TestCase):
    def test_config_limits_legacy_and_api_values_preserving_lower_settings(self):
        run_c(r'''
#define _GNU_SOURCE
#include "carplay_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    struct carplay_config config, loaded;
    char error[256], json[4096], path[] = "/tmp/carplay-bitrate-XXXXXX";
    carplay_config_defaults(&config);
    assert(config.video_bitrate == 2000000);
    const char *legacy = "{\"videoBitrate\":8000000,\"videoFps\":20,\"carGps\":false}";
    assert(!carplay_config_parse_json(legacy, strlen(legacy), &config, error, sizeof(error)));
    assert(config.video_bitrate == 2000000 && config.video_fps == 20 && !config.car_gps);
    int fd = mkstemp(path); assert(fd >= 0);
    assert(write(fd, legacy, strlen(legacy)) == (ssize_t)strlen(legacy)); close(fd);
    assert(carplay_config_load(path, &loaded, error, sizeof(error)) == CARPLAY_CONFIG_LOADED);
    assert(loaded.video_bitrate == 2000000 && loaded.video_fps == 20 && !loaded.car_gps);
    config.video_bitrate = 8000000;
    assert(!carplay_config_to_json(&config, json, sizeof(json), NULL));
    assert(strstr(json, "\"videoBitrate\": 2000000"));
    assert(!carplay_config_save(path, &config, error, sizeof(error)));
    assert(carplay_config_load(path, &loaded, error, sizeof(error)) == CARPLAY_CONFIG_LOADED);
    assert(loaded.video_bitrate == 2000000);
    const char *lower = "{\"videoBitrate\":500000}";
    assert(!carplay_config_parse_json(lower, strlen(lower), &config, error, sizeof(error)));
    assert(config.video_bitrate == 500000);
    assert(!carplay_config_save(path, &config, error, sizeof(error)));
    assert(carplay_config_load(path, &loaded, error, sizeof(error)) == CARPLAY_CONFIG_LOADED);
    assert(loaded.video_bitrate == 500000);
    const char *invalid = "{\"videoBitrate\":-1}";
    assert(carplay_config_parse_json(invalid, strlen(invalid), &config, error, sizeof(error)) < 0);
    unlink(path); return 0;
}
''', ['src/carplay_config.c'])

    def test_native_encoder_caps_actual_child_arguments(self):
        run_c(r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int test_access(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int test_open(const char *path, int flags, ...) { (void)path; (void)flags; return open("/dev/null", O_WRONLY); }
static int test_execve(const char *file, char *const args[], char *const env[]) {
    (void)file; (void)env;
    assert(write(STDOUT_FILENO, args[6], strlen(args[6])) == (ssize_t)strlen(args[6]));
    _exit(0);
}
#define access test_access
#define open test_open
#define execve test_execve
#include "carplay_encoder.c"
#undef access
#undef open
#undef execve
int main(void) {
    unsigned values[] = {8000000, 2000000, 500000, 4294967295u};
    const char *expected[] = {"2000000", "2000000", "500000", "2000000"};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        struct carplay_encoder encoder; carplay_encoder_init(&encoder);
        assert(!carplay_encoder_start(&encoder, 1280, 720, values[i], 30));
        char bitrate[32] = {0}; ssize_t n = read(encoder.fd, bitrate, sizeof(bitrate)-1);
        assert(n > 0 && !strcmp(bitrate, expected[i]));
        carplay_encoder_stop(&encoder);
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
