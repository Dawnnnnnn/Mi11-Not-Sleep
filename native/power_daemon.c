/*
 * power_daemon - Power button interceptor for server phone module
 *
 * Grabs the physical power key input device exclusively via EVIOCGRAB,
 * then classifies presses as short or long:
 *   - Short press (<400ms): toggles screen brightness between 0 and saved value
 *   - Long press (>=400ms): forwards to a uinput virtual device so Android
 *     shows the power menu as usual
 *
 * All non-power-key events (e.g. volume down) are forwarded transparently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define DEFAULT_INPUT_DEV  "/dev/input/event0"
#define DEFAULT_TOUCH_DEV  "/dev/input/event4"
#define DEFAULT_VOLUP_DEV  "/dev/input/event2"
#define BRIGHTNESS_SYSFS   "/sys/class/backlight/panel0-backlight/brightness"

#define SHORT_PRESS_MS      400
#define DOUBLE_TAP_MS       400
#define MIN_FORWARD_HOLD_MS 800
#define DEFAULT_BRIGHTNESS  100
#define OPEN_RETRY_COUNT    30
#define OPEN_RETRY_DELAY    2
#define EMERGENCY_COUNT     5
#define EMERGENCY_WINDOW_MS 3000

typedef enum {
    STATE_IDLE,
    STATE_FIRST_DOWN,
    STATE_WAIT_SECOND,
    STATE_SECOND_DOWN,
    STATE_FORWARDING,
    STATE_DELAY_UP,
} State;

static volatile sig_atomic_t g_running = 1;
static int g_input_fd  = -1;
static int g_uinput_fd = -1;
static int g_touch_fd  = -1;
static int g_volup_fd  = -1;
static const char *g_touch_dev = DEFAULT_TOUCH_DEV;
static int g_dimmed = 0;
static int g_saved_sysfs = DEFAULT_BRIGHTNESS;
static int g_saved_settings = DEFAULT_BRIGHTNESS;
static long long g_volup_times[EMERGENCY_COUNT];
static int g_volup_idx = 0;

static void log_msg(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void emit(int fd, int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = val;
    write(fd, &ev, sizeof(ev));
}

static void emit_syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        log_msg("open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, KEY_POWER);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof(ud));
    strncpy(ud.name, "server-phone-keys", UINPUT_MAX_NAME_SIZE - 1);
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor  = 0x1234;
    ud.id.product = 0x5678;
    ud.id.version = 1;

    if (write(fd, &ud, sizeof(ud)) != sizeof(ud)) {
        log_msg("write uinput_user_dev: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        log_msg("UI_DEV_CREATE: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int read_sysfs_brightness(void) {
    int fd = open(BRIGHTNESS_SYSFS, O_RDONLY);
    if (fd < 0) return -1;
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static void write_sysfs_brightness(int value) {
    int fd = open(BRIGHTNESS_SYSFS, O_WRONLY);
    if (fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", value);
        write(fd, buf, len);
        close(fd);
    }
}

static int read_settings_brightness(void) {
    FILE *fp = popen("settings get system screen_brightness 2>/dev/null", "r");
    if (!fp) return -1;
    char buf[32];
    int val = -1;
    if (fgets(buf, sizeof(buf), fp))
        val = atoi(buf);
    pclose(fp);
    return val;
}

static void disable_touch(void) {
    if (g_touch_fd >= 0) return;
    g_touch_fd = open(g_touch_dev, O_RDONLY | O_NONBLOCK);
    if (g_touch_fd < 0) {
        log_msg("open touch %s: %s", g_touch_dev, strerror(errno));
        return;
    }
    if (ioctl(g_touch_fd, EVIOCGRAB, 1) < 0) {
        log_msg("grab touch: %s", strerror(errno));
        close(g_touch_fd);
        g_touch_fd = -1;
        return;
    }
    log_msg("touch disabled");
}

static void enable_touch(void) {
    if (g_touch_fd < 0) return;
    ioctl(g_touch_fd, EVIOCGRAB, 0);
    close(g_touch_fd);
    g_touch_fd = -1;
    log_msg("touch enabled");
}

static void emergency_restore(void) {
    enable_touch();
    write_sysfs_brightness(2047);
    system("settings put system screen_brightness 255");
    g_dimmed = 0;
    memset(g_volup_times, 0, sizeof(g_volup_times));
    log_msg("EMERGENCY: brightness maxed, touch enabled");
}

static void toggle_brightness(void) {
    if (g_dimmed) {
        enable_touch();

        write_sysfs_brightness(2047);
        usleep(20000);
        write_sysfs_brightness(g_saved_sysfs);

        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "settings put system screen_brightness %d", g_saved_settings);
        system(cmd);

        usleep(500000);
        write_sysfs_brightness(g_saved_sysfs);

        log_msg("restored sysfs=%d settings=%d", g_saved_sysfs, g_saved_settings);
        g_dimmed = 0;
    } else {
        int cur_sysfs = read_sysfs_brightness();
        if (cur_sysfs > 0)
            g_saved_sysfs = cur_sysfs;

        int cur_set = read_settings_brightness();
        if (cur_set >= 0)
            g_saved_settings = cur_set;
        if (g_saved_settings <= 0 && g_saved_sysfs <= 0) {
            g_saved_sysfs = DEFAULT_BRIGHTNESS;
            g_saved_settings = DEFAULT_BRIGHTNESS;
        }

        write_sysfs_brightness(0);
        system("settings put system screen_brightness 0");
        usleep(500000);
        write_sysfs_brightness(0);

        disable_touch();

        log_msg("dimmed (saved sysfs=%d settings=%d)", g_saved_sysfs, g_saved_settings);
        g_dimmed = 1;
    }
}

static void acquire_wakelock(void) {
    int fd = open("/sys/power/wake_lock", O_WRONLY);
    if (fd >= 0) {
        write(fd, "power_daemon", 12);
        close(fd);
    }
}

static void release_wakelock(void) {
    int fd = open("/sys/power/wake_unlock", O_WRONLY);
    if (fd >= 0) {
        write(fd, "power_daemon", 12);
        close(fd);
    }
}

static void cleanup(int sig) {
    (void)sig;
    g_running = 0;
}

static void cleanup_exit(void) {
    enable_touch();
    if (g_input_fd >= 0) {
        ioctl(g_input_fd, EVIOCGRAB, 0);
        close(g_input_fd);
        g_input_fd = -1;
    }
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
    release_wakelock();
    log_msg("daemon exited cleanly");
}

int main(int argc, char *argv[]) {
    const char *input_dev = (argc > 1) ? argv[1] : DEFAULT_INPUT_DEV;
    if (argc > 2) g_touch_dev = argv[2];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cleanup;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    log_msg("starting, input=%s", input_dev);

    acquire_wakelock();

    /* Open input device with retries */
    for (int i = 0; i < OPEN_RETRY_COUNT; i++) {
        g_input_fd = open(input_dev, O_RDONLY);
        if (g_input_fd >= 0) break;
        log_msg("open %s: %s (retry %d/%d)",
                input_dev, strerror(errno), i + 1, OPEN_RETRY_COUNT);
        sleep(OPEN_RETRY_DELAY);
    }
    if (g_input_fd < 0) {
        log_msg("FATAL: cannot open %s", input_dev);
        release_wakelock();
        return 1;
    }

    if (ioctl(g_input_fd, EVIOCGRAB, 1) < 0) {
        log_msg("FATAL: EVIOCGRAB: %s", strerror(errno));
        close(g_input_fd);
        release_wakelock();
        return 1;
    }
    log_msg("grabbed %s", input_dev);

    g_uinput_fd = setup_uinput();
    if (g_uinput_fd < 0) {
        log_msg("FATAL: setup_uinput failed");
        ioctl(g_input_fd, EVIOCGRAB, 0);
        close(g_input_fd);
        release_wakelock();
        return 1;
    }
    log_msg("uinput device created");
    usleep(500000);

    /* Open volume-up device (no grab — just listen) */
    const char *volup_dev = (argc > 3) ? argv[3] : DEFAULT_VOLUP_DEV;
    g_volup_fd = open(volup_dev, O_RDONLY | O_NONBLOCK);
    if (g_volup_fd >= 0)
        log_msg("listening on %s for emergency", volup_dev);
    else
        log_msg("open %s: %s (emergency key unavailable)", volup_dev, strerror(errno));

    State state = STATE_IDLE;
    long long press_time = 0, release_time = 0, fwd_time = 0;
    int skip_syn = 0;

    while (g_running) {
        int timeout_ms = -1;

        switch (state) {
        case STATE_FIRST_DOWN: {
            int rem = SHORT_PRESS_MS - (int)(now_ms() - press_time);
            timeout_ms = (rem > 0) ? rem : 0;
            break;
        }
        case STATE_WAIT_SECOND: {
            int rem = DOUBLE_TAP_MS - (int)(now_ms() - release_time);
            timeout_ms = (rem > 0) ? rem : 0;
            break;
        }
        case STATE_DELAY_UP: {
            int rem = MIN_FORWARD_HOLD_MS - (int)(now_ms() - fwd_time);
            timeout_ms = (rem > 0) ? rem : 0;
            break;
        }
        default:
            timeout_ms = -1;
            break;
        }

        struct pollfd pfds[2];
        pfds[0].fd = g_input_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = g_volup_fd;
        pfds[1].events = (g_volup_fd >= 0) ? POLLIN : 0;
        int nfds = (g_volup_fd >= 0) ? 2 : 1;

        int ret = poll(pfds, nfds, timeout_ms);

        if (ret == 0) {
            switch (state) {
            case STATE_FIRST_DOWN:
                emit(g_uinput_fd, EV_KEY, KEY_POWER, 1);
                emit_syn(g_uinput_fd);
                fwd_time = now_ms();
                state = STATE_FORWARDING;
                log_msg("long press, forwarding key down");
                break;
            case STATE_WAIT_SECOND:
                state = STATE_IDLE;
                break;
            case STATE_DELAY_UP:
                emit(g_uinput_fd, EV_KEY, KEY_POWER, 0);
                emit_syn(g_uinput_fd);
                state = STATE_IDLE;
                log_msg("delayed key up forwarded");
                break;
            default:
                break;
            }
            continue;
        }
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_msg("poll: %s", strerror(errno));
            break;
        }

        /* --- Volume Up: emergency restore --- */
        if (nfds > 1 && (pfds[1].revents & POLLIN)) {
            struct input_event vev;
            while (read(g_volup_fd, &vev, sizeof(vev)) == (ssize_t)sizeof(vev)) {
                if (vev.type == EV_KEY && vev.code == KEY_VOLUMEUP && vev.value == 1) {
                    g_volup_times[g_volup_idx] = now_ms();
                    g_volup_idx = (g_volup_idx + 1) % EMERGENCY_COUNT;
                    long long oldest = g_volup_times[g_volup_idx];
                    if (oldest > 0 && now_ms() - oldest <= EMERGENCY_WINDOW_MS)
                        emergency_restore();
                }
            }
        }

        /* --- Power button: main state machine --- */
        if (!(pfds[0].revents & POLLIN))
            continue;

        struct input_event ev;
        if (read(g_input_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            if (errno == EINTR) continue;
            log_msg("read: %s", strerror(errno));
            break;
        }

        if (ev.type == EV_KEY && ev.code == KEY_POWER) {
            skip_syn = 1;

            if (ev.value == 1) { /* key down */
                switch (state) {
                case STATE_WAIT_SECOND:
                    press_time = now_ms();
                    state = STATE_SECOND_DOWN;
                    break;
                default:
                    press_time = now_ms();
                    state = STATE_FIRST_DOWN;
                    break;
                }
            } else if (ev.value == 0) { /* key up */
                switch (state) {
                case STATE_FIRST_DOWN:
                    release_time = now_ms();
                    state = STATE_WAIT_SECOND;
                    break;
                case STATE_SECOND_DOWN:
                    toggle_brightness();
                    state = STATE_IDLE;
                    break;
                case STATE_FORWARDING:
                    if (now_ms() - fwd_time >= MIN_FORWARD_HOLD_MS) {
                        emit(g_uinput_fd, EV_KEY, KEY_POWER, 0);
                        emit_syn(g_uinput_fd);
                        state = STATE_IDLE;
                        log_msg("key up forwarded");
                    } else {
                        state = STATE_DELAY_UP;
                    }
                    break;
                default:
                    state = STATE_IDLE;
                    break;
                }
            }
        } else if (ev.type == EV_SYN && skip_syn) {
            skip_syn = 0;
        } else {
            write(g_uinput_fd, &ev, sizeof(ev));
        }
    }

    cleanup_exit();
    return 0;
}
