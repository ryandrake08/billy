#include "motors.h"
#include "hal.h"
#include "board.h"
#include "fish_config.h"
#include "peripherals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "motors";

// Each motor is IN1 = LEDC PWM, IN2 held low (spring-return; board.h). Every motor belongs to one
// of two DRV8833-chip groups (motors_group_t); nSLEEP/nFAULT are per-group.

#define MOTOR_PWM_FREQ_HZ     20000              // above audible range -- avoids motor whine
#define MOTOR_PWM_RESOLUTION  10
#define MOTOR_DUTY_MAX        ((1u << MOTOR_PWM_RESOLUTION) - 1)

typedef enum
{
    MOTOR_MOUTH = 0,
    MOTOR_HEAD  = 1,
    MOTOR_TAIL  = 2,
} motor_channel_t;

#define MOTOR_MASK(ch)  (1u << (ch))
#define GROUP_MASK(grp) (1u << (grp))

// nFAULT is unreliable--asserting when unexpected. We'll ignore it for now.
#define MOTORS_DEBUG_IGNORE_NFAULT 1

static const struct
{
    int      nsleep_pin;
    int      nfault_pin;
    uint32_t channel_mask;
} s_groups[MOTORS_GROUP_COUNT] = {
    [MOTORS_GROUP_MOUTH_HEAD] = { BOARD_MOUTH_HEAD_NSLEEP, BOARD_MOUTH_HEAD_NFAULT,
                                   MOTOR_MASK(MOTOR_MOUTH) | MOTOR_MASK(MOTOR_HEAD) },
    [MOTORS_GROUP_TAIL]       = { BOARD_TAIL_NSLEEP, BOARD_TAIL_NFAULT, MOTOR_MASK(MOTOR_TAIL) },
};

static const motors_group_t s_channel_group[3] = {
    [MOTOR_MOUTH] = MOTORS_GROUP_MOUTH_HEAD,
    [MOTOR_HEAD]  = MOTORS_GROUP_MOUTH_HEAD,
    [MOTOR_TAIL]  = MOTORS_GROUP_TAIL,
};

typedef struct
{
    bool     fault_latched;
    uint32_t fault_commanded_mask;
} group_fault_state_t;

static group_fault_state_t s_fault_state[MOTORS_GROUP_COUNT];
static uint32_t     s_commanded_mask;
static uint32_t     s_fault_count;
static uint32_t     s_pending_report_mask;   // groups newly latched since the task last drained
static TaskHandle_t s_fault_task;

static const char *group_name(motors_group_t grp)
{
    return grp == MOTORS_GROUP_TAIL ? "tail" : "mouth/head";
}

static void motor_fault_isr(void *arg)
{
    // Stop the affected group(s) before doing any bookkeeping. The GPIO ISR service is
    // deliberately installed without ESP_INTR_FLAG_IRAM, so this handler and hal_gpio_set need
    // not live in IRAM.
    uint32_t group_mask = (uint32_t) (uintptr_t) arg;
    bool any_newly_latched = false;

    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        if (!(group_mask & GROUP_MASK(g)))
        {
            continue;
        }
#if !MOTORS_DEBUG_IGNORE_NFAULT
        hal_gpio_set(s_groups[g].nsleep_pin, false);
#endif
        if (!__atomic_exchange_n(&s_fault_state[g].fault_latched, true, __ATOMIC_ACQ_REL))
        {
            any_newly_latched = true;
            __atomic_store_n(&s_fault_state[g].fault_commanded_mask,
                             __atomic_load_n(&s_commanded_mask, __ATOMIC_RELAXED) & s_groups[g].channel_mask,
                             __ATOMIC_RELAXED);
            __atomic_fetch_or(&s_pending_report_mask, GROUP_MASK(g), __ATOMIC_RELAXED);
        }
    }
    if (!any_newly_latched)
    {
        return;
    }
    __atomic_add_fetch(&s_fault_count, 1, __ATOMIC_RELAXED);

    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_fault_task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static bool motor_pin_fault_active(int nfault_pin)
{
    return !hal_gpio_get(nfault_pin);
}

static bool motor_group_faulted(motors_group_t grp)
{
#if MOTORS_DEBUG_IGNORE_NFAULT
    return false;
#else
    return __atomic_load_n(&s_fault_state[grp].fault_latched, __ATOMIC_ACQUIRE);
#endif
}

static bool motor_any_group_faulted(void)
{
    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        if (motor_group_faulted(g))
        {
            return true;
        }
    }
    return false;
}

static bool motor_any_pin_fault_active(void)
{
    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        if (motor_pin_fault_active(s_groups[g].nfault_pin))
        {
            return true;
        }
    }
    return false;
}

static bool motor_set_duty(motor_channel_t ch, uint32_t duty)
{
    motors_group_t grp = s_channel_group[ch];
    if (duty > MOTOR_DUTY_MAX) duty = MOTOR_DUTY_MAX;
    if (duty > 0 && motor_group_faulted(grp))
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
    if (duty > 0 && motor_group_faulted(grp))
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

static void motor_clear_group_duties(motors_group_t grp)
{
    for (int ch = 0; ch < 3; ch++)
    {
        if (s_channel_group[ch] != grp)
        {
            continue;
        }
        hal_pwm_set_duty(ch, 0);
        __atomic_fetch_and(&s_commanded_mask, ~MOTOR_MASK(ch), __ATOMIC_RELAXED);
    }
}

static void motor_fault_task(void *arg)
{
    (void) arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t report_mask = __atomic_exchange_n(&s_pending_report_mask, 0, __ATOMIC_ACQ_REL);
        float vmotor = peripherals_read_vmotor_volts();
        for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
        {
            if (!(report_mask & GROUP_MASK(g)))
            {
                continue;
            }
#if !MOTORS_DEBUG_IGNORE_NFAULT
            motor_clear_group_duties(g);
#endif
            uint32_t mask = __atomic_load_n(&s_fault_state[g].fault_commanded_mask, __ATOMIC_RELAXED);
            ESP_LOGE(TAG,
                     "nFAULT #%u on %s: %s; commanded mouth=%d head=%d tail=%d, Vmotor=%.2fV",
                     (unsigned) __atomic_load_n(&s_fault_count, __ATOMIC_RELAXED), group_name(g),
                     MOTORS_DEBUG_IGNORE_NFAULT ? "motors left running" : "motors parked",
                     (mask & MOTOR_MASK(MOTOR_MOUTH)) != 0,
                     (mask & MOTOR_MASK(MOTOR_HEAD)) != 0,
                     (mask & MOTOR_MASK(MOTOR_TAIL)) != 0, vmotor);
        }
    }
}

void motors_init(void)
{
    // IN2 pins are plain GPIO outputs, held low -- each motor is unidirectional (spring return).
    hal_gpio_init_output(BOARD_MOUTH_IN2, false);
    hal_gpio_init_output(BOARD_HEAD_IN2, false);
    hal_gpio_init_output(BOARD_TAIL_IN2, false);

    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        // nSLEEP: starts low -- motors stay parked until the first drive call enables their group.
        // Release any hold left over from a deep sleep the chip just woke from (motors_hold_for_sleep()
        // holds these low through sleep) -- level is already the desired 0, so this can't glitch it.
        hal_gpio_init_output(s_groups[g].nsleep_pin, false);
        hal_gpio_hold_disable(s_groups[g].nsleep_pin);

        // nFAULT: open-drain from each chip.
        hal_gpio_init_input(s_groups[g].nfault_pin, true);
    }

    // One LEDC timer shared by all three motor channels (same frequency/resolution).
    hal_pwm_init_timer(MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RESOLUTION);
    hal_pwm_init_channel(MOTOR_MOUTH, BOARD_MOUTH_IN1);
    hal_pwm_init_channel(MOTOR_HEAD,  BOARD_HEAD_IN1);
    hal_pwm_init_channel(MOTOR_TAIL,  BOARD_TAIL_IN1);

    BaseType_t created = xTaskCreate(motor_fault_task, "motor_fault", 2048, NULL, 6,
                                     &s_fault_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    // Install one ISR per distinct physical nFAULT pin.
    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        uint32_t group_mask = GROUP_MASK(g);
        bool already_installed = false;
        for (int other = 0; other < MOTORS_GROUP_COUNT; other++)
        {
            if (other == g || s_groups[other].nfault_pin != s_groups[g].nfault_pin)
            {
                continue;
            }
            group_mask |= GROUP_MASK(other);
            already_installed = already_installed || (other < g);
        }
        if (already_installed)
        {
            continue;
        }

        hal_gpio_add_falling_interrupt(s_groups[g].nfault_pin, motor_fault_isr,
                                       (void *) (uintptr_t) group_mask);
        if (motor_pin_fault_active(s_groups[g].nfault_pin))
        {
            // A line already low has no falling edge for the interrupt hardware to catch.
            for (int gr = 0; gr < MOTORS_GROUP_COUNT; gr++)
            {
                if (!(group_mask & GROUP_MASK(gr)))
                {
                    continue;
                }
                __atomic_store_n(&s_fault_state[gr].fault_commanded_mask, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&s_fault_state[gr].fault_latched, true, __ATOMIC_RELEASE);
            }
            __atomic_add_fetch(&s_fault_count, 1, __ATOMIC_RELAXED);
            __atomic_fetch_or(&s_pending_report_mask, group_mask, __ATOMIC_RELAXED);
            xTaskNotifyGive(s_fault_task);
        }
    }

    ESP_LOGI(TAG, "init: motors — 3x LEDC PWM @ %d Hz configured, nSLEEP low (parked)",
             MOTOR_PWM_FREQ_HZ);
}

void motors_park(void)
{
    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        hal_gpio_set(s_groups[g].nsleep_pin, false);
    }
    motor_clear_all_duties();
}

void motors_hold_for_sleep(void)
{
    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        hal_gpio_hold_enable(s_groups[g].nsleep_pin);
    }
}

bool motors_enable(motors_group_t grp)
{
    if (motor_group_faulted(grp))
    {
        ESP_LOGW(TAG, "enable refused: nFAULT is latched (%s)", group_name(grp));
        return false;
    }
    hal_gpio_set(s_groups[grp].nsleep_pin, true);
    vTaskDelay(pdMS_TO_TICKS(20));   // let the DRV8833 settle out of sleep before reading nFAULT
    if (motor_group_faulted(grp))
    {
        hal_gpio_set(s_groups[grp].nsleep_pin, false);
        ESP_LOGW(TAG, "enable interrupted by nFAULT (%s)", group_name(grp));
        return false;
    }
    return true;
}

// No-op (returns true) unless a fault is currently latched on this group.
bool motors_recover_if_faulted(motors_group_t grp)
{
    if (!motor_group_faulted(grp))
    {
        return true;
    }

    hal_gpio_set(s_groups[grp].nsleep_pin, false);
    motor_clear_group_duties(grp);
    if (motor_pin_fault_active(s_groups[grp].nfault_pin))
    {
        ESP_LOGW(TAG, "recovery refused: nFAULT is still low (%s)", group_name(grp));
        return false;
    }

    // Clear immediately before waking so a new falling edge during startup re-latches the fault.
    __atomic_store_n(&s_fault_state[grp].fault_latched, false, __ATOMIC_RELEASE);
    hal_gpio_set(s_groups[grp].nsleep_pin, true);
    vTaskDelay(pdMS_TO_TICKS(20));
    bool recovered = !motor_group_faulted(grp) && !motor_pin_fault_active(s_groups[grp].nfault_pin);
    hal_gpio_set(s_groups[grp].nsleep_pin, false);
    if (!recovered)
    {
        __atomic_store_n(&s_fault_state[grp].fault_latched, true, __ATOMIC_RELEASE);
        ESP_LOGW(TAG, "recovery failed: nFAULT asserted while waking the driver (%s)", group_name(grp));
    }
    else
    {
        ESP_LOGI(TAG, "motor fault recovered at activation (%s); driver parked until commanded",
                 group_name(grp));
    }
    return recovered;
}

bool motors_set_mouth_pct(uint8_t pct)
{
    if (motor_group_faulted(MOTORS_GROUP_MOUTH_HEAD))
    {
        ESP_LOGW(TAG, "mouth command prevented by nFAULT");
        return false;
    }
    if (pct > 100) pct = 100;
    return motor_set_duty(MOTOR_MOUTH, (MOTOR_DUTY_MAX * pct) / 100);
}

// Timing is runtime-tunable (fish_config.h) -- tail_flap_ms drives out then lets the spring
// return it; tail_settle_ms is the spring-return travel + mechanical ring-down before it's safe
// to listen.
bool motors_tail_flap(void)
{
    const fish_config_t *cfg = fish_config_get();
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
    if (!motors_enable(MOTORS_GROUP_TAIL))
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
    if (motor_group_faulted(MOTORS_GROUP_TAIL))
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
    if (!motors_enable(MOTORS_GROUP_MOUTH_HEAD))
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
    if (motor_group_faulted(MOTORS_GROUP_MOUTH_HEAD))
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

    if (motor_any_group_faulted())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        if (!motors_enable(g))
        {
            ESP_LOGE(TAG, "motor self-test: refusing to start with nFAULT latched (%s)", group_name(g));
            motors_park();
            return;
        }
    }
    if (motor_any_group_faulted() || motor_any_pin_fault_active())
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
            if (motor_group_faulted(s_channel_group[ch]))
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

    if (motor_any_group_faulted())
    {
        ESP_LOGE(TAG, "motor stress test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
#if !MOTORS_DEBUG_IGNORE_NFAULT
        return;
#endif
    }

    for (int g = 0; g < MOTORS_GROUP_COUNT; g++)
    {
        if (!motors_enable(g))
        {
            ESP_LOGE(TAG, "motor stress test: refusing to start with nFAULT latched (%s)", group_name(g));
#if !MOTORS_DEBUG_IGNORE_NFAULT
            motors_park();
            return;
#endif
        }
    }
    if (motor_any_group_faulted() || motor_any_pin_fault_active())
    {
        ESP_LOGE(TAG, "motor stress test: nFAULT low right after nSLEEP high, at 0%% duty — check the motor rail before proceeding");
#if !MOTORS_DEBUG_IGNORE_NFAULT
        motors_park();
        return;
#endif
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
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (motor_any_group_faulted())
        {
            ESP_LOGE(TAG, "motor stress test: nFAULT tripped during stage %u (%s) — stopping here", (unsigned) (i + 1), stages[i].cumulative);
#if !MOTORS_DEBUG_IGNORE_NFAULT
            motors_park();
            return;
#endif
        }
    }

    motor_clear_all_duties();
    ESP_LOGI(TAG, "motor stress test: all three back to 0%%");

    motors_park();
    ESP_LOGI(TAG, "motor stress test: done — nSLEEP low (parked)");
}
