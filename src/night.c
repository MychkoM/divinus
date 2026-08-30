#include "night.h"

#include <fcntl.h>
#include <time.h>

/* ---- [divinus-142] auto day/night from the sensor itself (light_source: sensor) ----
 * This board has NO light sensor on any SoC pin (ir_sensor_pin 6 = GPIO0_6
 * SENSOR_CLK, a fake -- auto by it never fires), so darkness is read from the
 * SC1035's own AE state over /dev/hi_i2c:
 *   0x3e01:0x3e02 = integration time: lines = (r1<<4)|(r2>>4), 2..998 (998=pegged)
 *   0x3e09        = analog gain, tier in bits[6:4]: 0=1x .. 9=~7x .. 15=~28x+
 * dark   := exp >= dark_exposure_min   && tier >= dark_gain_tier
 * bright := exp <= bright_exposure_max && tier <= bright_gain_tier
 * in between: keep the current verdict (hysteresis band). Auto switches are
 * further spaced by switch_hold_s so IR-LED washout of the scene cannot
 * oscillate day/night. */
#define NL_I2C_DEV   "/dev/hi_i2c"
#define NL_I2C_ADDR  0x60
#define NL_CMD_READ  0x03

struct nl_i2c_r {            /* same ABI as /dev/hi_i2c CMD_I2C_READ payload */
    unsigned char dev_addr;
    unsigned char pad_[3];
    unsigned int  reg_addr;
    unsigned int  addr_byte_num;
    unsigned int  data;
    unsigned int  data_byte_num;
};

static int nl_lines, nl_tier, nl_dark = -1;

int night_light_last(int *lines, int *tier, int *dark)
{
    if (lines) *lines = nl_lines;
    if (tier) *tier = nl_tier;
    if (dark) *dark = nl_dark;
    return 0;
}

static int night_light_state(void)
{
    struct nl_i2c_r d;
    unsigned int r1, r2, r9;
    int fd = open(NL_I2C_DEV, 0);

    if (fd < 0) return -1;
    memset(&d, 0, sizeof d);
    d.dev_addr = NL_I2C_ADDR;
    d.addr_byte_num = 2;
    d.data_byte_num = 1;
    d.reg_addr = 0x3e01;
    if (ioctl(fd, NL_CMD_READ, &d)) { close(fd); return -1; }
    r1 = d.data & 0xff;
    d.reg_addr = 0x3e02;
    if (ioctl(fd, NL_CMD_READ, &d)) { close(fd); return -1; }
    r2 = d.data & 0xff;
    d.reg_addr = 0x3e09;
    if (ioctl(fd, NL_CMD_READ, &d)) { close(fd); return -1; }
    r9 = d.data & 0xff;
    close(fd);

    nl_lines = (int)((r1 << 4) | (r2 >> 4));
    nl_tier = (int)(r9 >> 4);
    if (nl_lines >= app_config.dark_exposure_min &&
        nl_tier >= app_config.dark_gain_tier)
        nl_dark = 1;
    else if (nl_lines <= app_config.bright_exposure_max &&
             nl_tier <= app_config.bright_gain_tier)
        nl_dark = 0;
    /* else: inside the hysteresis band -> keep the previous verdict */
    return 0;
}

char nightOn = 0;
static bool grayscale = false, ircut = true, irled = false, manual = false;
pthread_t nightPid = 0;

bool night_grayscale_on(void) { return grayscale; }

bool night_ircut_on(void) { return ircut; }

bool night_irled_on(void) { return irled; }

bool night_manual_on(void) { return manual; }

bool night_mode_on(void) { return grayscale && !ircut && irled; }

void night_grayscale(bool enable) {
    set_grayscale(enable);
    grayscale = enable;
}

void night_ircut(bool enable) {
    gpio_write(app_config.ir_cut_pin1, !enable);
    gpio_write(app_config.ir_cut_pin2, enable);
    usleep(app_config.pin_switch_delay_us * 100);
    gpio_write(app_config.ir_cut_pin1, false);
    gpio_write(app_config.ir_cut_pin2, false);
    ircut = enable;
}

void night_irled(bool enable) {
    gpio_write(app_config.ir_led_pin, enable);
    irled = enable;
}

void night_manual(bool enable) { manual = enable; }

// Sleep in short slices so night_disable() (pthread_join) does not wait for a
// full check_interval_s; web/api callers get an answer in <0.5 s.
static void night_sleep(unsigned seconds)
{
    for (unsigned i = 0; i < seconds * 2; i++)
    {
        if (!nightOn || !keepRunning) return;
        usleep(500000);
    }
}

void night_mode(bool enable) {
    // Idempotent: re-applying every poll cycle hammers the IR-cut coil and
    // re-arms VENC grayscale constantly. Only act on an actual transition.
    if (grayscale == enable && ircut == !enable && irled == enable)
        return;
    HAL_INFO("night", "Changing mode to %s\n", enable ? "NIGHT" : "DAY");
    night_grayscale(enable);
    night_ircut(!enable);
    night_irled(enable);
}

void *night_thread(void) {
    gpio_init();
    usleep(10000);

    // In manual mode the thread must not recompute the composite night state:
    // every /api/night query restarts this thread, and re-applying
    // night_mode(night_mode_on()) here would silently undo single manual
    // toggles (e.g. grayscale=1 alone fails the AND and forces a DAY reset).
    if (!manual)
        night_mode(night_mode_on());

    /* [divinus-142] config-locked mode (mode: day|night): apply once and hold;
     * runtime API manual toggles still win over it. */
    if (!strcmp(app_config.night_mode_cfg, "day") ||
        !strcmp(app_config.night_mode_cfg, "night")) {
        if (!manual)
            night_mode(app_config.night_mode_cfg[0] == 'n');
        while (keepRunning && nightOn)
            night_sleep(3600);
        usleep(10000);
        gpio_deinit();
        HAL_INFO("night", "Night mode thread is closing...\n");
        nightOn = 0;
        return NULL;
    }

    if (app_config.adc_device[0]) {
        int adc_fd = -1;
        fd_set adc_fds;
        int cnt = 0, tmp = 0, val;

        if ((adc_fd = open(app_config.adc_device, O_RDONLY | O_NONBLOCK)) <= 0) {
            HAL_DANGER("night", "Could not open the ADC virtual device!\n");
            return NULL;
        }
        while (keepRunning && nightOn) {
            if (read(adc_fd, &val, sizeof(val)) > 0) {
                usleep(10000);
                tmp += val;
                cnt++;
            }
            if (cnt == 12) {
                tmp /= cnt;
                if (!manual) night_mode(tmp >= app_config.adc_threshold);
                cnt = tmp = 0;
            }
            usleep(app_config.check_interval_s * 1000000 / 12);
        }
        if (adc_fd) close(adc_fd);
    } else if (!strcmp(app_config.night_mode_cfg, "auto") &&
               !strcmp(app_config.light_source, "sensor")) {
        /* [divinus-142] AE-state detector: SC1035 exposure+gain = darkness */
        time_t last_flip = 0;
        while (keepRunning && nightOn) {
            if (night_light_state() == 0 && nl_dark >= 0 && !manual) {
                int want = nl_dark ? 1 : 0;
                time_t now = time(NULL);
                if (want != (int)night_mode_on() &&
                    (now - last_flip) >= (time_t)app_config.switch_hold_s) {
                    HAL_INFO("night", "auto -> %s (exp=%d tier=%d)\n",
                        want ? "NIGHT" : "DAY", nl_lines, nl_tier);
                    night_mode(want);
                    last_flip = now;
                }
            }
            night_sleep(app_config.check_interval_s);
        }
    } else if (app_config.ir_sensor_pin == 999) {
        while (keepRunning) sleep(1);
    } else {
        while (keepRunning && nightOn) {
            bool state = false;
            if (!gpio_read(app_config.ir_sensor_pin, &state))
                if (!manual) night_mode(state);

            night_sleep(app_config.check_interval_s);
        }
    }

    usleep(10000);
    gpio_deinit();
    HAL_INFO("night", "Night mode thread is closing...\n");
    nightOn = 0;
}

int night_enable(void) {
    int ret = EXIT_SUCCESS;

    if (nightOn) return ret;

    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    size_t stacksize;
    pthread_attr_getstacksize(&thread_attr, &stacksize);
    size_t new_stacksize = 16 * 1024;
    if (pthread_attr_setstacksize(&thread_attr, new_stacksize))
        HAL_DANGER("night", "Error:  Can't set stack size %zu\n", new_stacksize);
    pthread_create(&nightPid, &thread_attr, (void *(*)(void *))night_thread, NULL);
    if (pthread_attr_setstacksize(&thread_attr, stacksize))
        HAL_DANGER("night", "Error:  Can't set stack size %zu\n", stacksize);
    pthread_attr_destroy(&thread_attr);

    nightOn = 1;

    return ret;
}

void night_disable(void) {
    if (!nightOn) return;

    nightOn = 0;
    pthread_join(nightPid, NULL);
}
