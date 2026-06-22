#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "hw_memmap.h"
#include "debug.h"
#include "gpio.h"
#include "hw_i2c.h"
#include "hw_types.h"
#include "i2c.h"
#include "pwm.h"
#include "pin_map.h"
#include "sysctl.h"
#include "systick.h"
#include "interrupt.h"
#include "uart.h"
#include "hw_ints.h"

/************************** NOP macro **************************/
#ifndef __NOP
#define __NOP() __asm volatile ("nop")
#endif

#define SYSTICK_FREQUENCY       1000        // 1000Hz
#define I2C_FLASHTIME           500         // 500ms
#define GPIO_FLASHTIME          300         // 300ms

/************************** I2C device addresses **************************/
#define TCA6424_I2CADDR         0x22
#define PCA9557_I2CADDR         0x18

#define PCA9557_INPUT           0x00
#define PCA9557_OUTPUT          0x01
#define PCA9557_POLINVERT       0x02
#define PCA9557_CONFIG          0x03

#define TCA6424_CONFIG_PORT0    0x0c
#define TCA6424_CONFIG_PORT1    0x0d
#define TCA6424_CONFIG_PORT2    0x0e

#define TCA6424_INPUT_PORT0     0x00
#define TCA6424_INPUT_PORT1     0x01
#define TCA6424_INPUT_PORT2     0x02

#define TCA6424_OUTPUT_PORT0    0x04
#define TCA6424_OUTPUT_PORT1    0x05
#define TCA6424_OUTPUT_PORT2    0x06

/************************** Display configuration **************************/
#define DP_SEG                  0x80
#define SCAN_REVERSE            0
#define SEPARATOR_STYLE         0
#define BRIGHTNESS              1

/************************** Boot timing (unit: 10ms) **************************/
#define BOOT_FULL_BRIGHT        100     // all segments on for 1s
#define BOOT_FULL_OFF           100     // all off for 1s
#define BOOT_STUDENT_ID         300     // student ID for 3s
#define BOOT_NAME               300     // name for 3s
#define BOOT_VERSION            200     // version for 2s

#define STUDENT_ID              "20260001"
#define STUDENT_NAME            "HUZHENYE"
#define SOFTWARE_VERSION        "V1.0    "

/************************** Display modes **************************/
#define DISP_MODE_TIME          0       // HH.MM.SS
#define DISP_MODE_DATE_SHORT    1       // YY.MM.DD
#define DISP_MODE_DATE_LONG     2       // YYYY.MMDD

/************************** Edit modes **************************/
#define EDIT_NONE               0
#define EDIT_DATE               1
#define EDIT_TIME               2
#define EDIT_ALARM              3

#define EDIT_FIELD_YEAR         0
#define EDIT_FIELD_MONTH        1
#define EDIT_FIELD_DAY          2
#define EDIT_FIELD_HOUR         0
#define EDIT_FIELD_MINUTE       1
#define EDIT_FIELD_SECOND       2

/************************** Scroll speed presets **************************/
#define SCROLL_SPEED_SLOW       5       // advance every 500ms (5 * 100ms)
#define SCROLL_SPEED_FAST       2       // advance every 200ms (2 * 100ms)

/************************** LED bit assignments **************************/
#define LED_HEARTBEAT           0x01    // bit 0 — heartbeat 1Hz
#define LED_ALARM               0x02    // bit 1 — alarm enabled/ringing
#define LED_EDIT                0x04    // bit 2 — edit mode active
#define LED_TX                  0x08    // bit 3 — TX activity (300ms blink)
#define LED_RX                  0x10    // bit 4 — RX activity / NTP synced (steady)
#define LED_SUN                 0x20    // bit 5 — weather SUN
#define LED_RAI_SNO             0x40    // bit 6 — weather RAI/SNO
#define LED_HI_TEMP             0x80    // bit 7 — temp >=30°C

/************************** Beeper control **************************/
// S800 uses PS1720P02 (C96061) PASSIVE piezoelectric buzzer on PK5 (M0PWM7).
// Resonant frequency: 2kHz. Drive via PWM0 Gen3 Output7 at 2kHz / 50% duty.

/************************** Key names (for protocol) **************************/
static const char *KEY_NAMES[] = {
    "ADD", "FUNC", "SHIFT", "SPEED",
    "SAVE", "FORMAT", "DISP", "EXT",
    "USER1", "USER2"
};
#define NUM_KEYS 10

/************************** Function declarations **************************/
void        Delay(uint32_t value);
void        Delay_us(uint32_t us);
void        S800_GPIO_Init(void);
uint8_t     I2C0_WriteByte(uint8_t DevAddr, uint8_t RegAddr, uint8_t WriteData);
uint8_t     I2C0_ReadByte(uint8_t DevAddr, uint8_t RegAddr);
void        S800_I2C0_Init(void);
void        S800_UART_Init(void);
void        Boot_Sequence(void);
uint8_t     CharToSeg7(char c);
void        ShowString(const char *str, uint32_t duration, uint8_t led_state);

// Time/date
uint8_t     is_leap_year(uint16_t year);
uint8_t     get_days_in_month(uint16_t year, uint8_t month);
void        update_time(void);
void        update_display(void);

// Protocol
int         match_abbrev(const char *input, const char *full);
void        process_uart_command(void);
void        send_response(const char *msg);
void        send_event_disp(void);
void        send_event_led(void);
void        send_event_key(uint8_t key_idx);
void        send_event_alarm(uint8_t on);
void        send_event_edit(const char *type, const char *value);
void        send_event_mode(const char *state);

// Key scanning
void        key_scan(void);
void        key_action(uint8_t key_idx, uint8_t long_press);

// LED / beeper
void        led_update(void);
void        beep_on(void);
void        beep_off(void);
void        alarm_check(void);
void        alarm_stop(void);

// Edit state machine
void        edit_enter(uint8_t mode);
void        edit_exit(uint8_t save);
void        edit_tick(void);

/************************** Segment font table **************************/
uint8_t seg7[] = {
    0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f,
    0x77,0x7c,0x58,0x5e,0x79,0x71,0x5c,0x00,0x40
};

/************************** System tick variables **************************/
volatile uint16_t systick_10ms_counter, systick_100ms_counter;
volatile uint8_t  systick_10ms_status, systick_100ms_status;
volatile uint8_t  result, cnt, key_value, gpio_status;
volatile uint8_t  rightshift = 0x01;
uint32_t ui32SysClock, ui32IntPriorityMask;
uint8_t uart_receive_char;
volatile int i = 0;
volatile char receive[65];                       // RX line buffer, ISR+foreground shared
volatile uint8_t uart_cmd_ready = 0;
static uint8_t receive_overflow = 0;             // 1 = line exceeded 64 chars
const char *ATCLASS = "AT+CLASS#";
const char *ATCODE = "AT+STUDENTCODE#";
const char *CLASS = "CLASSF17XXXXX";
const char *CODE = "CODE517XXXXXXX";

/************************** Time/date state **************************/
volatile uint8_t  hour = 12, minute = 34, second = 56;
volatile uint16_t year = 2026;
volatile uint8_t  month = 6, day = 8;

/************************** Day-of-week tracking **************************/
#define DOW_MON 1
#define DOW_TUE 2
#define DOW_WED 3
#define DOW_THU 4
#define DOW_FRI 5
#define DOW_SAT 6
#define DOW_SUN 7
volatile uint8_t  day_of_week = DOW_MON;       // 2026-06-08 = Monday
static const char *DOW_NAMES[] = {"","MON","TUE","WED","THU","FRI","SAT","SUN"};

/************************** Alarm state — 7 slots (Mon-Sun) **************************/
#define ALARM_SLOTS 7
volatile uint8_t  alarm_hour[ALARM_SLOTS];      // per-day alarm hour
volatile uint8_t  alarm_minute[ALARM_SLOTS];    // per-day alarm minute
volatile uint8_t  alarm_second[ALARM_SLOTS];    // per-day alarm second
volatile uint8_t  alarm_enabled_mask;           // bit 0=Mon ... bit 6=Sun

#define ALARM_IDX          (day_of_week - 1)
#define ALARM_CUR_HOUR     alarm_hour[ALARM_IDX]
#define ALARM_CUR_MIN      alarm_minute[ALARM_IDX]
#define ALARM_CUR_SEC      alarm_second[ALARM_IDX]
#define ALARM_CUR_ENABLED  (alarm_enabled_mask & (1 << ALARM_IDX))
#define ALARM_SET_ENABLED  (alarm_enabled_mask |= (1 << ALARM_IDX))
#define ALARM_SET_DISABLED (alarm_enabled_mask &= ~(1 << ALARM_IDX))

volatile uint8_t  alarm_ringing = 0;
static uint8_t    alarm_beep_phase = 0;
static uint8_t    alarm_beep_timer = 0;
static uint8_t    alarm_total_timer = 0;
static uint8_t    alarm_snooze = 0;
static uint8_t    alarm_led_blink = 0;

/************************** Display state **************************/
uint8_t  disp_mode = DISP_MODE_TIME;
uint8_t  disp_on = 1;
static uint8_t sec_counter = 0;
uint8_t  disp_buf[8] = {0};
uint8_t  dp_buf[8] = {0};
char     disp_chars[9] = "        ";       // 8 ASCII chars for *EVT:DISP
volatile uint8_t scan_cnt = 0;

/************************** Format / scroll state **************************/
uint8_t  format_direction = 0;             // 0=LEFT, 1=RIGHT
uint8_t  scroll_speed_level = 0;           // 0=slow(500ms), 1=fast(200ms)
char     scroll_msg[33] = {0};             // scroll message (max 32 chars)
uint8_t  scroll_active = 0;                // scroll message is active
uint8_t  scroll_pos = 0;                   // current scroll offset
uint8_t  scroll_len = 0;                   // length of scroll message
static uint8_t scroll_timer = 0;           // 100ms ticks for scroll advance
static uint8_t scroll_cycle_count = 0;     // completed cycles for auto-return
static uint8_t scroll_static_timer = 0;    // for ≤8-char static display (1s ticks)

/************************** LED state **************************/
volatile uint8_t led_byte = 0x00;          // PCA9557 LED byte
static uint8_t   led_takeover = 0;         // 1 = *SET:LED override, inhibits auto-LED logic
static volatile uint8_t led_heartbeat_timer = 0;  // 100ms ticks for heartbeat toggle
static volatile uint8_t uart_tx_timer = 0;        // TX LED blink duration (ISR→foreground)
static volatile uint8_t uart_rx_timer = 0;        // RX LED blink duration (ISR→foreground)

/************************** Weather / NTP state (E1/E2 extensions) **************************/
static int8_t   weather_temp = -99;        // temperature in Celsius (-99 = no data)
static uint8_t  weather_code = 0;          // 0=none, 1=SUN, 2=CLD, 3=OVC, 4=RAI, 5=SNO, 6=FOG
static uint8_t  weather_age = 0;           // seconds since last update (255=stale)
static uint8_t  ntp_synced = 0;            // 0=unsynced, 1=synced, 2=drift>24h
static uint32_t ntp_last_sync = 0;         // uptime_seconds at last sync

/************************** Key state **************************/
static uint16_t key_debounce[10] = {0};    // debounce counter per key
static uint16_t key_hold_time[10] = {0};   // hold duration in 10ms ticks per key
static uint8_t  key_pressed[10] = {0};     // current stable state per key

/************************** Edit state machine **************************/
volatile uint8_t edit_mode = EDIT_NONE;    // current edit mode
volatile uint8_t edit_field = 0;           // current field being edited
static uint16_t  edit_timeout = 0;         // 10ms ticks since last action
static uint8_t   edit_blink = 0;           // blink phase (toggled every ~300ms)
static uint8_t   edit_blink_timer = 0;     // 100ms ticks for blink

/************************** Night mode **************************/
volatile uint8_t night_mode = 0;           // 0=DAY, 1=NIGHT

/************************** Uptime **************************/
volatile uint32_t uptime_seconds = 0;

/************************** Protocol helpers **************************/
static uint8_t uart_tx_activity = 0;
static uint8_t suppress_key_event = 0;   // 1 = don't send *EVT:KEY (for *SET:KEY)
static uint8_t beep_timer = 0;           // remaining beep duration in 100ms ticks
static uint8_t beep_active = 0;          // 1 = beeper is sounding

/************************** Shared buffers (file-scope to save stack) **************************/
static char cmd_parse_buf[64];     // command parsing buffer
static char cmd_token_buf[32];     // reconstructed token
static char cmd_params_buf[64];    // original-case params copy

/************************** Utility functions **************************/
void Delay_us(uint32_t us) {
    uint32_t loops = us * (ui32SysClock / 1000000) / 3;
    while (loops--) {
        __NOP(); __NOP(); __NOP();
    }
}

void Delay(uint32_t value) {
    uint32_t ui32Loop;
    for (ui32Loop = 0; ui32Loop < value; ui32Loop++) {};
}

/************************** UART output helpers **************************/
void UARTStringPut(uint8_t *cMessage) {
    while (*cMessage != '\0')
        UARTCharPut(UART0_BASE, *(cMessage++));
}

void UARTStringPutNonBlocking(const char *cMessage) {
    while (*cMessage != '\0')
        UARTCharPutNonBlocking(UART0_BASE, *(cMessage++));
}

void send_response(const char *msg) {
    UARTStringPut((uint8_t *)msg);
    uart_tx_activity = 1;
}

/************************** Abbreviation matching **************************/
/**
 * Match input against a pattern with abbreviation support.
 * Uppercase chars in 'full' are mandatory; lowercase are optional.
 * Example: "MINute" matches "MIN", "MINU", "MINUT", "MINUTE" but not "MI".
 * Returns 1 on match, 0 otherwise.
 */
int match_abbrev(const char *input, const char *full) {
    int i;
    int input_len = (int)strlen(input);

    // Input must be a case-insensitive prefix of full
    for (i = 0; i < input_len && full[i]; i++) {
        if (toupper((unsigned char)input[i]) != toupper((unsigned char)full[i]))
            return 0;
    }
    if (i < input_len) return 0;  // input longer than full → no match

    // Input must cover all uppercase (mandatory) characters
    int min_len = 0;
    for (i = 0; full[i]; i++) {
        if (isupper((unsigned char)full[i])) min_len = i + 1;
    }

    return input_len >= min_len;
}

/************************** LED management **************************/
void led_update(void) {
    // If *SET:LED took over, skip all auto-logic — just write led_byte as-is
    if (led_takeover) {
        SysTickIntDisable();
        while (I2CMasterBusy(I2C0_BASE)) {};
        I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
        SysTickIntEnable();
        return;
    }

    uint8_t out = led_byte;

    // Heartbeat blink: toggle every 500ms
    if (led_heartbeat_timer >= 5) {
        led_heartbeat_timer = 0;
        if (out & LED_HEARTBEAT)
            out &= ~LED_HEARTBEAT;
        else
            out |= LED_HEARTBEAT;
    }

    // TX LED auto-off after ~300ms
    if (uart_tx_timer > 0) {
        uart_tx_timer--;
        out |= LED_TX;
    } else {
        out &= ~LED_TX;
    }

    // RX LED auto-off after ~300ms
    if (uart_rx_timer > 0) {
        uart_rx_timer--;
        out |= LED_RX;
    } else {
        out &= ~LED_RX;
    }

    // Night mode: only keep heartbeat LED; suppress all others
    if (night_mode) {
        out &= LED_HEARTBEAT;
    } else {
        // LED1 (ALARM): enabled=steady on, ringing=fast blink (toggle every 300ms)
        if (alarm_ringing) {
            // Fast blink: toggle via alarm_led_blink each 300ms tick
            if (alarm_led_blink)
                out |= LED_ALARM;
            else
                out &= ~LED_ALARM;
        } else if (ALARM_CUR_ENABLED) {
            out |= LED_ALARM;   // armed = steady on
        } else {
            out &= ~LED_ALARM;
        }

        // Edit LED follows edit_mode
        if (edit_mode != EDIT_NONE)
            out |= LED_EDIT;
        else
            out &= ~LED_EDIT;

        // Weather LEDs per spec §11/§15
        if (weather_code == 1 && weather_temp != -99)   // SUN
            out |= LED_SUN;
        else if (weather_code >= 4 && weather_temp != -99) // RAI/SNO
            out |= LED_RAI_SNO;
        else
            out &= ~(LED_SUN | LED_RAI_SNO);

        if (weather_temp >= 30)                       // >=30°C
            out |= LED_HI_TEMP;
        else
            out &= ~LED_HI_TEMP;
    }

    // NTP synced: overlay on RX bit (LED4) — RX only blinks 300ms then goes dark
    if (ntp_synced == 1)
        out |= LED_RX;

    led_byte = out;

    // Write to PCA9557 with SysTick disabled to avoid I2C bus conflict
    // (SysTick ISR also writes to TCA6424 via I2C0 at 1kHz)
    SysTickIntDisable();
    while (I2CMasterBusy(I2C0_BASE)) {};  // drain in-flight ISR transaction
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
    SysTickIntEnable();
}

/************************** Beeper control **************************/
// S800 uses PS1720P02 (C96061) PASSIVE piezoelectric buzzer.
// Resonant frequency: 2kHz. Rated voltage: 3V on PK5 (M0PWM7).
// Drive via PWM0 Gen3 Output7 at 2kHz / 50% duty.

void Beeper_PWM_Init(void);

void beep_on(void) {
    if (night_mode) return;
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_7,
        PWMGenPeriodGet(PWM0_BASE, PWM_GEN_3) / 2);
    PWMGenEnable(PWM0_BASE, PWM_GEN_3);
}

void beep_off(void) {
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_7, 0);
    PWMGenDisable(PWM0_BASE, PWM_GEN_3);
    beep_active = 0;
    beep_timer = 0;
}

/************************** Alarm state machine **************************/
void alarm_check(void) {
    if (alarm_snooze) return;

    // Compute slot index ONCE — macros re-evaluate (day_of_week - 1) each time
    uint8_t idx = day_of_week - 1;
    if (idx >= ALARM_SLOTS) return;
    if (!(alarm_enabled_mask & (1 << idx))) return;

    // Check if current time matches today's alarm time
    if (!alarm_ringing &&
        hour == alarm_hour[idx] &&
        minute == alarm_minute[idx] &&
        second == alarm_second[idx]) {
        // Start alarm
        alarm_ringing = 1;
        alarm_beep_phase = 0;
        alarm_beep_timer = 0;
        alarm_total_timer = 0;
        send_event_alarm(1);
    }

    // Alarm beep pattern: on-200ms, off-200ms, on-200ms, off-200ms...
    if (alarm_ringing) {
        alarm_beep_timer++;
        alarm_total_timer++;

        // Auto-stop after 10 seconds (100 * 100ms)
        if (alarm_total_timer >= 100) {
            alarm_stop();
            return;
        }

        // Beep pattern: 2 ticks on, 2 ticks off (200ms each)
        switch (alarm_beep_phase) {
            case 0:  // off before first beep
                beep_off();
                if (alarm_beep_timer >= 2) {
                    alarm_beep_phase = 1;
                    alarm_beep_timer = 0;
                }
                break;
            case 1:  // beep on
                beep_on();
                if (alarm_beep_timer >= 2) {
                    alarm_beep_phase = 2;
                    alarm_beep_timer = 0;
                }
                break;
            case 2:  // off between beeps
                beep_off();
                if (alarm_beep_timer >= 2) {
                    alarm_beep_phase = 3;
                    alarm_beep_timer = 0;
                }
                break;
            case 3:  // second beep on
                beep_on();
                if (alarm_beep_timer >= 2) {
                    alarm_beep_phase = 0;
                    alarm_beep_timer = 0;
                    // Pattern repeats until alarm_total_timer reaches 100 (10s)
                }
                break;
        }
    }
}

void alarm_stop(void) {
    alarm_ringing = 0;
    alarm_beep_phase = 0;
    alarm_beep_timer = 0;
    alarm_total_timer = 0;
    beep_off();
    send_event_alarm(0);
}

/************************** Character to 7-segment conversion **************************/
uint8_t CharToSeg7(char c) {
    if (c >= '0' && c <= '9') {
        return seg7[c - '0'];
    } else if (c >= 'A' && c <= 'Z') {
        switch (c) {
            case 'A': return 0x77; case 'B': return 0x7C; case 'C': return 0x39;
            case 'D': return 0x5E; case 'E': return 0x79; case 'F': return 0x71;
            case 'G': return 0x3D; case 'H': return 0x76; case 'I': return 0x30;
            case 'J': return 0x1E; case 'K': return 0x7A; case 'L': return 0x3C;
            case 'M': return 0x55; case 'N': return 0x37; case 'O': return 0x3F;
            case 'P': return 0x73; case 'Q': return 0x67; case 'R': return 0x70;
            case 'S': return 0x6D; case 'T': return 0x78; case 'U': return 0x3E;
            case 'V': return 0x7E; case 'W': return 0x6A; case 'X': return 0x36;
            case 'Y': return 0x6E; case 'Z': return 0x49;
            default: return 0x00;
        }
    } else if (c >= 'a' && c <= 'z') {
        switch (c) {
            case 'c': return 0x58;  // d,e,g
            case 'e': return 0x7B;  // a,b,d,e,f,g
            case 'h': return 0x74;  // c,e,f,g
            case 'i': return 0x10;  // e
            case 'j': return 0x0E;  // b,c,d
            case 'l': return 0x38;  // d,e,f
            case 'n': return 0x54;  // c,e,g
            case 'o': return 0x5C;  // c,d,e,g
            case 'r': return 0x50;  // e,g
            case 'u': return 0x1C;  // c,d,e
            default: return CharToSeg7(c - 'a' + 'A');
        }
    } else if (c == '.') {
        return DP_SEG;
    } else if (c == '-') {
        return 0x40;
    } else {
        return 0x00;  // blank for unknown characters
    }
}

/************************** ShowString (blocking, for boot) **************************/
void ShowString(const char *str, uint32_t duration, uint8_t led_state) {
    int j, k;
    uint8_t seg_data[8];

    for (j = 0; j < 8; j++) {
        if (str[j] == '\0') break;
        seg_data[j] = CharToSeg7(str[j]);
    }
    for (; j < 8; j++) {
        seg_data[j] = 0x00;
    }

    for (k = 0; k < duration; k++) {
        for (j = 0; j < 8; j++) {
            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
            Delay_us(10);

            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, seg_data[j]);
            Delay_us(10);

            uint8_t bit_pos = SCAN_REVERSE ? (7 - j) : j;
            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 1 << bit_pos);

            result = I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_state);

            Delay_us(100 * BRIGHTNESS);
        }
    }
}

/************************** Boot sequence **************************/
void Boot_Sequence(void) {
    SysTickIntDisable();

    // 1. All segments + LED full bright → full off
    ShowString("88888888", BOOT_FULL_BRIGHT, 0xFF);
    ShowString("        ", BOOT_FULL_OFF, 0x00);

    // 2. Student ID (blink 1 time)
    ShowString(STUDENT_ID, BOOT_STUDENT_ID / 2, 0xFF);
    ShowString("        ", 50, 0x00);
    ShowString(STUDENT_ID, BOOT_STUDENT_ID / 2, 0xFF);

    // 3. Blank
    ShowString("        ", BOOT_FULL_OFF, 0x00);

    // 4. Name (blink 1 time)
    ShowString(STUDENT_NAME, BOOT_NAME / 2, 0xFF);
    ShowString("        ", 50, 0x00);
    ShowString(STUDENT_NAME, BOOT_NAME / 2, 0xFF);

    // 5. Blank
    ShowString("        ", BOOT_FULL_OFF, 0x00);

    // 6. Software version
    ShowString(SOFTWARE_VERSION, BOOT_VERSION, 0x00);

    // 7. Blank
    ShowString("        ", 50, 0x00);

    // Reset display buffer
    memset(disp_buf, 0, sizeof(disp_buf));
    memset(dp_buf, 0, sizeof(dp_buf));
    memset(disp_chars, ' ', 8);

    SysTickIntEnable();
}

/************************** Time/date functions **************************/
uint8_t is_leap_year(uint16_t year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

uint8_t get_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month - 1];
}

void update_time(void) {
    second++;

    if (second >= 60) {
        second = 0;
        minute++;

        if (minute >= 60) {
            minute = 0;
            hour++;

            if (hour >= 24) {
                hour = 0;
                day++;

                if (day > get_days_in_month(year, month)) {
                    day = 1;
                    month++;

                    if (month > 12) {
                        month = 1;
                        year++;
                    }
                }
                // Advance day-of-week on midnight rollover
                day_of_week++;
                if (day_of_week > DOW_SUN) day_of_week = DOW_MON;
            }
        }
    }
}

/************************** Display update **************************/
void update_display(void) {
    char str[9] = "        ";
    int j;

    if (!disp_on) {
        // Display off: all blanks
        for (j = 0; j < 8; j++) {
            disp_buf[j] = 0x00;
            dp_buf[j] = 0;
            disp_chars[j] = ' ';
        }
        return;
    }

    // In night mode, only show HH:MM (4 chars), rest blank
    if (night_mode) {
        sprintf(str, "%02d%02d    ", hour, minute);
        memset(dp_buf, 0, sizeof(dp_buf));
        dp_buf[1] = 1;  // HH.MM separator
    }
    // Scroll message active → show scrolling text
    else if (scroll_active && scroll_len > 0) {
        // Build 8-char window into scroll message
        uint8_t pos = scroll_pos;
        for (j = 0; j < 8; j++) {
            // wrap: if pos+j >= len, pad with space or wrap from start
            if (pos + j < scroll_len) {
                str[j] = scroll_msg[pos + j];
            } else if (pos + j < scroll_len + 8) {
                str[j] = ' ';  // pad with space after message
            } else {
                // wrap-around: show beginning of message
                uint8_t wrap_idx = (pos + j - scroll_len - 8) % scroll_len;
                str[j] = scroll_msg[wrap_idx];
            }
        }
        memset(dp_buf, 0, sizeof(dp_buf));
    }
    // Normal time/date display
    else if (edit_mode != EDIT_NONE) {
        // Show edit values
        memset(dp_buf, 0, sizeof(dp_buf));
        if (edit_mode == EDIT_DATE) {
            sprintf(str, "%04d%02d%02d", year, month, day);
            dp_buf[3] = 1;  // YYYY.MM
            dp_buf[5] = 1;  // MM.DD
        } else if (edit_mode == EDIT_TIME) {
            sprintf(str, "%02d%02d%02d  ", hour, minute, second);
            dp_buf[1] = 1;  // HH.MM
            dp_buf[3] = 1;  // MM.SS
        } else if (edit_mode == EDIT_ALARM) {
            sprintf(str, "%02d%02d%02d  ", ALARM_CUR_HOUR, ALARM_CUR_MIN, ALARM_CUR_SEC);
            dp_buf[1] = 1;
            dp_buf[3] = 1;
        }
    }
    // Normal time/date display
    else {
        memset(dp_buf, 0, sizeof(dp_buf));

        switch (disp_mode) {
            case DISP_MODE_TIME:
                sprintf(str, "%02d%02d%02d  ", hour, minute, second);
                dp_buf[1] = 1;  // HH.MM
                dp_buf[3] = 1;  // MM.SS
                break;

            case DISP_MODE_DATE_SHORT:
                sprintf(str, "%02d%02d%02d  ", year % 100, month, day);
                dp_buf[1] = 1;
                dp_buf[3] = 1;
                break;

            case DISP_MODE_DATE_LONG:
                sprintf(str, "%04d%02d%02d", year, month, day);
                dp_buf[3] = 1;  // YYYY.MM
                dp_buf[5] = 1;  // MM.DD
                break;
        }
    }

    // FORMAT RIGHT: reverse display string and dp_buf together.
    // Dots are mirrored then shifted right by 1 (spec §7: "下一位").
    if (format_direction == 1) {
        char rev[9];
        uint8_t rev_dp[8] = {0};
        for (j = 0; j < 8; j++) {
            rev[j] = str[7 - j];
        }
        for (j = 0; j < 7; j++) {
            // mirror dp then shift right by 1:
            // dp at original pos k → mirrored to pos 7-k → shifted to pos 6-k
            rev_dp[j] = dp_buf[6 - j];
        }
        memcpy(str, rev, 8);
        memcpy(dp_buf, rev_dp, 8);
    }

    // Store ASCII chars for *EVT:DISP
    memcpy(disp_chars, str, 8);

    // Edit mode blinking: blank the current field during blink-on phase
    // (local behavior per §3.5, not mirrored to PC per protocol spec)
    if (edit_mode != EDIT_NONE && edit_blink) {
        if (edit_mode == EDIT_DATE) {
            // Edit DATE always shows YYYY.MMDD — year takes 4 digits
            uint8_t start, count;
            if (edit_field == EDIT_FIELD_YEAR) {
                start = 0; count = 4;
            } else if (edit_field == EDIT_FIELD_MONTH) {
                start = 4; count = 2;
            } else {
                start = 6; count = 2;
            }
            for (j = start; j < start + count && j < 8; j++) {
                str[j] = ' ';
            }
        } else if (edit_mode == EDIT_TIME || edit_mode == EDIT_ALARM) {
            uint8_t start = (edit_field == EDIT_FIELD_HOUR) ? 0 :
                            (edit_field == EDIT_FIELD_MINUTE) ? 2 : 4;
            uint8_t count = 2;
            for (j = start; j < start + count && j < 8; j++) {
                str[j] = ' ';
            }
        }
    }

    // Convert to segment patterns
    for (j = 0; j < 8; j++) {
        disp_buf[j] = CharToSeg7(str[j]);
        if (dp_buf[j] && SEPARATOR_STYLE == 0) {
            disp_buf[j] |= DP_SEG;
        }
    }
}

/************************** Event reporting (S800 → PC) **************************/
void send_event_disp(void) {
    char buf[64];
    uint8_t dp_hex = 0;
    int j;

    // Build dp hex: MSB = digit 0, LSB = digit 7
    for (j = 0; j < 8; j++) {
        if (dp_buf[j]) dp_hex |= (1 << (7 - j));
    }

    // Build character string (replace space with '_' per protocol)
    char safe_chars[9];
    for (j = 0; j < 8; j++) {
        safe_chars[j] = (disp_chars[j] == ' ') ? '_' : disp_chars[j];
    }
    safe_chars[8] = '\0';

    sprintf(buf, "*EVT:DISP %s %02X\r\n", safe_chars, dp_hex);
    send_response(buf);
}

void send_event_led(void) {
    char buf[32];
    sprintf(buf, "*EVT:LED %02X\r\n", led_byte);
    send_response(buf);
}

void send_event_key(uint8_t key_idx) {
    char buf[32];
    if (key_idx < NUM_KEYS) {
        sprintf(buf, "*EVT:KEY %s\r\n", KEY_NAMES[key_idx]);
        send_response(buf);
    }
}

void send_event_alarm(uint8_t on) {
    if (on)
        send_response("*EVT:ALARM\r\n");
    else
        send_response("*EVT:ALARM_OFF\r\n");
}

void send_event_edit(const char *type, const char *value) {
    char buf[64];
    sprintf(buf, "*EVT:EDIT %s %s\r\n", type, value);
    send_response(buf);
}

void send_event_mode(const char *state) {
    char buf[32];
    sprintf(buf, "*EVT:MODE %s\r\n", state);
    send_response(buf);
}

/************************** Key scanning **************************/
void key_scan(void) {
    uint8_t i2c_keys, gpio_keys;
    uint16_t raw_state;
    int i;

    // Read I2C keys from TCA6424 Port0 (8 keys, active low)
    SysTickIntDisable();
    while (I2CMasterBusy(I2C0_BASE)) {};  // drain ISR's in-flight transaction
    i2c_keys = I2C0_ReadByte(TCA6424_I2CADDR, TCA6424_INPUT_PORT0);
    SysTickIntEnable();

    // Read GPIO keys: PJ0=USER1, PJ1=USER2 (active low with pull-up)
    gpio_keys = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    // Build combined 10-bit raw state (0-7=I2C, 8=USER1, 9=USER2)
    // Both I2C and GPIO are active-low (0=press). Invert so 1=press.
    raw_state = (uint16_t)(~i2c_keys) & 0xFF;         // bits 0-7, inverted: 1=press
    if (!(gpio_keys & GPIO_PIN_0)) raw_state |= (1U << 8);  // USER1
    if (!(gpio_keys & GPIO_PIN_1)) raw_state |= (1U << 9);  // USER2

    // If in alarm ringing, check FUNC (key index 1) for quick stop
    if (alarm_ringing) {
        uint8_t func_pressed = !(i2c_keys & 0x02);  // FUNC = bit 1
        static uint8_t func_prev = 0;
        if (func_pressed && !func_prev) {
            alarm_stop();
            func_prev = 1;
            return;  // don't process other keys during alarm stop
        }
        func_prev = func_pressed;
        // During alarm, don't process other key actions
        // (alarm ringing blocks edit mode entry per §3.4)
        return;
    }

    // Debounce: each key must be stable for 2 consecutive scans (20ms)
    for (i = 0; i < NUM_KEYS; i++) {
        uint8_t is_pressed = (raw_state >> i) & 0x01;

        if (is_pressed) {
            if (key_debounce[i] < 3) key_debounce[i]++;
            if (key_debounce[i] == 2 && !key_pressed[i]) {
                // Key just pressed
                key_pressed[i] = 1;
                key_hold_time[i] = 0;
                key_action(i, 0);  // short press action
            }
        } else {
            if (key_debounce[i] > 0) key_debounce[i]--;
            if (key_debounce[i] == 0 && key_pressed[i]) {
                // Key just released
                key_pressed[i] = 0;
            }
        }

        // Track hold time for long press detection
        if (key_pressed[i]) {
            key_hold_time[i]++;
            // Long press: ~1 second (100 * 10ms)
            if (key_hold_time[i] == 100) {
                key_action(i, 1);  // long press action
            }
        }
    }
}

/************************** Key action dispatch **************************/
void key_action(uint8_t key_idx, uint8_t long_press) {
    // key_idx: 0=ADD,1=FUNC,2=SHIFT,3=SPEED,4=SAVE,5=FORMAT,6=DISP,7=EXT,8=USER1,9=USER2

    // Send *EVT:KEY to PC for physical keys only (not *SET:KEY simulated)
    if (!suppress_key_event) {
        send_event_key(key_idx);
    }

    switch (key_idx) {
        case 0: // ADD
            if (edit_mode != EDIT_NONE) {
                edit_timeout = 0;
                // Increment current field
                if (edit_mode == EDIT_DATE) {
                    if (edit_field == EDIT_FIELD_YEAR) {
                        year++; if (year > 2099) year = 2025;
                    } else if (edit_field == EDIT_FIELD_MONTH) {
                        month++; if (month > 12) month = 1;
                    } else if (edit_field == EDIT_FIELD_DAY) {
                        day++; if (day > get_days_in_month(year, month)) day = 1;
                    }
                } else if (edit_mode == EDIT_TIME) {
                    if (edit_field == EDIT_FIELD_HOUR) {
                        hour++; if (hour > 23) hour = 0;
                    } else if (edit_field == EDIT_FIELD_MINUTE) {
                        minute++; if (minute > 59) minute = 0;
                    } else if (edit_field == EDIT_FIELD_SECOND) {
                        second++; if (second > 59) second = 0;
                    }
                } else if (edit_mode == EDIT_ALARM) {
                    if (edit_field == EDIT_FIELD_HOUR) {
                        ALARM_CUR_HOUR++; if (ALARM_CUR_HOUR > 23) ALARM_CUR_HOUR = 0;
                    } else if (edit_field == EDIT_FIELD_MINUTE) {
                        ALARM_CUR_MIN++; if (ALARM_CUR_MIN > 59) ALARM_CUR_MIN = 0;
                    } else if (edit_field == EDIT_FIELD_SECOND) {
                        ALARM_CUR_SEC++; if (ALARM_CUR_SEC > 59) ALARM_CUR_SEC = 0;
                    }
                }
                update_display();
            }
            if (long_press) {
                // Long ADD: rapid add at ~5Hz
                // Handled by key_hold_time repeat
            }
            break;

        case 1: // FUNC
            if (alarm_ringing) {
                alarm_stop();  // stop alarm immediately
                break;
            }
            if (long_press) {
                // Long FUNC = save and exit
                if (edit_mode != EDIT_NONE) {
                    edit_exit(1);
                }
            } else {
                // Short FUNC: cycle edit mode in a loop
                if (edit_mode == EDIT_NONE) {
                    edit_enter(EDIT_DATE);
                } else if (edit_mode == EDIT_DATE) {
                    edit_enter(EDIT_TIME);
                } else if (edit_mode == EDIT_TIME) {
                    edit_enter(EDIT_ALARM);
                } else {
                    // EDIT_ALARM: loop back to DATE
                    edit_enter(EDIT_DATE);
                }
            }
            break;

        case 2: // SHIFT
            if (edit_mode != EDIT_NONE) {
                edit_timeout = 0;
                edit_field = (edit_field + 1) % 3;
            }
            break;

        case 3: // SPEED
            scroll_speed_level = !scroll_speed_level;  // toggle speed
            break;

        case 4: // SAVE
            edit_exit(1);  // save and exit
            break;

        case 5: // FORMAT
            format_direction = !format_direction;  // toggle LEFT/RIGHT
            scroll_pos = 0;  // reset scroll
            update_display();
            break;

        case 6: // DISP
            disp_mode++;
            if (disp_mode > DISP_MODE_DATE_LONG) disp_mode = DISP_MODE_TIME;
            scroll_active = 0;  // exit scroll mode
            update_display();
            break;

        case 7: // EXT — toggle alarm enable/disable for current day
            if (ALARM_CUR_ENABLED) {
                ALARM_SET_DISABLED;
            } else {
                ALARM_SET_ENABLED;
            }
            if (ALARM_CUR_ENABLED) {
                send_response("*EVT:ALARM:SET ON\r\n");
            } else {
                alarm_ringing = 0;
                beep_off();
                send_response("*EVT:ALARM:SET OFF\r\n");
            }
            // Brief visual confirmation: blink edit LED once
            {
                uint8_t saved = led_byte;
                led_byte |= LED_EDIT;
                SysTickIntDisable();
                while (I2CMasterBusy(I2C0_BASE)) {};
                I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
                SysTickIntEnable();
                Delay(100000);
                led_byte = saved;
                SysTickIntDisable();
                while (I2CMasterBusy(I2C0_BASE)) {};
                I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
                SysTickIntEnable();
            }
            break;

        case 8: // USER1 — NTP sync (PC handles via *EVT:KEY USER1)
            if (!suppress_key_event) send_event_key(8);
            break;

        case 9: // USER2 — short weather display
            if (weather_temp != -99) {
                char wbuf[33];
                const char *wname[] = {"---", "SUN", "CLD", "OVC", "RAI", "SNO", "FOG"};
                const char *wn = (weather_code <= 6) ? wname[weather_code] : "---";
                if (weather_temp >= 0)
                    sprintf(wbuf, "%2d C %s", weather_temp, wn);
                else
                    sprintf(wbuf, "%3dC %s", weather_temp, wn);
                cmd_set_msg(wbuf);
            } else {
                cmd_set_msg("-- C ---");
            }
            if (!suppress_key_event) send_event_key(9);
            break;
    }
}

/************************** Edit state machine **************************/
// Backup copies for cancel (restore original values)
static uint8_t  backup_hour, backup_minute, backup_second;
static uint16_t backup_year;
static uint8_t  backup_month, backup_day;
static uint8_t  backup_alarm_hour, backup_alarm_minute, backup_alarm_second;
static uint8_t  backup_alarm_enabled;

void edit_enter(uint8_t mode) {
    // Backup current values before entering edit mode
    backup_hour = hour; backup_minute = minute; backup_second = second;
    backup_year = year; backup_month = month; backup_day = day;
    backup_alarm_hour = ALARM_CUR_HOUR; backup_alarm_minute = ALARM_CUR_MIN;
    backup_alarm_second = ALARM_CUR_SEC;
    backup_alarm_enabled = ALARM_CUR_ENABLED ? 1 : 0;

    edit_mode = mode;
    edit_field = 0;
    edit_timeout = 0;
    edit_blink = 0;
    edit_blink_timer = 0;
    update_display();
}

void edit_exit(uint8_t save) {
    if (save && edit_mode != EDIT_NONE) {
        // Report edit save event
        char value[16];
        if (edit_mode == EDIT_DATE) {
            sprintf(value, "%04d.%02d.%02d", year, month, day);
            send_event_edit("DATE", value);
        } else if (edit_mode == EDIT_TIME) {
            sprintf(value, "%02d.%02d.%02d", hour, minute, second);
            send_event_edit("TIME", value);
        } else if (edit_mode == EDIT_ALARM) {
            sprintf(value, "%02d.%02d.%02d", ALARM_CUR_HOUR, ALARM_CUR_MIN, ALARM_CUR_SEC);
            send_event_edit("ALARM", value);
        }
    } else if (edit_mode != EDIT_NONE) {
        // Cancel: restore original values (only if actually in edit mode)
        hour = backup_hour; minute = backup_minute; second = backup_second;
        year = backup_year; month = backup_month; day = backup_day;
        ALARM_CUR_HOUR = backup_alarm_hour; ALARM_CUR_MIN = backup_alarm_minute;
        ALARM_CUR_SEC = backup_alarm_second;
        if (backup_alarm_enabled) ALARM_SET_ENABLED; else ALARM_SET_DISABLED;
    }

    edit_mode = EDIT_NONE;
    edit_field = 0;
    edit_timeout = 0;
    edit_blink = 0;
    update_display();
}

void edit_tick(void) {
    if (edit_mode == EDIT_NONE) return;

    // 5-second timeout: auto-exit without save
    edit_timeout++;
    if (edit_timeout >= 500) {  // 500 * 10ms = 5s
        edit_exit(0);  // cancel
    }
    // Blink is handled in the 100ms loop via edit_blink_timer
}

/************************** Command handlers **************************/

// *RST [DATE] [YEAR] [MONTH] [DATE]
// Reset clock/date/alarm to defaults, FORMAT=LEFT
void cmd_rst(const char *params) {
    // Parse optional parameters
    char buf[64];
    strncpy(buf, params, 64);
    buf[63] = '\0';

    // Uppercase for matching
    char *p = buf;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    // Default reset values
    uint8_t reset_clock = 1, reset_date = 1, reset_alarm = 1;

    // Check if specific sub-commands are given
    if (buf[0] != '\0') {
        reset_clock = reset_date = reset_alarm = 0;  // selective reset

        p = buf;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;

            // Extract token
            char *token = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            char saved = *p;
            *p = '\0';

            if (match_abbrev(token, "DATE"))     reset_date = 1;
            else if (match_abbrev(token, "YEAR"))  reset_date = 1;
            else if (match_abbrev(token, "MONTH")) reset_date = 1;
            else if (match_abbrev(token, "TIME"))   reset_clock = 1;
            else if (match_abbrev(token, "ALARM"))  reset_alarm = 1;
            else {
                // Unknown parameter
                char err[32];
                sprintf(err, "ERROR PARAM\r\n");
                send_response(err);
                return;
            }

            if (saved == '\0') break;
            p++;
        }
    }

    if (reset_date) {
        year = 2026; month = 6; day = 8;
    }
    if (reset_clock) {
        hour = 0; minute = 0; second = 0;
    }
    if (reset_alarm) {
        int a_i;
        for (a_i = 0; a_i < ALARM_SLOTS; a_i++) {
            alarm_hour[a_i] = 6; alarm_minute[a_i] = 0; alarm_second[a_i] = 0;
        }
        alarm_enabled_mask = 0x1F;  // Mon-Fri enabled (bits 0-4)
    }

    format_direction = 0;   // FORMAT=LEFT
    disp_on = 1;
    scroll_active = 0;
    scroll_cycle_count = 0; scroll_static_timer = 0;
    led_takeover = 0;        // exit LED takeover
    disp_mode = DISP_MODE_TIME;
    edit_exit(0);

    update_display();
    send_response("OK\r\n");
}

// *SET:DATE YEAR <val> MONTH <val> DATE <val>
// Also accepts *SET:DATE YEAR MONTH DATE <y> <m> <d> (keywords-first)
void cmd_set_date(const char *params) {
    char buf[64];
    strncpy(buf, params, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    // Gather all tokens and upcase them
    char *tokens[8]; int ntok = 0;
    char *p = buf;
    while (*p && ntok < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[ntok] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
        ntok++;
    }
    // Upcase
    int i;
    for (i = 0; i < ntok; i++) {
        char *s = tokens[i];
        while (*s) { *s = toupper((unsigned char)*s); s++; }
    }

    // Track which keywords were explicitly mentioned
    int yr_kw = 0, mo_kw = 0, dy_kw = 0;
    int kw_count = 0;
    for (i = 0; i < ntok; i++) {
        if (match_abbrev(tokens[i], "YEAR"))  { yr_kw = 1; kw_count++; continue; }
        if (match_abbrev(tokens[i], "MONTH")) { mo_kw = 1; kw_count++; continue; }
        if (match_abbrev(tokens[i], "DATE"))  { dy_kw = 1; kw_count++; continue; }
    }
    if (kw_count == 0) { send_response("ERROR SYNTAX\r\n"); return; }

    // Assign values in order, but only to keywords that were actually present
    int yr_val = -1, mo_val = -1, dy_val = -1;
    int val_idx = 0;
    int vals[4] = {-1, -1, -1, -1};
    for (i = 0; i < ntok; i++) {
        if (match_abbrev(tokens[i], "YEAR"))  continue;
        if (match_abbrev(tokens[i], "MONTH")) continue;
        if (match_abbrev(tokens[i], "DATE"))  continue;
        if (!isdigit((unsigned char)tokens[i][0]) && tokens[i][0] != '-')
            { send_response("ERROR SYNTAX\r\n"); return; }
        if (val_idx < 4) vals[val_idx++] = atoi(tokens[i]);
    }
    val_idx = 0;
    if (yr_kw && val_idx < 4) yr_val = vals[val_idx++];
    if (mo_kw && val_idx < 4) mo_val = vals[val_idx++];
    if (dy_kw && val_idx < 4) dy_val = vals[val_idx++];

    int any_set = 0;
    if (yr_val >= 0) {
        if (yr_val < 2025 || yr_val > 2099) { send_response("ERROR RANGE\r\n"); return; }
        year = (uint16_t)yr_val; any_set = 1;
    }
    if (mo_val >= 0) {
        if (mo_val < 1 || mo_val > 12) { send_response("ERROR RANGE\r\n"); return; }
        month = (uint8_t)mo_val; any_set = 1;
    }
    if (dy_val >= 0) {
        if (dy_val < 1 || dy_val > 31) { send_response("ERROR RANGE\r\n"); return; }
        day = (uint8_t)dy_val; any_set = 1;
    }

    if (day > get_days_in_month(year, month))
        day = get_days_in_month(year, month);

    if (any_set) { update_display(); send_response("OK\r\n"); }
    else send_response("ERROR SYNTAX\r\n");
}

// *SET:TIME HOUR <val> MINute <val> SECond <val> / OFF
// Also accepts *SET:TIME HOUR MIN SEC <h> <m> <s> (keywords-first)
void cmd_set_time(const char *params) {
    char buf[64];
    strncpy(buf, params, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    // Gather all tokens
    char *tokens[8]; int ntok = 0;
    char *p = buf;
    while (*p && ntok < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[ntok] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
        ntok++;
    }
    int i;
    for (i = 0; i < ntok; i++) {
        char *s = tokens[i];
        while (*s) { *s = toupper((unsigned char)*s); s++; }
    }

    // Check for OFF
    for (i = 0; i < ntok; i++) {
        if (match_abbrev(tokens[i], "OFF")) {
            disp_on = 0; update_display(); send_response("OK\r\n"); return;
        }
    }

    int h_val = -1, m_val = -1, s_val = -1;
    for (i = 0; i < ntok; i++) {
        if (match_abbrev(tokens[i], "HOUR"))   continue;
        if (match_abbrev(tokens[i], "MINute")) continue;
        if (match_abbrev(tokens[i], "SECond")) continue;
        if (!isdigit((unsigned char)tokens[i][0]) && tokens[i][0] != '-')
            { send_response("ERROR SYNTAX\r\n"); return; }
        int v = atoi(tokens[i]);
        if (h_val < 0) h_val = v;
        else if (m_val < 0) m_val = v;
        else s_val = v;
    }

    int any_set = 0;
    if (h_val >= 0) { if (h_val > 23) { send_response("ERROR RANGE\r\n"); return; }
        hour = (uint8_t)h_val; any_set = 1; }
    if (m_val >= 0) { if (m_val > 59) { send_response("ERROR RANGE\r\n"); return; }
        minute = (uint8_t)m_val; second = 0; any_set = 1; }
    if (s_val >= 0) { if (s_val > 59) { send_response("ERROR RANGE\r\n"); return; }
        second = (uint8_t)s_val; any_set = 1; }

    if (any_set) { update_display(); send_response("OK\r\n"); }
    else send_response("ERROR SYNTAX\r\n");
}

// *SET:DISPlay ON / OFF
void cmd_set_display(const char *params) {
    char buf[16], *p;
    strncpy(buf, params, 16);
    buf[15] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    if (strcmp(p, "ON") == 0) {
        disp_on = 1;
        scroll_active = 0;
        update_display();
        send_response("OK\r\n");
    } else if (strcmp(p, "OFF") == 0) {
        disp_on = 0;
        update_display();
        send_response("OK\r\n");
    } else {
        send_response("ERROR PARAM\r\n");
    }
}

// *SET:FORMAT LEFT / RIGHT
void cmd_set_format(const char *params) {
    char buf[16], *p;
    strncpy(buf, params, 16);
    buf[15] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    if (strcmp(p, "LEFT") == 0) {
        format_direction = 0;
        scroll_pos = 0;
        update_display();
        send_response("OK\r\n");
    } else if (strcmp(p, "RIGHT") == 0) {
        format_direction = 1;
        scroll_pos = 0;
        update_display();
        send_response("OK\r\n");
    } else {
        send_response("ERROR PARAM\r\n");
    }
}

// *SET:MSG <text>  — up to 32 chars, preserve case
void cmd_set_msg(const char *params) {
    // params points to the message text (original case preserved)
    const char *p = params;
    while (*p == ' ' || *p == '\t') p++;

    uint8_t len = (uint8_t)strlen(p);
    if (len > 32) len = 32;

    if (len == 0) {
        send_response("ERROR SYNTAX\r\n");
        return;
    }

    memset(scroll_msg, 0, sizeof(scroll_msg));
    memcpy(scroll_msg, p, len);
    scroll_len = len;
    scroll_pos = 0;
    scroll_active = 1;
    scroll_timer = 0;
    scroll_cycle_count = 0;    // reset auto-return cycle counter
    scroll_static_timer = 0;   // reset static hold timer

    update_display();
    send_response("OK\r\n");
}

// *SET:BEEP <10-5000>  — remote beep duration in ms (non-blocking)
void cmd_set_beep(const char *params) {
    const char *p = params;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0') {
        send_response("ERROR PARAM\r\n");
        return;
    }

    int duration = atoi(p);
    if (duration < 10 || duration > 5000) {
        send_response("ERROR RANGE\r\n");
        return;
    }

    // Non-blocking: start beep and set timer
    beep_on();
    beep_active = 1;
    beep_timer = (uint8_t)((duration + 50) / 100);  // convert ms to 100ms ticks
    if (beep_timer < 1) beep_timer = 1;

    send_response("OK\r\n");
}

// *SET:LED <hex2>  — direct LED byte set (takeover mode)
void cmd_set_led(const char *params) {
    const char *p = params;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0') {
        send_response("ERROR PARAM\r\n");
        return;
    }

    char hex[3] = {0};
    hex[0] = (p[0] != '\0') ? p[0] : '0';
    hex[1] = (p[1] != '\0') ? p[1] : '0';

    int val;
    if (sscanf(hex, "%2x", &val) != 1) {
        // Try with 0x prefix
        if (sscanf(p, "%x", &val) != 1) {
            send_response("ERROR PARAM\r\n");
            return;
        }
    }

    uint8_t v = (uint8_t)(val & 0xFF);

    if (v == 0x00) {
        // 00 exits takeover and restores auto-LED logic
        led_takeover = 0;
        led_byte = LED_HEARTBEAT;  // restore default heartbeat
    } else {
        // Enter takeover: suppress all auto-LED logic
        led_takeover = 1;
        led_byte = v;
    }

    SysTickIntDisable();
    while (I2CMasterBusy(I2C0_BASE)) {};
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
    SysTickIntEnable();
    send_response("OK\r\n");
}

// *SET:KEY <NAME>  — simulate key press (10 key names)
void cmd_set_key(const char *params) {
    char buf[16], *p;
    strncpy(buf, params, 16);
    buf[15] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    // Match against key names
    int key_idx = -1;
    if (strcmp(p, "FUNC") == 0)   key_idx = 1;
    else if (strcmp(p, "SHIFT") == 0)  key_idx = 2;
    else if (strcmp(p, "ADD") == 0)    key_idx = 0;
    else if (strcmp(p, "SAVE") == 0)   key_idx = 4;
    else if (strcmp(p, "DISP") == 0)   key_idx = 6;
    else if (strcmp(p, "SPEED") == 0)  key_idx = 3;
    else if (strcmp(p, "FORMAT") == 0) key_idx = 5;
    else if (strcmp(p, "EXT") == 0)    key_idx = 7;
    else if (strcmp(p, "USER1") == 0)  key_idx = 8;
    else if (strcmp(p, "USER2") == 0)  key_idx = 9;

    if (key_idx < 0) {
        send_response("ERROR PARAM\r\n");
        return;
    }

    // Simulate key action (do NOT send *EVT:KEY to avoid PC-side loopback)
    suppress_key_event = 1;
    key_action(key_idx, 0);
    suppress_key_event = 0;
    send_response("OK\r\n");
}

// *SET:MODE DAY / NIGHT (extension)
void cmd_set_mode(const char *params) {
    char buf[16], *p;
    strncpy(buf, params, 16);
    buf[15] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    if (strcmp(p, "DAY") == 0) {
        night_mode = 0;
        alarm_snooze = 0;
        update_display();
        send_event_mode("DAY");
        send_response("OK\r\n");
    } else if (strcmp(p, "NIGHT") == 0 || strcmp(p, "NIG") == 0) {
        // "NIG" for "NIGHT" abbreviation (NIGht)
        night_mode = 1;
        alarm_snooze = 1;  // suppress beeper at night
        update_display();
        send_event_mode("NIGHT");
        send_response("OK\r\n");
    } else {
        send_response("ERROR PARAM\r\n");
    }
}

// *SET:ALARM [slot] HOUR <val> MINute <val> SECond <val> / OFF / ON
// Also accepts *SET:ALARM HOUR MIN SEC <h> <m> <s> (keywords-first)
// slot (optional): MON/TUE/WED/THU/FRI/SAT/SUN/ALL (default=current day)
void cmd_set_alarm(const char *params) {
    char buf[64];
    strncpy(buf, params, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    // Gather all tokens and upcase
    char *tokens[10]; int ntok = 0;
    char *p = buf;
    while (*p && ntok < 10) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[ntok] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
        ntok++;
    }
    if (ntok == 0) { send_response("ERROR SYNTAX\r\n"); return; }
    int i;
    for (i = 0; i < ntok; i++) {
        char *s = tokens[i];
        while (*s) { *s = toupper((unsigned char)*s); s++; }
    }

    // Check first token for day-of-week slot
    int slot = ALARM_IDX;  // default=current day
    int start_i = 0;
    if (match_abbrev(tokens[0], "MONday"))      { slot = 0; start_i = 1; }
    else if (match_abbrev(tokens[0], "TUEsday")) { slot = 1; start_i = 1; }
    else if (match_abbrev(tokens[0], "WEDnesday")){ slot = 2; start_i = 1; }
    else if (match_abbrev(tokens[0], "THUrsday")) { slot = 3; start_i = 1; }
    else if (match_abbrev(tokens[0], "FRIday"))  { slot = 4; start_i = 1; }
    else if (match_abbrev(tokens[0], "SATurday")) { slot = 5; start_i = 1; }
    else if (match_abbrev(tokens[0], "SUNday"))   { slot = 6; start_i = 1; }
    else if (match_abbrev(tokens[0], "ALL"))      { slot = -1; start_i = 1; }

    int set_slot_start = (slot == -1) ? 0 : slot;
    int set_slot_end   = (slot == -1) ? ALARM_SLOTS : slot + 1;

    // OFF / ON
    for (i = start_i; i < ntok; i++) {
        if (match_abbrev(tokens[i], "OFF")) {
            int si;
            for (si = set_slot_start; si < set_slot_end; si++)
                alarm_enabled_mask &= ~(1 << si);
            alarm_ringing = 0; beep_off();
            send_response("OK\r\n"); return;
        }
        if (match_abbrev(tokens[i], "ON")) {
            int si;
            for (si = set_slot_start; si < set_slot_end; si++)
                alarm_enabled_mask |= (1 << si);
            send_response("OK\r\n"); return;
        }
    }

    // Identify time values
    int h_val = -1, m_val = -1, s_val = -1;
    for (i = start_i; i < ntok; i++) {
        if (match_abbrev(tokens[i], "HOUR"))   continue;
        if (match_abbrev(tokens[i], "MINute")) continue;
        if (match_abbrev(tokens[i], "SECond")) continue;
        if (!isdigit((unsigned char)tokens[i][0]) && tokens[i][0] != '-')
            { send_response("ERROR SYNTAX\r\n"); return; }
        int v = atoi(tokens[i]);
        if (h_val < 0) h_val = v;
        else if (m_val < 0) m_val = v;
        else s_val = v;
    }

    int any_set = 0;
    if (h_val >= 0) { if (h_val > 23) { send_response("ERROR RANGE\r\n"); return; }
        int si; for (si = set_slot_start; si < set_slot_end; si++)
            alarm_hour[si] = (uint8_t)h_val; any_set = 1; }
    if (m_val >= 0) { if (m_val > 59) { send_response("ERROR RANGE\r\n"); return; }
        int si; for (si = set_slot_start; si < set_slot_end; si++)
            alarm_minute[si] = (uint8_t)m_val; any_set = 1; }
    if (s_val >= 0) { if (s_val > 59) { send_response("ERROR RANGE\r\n"); return; }
        int si; for (si = set_slot_start; si < set_slot_end; si++)
            alarm_second[si] = (uint8_t)s_val; any_set = 1; }

    if (any_set) {
        int si;
        for (si = set_slot_start; si < set_slot_end; si++)
            alarm_enabled_mask |= (1 << si);
        send_response("OK\r\n");
    } else {
        send_response("ERROR SYNTAX\r\n");
    }
}

// *GET [DATE] [TIME] [FORMAT] [ALARM] [DISP] [MODE]
void cmd_get(const char *params) {
    char buf[32], *p;
    strncpy(buf, params, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0' || match_abbrev(p, "TIME")) {
        char resp[48];
        if (format_direction == 1) {
            // FORMAT RIGHT: value order reversed AND each 2-digit number digit-reversed
            uint8_t rh = (hour % 10) * 10 + (hour / 10);
            uint8_t rm = (minute % 10) * 10 + (minute / 10);
            uint8_t rs = (second % 10) * 10 + (second / 10);
            sprintf(resp, "OK %02d.%02d.%02d\r\n", rs, rm, rh);
        } else {
            sprintf(resp, "OK %02d.%02d.%02d\r\n", hour, minute, second);
        }
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "DATE")) {
        char resp[48];
        if (format_direction == 1) {
            uint8_t rd = (day % 10) * 10 + (day / 10);
            uint8_t rm = (month % 10) * 10 + (month / 10);
            // 4-digit year digit-reversed: 2026 → 6202
            uint16_t ry = ((year % 10) * 1000 + ((year/10) % 10) * 100 +
                           ((year/100) % 10) * 10 + (year / 1000));
            sprintf(resp, "OK %02d.%02d.%04d\r\n", rd, rm, ry);
        } else {
            sprintf(resp, "OK %04d.%02d.%02d\r\n", year, month, day);
        }
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "FORMAT")) {
        char resp[48];
        sprintf(resp, "OK %s\r\n", format_direction ? "RIGHT" : "LEFT");
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "ALARM")) {
        // Compact format to avoid UART RX overflow: MON,HH,MM,SS,ON;TUE,...
        char resp[128];
        int off = sprintf(resp, "OK ");
        int si;
        for (si = 0; si < ALARM_SLOTS; si++) {
            off += sprintf(resp + off, "%s%s,%d,%d,%d,%d",
                si ? " ":"", DOW_NAMES[si + 1],
                alarm_hour[si], alarm_minute[si], alarm_second[si],
                (alarm_enabled_mask & (1 << si)) ? 1 : 0);
        }
        sprintf(resp + off, "\r\n");
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "DISP")) {
        char resp[48];
        sprintf(resp, "OK %s\r\n", disp_on ? "ON" : "OFF");
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "MODE")) {
        char resp[48];
        sprintf(resp, "OK %s\r\n", night_mode ? "NIGHT" : "DAY");
        send_response(resp);
        return;
    }

    send_response("ERROR SYNTAX\r\n");
}

// *SET:WEA <temp> <COND>  — weather data from PC (spec §15)
// temp: -40..+50, COND: SUN/CLD/OVC/RAI/SNO/FOG
void cmd_set_wea(const char *params) {
    const char *p = params;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') { send_response("ERROR PARAM\r\n"); return; }
    int t = atoi(p);
    if (t < -40 || t > 50) { send_response("ERROR RANGE\r\n"); return; }
    weather_temp = (int8_t)t;
    while (*p && *p != ' ') p++;
    while (*p == ' ' || *p == '\t') p++;
    char buf[8];
    strncpy(buf, p, 4);
    buf[4] = '\0';
    char *s = buf;
    while (*s) { *s = toupper((unsigned char)*s); s++; }
    if (match_abbrev(buf, "SUN"))      weather_code = 1;
    else if (match_abbrev(buf, "CLD")) weather_code = 2;
    else if (match_abbrev(buf, "OVC")) weather_code = 3;
    else if (match_abbrev(buf, "RAI")) weather_code = 4;
    else if (match_abbrev(buf, "SNO")) weather_code = 5;
    else if (match_abbrev(buf, "FOG")) weather_code = 6;
    else { send_response("ERROR PARAM\r\n"); return; }
    weather_age = 0;
    send_response("OK\r\n");
}

// *PING
void cmd_ping(void) {
    char resp[32];
    sprintf(resp, "*PONG %lu\r\n", uptime_seconds);
    send_response(resp);
}

/************************** UART command processor **************************/
void process_uart_command(void) {
    uart_cmd_ready = 0;

    strncpy(cmd_parse_buf, (const char *)receive, sizeof(cmd_parse_buf));
    cmd_parse_buf[sizeof(cmd_parse_buf) - 1] = '\0';
    memset((void *)receive, 0, sizeof(receive));
    i = 0;

    // Trim whitespace
    char *p = cmd_parse_buf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    int len = (int)strlen(p);
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t' ||
           p[len-1] == '\r' || p[len-1] == '\n')) {
        p[--len] = '\0';
    }

    if (*p == '\0') return;

    // Check for AT command (legacy support)
    if (strncmp(p, "AT+", 3) == 0) {
        for (int j = 0; p[j]; j++) p[j] = toupper((unsigned char)p[j]);
        if (strcmp(p, "AT+CLASS#") == 0) {
            send_response((char *)CLASS);
            send_response("\r\n");
        } else if (strcmp(p, "AT+STUDENTCODE#") == 0) {
            send_response((char *)CODE);
            send_response("\r\n");
        }
        return;
    }

    // Extract command token (up to first space or end-of-string)
    char *token_start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    char saved_token_end = *p;
    *p = '\0';
    char *params = (saved_token_end == '\0') ? p : p + 1;
    while (*params == ' ' || *params == '\t') params++;

    // Uppercase token
    for (int i = 0; token_start[i]; i++)
        token_start[i] = toupper((unsigned char)token_start[i]);

    // ---- Handle space before colon: "*SET : TIME ..." ----
    if (token_start[0] == '*' && strchr(token_start, ':') == NULL &&
        params[0] == ':') {
        char *sub_start = params + 1;  // skip ':'
        while (*sub_start == ' ' || *sub_start == '\t') sub_start++;
        char *sub_end = sub_start;
        while (*sub_end && *sub_end != ' ' && *sub_end != '\t') sub_end++;
        char saved_sub = *sub_end;
        *sub_end = '\0';

        snprintf(cmd_token_buf, sizeof(cmd_token_buf), "%s:%s", token_start, sub_start);
        *sub_end = saved_sub;
        params = sub_end;
        while (*params == ' ' || *params == '\t') params++;
    } else {
        strncpy(cmd_token_buf, token_start, sizeof(cmd_token_buf));
        cmd_token_buf[sizeof(cmd_token_buf) - 1] = '\0';
    }

    // Save original-case params for commands that need it (MSG)
    strncpy(cmd_params_buf, params, sizeof(cmd_params_buf));
    cmd_params_buf[sizeof(cmd_params_buf) - 1] = '\0';

    // Use reconstructed token for dispatch
    char *tok = cmd_token_buf;
    #define MATCH_CMD(tok, full, minlen) \
        ((int)strlen(tok) >= (minlen) && strncmp(tok, full, strlen(tok)) == 0)

    // Dispatch command
    if (MATCH_CMD(tok, "*RST", 4)) {
        cmd_rst(params);
    } else if (MATCH_CMD(tok, "*SET:DATE", 8)) {
        cmd_set_date(params);
    } else if (MATCH_CMD(tok, "*SET:TIME", 8)) {
        cmd_set_time(params);
    } else if (MATCH_CMD(tok, "*SET:DISPLAY", 9)) {
        cmd_set_display(params);
    } else if (MATCH_CMD(tok, "*SET:FORMAT", 9)) {
        cmd_set_format(params);
    } else if (MATCH_CMD(tok, "*SET:MSG", 7)) {
        // Use original-case params for message text
        cmd_set_msg(cmd_params_buf);
    } else if (MATCH_CMD(tok, "*SET:BEEP", 8)) {
        cmd_set_beep(params);
    } else if (MATCH_CMD(tok, "*SET:LED", 7)) {
        cmd_set_led(params);
    } else if (MATCH_CMD(tok, "*SET:KEY", 7)) {
        cmd_set_key(params);
    } else if (MATCH_CMD(tok, "*SET:MODE", 8)) {
        cmd_set_mode(params);
    } else if (MATCH_CMD(tok, "*SET:ALARM", 9)) {
        cmd_set_alarm(params);
    } else if (MATCH_CMD(tok, "*SET:WEA", 7)) {
        cmd_set_wea(params);
    } else if (strncmp(tok, "*GET", 4) == 0 &&
               (int)strlen(tok) <= 12) {
        const char *sub = params;
        if (tok[4] == ':' && tok[5] != '\0') {
            sub = tok + 5;  // sub-command appended to "*GET:"
        } else if (tok[4] == ':') {
            // trailing colon, sub-command is in params (e.g. *GET: TIME)
            sub = params;
        } else if (tok[4] != '\0') {
            sub = tok + 4;  // no colon, e.g. *GETTIME
        }
        cmd_get(sub);
    } else if (MATCH_CMD(tok, "*PING", 5)) {
        cmd_ping();
    } else {
        char err[46];
        sprintf(err, "ERROR '%s'\r\n", tok);
        send_response(err);
    }
    #undef MATCH_CMD
}

/************************** Main function **************************/
int main(void) {
    volatile uint16_t gpio_flash_cnt;

    // 25MHz external crystal → PLL 480MHz → SYSDIV 24 = 20MHz (exact)
    // UART 115200 error: 0.06% vs PIOSC ±3% — eliminates bit corruption
    ui32SysClock = SysCtlClockFreqSet(
        (SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480),
        20000000);

    SysTickPeriodSet(ui32SysClock / SYSTICK_FREQUENCY);
    SysTickEnable();
    SysTickIntEnable();

    S800_GPIO_Init();
    S800_I2C0_Init();
    S800_UART_Init();
    Beeper_PWM_Init();  // set up 2kHz PWM on PK5 for PS1720P02 passive buzzer

    IntEnable(INT_UART0);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    IntMasterEnable();

    ui32IntPriorityMask = IntPriorityMaskGet();
    IntPriorityGroupingSet(7);
    IntPrioritySet(INT_UART0, 0x00);
    IntPrioritySet(FAULT_SYSTICK, 0xe0);

    /************************** Boot sequence **************************/
    Boot_Sequence();
    update_display();

    // Initial LED state: heartbeat on
    led_byte = LED_HEARTBEAT;
    SysTickIntDisable();
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
    SysTickIntEnable();

    // Initialize alarm slots: workdays 08:30, weekends off
    {
        int a_i;
        for (a_i = 0; a_i < ALARM_SLOTS; a_i++) {
            alarm_hour[a_i] = 6; alarm_minute[a_i] = 0; alarm_second[a_i] = 0;
        }
        alarm_enabled_mask = 0x1F;  // Mon-Fri enabled
    }

    // Main loop
    gpio_flash_cnt = 0;
    scroll_timer = 0;

    while (1) {
        // ---- 10ms tasks ----
        if (systick_10ms_status) {
            systick_10ms_status = 0;

            // GPIOF heartbeat LED (board-level indicator)
            if (++gpio_flash_cnt >= GPIO_FLASHTIME / 10) {
                gpio_flash_cnt = 0;
                if (gpio_status)
                    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0, GPIO_PIN_0);
                else
                    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0, 0);
                gpio_status = !gpio_status;
            }

            // Key scanning
            key_scan();

            // Edit timeout tick
            edit_tick();

            // LED TX/RX timer
            if (uart_tx_activity) { uart_tx_timer = 3; uart_tx_activity = 0; }
        }

        // ---- 100ms tasks ----
        if (systick_100ms_status) {
            systick_100ms_status = 0;

            // LED heartbeat timer
            led_heartbeat_timer++;

            // Alarm LED fast-blink phase: toggle every 2 ticks (200ms)
            if (alarm_ringing) {
                static uint8_t alarm_blink_timer = 0;
                alarm_blink_timer++;
                if (alarm_blink_timer >= 2) {
                    alarm_blink_timer = 0;
                    alarm_led_blink = !alarm_led_blink;
                }
            }

            // Update LED (writes to PCA9557)
            led_update();

            // Edit blink timer (runs even when display isn't updated each second)
            if (edit_mode != EDIT_NONE) {
                edit_blink_timer++;
                if (edit_blink_timer >= 3) {  // 300ms blink period
                    edit_blink_timer = 0;
                    edit_blink = !edit_blink;
                    update_display();  // refresh to show/hide blinking fields
                }
            }

            // Scroll processing (with auto-return per §12)
            if (scroll_active) {
                if (scroll_len <= 8) {
                    // ≤8 chars: static display for 2-3s then auto-return to clock
                    scroll_static_timer++;
                    if (scroll_static_timer >= 25) {  // 25*100ms = 2.5s
                        scroll_active = 0;
                        scroll_len = 0;
                        scroll_static_timer = 0;
                        scroll_pos = 0;
                        memset(scroll_msg, 0, sizeof(scroll_msg));
                        update_display();
                    }
                } else {
                    // >8 chars: advance; auto-return after 1 full cycle
                    scroll_timer++;
                    uint8_t speed = scroll_speed_level ? SCROLL_SPEED_FAST : SCROLL_SPEED_SLOW;
                    if (scroll_timer >= speed) {
                        scroll_timer = 0;
                        if (format_direction == 0) {  // LEFT
                            scroll_pos++;
                            if (scroll_pos >= scroll_len + 8) {
                                scroll_pos = 0;
                                scroll_cycle_count++;
                            }
                        } else {  // RIGHT
                            if (scroll_pos == 0) {
                                scroll_pos = scroll_len + 8 - 1;
                                scroll_cycle_count++;
                            } else {
                                scroll_pos--;
                            }
                        }
                        // Auto-return after 1 full cycle
                        if (scroll_cycle_count >= 1) {
                            scroll_active = 0;
                            scroll_len = 0;
                            scroll_cycle_count = 0;
                            scroll_pos = 0;
                            scroll_timer = 0;
                            memset(scroll_msg, 0, sizeof(scroll_msg));
                        }
                        update_display();
                    }
                }
            }

            // Second tick
            if (++sec_counter >= 10) {  // 10 * 100ms = 1s
                sec_counter = 0;
                update_time();
                update_display();
                uptime_seconds++;

                // 1Hz heartbeat: send *EVT:DISP + *EVT:LED
                send_event_disp();
                send_event_led();

                // Weather age: max 255 seconds before stale
                if (weather_temp != -99 && weather_age < 255)
                    weather_age++;

                // NTP drift check: >24h since last sync → DRIFT
                if (ntp_synced == 1 &&
                    uptime_seconds - ntp_last_sync > 86400)
                    ntp_synced = 2;
            }

            // Alarm state machine
            alarm_check();

            // Non-blocking beep timer
            if (beep_active) {
                if (beep_timer > 0) {
                    beep_timer--;
                }
                if (beep_timer == 0) {
                    beep_off();
                    beep_active = 0;
                }
            }
        }

        // ---- UART command processing ----
        if (uart_cmd_ready) {
            process_uart_command();
        }
    }
}

/************************** Hardware initialization functions **************************/
void S800_UART_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA));

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);

    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, ui32SysClock, 115200,
        (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX6_8, UART_FIFO_RX6_8);

    UARTStringPut((uint8_t *)"\r\nClock System Ready\r\n");
}

void S800_GPIO_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF));
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOJ));
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPION));

    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_0);    // heartbeat LED
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0);    // USER1 indicator
    // PN1 (beeper) is now driven by PWM0 on PK5 — see Beeper_PWM_Init()
    GPIOPadConfigSet(GPIO_PORTN_BASE, GPIO_PIN_0,
        GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1,
        GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
}

void S800_I2C0_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    I2CMasterInitExpClk(I2C0_BASE, ui32SysClock, true);
    I2CMasterEnable(I2C0_BASE);

    // TCA6424: Port0 = input (keys), Port1 = output (segments), Port2 = output (digit select)
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xFF);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00);

    // PCA9557: all output (LEDs)
    (void)I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00);
    (void)I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, 0xFF);  // all off (active low)
}

/************************** Beeper PWM Init (2kHz on PK5 via M0PWM7) **************************/
void Beeper_PWM_Init(void) {
    // Configure PWM0 Gen3 Output7 (PK5) at 2kHz / 50% duty for PS1720P02 passive buzzer.
    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0));

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOK);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOK));

    GPIOPinConfigure(GPIO_PK5_M0PWM7);           // PK5 alternate function = M0PWM7
    GPIOPinTypePWM(GPIO_PORTK_BASE, GPIO_PIN_5); // set PK5 as PWM pin

    PWMGenConfigure(PWM0_BASE, PWM_GEN_3,
        PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
    PWMGenPeriodSet(PWM0_BASE, PWM_GEN_3, ui32SysClock / 2000);  // 2kHz
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_7,
        PWMGenPeriodGet(PWM0_BASE, PWM_GEN_3) / 2);              // 50% duty
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, true);              // enable output
    // Do NOT start the PWM generator yet — beep_on() does that.
}

uint8_t I2C0_WriteByte(uint8_t DevAddr, uint8_t RegAddr, uint8_t WriteData) {
    uint8_t rop;
    while (I2CMasterBusy(I2C0_BASE)) {};
    I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, false);
    I2CMasterDataPut(I2C0_BASE, RegAddr);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    while (I2CMasterBusy(I2C0_BASE)) {};
    rop = (uint8_t)I2CMasterErr(I2C0_BASE);

    I2CMasterDataPut(I2C0_BASE, WriteData);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
    while (I2CMasterBusy(I2C0_BASE)) {};

    rop = (uint8_t)I2CMasterErr(I2C0_BASE);
    return rop;
}

uint8_t I2C0_ReadByte(uint8_t DevAddr, uint8_t RegAddr) {
    uint8_t value;
    while (I2CMasterBusy(I2C0_BASE)) {};
    I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, false);
    I2CMasterDataPut(I2C0_BASE, RegAddr);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
    while (I2CMasterBusBusy(I2C0_BASE));
    (void)I2CMasterErr(I2C0_BASE);
    Delay_us(10);
    I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, true);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);
    while (I2CMasterBusBusy(I2C0_BASE));
    value = I2CMasterDataGet(I2C0_BASE);
    Delay_us(10);
    return value;
}

/************************** SysTick ISR (1kHz) **************************/
void SysTick_Handler(void) {
    // ---- Display scanning (8 digits, dynamic) ----
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
    Delay_us(5);

    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, disp_buf[scan_cnt]);
    Delay_us(5);

    uint8_t bit_pos = SCAN_REVERSE ? (7 - scan_cnt) : scan_cnt;
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 1 << bit_pos);

    scan_cnt++;
    if (scan_cnt >= 8) scan_cnt = 0;

    // ---- Timing counters ----
    if (systick_100ms_counter != 0)
        systick_100ms_counter--;
    else {
        systick_100ms_counter = SYSTICK_FREQUENCY / 10;
        systick_100ms_status = 1;
    }

    if (systick_10ms_counter != 0)
        systick_10ms_counter--;
    else {
        systick_10ms_counter = SYSTICK_FREQUENCY / 100;
        systick_10ms_status = 1;
    }

    // USER1 indicator (PN0 mirrors PJ0)
    if (GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0) == 0)
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
    else
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);
}

/************************** UART0 ISR **************************/
void UART0_Handler(void) {
    int32_t uart0_int_status;
    uart0_int_status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, uart0_int_status);

    // RX activity → blink LED
    uart_rx_timer = 3;

    // Read available characters
    while (UARTCharsAvail(UART0_BASE)) {
        char ch = (char)UARTCharGetNonBlocking(UART0_BASE);

        // Handle \r\n line ending
        if (ch == '\r') continue;  // skip CR
        if (ch == '\n') {
            // Complete line received
            if (receive_overflow) {
                // Line exceeded 64 chars — discard and report
                receive_overflow = 0;
                i = 0;
                memset(receive, 0, sizeof(receive));
                UARTStringPut((uint8_t *)"ERROR LINE TOO LONG\r\n");
                return;
            }
            receive[i] = '\0';
            i = 0;
            uart_cmd_ready = 1;
            return;
        }

        // Accumulate character (allow up to 64 chars, index 0-63)
        if (i < 64) {
            receive[i++] = ch;
        } else {
            // Line too long — set overflow, discard rest
            receive_overflow = 1;
        }
    }

    // Timeout: flush partial buffer (also check overflow)
    if (uart0_int_status == 0x40) {
        if (receive_overflow) {
            receive_overflow = 0;
            i = 0;
            memset(receive, 0, sizeof(receive));
            UARTStringPut((uint8_t *)"ERROR LINE TOO LONG\r\n");
            return;
        }
        if (i > 0) {
            receive[i] = '\0';
            i = 0;
            uart_cmd_ready = 1;
        }
    }
}
