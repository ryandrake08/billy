#include "motors.h"
#include "hal.h"
#include "board.h"
#include "fish_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "motors";

// Each motor is IN1 = LEDC PWM, IN2 held low (spring-return; board.h). nSLEEP gates both DRV8833
// chips together (high = enabled, low = ~uA parked); nFAULT is both chips' open-drain fault line,
// wire-OR'd onto one input (low = OCP/thermal/UVLO on either chip).

#define MOTOR_PWM_FREQ_HZ     20000              // above audible range -- avoids motor whine
#define MOTOR_PWM_RESOLUTION  10
#define MOTOR_DUTY_MAX        ((1u << MOTOR_PWM_RESOLUTION) - 1)

typedef enum
{
    MOTOR_MOUTH = 0,
    MOTOR_HEAD  = 1,
    MOTOR_TAIL  = 2,
} motor_channel_t;

static bool motor_fault_active(void)
{
    return !hal_gpio_get(BOARD_DRV_NFAULT);
}

static void motor_set_duty(motor_channel_t ch, uint32_t duty)
{
    if (duty > MOTOR_DUTY_MAX) duty = MOTOR_DUTY_MAX;
    hal_pwm_set_duty(ch, duty);
}

void motors_init(void)
{
    // IN2 pins are plain GPIO outputs, held low -- each motor is unidirectional (spring return).
    hal_gpio_init_output(BOARD_MOUTH_IN2, false);
    hal_gpio_init_output(BOARD_HEAD_IN2, false);
    hal_gpio_init_output(BOARD_TAIL_IN2, false);

    // nSLEEP: starts low -- motors stay parked until the first drive call enables it. Release any
    // hold left over from a deep sleep the chip just woke from (motors_hold_for_sleep() holds
    // this low through sleep) -- level is already the desired 0, so this can't glitch it.
    hal_gpio_init_output(BOARD_DRV_NSLEEP, false);
    hal_gpio_hold_disable(BOARD_DRV_NSLEEP);

    // nFAULT: open-drain from both chips, wire-OR'd. External pull-up is mandatory; the internal
    // pull is harmless alongside it.
    hal_gpio_init_input(BOARD_DRV_NFAULT, true);

    // One LEDC timer shared by all three motor channels (same frequency/resolution).
    hal_pwm_init_timer(MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RESOLUTION);
    hal_pwm_init_channel(MOTOR_MOUTH, BOARD_MOUTH_IN1);
    hal_pwm_init_channel(MOTOR_HEAD,  BOARD_HEAD_IN1);
    hal_pwm_init_channel(MOTOR_TAIL,  BOARD_TAIL_IN1);

    ESP_LOGI(TAG, "init: motors — 3x LEDC PWM @ %d Hz configured, nSLEEP low (parked)",
             MOTOR_PWM_FREQ_HZ);
}

void motors_park(void)
{
    hal_gpio_set(BOARD_DRV_NSLEEP, false);
}

void motors_hold_for_sleep(void)
{
    hal_gpio_hold_enable(BOARD_DRV_NSLEEP);
}

void motors_enable(void)
{
    hal_gpio_set(BOARD_DRV_NSLEEP, true);
}

void motors_set_mouth_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    motor_set_duty(MOTOR_MOUTH, (MOTOR_DUTY_MAX * pct) / 100);
}

static void motor_warn_if_fault(const char *what)
{
    if (motor_fault_active())
    {
        ESP_LOGW(TAG, "%s: nFAULT low — possible stall/OCP", what);
    }
}

// Timing is runtime-tunable (fish_config.h) -- tail_flap_ms drives out then lets the spring
// return it; tail_settle_ms is the spring-return travel + mechanical ring-down before it's safe
// to listen.
void motors_tail_flap(void)
{
    const fish_config_t *cfg = fish_config_get();
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
    motors_enable();
    motor_set_duty(MOTOR_TAIL, MOTOR_DUTY_MAX);
    vTaskDelay(pdMS_TO_TICKS(cfg->tail_flap_ms));
    motor_set_duty(MOTOR_TAIL, 0);
    vTaskDelay(pdMS_TO_TICKS(cfg->tail_settle_ms));
    motor_warn_if_fault("tail flap");
}

// Non-blocking -- the spring-return motor stays driven until motors_head_relax() is called.
void motors_head_out(void)
{
    ESP_LOGI(TAG, "head out — 'I'm talking'");
    motors_enable();
    motor_set_duty(MOTOR_HEAD, MOTOR_DUTY_MAX);
}

void motors_head_relax(void)
{
    ESP_LOGI(TAG, "head relax");
    motor_set_duty(MOTOR_HEAD, 0);
    motor_warn_if_fault("head");
}

// Fault-isolating: one motor at a time, sweeping duty from low to full so you can find the
// minimum duty that actually overcomes the mechanism's spring preload/gearing -- stops
// immediately on any fault rather than moving on to the next motor. No forced stall -- the motor
// shafts are friction-fit to their gears, so a deliberate stall risks the gears more than it
// proves the driver's OCP works. Watch the motor itself while this runs; the firmware has no
// current sense, only nFAULT. NOTE: the duty->motion threshold tracks the motor rail's actual
// voltage (there's no buck between the battery and the motors), so a sweep run on a sagged
// battery will read higher thresholds than the same sweep on a fresh one.
void motors_selftest(void)
{
    ESP_LOGI(TAG, "motor self-test — one motor at a time, sweeping duty to find the motion threshold");

    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    motors_enable();
    vTaskDelay(pdMS_TO_TICKS(20));   // let both DRV8833s settle out of sleep before reading nFAULT
    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT low right after nSLEEP high, at 0%% duty — check the motor rail before proceeding");
        motors_park();
        return;
    }

    const struct { motor_channel_t ch; const char *name; } motors[] = {
        { MOTOR_MOUTH, "mouth" },
        { MOTOR_HEAD,  "head" },
        { MOTOR_TAIL,  "tail" },
    };
    const uint8_t sweep_pct[] = { 50, 60, 70, 80, 90, 100 };

    for (size_t i = 0; i < sizeof(motors) / sizeof(motors[0]); i++)
    {
        const char *name = motors[i].name;
        motor_channel_t ch = motors[i].ch;

        for (size_t s = 0; s < sizeof(sweep_pct) / sizeof(sweep_pct[0]); s++)
        {
            uint32_t duty = (MOTOR_DUTY_MAX * sweep_pct[s]) / 100;
            ESP_LOGI(TAG, "motor self-test: %s — %u%% duty, 2 s", name, (unsigned) sweep_pct[s]);
            motor_set_duty(ch, duty);
            vTaskDelay(pdMS_TO_TICKS(2000));
            motor_set_duty(ch, 0);
            if (motor_fault_active())
            {
                ESP_LOGE(TAG, "motor self-test: %s tripped nFAULT at %u%% duty — stopping here", name, (unsigned) sweep_pct[s]);
                motors_park();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(800));   // stopped, for a clean before/after contrast with the next step
        }

        vTaskDelay(pdMS_TO_TICKS(1500));   // extra pause before moving on to the next motor
    }

    motors_park();
    ESP_LOGI(TAG, "motor self-test: done, all three motors clean — nSLEEP low (parked)");
}

// Progressive combined-load test: stacks motors on one at a time -- head, then head+tail, then
// head+tail+mouth -- each stage at 100% duty for 2 s, so the motor rail's sag under real combined
// load can be read directly off a scope/DMM. motors_selftest() only ever drives one motor at a
// time, so it can't show this -- combined current draw (and the resulting rail sag) doesn't show
// up until more than one motor is actually driven at once. Stops immediately on any nFAULT trip.
// The firmware has no current or voltage sense of its own; watch the rail externally while this
// runs.
void motors_stresstest(void)
{
    ESP_LOGI(TAG, "motor stress test — progressively loading head, then head+tail, then head+tail+mouth, 100%% duty, 2 s each");

    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor stress test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    motors_enable();
    vTaskDelay(pdMS_TO_TICKS(20));   // let both DRV8833s settle out of sleep before reading nFAULT
    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor stress test: nFAULT low right after nSLEEP high, at 0%% duty — check the motor rail before proceeding");
        motors_park();
        return;
    }

    const struct { motor_channel_t ch; const char *cumulative; } stages[] = {
        { MOTOR_HEAD,  "head" },
        { MOTOR_TAIL,  "head+tail" },
        { MOTOR_MOUTH, "head+tail+mouth" },
    };

    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
    {
        motor_set_duty(stages[i].ch, MOTOR_DUTY_MAX);
        ESP_LOGI(TAG, "motor stress test: stage %u — %s now at 100%%, holding 2 s", (unsigned) (i + 1), stages[i].cumulative);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (motor_fault_active())
        {
            ESP_LOGE(TAG, "motor stress test: nFAULT tripped during stage %u (%s) — stopping here", (unsigned) (i + 1), stages[i].cumulative);
            motor_set_duty(MOTOR_MOUTH, 0);
            motor_set_duty(MOTOR_HEAD, 0);
            motor_set_duty(MOTOR_TAIL, 0);
            motors_park();
            return;
        }
    }

    motor_set_duty(MOTOR_MOUTH, 0);
    motor_set_duty(MOTOR_HEAD, 0);
    motor_set_duty(MOTOR_TAIL, 0);
    ESP_LOGI(TAG, "motor stress test: all three back to 0%%");

    motors_park();
    ESP_LOGI(TAG, "motor stress test: done — nSLEEP low (parked)");
}
