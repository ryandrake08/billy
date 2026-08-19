#include "motors.h"
#include "hal.h"
#include "board.h"
#include "fish_config.h"
#include "sensors.h"
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

#define MOTOR_MASK(ch) (1u << (ch))

static bool         s_fault_latched;
static uint32_t     s_fault_count;
static uint32_t     s_commanded_mask;
static uint32_t     s_fault_commanded_mask;
static TaskHandle_t s_fault_task;

static void motor_fault_isr(void *arg)
{
    (void) arg;

    // Stop both chips before doing any bookkeeping. The GPIO ISR service is deliberately
    // installed without ESP_INTR_FLAG_IRAM, so this handler and gpio_set_level need not live in
    // IRAM; hal_gpio_set() is the same thin gpio_set_level wrapper used outside the ISR.
    hal_gpio_set(BOARD_DRV_NSLEEP, false);
    if (__atomic_exchange_n(&s_fault_latched, true, __ATOMIC_ACQ_REL))
    {
        return;
    }
    __atomic_store_n(&s_fault_commanded_mask,
                     __atomic_load_n(&s_commanded_mask, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_fault_count, 1, __ATOMIC_RELAXED);

    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_fault_task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static bool motor_fault_active(void)
{
    return !hal_gpio_get(BOARD_DRV_NFAULT);
}

static bool motor_set_duty(motor_channel_t ch, uint32_t duty)
{
    if (duty > MOTOR_DUTY_MAX) duty = MOTOR_DUTY_MAX;
    if (duty > 0 && motors_faulted())
    {
        return false;
    }
    hal_pwm_set_duty(ch, duty);
    if (duty > 0)
    {
        __atomic_fetch_or(&s_commanded_mask, MOTOR_MASK(ch), __ATOMIC_RELAXED);
    }
    else
    {
        __atomic_fetch_and(&s_commanded_mask, ~MOTOR_MASK(ch), __ATOMIC_RELAXED);
    }
    if (duty > 0 && motors_faulted())
    {
        hal_pwm_set_duty(ch, 0);
        __atomic_fetch_and(&s_commanded_mask, ~MOTOR_MASK(ch), __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

static void motor_clear_all_duties(void)
{
    hal_pwm_set_duty(MOTOR_MOUTH, 0);
    hal_pwm_set_duty(MOTOR_HEAD, 0);
    hal_pwm_set_duty(MOTOR_TAIL, 0);
    __atomic_store_n(&s_commanded_mask, 0, __ATOMIC_RELAXED);
}

static void motor_fault_task(void *arg)
{
    (void) arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        motor_clear_all_duties();
        float vmotor = sensors_read_vmotor_volts();
        uint32_t mask = __atomic_load_n(&s_fault_commanded_mask, __ATOMIC_RELAXED);
        uint32_t count = __atomic_load_n(&s_fault_count, __ATOMIC_RELAXED);
        ESP_LOGE(TAG,
                 "nFAULT #%u: motors parked; commanded mouth=%d head=%d tail=%d, Vmotor=%.2fV",
                 (unsigned) count, (mask & MOTOR_MASK(MOTOR_MOUTH)) != 0,
                 (mask & MOTOR_MASK(MOTOR_HEAD)) != 0,
                 (mask & MOTOR_MASK(MOTOR_TAIL)) != 0, vmotor);
    }
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

    BaseType_t created = xTaskCreate(motor_fault_task, "motor_fault", 2048, NULL, 6,
                                     &s_fault_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    hal_gpio_add_falling_interrupt(BOARD_DRV_NFAULT, motor_fault_isr, NULL);
    if (motor_fault_active())
    {
        // A line already low has no falling edge for the interrupt hardware to catch.
        __atomic_store_n(&s_fault_commanded_mask, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_fault_count, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s_fault_latched, true, __ATOMIC_RELEASE);
        xTaskNotifyGive(s_fault_task);
    }

    ESP_LOGI(TAG, "init: motors — 3x LEDC PWM @ %d Hz configured, nSLEEP low (parked)",
             MOTOR_PWM_FREQ_HZ);
}

void motors_park(void)
{
    hal_gpio_set(BOARD_DRV_NSLEEP, false);
    motor_clear_all_duties();
}

void motors_hold_for_sleep(void)
{
    hal_gpio_hold_enable(BOARD_DRV_NSLEEP);
}

bool motors_enable(void)
{
    if (motors_faulted())
    {
        ESP_LOGW(TAG, "enable refused: nFAULT is latched");
        return false;
    }
    hal_gpio_set(BOARD_DRV_NSLEEP, true);
    if (motors_faulted())
    {
        hal_gpio_set(BOARD_DRV_NSLEEP, false);
        ESP_LOGW(TAG, "enable interrupted by nFAULT");
        return false;
    }
    return true;
}

bool motors_faulted(void)
{
    return __atomic_load_n(&s_fault_latched, __ATOMIC_ACQUIRE);
}

bool motors_recover(void)
{
    if (!motors_faulted())
    {
        return true;
    }

    motors_park();
    if (motor_fault_active())
    {
        ESP_LOGW(TAG, "recovery refused: nFAULT is still low");
        return false;
    }

    // Clear immediately before waking so a new falling edge during startup re-latches the fault.
    __atomic_store_n(&s_fault_latched, false, __ATOMIC_RELEASE);
    hal_gpio_set(BOARD_DRV_NSLEEP, true);
    vTaskDelay(pdMS_TO_TICKS(20));
    bool recovered = !motors_faulted() && !motor_fault_active();
    motors_park();
    if (!recovered)
    {
        __atomic_store_n(&s_fault_latched, true, __ATOMIC_RELEASE);
        ESP_LOGW(TAG, "recovery failed: nFAULT asserted while waking the drivers");
    }
    return recovered;
}

bool motors_set_mouth_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (!motor_set_duty(MOTOR_MOUTH, (MOTOR_DUTY_MAX * pct) / 100) || motors_faulted())
    {
        ESP_LOGW(TAG, "mouth command prevented by nFAULT");
        return false;
    }
    return true;
}

// Timing is runtime-tunable (fish_config.h) -- tail_flap_ms drives out then lets the spring
// return it; tail_settle_ms is the spring-return travel + mechanical ring-down before it's safe
// to listen.
bool motors_tail_flap(void)
{
    const fish_config_t *cfg = fish_config_get();
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
    if (!motors_enable())
    {
        ESP_LOGW(TAG, "tail flap unable to enable motors");
        return false;
    }
    if (!motor_set_duty(MOTOR_TAIL, MOTOR_DUTY_MAX))
    {
        ESP_LOGW(TAG, "tail flap prevented by nFAULT");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(cfg->tail_flap_ms));
    motor_set_duty(MOTOR_TAIL, 0);
    vTaskDelay(pdMS_TO_TICKS(cfg->tail_settle_ms));
    if (motors_faulted())
    {
        ESP_LOGW(TAG, "tail flap interrupted by nFAULT");
        return false;
    }
    return true;
}

// Non-blocking -- the spring-return motor stays driven until motors_head_relax() is called.
bool motors_head_out(void)
{
    ESP_LOGI(TAG, "head out — 'I'm talking'");
    if (!motors_enable())
    {
        ESP_LOGW(TAG, "head out unable to enable motors");
        return false;
    }
    if (!motor_set_duty(MOTOR_HEAD, MOTOR_DUTY_MAX))
    {
        ESP_LOGW(TAG, "head out prevented by nFAULT");
        return false;
    }
    return true;
}

bool motors_head_relax(void)
{
    ESP_LOGI(TAG, "head relax");
    motor_set_duty(MOTOR_HEAD, 0);
    if (motors_faulted())
    {
        ESP_LOGW(TAG, "head: nFAULT event latched — motors parked");
        return false;
    }
    return true;
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

    if (motors_faulted())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    if (!motors_enable())
    {
        ESP_LOGE(TAG, "motor self-test: refusing to start with nFAULT latched");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));   // let both DRV8833s settle out of sleep before reading nFAULT
    if (motors_faulted() || motor_fault_active())
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
            if (motors_faulted())
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
// Firmware records Vmotor after an nFAULT event, but an external scope/DMM is still required to
// observe the rail continuously through each stage.
void motors_stresstest(void)
{
    ESP_LOGI(TAG, "motor stress test — progressively loading head, then head+tail, then head+tail+mouth, 100%% duty, 2 s each");

    if (motors_faulted())
    {
        ESP_LOGE(TAG, "motor stress test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    if (!motors_enable())
    {
        ESP_LOGE(TAG, "motor stress test: refusing to start with nFAULT latched");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));   // let both DRV8833s settle out of sleep before reading nFAULT
    if (motors_faulted() || motor_fault_active())
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
        if (motors_faulted())
        {
            ESP_LOGE(TAG, "motor stress test: nFAULT tripped during stage %u (%s) — stopping here", (unsigned) (i + 1), stages[i].cumulative);
            motors_park();
            return;
        }
    }

    motor_clear_all_duties();
    ESP_LOGI(TAG, "motor stress test: all three back to 0%%");

    motors_park();
    ESP_LOGI(TAG, "motor stress test: done — nSLEEP low (parked)");
}
