#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>

#define DEV_ALARM_IO   "/dev/alarm_io"
#define DEV_ALARM_KEY  "/dev/alarm_key"
#define DEV_BH1750     "/dev/bh1750"
#define DEV_MPU6050    "/dev/mpu6050"

#define DARK_LUX_THRESH        100
#define ACCEL_DELTA_THRESH    4000
#define GYRO_DELTA_THRESH     6000
#define MOTION_HIT_REQUIRED      3

#define LOOP_INTERVAL_MS       100
#define ARMED_BLINK_MS         500
#define ALARM_BLINK_MS         200
#define ALARM_BEEP_MS          200

struct mpu6050_frame {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t temp_raw;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
};

enum alarm_state {
    STATE_IDLE = 0,
    STATE_ARMED,
    STATE_ALARM,
    STATE_MUTE,
};

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int abs_int(int x)
{
    return x < 0 ? -x : x;
}

static float mpu_temp_c(int16_t raw)
{
    return raw / 340.0f + 36.53f;
}

static int write_alarm_io(int fd, const char *name, int value)
{
    char buf[32];
    int len;
    ssize_t ret;

    len = snprintf(buf, sizeof(buf), "%s %d", name, value ? 1 : 0);
    ret = write(fd, buf, len);
    if (ret < 0) 
	{
        perror("write /dev/alarm_io");
        return -1;
    }

    return 0;
}

static int set_blue(int fd, int on)   { return write_alarm_io(fd, "blue", on); }
static int set_red(int fd, int on)    { return write_alarm_io(fd, "red", on); }
static int set_buzzer(int fd, int on) { return write_alarm_io(fd, "buzzer", on); }

static int read_key_event(int fd, int *event)
{
    int value = 0;
    ssize_t ret;

    ret = read(fd, &value, sizeof(value));
    if (ret < 0) 
	{
        perror("read /dev/alarm_key");
        return -1;
    }
    if (ret != (ssize_t)sizeof(value)) 
	{
        fprintf(stderr, "alarm_key read size invalid: %zd\n", ret);
        return -1;
    }

    *event = value;
    return 0;
}

static int read_lux(int fd, int *lux)
{
    int value = 0;
    ssize_t ret;

    ret = read(fd, &value, sizeof(value));
    if (ret < 0) 
	{
        perror("read /dev/bh1750");
        return -1;
    }
    if (ret != (ssize_t)sizeof(value)) 
	{
        fprintf(stderr, "bh1750 read size invalid: %zd\n", ret);
        return -1;
    }

    *lux = value;
    return 0;
}

static int read_mpu(int fd, struct mpu6050_frame *frame)
{
    ssize_t ret;

    ret = read(fd, frame, sizeof(*frame));
    if (ret < 0) 
	{
        perror("read /dev/mpu6050");
        return -1;
    }
    if (ret != (ssize_t)sizeof(*frame)) 
	{
        fprintf(stderr, "mpu6050 read size invalid: %zd\n", ret);
        return -1;
    }

    return 0;
}

static int motion_detected(const struct mpu6050_frame *cur, const struct mpu6050_frame *prev, int *accel_delta_sum, int *gyro_delta_sum)
{
    int ad, gd;

    ad = abs_int(cur->accel_x - prev->accel_x) + abs_int(cur->accel_y - prev->accel_y) + abs_int(cur->accel_z - prev->accel_z);

    gd = abs_int(cur->gyro_x - prev->gyro_x) + abs_int(cur->gyro_y - prev->gyro_y) + abs_int(cur->gyro_z - prev->gyro_z);

    if (accel_delta_sum)
        *accel_delta_sum = ad;
    if (gyro_delta_sum)
        *gyro_delta_sum = gd;

    return (ad > ACCEL_DELTA_THRESH || gd > GYRO_DELTA_THRESH);
}

static void apply_state_outputs(int fd_alarm_io, enum alarm_state state, long long now, long long *last_led_toggle,
                                long long *last_buzzer_toggle, int *blue_on, int *red_on, int *buzzer_on)
{
    switch (state) {
    case STATE_IDLE:
        if (!*blue_on) 
		{
            set_blue(fd_alarm_io, 1);
            *blue_on = 1;
        }
        if (*red_on) 
		{
            set_red(fd_alarm_io, 0);
            *red_on = 0;
        }
        if (*buzzer_on) 
		{
            set_buzzer(fd_alarm_io, 0);
            *buzzer_on = 0;
        }
        break;

    case STATE_ARMED:
        if (*red_on) 
		{
            set_red(fd_alarm_io, 0);
            *red_on = 0;
        }
        if (*buzzer_on) 
		{
            set_buzzer(fd_alarm_io, 0);
            *buzzer_on = 0;
        }
        if (now - *last_led_toggle >= ARMED_BLINK_MS) 
		{
            *blue_on = !(*blue_on);
            set_blue(fd_alarm_io, *blue_on);
            *last_led_toggle = now;
        }
        break;

    case STATE_ALARM:
        if (*blue_on) 
		{
            set_blue(fd_alarm_io, 0);
            *blue_on = 0;
        }
        if (now - *last_led_toggle >= ALARM_BLINK_MS) 
		{
            *red_on = !(*red_on);
            set_red(fd_alarm_io, *red_on);
            *last_led_toggle = now;
        }
        if (now - *last_buzzer_toggle >= ALARM_BEEP_MS) 
		{
            *buzzer_on = !(*buzzer_on);
            set_buzzer(fd_alarm_io, *buzzer_on);
            *last_buzzer_toggle = now;
        }
        break;

    case STATE_MUTE:
        if (*blue_on)
		{
            set_blue(fd_alarm_io, 0);
            *blue_on = 0;
        }
        if (*buzzer_on) 
		{
            set_buzzer(fd_alarm_io, 0);
            *buzzer_on = 0;
        }
        if (now - *last_led_toggle >= ALARM_BLINK_MS) 
		{
            *red_on = !(*red_on);
            set_red(fd_alarm_io, *red_on);
            *last_led_toggle = now;
        }
        break;
    }
}

static const char *state_to_string(enum alarm_state state)
{
    switch (state) 
	{
    	case STATE_IDLE:  return "IDLE";
    	case STATE_ARMED: return "ARMED";
    	case STATE_ALARM: return "ALARM";
    	case STATE_MUTE:  return "MUTE";
    	default:          return "UNKNOWN";
    }
}

static int write_status_json(enum alarm_state state, int lux, int dark_env, int motion, int motion_hits, int accel_delta,
                             int gyro_delta, float temp)
{
    FILE *fp;
    const char *tmp_path = "/tmp/alarm_status.json.tmp";
    const char *real_path = "/tmp/alarm_status.json";

    fp = fopen(tmp_path, "w");
    if (!fp) {
        perror("fopen status tmp");
        return -1;
    }

    fprintf(fp,
            "{\n"
            "  \"state\": \"%s\",\n"
            "  \"lux\": %d,\n"
            "  \"dark\": %d,\n"
            "  \"motion\": %d,\n"
            "  \"motion_hits\": %d,\n"
            "  \"accel_delta\": %d,\n"
            "  \"gyro_delta\": %d,\n"
            "  \"temp\": %.2f,\n"
            "  \"timestamp\": %lld\n"
            "}\n",
            state_to_string(state), lux, dark_env, motion, motion_hits, accel_delta, gyro_delta, temp, now_ms());

    fclose(fp);

    if (rename(tmp_path, real_path) < 0) 
	{
        perror("rename status json");
        return -1;
    }

    return 0;
}

int main(void)
{
    int fd_alarm_io = -1;
    int fd_alarm_key = -1;
    int fd_bh1750 = -1;
    int fd_mpu6050 = -1;

    struct pollfd pfd;
    int poll_ret;

    enum alarm_state state = STATE_IDLE;
    struct mpu6050_frame cur_frame = {0};
    struct mpu6050_frame prev_frame = {0};

    int first_mpu = 1;
    int key_event = 0;
    int lux = 0;
    int dark_env = 0;
    int motion = 0;
    int motion_hits = 0;
    int accel_delta = 0;
    int gyro_delta = 0;

    int blue_on = 0;
    int red_on = 0;
    int buzzer_on = 0;

    long long t_now;
    long long last_led_toggle = 0;
    long long last_buzzer_toggle = 0;
    long long last_status_print = 0;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fd_alarm_io = open(DEV_ALARM_IO, O_WRONLY);
    if (fd_alarm_io < 0) 
	{
        perror("open /dev/alarm_io");
        goto out;
    }

    fd_alarm_key = open(DEV_ALARM_KEY, O_RDONLY);
    if (fd_alarm_key < 0) 
	{
        perror("open /dev/alarm_key");
        goto out;
    }

    fd_bh1750 = open(DEV_BH1750, O_RDONLY);
    if (fd_bh1750 < 0) 
	{
        perror("open /dev/bh1750");
        goto out;
    }

    fd_mpu6050 = open(DEV_MPU6050, O_RDONLY);
    if (fd_mpu6050 < 0) 
	{
        perror("open /dev/mpu6050");
        goto out;
    }

    set_blue(fd_alarm_io, 1);
    set_red(fd_alarm_io, 0);
    set_buzzer(fd_alarm_io, 0);
    blue_on = 1;

    pfd.fd = fd_alarm_key;
    pfd.events = POLLIN;

    printf("alarm app start\n");
    printf("[STATE] IDLE\n");

    while (g_running) 
	{
        pfd.revents = 0;
        poll_ret = poll(&pfd, 1, LOOP_INTERVAL_MS);
        t_now = now_ms();

        if (poll_ret < 0) 
		{
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (poll_ret > 0 && (pfd.revents & POLLIN)) 
		{
            if (read_key_event(fd_alarm_key, &key_event) == 0 && key_event == 1) 
			{
                if (state == STATE_IDLE) 
				{
                    state = STATE_ARMED;
                    motion_hits = 0;
                    last_led_toggle = t_now;
                    printf("[KEY] IDLE -> ARMED\n");
                } 
				else if (state == STATE_ARMED) 
				{
                    state = STATE_IDLE;
                    motion_hits = 0;
                    printf("[KEY] ARMED -> IDLE\n");
                }
				else if (state == STATE_ALARM) 
				{
                    state = STATE_MUTE;
                    printf("[KEY] ALARM -> MUTE\n");
                }
				else if (state == STATE_MUTE) 
				{
                    state = STATE_IDLE;
                    motion_hits = 0;
                    printf("[KEY] MUTE -> IDLE\n");
                }
            }
        }

        if (read_lux(fd_bh1750, &lux) == 0)
            dark_env = (lux < DARK_LUX_THRESH);

        if (read_mpu(fd_mpu6050, &cur_frame) == 0) 
		{
            if (first_mpu) 
			{
                prev_frame = cur_frame;
                first_mpu = 0;
            }

            motion = motion_detected(&cur_frame, &prev_frame,
                                     &accel_delta, &gyro_delta);
            prev_frame = cur_frame;
        } 
		else
		{
            motion = 0;
            accel_delta = 0;
            gyro_delta = 0;
        }

        if (state == STATE_ARMED) 
		{
            if (dark_env && motion) 
			{
                motion_hits++;
            } 
			else if (motion_hits > 0) 
			{
                motion_hits--;
            }

            if (motion_hits >= MOTION_HIT_REQUIRED) 
			{
                state = STATE_ALARM;
                last_led_toggle = t_now;
                last_buzzer_toggle = t_now;
                printf("[ALARM] triggered: lux=%d accel_delta=%d gyro_delta=%d temp=%.2fC\n",
                       lux, accel_delta, gyro_delta, mpu_temp_c(cur_frame.temp_raw));
            }
        }

        apply_state_outputs(fd_alarm_io, state, t_now,
                            &last_led_toggle, &last_buzzer_toggle,
                            &blue_on, &red_on, &buzzer_on);

		write_status_json(state, lux, dark_env, motion, motion_hits,
                  accel_delta, gyro_delta, mpu_temp_c(cur_frame.temp_raw));

        if (t_now - last_status_print >= 1000) 
		{
            printf("[INFO] state=%d lux=%d dark=%d motion=%d hit=%d "
                   "accel_delta=%d gyro_delta=%d temp=%.2fC\n",
                   state, lux, dark_env, motion, motion_hits,
                   accel_delta, gyro_delta, mpu_temp_c(cur_frame.temp_raw));
            last_status_print = t_now;
        }
    }

out:
    if (fd_alarm_io >= 0) 
	{
        set_blue(fd_alarm_io, 0);
        set_red(fd_alarm_io, 0);
        set_buzzer(fd_alarm_io, 0);
        close(fd_alarm_io);
    }
    if (fd_mpu6050 >= 0)
        close(fd_mpu6050);
    if (fd_bh1750 >= 0)
        close(fd_bh1750);
    if (fd_alarm_key >= 0)
        close(fd_alarm_key);

    return 0;
}
