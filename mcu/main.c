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
#define LED_HEARTBEAT           0x01    // bit 0
#define LED_ALARM               0x02    // bit 1
#define LED_EDIT                0x04    // bit 2
#define LED_TX                  0x08    // bit 3
#define LED_RX                  0x10    // bit 4
#define LED_NTP                 0x20    // bit 5 (extension)
#define LED_RESERVED1           0x40    // bit 6
#define LED_RESERVED2           0x80    // bit 7

/************************** Beeper control **************************/
#define BEEPER_PORT             GPIO_PORTN_BASE
#define BEEPER_PIN              GPIO_PIN_1

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
int i = 0;
char receive[64];
volatile uint8_t uart_cmd_ready = 0;
const char *ATCLASS = "AT+CLASS#";
const char *ATCODE = "AT+STUDENTCODE#";
const char *CLASS = "CLASSF17XXXXX";
const char *CODE = "CODE517XXXXXXX";

/************************** Time/date state **************************/
volatile uint8_t  hour = 12, minute = 34, second = 56;
volatile uint16_t year = 2026;
volatile uint8_t  month = 6, day = 8;

/************************** Alarm state **************************/
volatile uint8_t  alarm_hour = 0, alarm_minute = 0, alarm_second = 0;
volatile uint8_t  alarm_enabled = 1;
volatile uint8_t  alarm_ringing = 0;
static uint8_t    alarm_beep_phase = 0;    // 0=off-wait, 1=on, 2=off-wait2, 3=on
static uint8_t    alarm_beep_timer = 0;    // 100ms ticks per phase
static uint8_t    alarm_total_timer = 0;   // total 100ms ticks (max 100 = 10s)
static uint8_t    alarm_snooze = 0;        // suppress flag for night mode

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

/************************** LED state **************************/
volatile uint8_t led_byte = 0x00;          // PCA9557 LED byte
static uint8_t   led_heartbeat_timer = 0;  // 100ms ticks for heartbeat toggle
static uint8_t   uart_tx_timer = 0;        // TX LED blink duration
static uint8_t   uart_rx_timer = 0;        // RX LED blink duration

/************************** Key state **************************/
static uint8_t  key_prev_raw = 0xFF;       // previous raw key state (I2C + GPIO)
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

    // In night mode, only keep heartbeat LED; suppress all others
    if (night_mode) {
        out &= LED_HEARTBEAT;  // mask all except heartbeat
    } else {
        // Alarm LED follows alarm_ringing
        if (alarm_ringing)
            out |= LED_ALARM;
        else
            out &= ~LED_ALARM;

        // Edit LED follows edit_mode
        if (edit_mode != EDIT_NONE)
            out |= LED_EDIT;
        else
            out &= ~LED_EDIT;
    }

    led_byte = out;

    // Write to PCA9557 with SysTick disabled to avoid I2C bus conflict
    // (SysTick ISR also writes to TCA6424 via I2C0 at 1kHz)
    SysTickIntDisable();
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_byte);
    SysTickIntEnable();
}

/************************** Beeper control **************************/
// Passive buzzer requires AC square wave, not DC level.
// SysTick ISR (1kHz) toggles the pin when beep_toggle_enabled is set,
// producing a ~500Hz audible tone.
static volatile uint8_t beep_toggle_enabled = 0;
static uint8_t beep_phase = 0;             // toggled each SysTick for ~500Hz tone

void beep_on(void) {
    if (night_mode) return;  // suppressed in night mode
    beep_toggle_enabled = 1;
}

void beep_off(void) {
    beep_toggle_enabled = 0;
    GPIOPinWrite(BEEPER_PORT, BEEPER_PIN, 0);  // drive pin low when silent
}

/************************** Alarm state machine **************************/
void alarm_check(void) {
    if (!alarm_enabled || alarm_snooze) return;

    // Check if current time matches alarm time
    if (!alarm_ringing &&
        hour == alarm_hour &&
        minute == alarm_minute &&
        second == alarm_second) {
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
            case 'G': return 0x3D; case 'H': return 0x76; case 'I': return 0x06;
            case 'J': return 0x0E; case 'K': return 0x76; case 'L': return 0x38;
            case 'M': return 0x55; case 'N': return 0x54; case 'O': return 0x5C;
            case 'P': return 0x73; case 'Q': return 0x67; case 'R': return 0x50;
            case 'S': return 0x6D; case 'T': return 0x78; case 'U': return 0x3E;
            case 'V': return 0x1C; case 'W': return 0x7E; case 'X': return 0x76;
            case 'Y': return 0x6E; case 'Z': return 0x5B;
            default: return 0x00;
        }
    } else if (c >= 'a' && c <= 'z') {
        return CharToSeg7(c - 'a' + 'A');
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
            uint8_t idx = (pos + j) % scroll_len;
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
        } else if (edit_mode == EDIT_TIME) {
            sprintf(str, "%02d%02d%02d  ", hour, minute, second);
            dp_buf[1] = 1;  // HH.MM
            dp_buf[3] = 1;  // MM.SS
        } else if (edit_mode == EDIT_ALARM) {
            sprintf(str, "%02d%02d%02d  ", alarm_hour, alarm_minute, alarm_second);
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
                break;
        }
    }

    // FORMAT RIGHT: reverse the display string.
    // dp_buf stays the same — dots fall between the same digit pairs,
    // just displayed in reverse order. (e.g., 12.30.45 → 54.03.21)
    if (format_direction == 1) {
        char rev[9];
        for (j = 0; j < 8; j++) {
            rev[j] = str[7 - j];
        }
        memcpy(str, rev, 8);
    }

    // Store ASCII chars for *EVT:DISP
    memcpy(disp_chars, str, 8);

    // Edit mode blinking: blank the current field during blink-on phase
    // (local behavior per §3.5, not mirrored to PC per protocol spec)
    if (edit_mode != EDIT_NONE && edit_blink) {
        if (edit_mode == EDIT_DATE) {
            uint8_t start = (edit_field == EDIT_FIELD_YEAR) ? 0 :
                            (edit_field == EDIT_FIELD_MONTH) ? 2 : 4;
            uint8_t count = (edit_field == EDIT_FIELD_YEAR) ? 2 :
                            (edit_field == EDIT_FIELD_MONTH) ? 2 : 2;
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
    uint8_t raw_state;
    int i;

    // Read I2C keys from TCA6424 Port0 (8 keys, active low)
    // Disable SysTick to avoid I2C bus conflict with display scanning ISR
    SysTickIntDisable();
    i2c_keys = I2C0_ReadByte(TCA6424_I2CADDR, TCA6424_INPUT_PORT0);
    SysTickIntEnable();

    // Read GPIO keys: PJ0=USER1, PJ1=USER2 (active low with pull-up)
    gpio_keys = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    // Build combined 10-bit raw state (0-7=I2C, 8=USER1, 9=USER2)
    // Active low: 0 = pressed
    raw_state = i2c_keys;                           // bits 0-7 from I2C
    if (!(gpio_keys & GPIO_PIN_0)) raw_state |= (1 << 8);  // USER1
    if (!(gpio_keys & GPIO_PIN_1)) raw_state |= (1 << 9);  // USER2

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

    key_prev_raw = raw_state;
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
                        alarm_hour++; if (alarm_hour > 23) alarm_hour = 0;
                    } else if (edit_field == EDIT_FIELD_MINUTE) {
                        alarm_minute++; if (alarm_minute > 59) alarm_minute = 0;
                    } else if (edit_field == EDIT_FIELD_SECOND) {
                        alarm_second++; if (alarm_second > 59) alarm_second = 0;
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
            if (long_press) {
                // Long FUNC = save and exit (same as SAVE)
                edit_exit(1);
            } else {
                // Short FUNC: cycle edit mode or stop alarm
                if (edit_mode == EDIT_NONE) {
                    edit_enter(EDIT_DATE);
                } else if (edit_mode == EDIT_DATE) {
                    edit_enter(EDIT_TIME);
                } else if (edit_mode == EDIT_TIME) {
                    edit_enter(EDIT_ALARM);
                } else {
                    edit_exit(0);  // exit without save
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

        case 7: // EXT
            // Reserved for expansion
            break;

        case 8: // USER1 — request PC time sync
            // Send event, PC should respond with *SET:TIME
            send_response("*EVT:KEY USER1\r\n");
            break;

        case 9: // USER2 — show weather if available (handled by PC)
            send_response("*EVT:KEY USER2\r\n");
            break;
    }
}

/************************** Edit state machine **************************/
// Backup copies for cancel (restore original values)
static uint8_t  backup_hour, backup_minute, backup_second;
static uint16_t backup_year;
static uint8_t  backup_month, backup_day;
static uint8_t  backup_alarm_hour, backup_alarm_minute, backup_alarm_second;

void edit_enter(uint8_t mode) {
    // Backup current values before entering edit mode
    backup_hour = hour; backup_minute = minute; backup_second = second;
    backup_year = year; backup_month = month; backup_day = day;
    backup_alarm_hour = alarm_hour; backup_alarm_minute = alarm_minute;
    backup_alarm_second = alarm_second;

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
            sprintf(value, "%02d.%02d.%02d", alarm_hour, alarm_minute, alarm_second);
            send_event_edit("ALARM", value);
        }
    } else {
        // Cancel: restore original values
        hour = backup_hour; minute = backup_minute; second = backup_second;
        year = backup_year; month = backup_month; day = backup_day;
        alarm_hour = backup_alarm_hour; alarm_minute = backup_alarm_minute;
        alarm_second = backup_alarm_second;
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
        return;
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
        alarm_hour = 0; alarm_minute = 0; alarm_second = 0;
    }

    format_direction = 0;   // FORMAT=LEFT
    disp_on = 1;
    scroll_active = 0;
    disp_mode = DISP_MODE_TIME;
    edit_exit(0);

    update_display();
    send_response("OK\r\n");
}

// *SET:DATE YEAR <val> MONTH <val> DATE <val>
void cmd_set_date(const char *params) {
    char buf[64], *p, *token;
    strncpy(buf, params, 64);
    buf[63] = '\0';

    p = buf;
    int any_set = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        token = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';

        // Get value
        char *val_str = NULL;
        if (saved != '\0') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            val_str = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            saved = *p;
            *p = '\0';
        }

        if (val_str == NULL || val_str[0] == '\0') {
            send_response("ERROR SYNTAX\r\n");
            return;
        }

        int val = atoi(val_str);

        // Uppercase token for matching
        char *tp = token;
        while (*tp) { *tp = toupper((unsigned char)*tp); tp++; }

        if (match_abbrev(token, "YEAR")) {
            if (val < 2025 || val > 2099) {
                send_response("ERROR RANGE\r\n");
                return;
            }
            year = (uint16_t)val;
            any_set = 1;
        } else if (match_abbrev(token, "MONTH")) {
            if (val < 1 || val > 12) {
                send_response("ERROR RANGE\r\n");
                return;
            }
            month = (uint8_t)val;
            any_set = 1;
        } else if (match_abbrev(token, "DATE")) {
            if (val < 1 || val > 31) {  // rough check, refined below
                send_response("ERROR RANGE\r\n");
                return;
            }
            day = (uint8_t)val;
            any_set = 1;
        } else {
            send_response("ERROR SYNTAX\r\n");
            return;
        }

        if (saved == '\0') break;
        *p = saved;
    }

    // Validate day against month
    if (day > get_days_in_month(year, month)) {
        day = get_days_in_month(year, month);
    }

    if (any_set) {
        update_display();
        send_response("OK\r\n");
    } else {
        send_response("ERROR SYNTAX\r\n");
    }
}

// *SET:TIME HOUR <val> MINute <val> SECond <val>
// Also: *SET:TIME OFF (turn off time display? Actually OFF is for setting time to 0 or...)
// From protocol: HOUR/MINUTE/SECOND/OFF — OFF might be a sub-command
void cmd_set_time(const char *params) {
    char buf[64], *p, *token;
    strncpy(buf, params, 64);
    buf[63] = '\0';

    p = buf;
    int any_set = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        token = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';

        // Uppercase token for matching
        char *tp = token;
        while (*tp) { *tp = toupper((unsigned char)*tp); tp++; }

        // Get value
        char *val_str = NULL;
        if (saved != '\0') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            val_str = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            saved = *p;
            *p = '\0';
        }

        if (match_abbrev(token, "OFF")) {
            // Turn off display?
            disp_on = 0;
            update_display();
            send_response("OK\r\n");
            return;
        }

        if (val_str == NULL || val_str[0] == '\0') {
            send_response("ERROR SYNTAX\r\n");
            return;
        }

        int val = atoi(val_str);

        if (match_abbrev(token, "HOUR")) {
            if (val < 0 || val > 23) {
                send_response("ERROR RANGE\r\n");
                return;
            }
            hour = (uint8_t)val;
            any_set = 1;
        } else if (match_abbrev(token, "MINute")) {
            if (val < 0 || val > 59) {
                send_response("ERROR RANGE\r\n");
                return;
            }
            minute = (uint8_t)val;
            second = 0;  // reset seconds on time set
            any_set = 1;
        } else if (match_abbrev(token, "SECond")) {
            if (val < 0 || val > 59) {
                send_response("ERROR RANGE\r\n");
                return;
            }
            second = (uint8_t)val;
            any_set = 1;
        } else {
            send_response("ERROR SYNTAX\r\n");
            return;
        }

        if (saved == '\0') break;
        *p = saved;
    }

    if (any_set) {
        update_display();
        send_response("OK\r\n");
    } else {
        send_response("ERROR SYNTAX\r\n");
    }
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
        // Empty message → clear scroll
        memset(scroll_msg, 0, sizeof(scroll_msg));
        scroll_active = 0;
        scroll_len = 0;
        scroll_pos = 0;
        update_display();
        send_response("OK\r\n");
        return;
    }

    memset(scroll_msg, 0, sizeof(scroll_msg));
    memcpy(scroll_msg, p, len);
    scroll_len = len;
    scroll_pos = 0;
    scroll_active = 1;
    scroll_timer = 0;

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

// *SET:LED <hex2>  — direct LED byte set
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

    led_byte = (uint8_t)(val & 0xFF);
    SysTickIntDisable();
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

// *GET [DATE] [TIME] [FORMAT]
void cmd_get(const char *params) {
    char buf[32], *p;
    strncpy(buf, params, 32);
    buf[31] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) { *p = toupper((unsigned char)*p); p++; }

    p = buf;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0' || match_abbrev(p, "TIME")) {
        char resp[32];
        sprintf(resp, "OK %02d %02d %02d\r\n", hour, minute, second);
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "DATE")) {
        char resp[32];
        sprintf(resp, "OK %04d %02d %02d\r\n", year, month, day);
        send_response(resp);
        return;
    }
    if (match_abbrev(p, "FORMAT")) {
        char resp[32];
        sprintf(resp, "OK %s\r\n", format_direction ? "RIGHT" : "LEFT");
        send_response(resp);
        return;
    }

    send_response("ERROR SYNTAX\r\n");
}

// *PING
void cmd_ping(void) {
    char resp[32];
    sprintf(resp, "*PONG %lu\r\n", uptime_seconds);
    send_response(resp);
}

/************************** UART command processor **************************/
void process_uart_command(void) {
    char cmd[64];
    uart_cmd_ready = 0;  // clear flag first

    strncpy(cmd, receive, 64);
    cmd[63] = '\0';
    memset(receive, 0, sizeof(receive));

    // Trim whitespace
    char *p = cmd;
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

    // ---- Handle space before colon: "*SET : TIME ..." ----
    // Reconstruct token: if token is "*SET" (or "*GET") and params starts
    // with ":", prepend the sub-command to complete the token.
    char reconstructed[32];
    int token_len_orig = (int)strlen(token_start);
    for (int i = 0; token_start[i]; i++)
        token_start[i] = toupper((unsigned char)token_start[i]);

    if (token_start[0] == '*' && strchr(token_start, ':') == NULL &&
        params[0] == ':') {
        // Extract sub-command word from params: skip ':' and any spaces
        // to get e.g. "TIME" from ": TIME HOUR 15"
        char *sub_start = params + 1;  // skip ':'
        while (*sub_start == ' ' || *sub_start == '\t') sub_start++;
        char *sub_end = sub_start;
        while (*sub_end && *sub_end != ' ' && *sub_end != '\t') sub_end++;
        char saved_sub = *sub_end;
        *sub_end = '\0';

        snprintf(reconstructed, sizeof(reconstructed), "%s:%s", token_start, sub_start);
        // Move params past the sub-command
        *sub_end = saved_sub;
        params = sub_end;
        while (*params == ' ' || *params == '\t') params++;
    } else {
        // Keep token as-is
        strncpy(reconstructed, token_start, sizeof(reconstructed));
        reconstructed[sizeof(reconstructed) - 1] = '\0';
    }

    // Save original-case params for commands that need it (MSG)
    char params_original[64];
    strncpy(params_original, params, 64);
    params_original[63] = '\0';

    // Use reconstructed token for dispatch
    char *tok = reconstructed;
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
        cmd_set_msg(params_original);
    } else if (MATCH_CMD(tok, "*SET:BEEP", 8)) {
        cmd_set_beep(params);
    } else if (MATCH_CMD(tok, "*SET:LED", 7)) {
        cmd_set_led(params);
    } else if (MATCH_CMD(tok, "*SET:KEY", 7)) {
        cmd_set_key(params);
    } else if (MATCH_CMD(tok, "*SET:MODE", 8)) {
        cmd_set_mode(params);
    } else if (strncmp(tok, "*GET", 4) == 0 &&
               (int)strlen(tok) <= 12) {
        // *GET, *GET:DATE, *GET:TIME, *GET:FORMAT
        // Sub-command may be in the tok suffix (*GET:TIME) or in params
        const char *sub = params;
        if (tok[4] == ':' && tok[5] != '\0') {
            sub = tok + 5;  // skip "*GET:"
        } else if (tok[4] != '\0') {
            // tok like "*GETDATE" (no colon) — extract after *GET
            sub = tok + 4;
        }
        cmd_get(sub);
    } else if (MATCH_CMD(tok, "*PING", 5)) {
        cmd_ping();
    } else {
        send_response("ERROR SYNTAX\r\n");
    }
    #undef MATCH_CMD
}

/************************** Main function **************************/
int main(void) {
    volatile uint16_t gpio_flash_cnt;

    // System clock init
    ui32SysClock = SysCtlClockFreqSet(
        (SYSCTL_XTAL_16MHZ | SYSCTL_OSC_INT | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480),
        20000000);

    SysTickPeriodSet(ui32SysClock / SYSTICK_FREQUENCY);
    SysTickEnable();
    SysTickIntEnable();

    S800_GPIO_Init();
    S800_I2C0_Init();
    S800_UART_Init();

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

            // Scroll advance
            if (scroll_active && scroll_len > 8) {
                scroll_timer++;
                uint8_t speed = scroll_speed_level ? SCROLL_SPEED_FAST : SCROLL_SPEED_SLOW;
                if (scroll_timer >= speed) {
                    scroll_timer = 0;
                    if (format_direction == 0) {  // LEFT
                        scroll_pos++;
                        if (scroll_pos >= scroll_len + 8) scroll_pos = 0;
                    } else {  // RIGHT
                        if (scroll_pos == 0) scroll_pos = scroll_len + 8 - 1;
                        else scroll_pos--;
                    }
                    update_display();
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
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_1);    // Beeper

    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1,
        GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
}

void S800_I2C0_Init(void) {
    uint8_t result;
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    I2CMasterInitExpClk(I2C0_BASE, ui32SysClock, true);
    I2CMasterEnable(I2C0_BASE);

    // TCA6424: Port0 = input (keys), Port1 = output (segments), Port2 = output (digit select)
    result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xFF);
    result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00);
    result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00);

    // PCA9557: all output (LEDs)
    result = I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00);
    result = I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, 0xFF);  // all off (active low)
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
    uint8_t value, rop;
    while (I2CMasterBusy(I2C0_BASE)) {};
    I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, false);
    I2CMasterDataPut(I2C0_BASE, RegAddr);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
    while (I2CMasterBusBusy(I2C0_BASE));
    rop = (uint8_t)I2CMasterErr(I2C0_BASE);
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

    // Beeper: toggle at 500Hz when active (passive buzzer needs AC)
    if (beep_toggle_enabled) {
        beep_phase = !beep_phase;
        if (beep_phase)
            GPIOPinWrite(BEEPER_PORT, BEEPER_PIN, BEEPER_PIN);
        else
            GPIOPinWrite(BEEPER_PORT, BEEPER_PIN, 0);
    }
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
            receive[i] = '\0';
            i = 0;
            uart_cmd_ready = 1;
            return;
        }

        // Accumulate character
        if (i < 63) {
            receive[i++] = ch;
        }
    }

    // Timeout: flush partial buffer
    if (uart0_int_status == 0x40) {
        if (i > 0) {
            receive[i] = '\0';
            i = 0;
            uart_cmd_ready = 1;
        }
    }
}
