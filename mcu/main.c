/**
 * 智能时钟系统 — MCU 端主程序
 * 基于 TM4C1294NCPDT (S800 板)
 * ARM Compiler 5 (C89) 兼容
 *
 * ARMCC5 V5.06 C89: optimization causes register-caching defects
 * in large multi-path parsers. Disable for this compilation unit.
 */
#pragma O0

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "hw_memmap.h"
#include "hw_ints.h"
#include "hw_types.h"
#include "hw_gpio.h"
#include "debug.h"
#include "gpio.h"
#include "hw_i2c.h"
#include "i2c.h"
#include "timer.h"
#include "pin_map.h"
#include "sysctl.h"
#include "systick.h"
#include "interrupt.h"
#include "uart.h"
#include "pwm.h"
#include <ctype.h>
#include <stdlib.h>

/* ================================================================
 * 硬件地址与常量
 * ================================================================ */
#define SYSTICK_FREQUENCY       1000UL
#define UART_RX_BUF_SIZE        128

#define TCA6424_I2CADDR         0x22
#define PCA9557_I2CADDR         0x18
#define PCA9557_INPUT           0x00
#define PCA9557_OUTPUT          0x01
#define PCA9557_POLINVERT       0x02
#define PCA9557_CONFIG          0x03
#define TCA6424_INPUT_PORT0     0x00
#define TCA6424_INPUT_PORT1     0x01
#define TCA6424_INPUT_PORT2     0x02
#define TCA6424_OUTPUT_PORT0    0x04
#define TCA6424_OUTPUT_PORT1    0x05
#define TCA6424_OUTPUT_PORT2    0x06
#define TCA6424_CONFIG_PORT0    0x0C
#define TCA6424_CONFIG_PORT1    0x0D
#define TCA6424_CONFIG_PORT2    0x0E

#define DISP_LEN                8
#define SCROLL_BUF_SIZE         40
#define KEY_QUEUE_SIZE          16
#define KEY_DEBOUNCE_MS         20
#define KEY_LONG_MS             800
#define KEY_REPEAT_MS           150
#define LINE_MAX                65

#define ALARM_SLOTS             7
#define DOW_MON 1
#define DOW_TUE 2
#define DOW_WED 3
#define DOW_THU 4
#define DOW_FRI 5
#define DOW_SAT 6
#define DOW_SUN 7

#define DISP_MODE_TIME          0
#define DISP_MODE_DATE_SHORT    1
#define DISP_MODE_DATE_LONG     2

#define LED_HEARTBEAT           0x01  /* LED0: 心跳 1Hz */
#define LED_ALARM               0x02  /* LED1: 闹钟 */
#define LED_EDIT                0x04  /* LED2: 编辑模式 */
#define LED_UART                0x08  /* LED3: UART TX+RX (合并) */
#define LED_SUN                 0x10  /* LED4: 晴天 SUN */
#define LED_RAI_SNO             0x20  /* LED5: 雨雪 RAI/SNO 1Hz呼吸 */
#define LED_HI_TEMP             0x40  /* LED6: 高温 ≥30°C */
#define LED_NTP                 0x80  /* LED7: NTP同步状态 */

/* Convert g_date.wday (0=Sun,1=Mon..6=Sat) to alarm slot (0=Mon..6=Sun) */
#define ALARM_IDX  ((uint8_t)(g_date.wday == 0 ? 6 : g_date.wday - 1))

/* ================================================================
 * 段码表
 * ================================================================ */
static const uint8_t seg7_table[] = {
    0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F, /* 0-9 */
    0x77,0x7C,0x39,0x5E,0x79,0x71,0x3D,0x76,0x30,0x1E, /* A-J */
    0x7A,0x3C,0x55,0x37,0x3F,0x73,0x67,0x70,0x6D,0x78, /* K-T */
    0x3E,0x1C,0x6A,0x36,0x6E,0x49                     /* U-Z */
};

static uint8_t seg7_encode(char ch)
{
    if (ch >= '0' && ch <= '9') return seg7_table[ch - '0'];
    if (ch >= 'A' && ch <= 'Z') return seg7_table[ch - 'A' + 10];
    if (ch >= 'a' && ch <= 'z') {
        switch (ch) {
            case 'c': return 0x58; case 'e': return 0x7B;
            case 'h': return 0x74; case 'i': return 0x10;
            case 'j': return 0x0E; case 'l': return 0x38;
            case 'n': return 0x54; case 'o': return 0x5C;
            case 'r': return 0x50; case 'u': return 0x1C;
            default:  return seg7_table[ch - 'a' + 10];
        }
    }
    if (ch == '-')  return 0x40;
    if (ch == '_')  return 0x08;
    if (ch == '.')  return 0x80;  /* dot as independent digit (dp segment only) */
    if (ch == 0xB0 || ch == '\'') return 0x63;  /* ° degree symbol (segments a,b,f,g) */
    if (ch == ' ')  return 0x00;
    return 0x00;
}

/* ================================================================
 * 数据类型
 * ================================================================ */
typedef struct {
    uint16_t y;
    uint8_t  m, d, wday;
} date_t;

typedef struct {
    uint8_t h, mi, s;
} time_t_;

typedef struct {
    time_t_ t;
    uint8_t enabled;
    uint8_t ringing;
} alarm_t;

typedef enum {
    FMT_LEFT,
    FMT_RIGHT
} format_t;

typedef enum {
    KEY_NONE = 0,
    KEY_FUNC,
    KEY_SHIFT,
    KEY_ADD,
    KEY_SAVE,
    KEY_DISP,
    KEY_SPEED,
    KEY_FORMAT,
    KEY_EXT,
    KEY_USER1,
    KEY_USER2
} key_code_t;

typedef enum {
    KEV_NONE = 0,
    KEV_DOWN,
    KEV_UP,
    KEV_LONG,
    KEV_REPEAT
} key_event_t;

typedef enum {
    STATE_BOOT,
    STATE_CLOCK,
    STATE_EDIT_DATE_LONG,
    STATE_EDIT_DATE,
    STATE_EDIT_TIME,
    STATE_EDIT_ALARM,
    STATE_SCROLL,
    STATE_MSG_STATIC,
    STATE_WEATHER,
    STATE_GAME
} sys_state_t;

typedef enum {
    KS_IDLE,
    KS_DEBOUNCE_DOWN,
    KS_PRESSED,
    KS_LONG_PRESSED,
    KS_DEBOUNCE_UP
} ks_state_t;

typedef enum {
    EF_NONE,
    EF_YEAR,
    EF_MONTH,
    EF_DAT,
    EF_HOUR,
    EF_MINUTE,
    EF_SECOND
} edit_field_t;

/* ================================================================
 * 全局变量
 * ================================================================ */
volatile uint32_t g_tick_ms;
volatile uint8_t  flag_5ms;
volatile uint8_t  flag_10ms;
volatile uint8_t  flag_100ms;
volatile uint8_t  flag_1s;

uint8_t  disp_buf[DISP_LEN];
uint8_t  disp_dp[DISP_LEN];
char     disp_char[DISP_LEN];
uint8_t  disp_blink_mask;
uint8_t  disp_on;
format_t g_format;

char     scroll_buf[SCROLL_BUF_SIZE];
uint8_t  scroll_len;
uint8_t  scroll_dp_bitmap;
int8_t   scroll_off;
uint8_t  scroll_speed;
uint8_t  scroll_dir;

volatile time_t_ g_time;
volatile date_t  g_date;
volatile alarm_t g_alarm;
volatile uint8_t g_alarm_beep_active;
static uint8_t  g_alarm_weather_beeps;    /* 雨雪额外响铃次数 */
static uint8_t  g_alarm_weather_led;      /* 高温 LED 慢闪 */

/* Multi-alarm 7-slot storage */
volatile time_t_ g_alarm_slot[ALARM_SLOTS];
volatile uint8_t g_alarm_slot_enabled_mask;

/* Extra state */
volatile uint8_t g_night_mode;
volatile uint8_t g_disp_mode;
volatile uint8_t g_scroll_speed_level;
volatile uint8_t g_weather_age;
volatile uint8_t g_suppress_key_evt;

static key_event_t key_queue_evt[KEY_QUEUE_SIZE];
static key_code_t  key_queue_code[KEY_QUEUE_SIZE];
static volatile uint8_t key_queue_wr;
static uint8_t          key_queue_rd;

volatile char     uart_rx_buf[UART_RX_BUF_SIZE];
volatile uint16_t uart_rx_head;
volatile uint16_t uart_rx_tail;
volatile uint8_t  rx_line_ready;
char              cmd_line[LINE_MAX];
volatile uint8_t  led_uart_tx_active;
volatile uint8_t  led_uart_rx_active;

volatile uint8_t g_led_override;
volatile uint8_t g_led_value;

sys_state_t g_state;
uint8_t     g_edit_pos;
uint32_t    g_last_activity_ms;
uint8_t     g_mode_day;
uint32_t    g_uptime_s;

int8_t  g_weather_temp;
char    g_weather_cond[4];
uint8_t g_weather_valid;

uint8_t  g_ntp_synced;
uint32_t g_ntp_last_sync_ms;

char    g_msg_text[33];
uint8_t  g_msg_len;
uint8_t  g_msg_active;
uint32_t g_msg_end_ms;

volatile uint8_t  g_game_active;
volatile uint16_t g_game_score;

/* 按键状态机静态变量 */
static ks_state_t    ks_state      = KS_IDLE;
static uint16_t      ks_stable;
static uint32_t      ks_change_time;
static key_code_t    ks_held_key;

/* 编辑 FSM 静态变量 */
static edit_field_t edit_field;
static uint8_t      edit_modified;

/* 编辑备份 */
static time_t_  edit_backup_time;
static date_t   edit_backup_date;
static alarm_t  edit_backup_alarm;

/* LED 静态变量 */
static uint8_t  led_pca9557_cache = 0xFF;
static uint32_t led_override_start_ms;  /* 接管开始时刻, 10s超时 */

/* 蜂鸣器静态变量 */
static uint32_t beep_start_ms = 0;
static uint8_t  beep_phase_on = 0;

/* 远程蜂鸣（非阻塞） */
static volatile uint8_t  remote_beep_active = 0;
static volatile uint32_t remote_beep_end_ms = 0;
static volatile uint8_t  beep_force_off;       /* 持久静音: 每100ms持续Beep_Off */

/* 显示扫描静态变量 */
static uint8_t  disp_cur_digit = 0;
static uint8_t  disp_bit_mask  = 0x01;

/* 时钟显示格式化辅助 */
static void Clock_FormatDisplay(void);
static void Report_LED(void);

/* ================================================================ */
uint32_t ui32SysClock;

/* ================================================================
 * 工具函数
 * ================================================================ */
uint8_t Tick_TimedOut(uint32_t start, uint32_t span_ms)
{
    return (g_tick_ms - start) >= span_ms;
}

static const char *keycode_to_name(key_code_t code)
{
    switch (code) {
        case KEY_FUNC:   return "FUNC";
        case KEY_SHIFT:  return "SHIFT";
        case KEY_ADD:    return "ADD";
        case KEY_SAVE:   return "SAVE";
        case KEY_DISP:   return "DISP";
        case KEY_SPEED:  return "SPEED";
        case KEY_FORMAT: return "FORMAT";
        case KEY_EXT:    return "EXT";
        case KEY_USER1:  return "USER1";
        case KEY_USER2:  return "USER2";
        default:         return "NONE";
    }
}

static uint8_t is_leap_year(uint16_t y)
{
    return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
}

static const uint8_t days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static const char *DOW_NAMES[] = {"","MON","TUE","WED","THU","FRI","SAT","SUN"};

static uint8_t cmd_match(const char *input, const char *prefix)
{
    while (*prefix) {
        if (*input != *prefix) return 0;
        input++; prefix++;
    }
    return 1;
}

static void skip_kw_rest(char **pp, const char *rest)
{
    while (*rest && (**pp == *rest || **pp == *rest + 32)) {
        (*pp)++; rest++;
    }
}

static void str_rev(char *s)
{
    int len = (int)strlen(s);
    int i;
    char tmp;
    for (i = 0; i < len / 2; i++) {
        tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
}

/* ================================================================
 * I2C 底层操作
 * ================================================================ */
static void I2C_WriteByte(uint8_t dev_addr, uint8_t reg, uint8_t data)
{
    while (I2CMasterBusy(I2C0_BASE)) {}

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, false);
    I2CMasterDataPut(I2C0_BASE, reg);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);

    while (I2CMasterBusy(I2C0_BASE)) {}

    I2CMasterDataPut(I2C0_BASE, data);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);

    while (I2CMasterBusy(I2C0_BASE)) {}
}

/**
 * I2C 读一个字节。修复: 统一使用 I2CMasterBusy()
 */
static uint8_t I2C_ReadByte(uint8_t dev_addr, uint8_t reg)
{
    while (I2CMasterBusy(I2C0_BASE)) {}

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, false);
    I2CMasterDataPut(I2C0_BASE, reg);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);

    while (I2CMasterBusy(I2C0_BASE)) {}

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, true);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);

    while (I2CMasterBusy(I2C0_BASE)) {}

    return (uint8_t)I2CMasterDataGet(I2C0_BASE);
}

/* ================================================================
 * 按键读取
 * ================================================================ */
static uint8_t Keys_ReadRaw(void)
{
    uint8_t val;
    SysTickIntDisable();
    while (I2CMasterBusy(I2C0_BASE)) {}  /* wait for in-flight I2C */
    val = I2C_ReadByte(TCA6424_I2CADDR, TCA6424_INPUT_PORT0);
    SysTickIntEnable();
    return ~val;
}

static uint8_t Keys_ReadUser(void)
{
    uint32_t pj;
    uint8_t result = 0;
    pj = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    if (!(pj & GPIO_PIN_0)) result |= 0x01;
    if (!(pj & GPIO_PIN_1)) result |= 0x02;
    return result;
}

static key_code_t raw_to_keycode(uint16_t raw)
{
    if (raw & 0x0001) return KEY_ADD;     /* K0  I2C bit 0 */
    if (raw & 0x0002) return KEY_FUNC;    /* K1  I2C bit 1 */
    if (raw & 0x0004) return KEY_SHIFT;   /* K2  I2C bit 2 */
    if (raw & 0x0008) return KEY_SPEED;   /* K3  I2C bit 3 */
    if (raw & 0x0010) return KEY_SAVE;    /* K4  I2C bit 4 */
    if (raw & 0x0020) return KEY_FORMAT;  /* K5  I2C bit 5 */
    if (raw & 0x0040) return KEY_DISP;    /* K6  I2C bit 6 */
    if (raw & 0x0080) return KEY_EXT;     /* K7  I2C bit 7 */
    if (raw & 0x0100) return KEY_USER1;   /* GPIO bit 8 */
    if (raw & 0x0200) return KEY_USER2;   /* GPIO bit 9 */
    return KEY_NONE;
}

/* ================================================================
 * 按键事件队列
 * ================================================================ */
static void KeyQueue_Push(key_code_t code, key_event_t evt)
{
    uint8_t next = (key_queue_wr + 1) % KEY_QUEUE_SIZE;
    if (next == key_queue_rd) return;
    key_queue_code[key_queue_wr] = code;
    key_queue_evt[key_queue_wr]  = evt;
    key_queue_wr = next;
}

key_event_t Key_GetEvent(key_code_t *out)
{
    key_event_t evt;
    if (key_queue_rd == key_queue_wr) return KEV_NONE;
    *out = key_queue_code[key_queue_rd];
    evt = key_queue_evt[key_queue_rd];
    key_queue_rd = (key_queue_rd + 1) % KEY_QUEUE_SIZE;
    return evt;
}

/* ================================================================
 * 按键扫描（每 10ms 调用）
 *
 * 统一状态机: I2C 按键 (8位) + GPIO USER1/USER2 (bit 8/9)
 * all_raw = uint16_t { USER2, USER1, K7..K0 }
 * KS_DEBOUNCE_UP 防释放毛刺; 无幻影 KEV_UP
 * ================================================================ */
void Key_Scan(void)
{
    uint8_t  tca_raw;
    uint8_t  user_raw;
    uint16_t all_raw;

    tca_raw  = Keys_ReadRaw();
    user_raw = Keys_ReadUser();
    all_raw = (uint16_t)tca_raw | ((uint16_t)user_raw << 8);

    switch (ks_state) {

    case KS_IDLE:
        if (all_raw != 0) {
            ks_state       = KS_DEBOUNCE_DOWN;
            ks_change_time = g_tick_ms;
        }
        break;

    case KS_DEBOUNCE_DOWN:
        if (all_raw == 0) {
            ks_state = KS_IDLE;  /* glitch, back to idle */
        } else if ((g_tick_ms - ks_change_time) >= KEY_DEBOUNCE_MS) {
            /* stable press confirmed */
            ks_stable     = all_raw;
            ks_held_key   = raw_to_keycode(all_raw);
            ks_state      = KS_PRESSED;
            ks_change_time = g_tick_ms;
            if (ks_held_key != KEY_NONE)
                KeyQueue_Push(ks_held_key, KEV_DOWN);
        }
        break;

    case KS_PRESSED:
        if (all_raw != ks_stable) {
            if (all_raw == 0) {
                /* all released → debounce the release */
                ks_state       = KS_DEBOUNCE_UP;
                ks_change_time = g_tick_ms;
            } else {
                /* mask changed but not zero (e.g. second key pressed)
                 * → re-debounce; do NOT push phantom KEV_UP */
                ks_state       = KS_DEBOUNCE_DOWN;
                ks_change_time = g_tick_ms;
            }
        } else if ((g_tick_ms - ks_change_time) >= KEY_LONG_MS) {
            KeyQueue_Push(ks_held_key, KEV_LONG);
            ks_state       = KS_LONG_PRESSED;
            ks_change_time = g_tick_ms;
        }
        break;

    case KS_LONG_PRESSED:
        if (all_raw != ks_stable) {
            if (all_raw == 0) {
                ks_state       = KS_DEBOUNCE_UP;
                ks_change_time = g_tick_ms;
            }
            /* mask change but still held: ignore during long-press */
        } else if ((g_tick_ms - ks_change_time) >= KEY_REPEAT_MS) {
            KeyQueue_Push(ks_held_key, KEV_REPEAT);
            ks_change_time = g_tick_ms;
        }
        break;

    case KS_DEBOUNCE_UP:
        if (all_raw == 0 && (g_tick_ms - ks_change_time) >= KEY_DEBOUNCE_MS) {
            /* release confirmed: push KEV_UP */
            if (ks_held_key != KEY_NONE)
                KeyQueue_Push(ks_held_key, KEV_UP);
            ks_state = KS_IDLE;
        } else if (all_raw != 0) {
            /* pressed again during release debounce → back to pressed */
            ks_state = KS_PRESSED;
        }
        break;
    }
}

/* ================================================================
 * Beeper (PK5, PWM0 Gen3 Out7, 2kHz/50%)
 * ================================================================ */
static void Beep_On(void)
{
    beep_force_off = 0;  /* 解除持久静音 */
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, true);
    PWMGenEnable(PWM0_BASE, PWM_GEN_3);
}

static void Beep_Off(void)
{
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, false);
    PWMGenDisable(PWM0_BASE, PWM_GEN_3);
    beep_force_off = 1;  /* 启动持久静音 */
}

/* 闹钟相位切换用 — PWM始终运行, 只开关输出, 零延迟 */
static void Beep_OutOn(void)
{
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, true);
}

static void Beep_OutOff(void)
{
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, false);
}

/* ================================================================
 * SysTick
 * ================================================================ */
void SysTick_Handler(void)
{
    static uint32_t cnt_5ms   = 0;
    static uint32_t cnt_10ms  = 0;
    static uint32_t cnt_100ms = 0;
    static uint32_t cnt_1s    = 0;

    g_tick_ms++;

    if (++cnt_5ms >= 5)   { cnt_5ms = 0;   flag_5ms   = 1; }
    if (++cnt_10ms >= 10)  { cnt_10ms = 0;  flag_10ms  = 1; }
    if (++cnt_100ms >= 100) { cnt_100ms = 0; flag_100ms = 1; }
    if (++cnt_1s >= 1000)   { cnt_1s = 0;    flag_1s    = 1; }

    /* USER1 indicator: PN0 mirrors PJ0 */
    if (GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0) == 0)
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
    else
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);
}

/* ================================================================
 * UART
 * ================================================================ */
void UART_Init(uint32_t baud)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA)) {}

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, ui32SysClock, baud,
                        (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                         UART_CONFIG_PAR_NONE));
}

static void UART_PutStr(const char *msg)
{
    while (*msg) {
        UARTCharPut(UART0_BASE, *msg++);
    }
}

void UART_PutStrNB(const char *msg)
{
    while (*msg) {
        UARTCharPut(UART0_BASE, *msg++);
    }
    led_uart_tx_active = 1;
}

void UART_RxIsrHandler(uint8_t byte)
{
    uint16_t next_head = (uart_rx_head + 1) % UART_RX_BUF_SIZE;
    if (next_head != uart_rx_tail) {
        uart_rx_buf[uart_rx_head] = (char)byte;
        uart_rx_head = next_head;
        if (byte == '\r' || byte == '\n') {
            rx_line_ready = 1;
        }
    }
    led_uart_rx_active = 1;
}

void UART0_Handler(void)
{
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status);
    while (UARTCharsAvail(UART0_BASE)) {
        char byte = (char)UARTCharGetNonBlocking(UART0_BASE);
        UART_RxIsrHandler((uint8_t)byte);
    }
}

/* ================================================================
 * 行整理
 * ================================================================ */
void ExtractLine(void)
{
    uint16_t idx;
    char ch;
    uint8_t is_msg;
    uint16_t peek_tail;
    char peek[16];
    uint8_t pi;
    char upper[16];
    uint8_t j;

    /* skip leading \r\n leftover from previous line */
    while (uart_rx_tail != uart_rx_head) {
        char lc = uart_rx_buf[uart_rx_tail];
        if (lc != '\r' && lc != '\n') break;
        uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;
    }

    idx = 0;
    is_msg = 0;

    /* 窥探是否是 *SET:MSG */
    peek_tail = uart_rx_tail;
    pi = 0;
    while (pi < 15 && peek_tail != uart_rx_head) {
        peek[pi] = uart_rx_buf[peek_tail];
        peek_tail = (peek_tail + 1) % UART_RX_BUF_SIZE;
        pi++;
    }
    peek[pi] = '\0';
    for (j = 0; j < pi; j++) {
        upper[j] = (peek[j] >= 'a' && peek[j] <= 'z')
                   ? peek[j] - 32 : peek[j];
    }
    upper[pi] = '\0';
    if (strncmp(upper, "*SET:MSG", 8) == 0) {
        is_msg = 1;
    }

    /* 逐字符读取 */
    while (uart_rx_tail != uart_rx_head && idx < (LINE_MAX - 1)) {
        ch = uart_rx_buf[uart_rx_tail];
        uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;

        if (ch == '\r' || ch == '\n') {
            if (idx == 0) continue;
            break;
        }

        if (!is_msg && ch == ' ') {
            continue;
        }

        if (!is_msg && ch >= 'a' && ch <= 'z') {
            ch -= 32;
        }

        cmd_line[idx++] = ch;
    }

    cmd_line[idx] = '\0';

    if (idx >= LINE_MAX - 1 && uart_rx_tail != uart_rx_head) {
        while (uart_rx_tail != uart_rx_head) {
            ch = uart_rx_buf[uart_rx_tail];
            uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;
            if (ch == '\r' || ch == '\n') break;
        }
        UART_PutStrNB("ERROR LINE TOO LONG\r\n");
        cmd_line[0] = '\0';
    }
}

/* ================================================================
 * 显示驱动
 * ================================================================ */
void Display_Init(void)
{
    uint8_t i;
    for (i = 0; i < DISP_LEN; i++) {
        disp_buf[i]  = 0x00;
        disp_dp[i]   = 0;
        disp_char[i] = ' ';
    }
    disp_blink_mask = 0;
    disp_on = 1;
    g_format = FMT_LEFT;
}

void Display_SetStr(const char *s, uint8_t dp_bitmap)
{
    uint8_t i;
    for (i = 0; i < DISP_LEN; i++) {
        char ch = (s && s[i]) ? s[i] : ' ';
        disp_char[i] = ch;
        disp_buf[i]  = seg7_encode(ch);
        disp_dp[i]   = (dp_bitmap >> (7 - i)) & 0x01;
    }
}

void Display_Refresh(void)
{
    uint8_t seg;
    uint8_t dp;
    uint8_t blink;
    uint8_t blink_phase;
    uint8_t on;
    uint8_t seg_data;

    seg = disp_buf[disp_cur_digit];
    dp  = disp_dp[disp_cur_digit];
    blink = (disp_blink_mask >> (7 - disp_cur_digit)) & 0x01;
    blink_phase = (g_tick_ms / 250) & 0x01;
    on = disp_on;
    if (blink && blink_phase) {
        on = 0;
    }
    if (!g_mode_day && disp_cur_digit >= 4) {
        on = 0;
    }

    seg_data = seg;
    if (dp) seg_data |= 0x80;

    /* 消鬼影 + 无闪烁: 清段码 → 切位 → 写新段码 */
    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, 0x00);
    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, on ? disp_bit_mask : 0x00);
    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, on ? seg_data : 0x00);

    disp_cur_digit = (disp_cur_digit + 1) % DISP_LEN;
    disp_bit_mask <<= 1;
    if (disp_bit_mask == 0) disp_bit_mask = 0x01;
}

/* ================================================================
 * 时钟走时
 * ================================================================ */
static void Time_Tick(void)
{
    uint8_t max_d;

    g_time.s++;
    if (g_time.s >= 60) {
        g_time.s = 0;
        g_time.mi++;
        if (g_time.mi >= 60) {
            g_time.mi = 0;
            g_time.h++;
            if (g_time.h >= 24) {
                g_time.h = 0;
                max_d = days_in_month[g_date.m - 1];
                if (g_date.m == 2 && is_leap_year(g_date.y))
                    max_d = 29;
                g_date.d++;
                if (g_date.d > max_d) {
                    g_date.d = 1;
                    g_date.m++;
                    if (g_date.m > 12) {
                        g_date.m = 1;
                        g_date.y++;
                    }
                }
                g_date.wday = (g_date.wday + 1) % 7;
            }
        }
    }
}

/* ================================================================
 * 时钟显示格式化
 * ================================================================ */
static void Clock_FormatDisplay(void)
{
    char str[DISP_LEN + 1];
    char rev[DISP_LEN + 1];
    uint8_t dp;
    int8_t i;

    if (g_state == STATE_GAME) {
        sprintf(str, "Sc %03d  ", g_game_score);
        Display_SetStr(str, 0x00);
        return;
    }

    if (g_night_mode) {
        str[0] = '0' + (g_time.h / 10);
        str[1] = '0' + (g_time.h % 10);
        str[2] = '0' + (g_time.mi / 10);
        str[3] = '0' + (g_time.mi % 10);
        str[4] = ' '; str[5] = ' '; str[6] = ' '; str[7] = ' ';
        str[DISP_LEN] = '\0';
        dp = (1 << 6);  /* HH.MM */
        Display_SetStr(str, dp);
        return;
    }

    if (g_disp_mode == DISP_MODE_TIME || g_state == STATE_EDIT_TIME) {
        sprintf(str, "%02d%02d%02d  ", g_time.h, g_time.mi, g_time.s);
        if (g_format == FMT_RIGHT) {
            for (i = 0; i < DISP_LEN; i++) rev[i] = str[DISP_LEN - 1 - i];
            rev[DISP_LEN] = '\0';
            dp = (1 << 4) | (1 << 2);
            Display_SetStr(rev, dp);
        } else {
            dp = (1 << 6) | (1 << 4);
            Display_SetStr(str, dp);
        }
    } else if (g_disp_mode == DISP_MODE_DATE_SHORT) {
        sprintf(str, "%02d%02d%02d  ", g_date.y % 100, g_date.m, g_date.d);
        if (g_format == FMT_RIGHT) {
            for (i = 0; i < DISP_LEN; i++) rev[i] = str[DISP_LEN - 1 - i];
            rev[DISP_LEN] = '\0';
            dp = (1 << 4) | (1 << 2);
            Display_SetStr(rev, dp);
        } else {
            dp = (1 << 6) | (1 << 4);
            Display_SetStr(str, dp);
        }
    } else {
        sprintf(str, "%04d%02d%02d", g_date.y, g_date.m, g_date.d);
        if (g_format == FMT_RIGHT) {
            for (i = 0; i < DISP_LEN; i++) rev[i] = str[DISP_LEN - 1 - i];
            rev[DISP_LEN] = '\0';
            dp = (1 << 4);  /* 1 dot: YYYY.MMDD → reversed .DDMMYYYY */
            Display_SetStr(rev, dp);
        } else {
            dp = (1 << 4);  /* 1 dot: YYYY.MMDD (FAQ Q12: 仅第4位有dp) */
            Display_SetStr(str, dp);
        }
    }
    str[DISP_LEN] = '\0';
}

/* ================================================================
 * 流水显示
 * ================================================================ */
void Scroll_Set(const char *text, uint8_t dp_bitmap)
{
    if (text == NULL) return;
    scroll_len = (uint8_t)strlen(text);
    if (scroll_len > SCROLL_BUF_SIZE - 1) scroll_len = SCROLL_BUF_SIZE - 1;
    memcpy(scroll_buf, text, scroll_len);
    scroll_buf[scroll_len] = '\0';
    scroll_dp_bitmap = dp_bitmap;
    scroll_off = 0;
    scroll_speed = 0;
    scroll_dir = 0;
}

void Scroll_Tick(void)
{
    char str[DISP_LEN + 1];
    uint8_t dp;
    int8_t i;
    int16_t idx;
    int16_t total_steps;

    if (scroll_len == 0) return;

    if (scroll_len <= DISP_LEN) {
        /* ≤8 chars: static display from scroll_buf — 长短信共用缓冲区 */
        Display_SetStr(scroll_buf, scroll_dp_bitmap);
        return;
    }

    /* >8 chars: full one-pass scroll */
    total_steps = (int16_t)scroll_len + 1;

    for (i = 0; i < DISP_LEN; i++) {
        if (scroll_dir == 0)
            idx = (int16_t)scroll_off + (int16_t)i;
        else
            idx = (int16_t)(scroll_len - 1) - (int16_t)scroll_off - (int16_t)i;
        if (idx >= 0 && idx < (int16_t)scroll_len)
            str[i] = scroll_buf[idx];
        else
            str[i] = ' ';
    }
    str[DISP_LEN] = '\0';
    dp = scroll_dp_bitmap;
    Display_SetStr(str, dp);

    scroll_off++;
    if (scroll_off >= total_steps) {
        scroll_off = 0;
        memset(scroll_buf, 0, sizeof(scroll_buf));
        scroll_len = 0;
        g_msg_active = 0;
        g_state = STATE_CLOCK;
        Clock_FormatDisplay();
    }
}

/* ================================================================
 * 闹钟
 * ================================================================ */
void Alarm_Check(void)
{
    uint8_t idx;
    if (g_alarm_beep_active) return;  /* already ringing */
    if (g_night_mode) return;

    idx = ALARM_IDX;  /* 0=Mon...6=Sun */
    if (idx >= ALARM_SLOTS) return;
    if (!(g_alarm_slot_enabled_mask & (1 << idx))) return;

    if (g_time.h == g_alarm_slot[idx].h &&
        g_time.mi == g_alarm_slot[idx].mi &&
        g_time.s == g_alarm_slot[idx].s) {
        g_alarm_beep_active = 1;
        beep_start_ms = g_tick_ms;
        beep_phase_on = 1;
        Beep_On();
        UART_PutStrNB("*EVT:ALARM\r\n");
        /* 天气-闹钟联动 */
        if (g_weather_valid) {
            if (strcmp(g_weather_cond, "RAI") == 0 ||
                strcmp(g_weather_cond, "SNO") == 0) {
                g_alarm_weather_beeps = 3;  /* 雨雪: 多响3声 */
            }
            if (g_weather_temp >= 30) {
                g_alarm_weather_led = 1;    /* 高温: 8LED 慢闪 */
            }
        }
    }
}

static void Alarm_Stop(void)
{
    if (g_alarm_beep_active) {
        g_alarm_beep_active = 0;
        g_alarm_weather_beeps = 0;
        g_alarm_weather_led = 0;
        beep_phase_on = 0;
        Beep_Off();
        UART_PutStrNB("*EVT:ALARM_OFF\r\n");
    }
}

/* ================================================================
 * LED 控制
 * ================================================================ */
static void PCA9557_Write(uint8_t data)
{
    I2C_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, data);
}

static void LED_Update(void)
{
    uint8_t val;
    uint8_t prev_cache;

    prev_cache = led_pca9557_cache;

    if (g_led_override) {
        PCA9557_Write(~g_led_value);
        led_pca9557_cache = g_led_value;
        if (led_pca9557_cache != prev_cache) {
            Report_LED();
        }
        return;
    }

    val = 0xFF;

    /* 高温闹钟联动: 8LED 1.5s 周期慢闪 (750ms亮/750ms灭) */
    if (g_alarm_weather_led && g_alarm_beep_active) {
        if ((g_tick_ms / 750) & 0x01) {
            val = 0x00;  /* 全亮 */
            led_pca9557_cache = val;
            PCA9557_Write(val);
            return;
        }
    }

    /* Night mode: only heartbeat LED */
    if (g_night_mode) {
        if ((g_tick_ms / 500) & 0x01) val &= ~LED_HEARTBEAT;
        led_pca9557_cache = val;
        PCA9557_Write(val);
        return;
    }

    /* LED0: 心跳 1Hz */
    if ((g_tick_ms / 500) & 0x01) {
        val &= ~LED_HEARTBEAT;
    }

    /* LED1: 闹钟 */
    if (g_alarm_beep_active) {
        if ((g_tick_ms / 100) & 0x01) {
            val &= ~LED_ALARM;
        }
    } else {
        uint8_t idx = ALARM_IDX;
        if (idx < ALARM_SLOTS && (g_alarm_slot_enabled_mask & (1 << idx)))
            val &= ~LED_ALARM;
    }

    /* LED2: 编辑模式 */
    if (g_state == STATE_EDIT_DATE_LONG ||
        g_state == STATE_EDIT_DATE ||
        g_state == STATE_EDIT_TIME ||
        g_state == STATE_EDIT_ALARM) {
        val &= ~LED_EDIT;
    }

    /* LED3: UART TX+RX 合并 (亮100ms) */
    if (led_uart_tx_active || led_uart_rx_active) {
        val &= ~LED_UART;
    }

    /* LED4: 晴天 SUN */
    if (g_weather_valid) {
        if (strcmp(g_weather_cond, "SUN") == 0)
            val &= ~LED_SUN;
    }

    /* LED5: 雨雪 RAI/SNO — 1Hz 呼吸 (PWM 模拟: 250ms亮/750ms灭) */
    if (g_weather_valid) {
        if (strcmp(g_weather_cond, "RAI") == 0 ||
            strcmp(g_weather_cond, "SNO") == 0) {
            if ((g_tick_ms % 1000) < 250)
                val &= ~LED_RAI_SNO;
        }
    }

    /* LED6: 高温 ≥30°C */
    if (g_weather_valid && g_weather_temp >= 30) {
        val &= ~LED_HI_TEMP;
    }

    /* LED7: NTP 同步状态 */
    if (g_ntp_synced == 1) {
        val &= ~LED_NTP;  /* SYNCED = 常亮 */
    } else if (g_ntp_synced == 2) {
        if ((g_tick_ms / 500) & 0x01) val &= ~LED_NTP;  /* DRIFT = 1Hz闪 */
    }

    led_pca9557_cache = val;
    PCA9557_Write(val);

    /* LED 状态变化时立即上报 */
    if (led_pca9557_cache != prev_cache) {
        Report_LED();
    }
}

/* ================================================================
 * GPIO / I2C 初始化
 * ================================================================ */
static void GPIO_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) {}
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_0);

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOJ)) {}
    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1,
                     GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPION)) {}
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1, 0);

    /* Beeper: PK5 → M0PWM7, PWM0 Gen3 Out7, 2kHz/50% */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOK);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOK)) {}
    GPIOPinTypeGPIOOutput(GPIO_PORTK_BASE, GPIO_PIN_5);
    GPIOPinConfigure(GPIO_PK5_M0PWM7);
    GPIOPinTypePWM(GPIO_PORTK_BASE, GPIO_PIN_5);

    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0)) {}
    PWMClockSet(PWM0_BASE, PWM_SYSCLK_DIV_1);
    PWMGenConfigure(PWM0_BASE, PWM_GEN_3,
                    PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
    PWMGenPeriodSet(PWM0_BASE, PWM_GEN_3, 8000);
    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_7, 2000);
    PWMOutputState(PWM0_BASE, PWM_OUT_7_BIT, true);
    PWMGenDisable(PWM0_BASE, PWM_GEN_3);
}

static void I2C_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) {}

    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    I2CMasterInitExpClk(I2C0_BASE, ui32SysClock, true);
    I2CMasterEnable(I2C0_BASE);

    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xFF);
    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00);
    I2C_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00);

    I2C_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00);
    I2C_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, 0xFF);
}

/* ================================================================
 * 开机画面
 * ================================================================ */
void Boot_Sequence(void)
{
    uint32_t start;
    uint32_t last_disp;
    int phase;
    int i;

    for (phase = 0; phase < 2; phase++) {
        for (i = 0; i < DISP_LEN; i++) disp_buf[i] = 0xFF;
        PCA9557_Write(0x00);
        start = g_tick_ms; last_disp = 0;
        while (g_tick_ms - start < 350) {
            if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
            if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
        }
        for (i = 0; i < DISP_LEN; i++) disp_buf[i] = 0x00;
        PCA9557_Write(0xFF);
        start = g_tick_ms; last_disp = 0;
        while (g_tick_ms - start < 350) {
            if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
            if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0xFF); }
        }
    }
    for (i = 0; i < DISP_LEN; i++) disp_buf[i] = 0xFF;
    PCA9557_Write(0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 350) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
    }

    /* 学号: 31910672 闪烁 */
    Display_SetStr("31910672", 0x00);
    PCA9557_Write(0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 350) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
    }
    for (i = 0; i < DISP_LEN; i++) disp_buf[i] = 0x00;
    PCA9557_Write(0xFF);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 200) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0xFF); }
    }
    Display_SetStr("31910672", 0x00);
    PCA9557_Write(0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 350) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
    }

    /* 姓名: HUZHENYE 闪烁 */
    Display_SetStr("HUZHENYE", 0x00);
    PCA9557_Write(0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 350) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
    }
    for (i = 0; i < DISP_LEN; i++) disp_buf[i] = 0x00;
    PCA9557_Write(0xFF);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 200) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0xFF); }
    }
    Display_SetStr("HUZHENYE", 0x00);
    PCA9557_Write(0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 350) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); PCA9557_Write(0x00); }
    }

    /* 版本号 1.5s */
    Display_SetStr("V1.0    ", 0x00);
    start = g_tick_ms; last_disp = 0;
    while (g_tick_ms - start < 1500) {
        if (Tick_TimedOut(last_disp, 2)) { last_disp = g_tick_ms; Display_Refresh(); }
        if (flag_10ms) { flag_10ms = 0; LED_Update(); }
    }
}


/* ================================================================
 * 编辑 FSM
 * ================================================================ */

static void Format_LongDateDisplay(void)
{
    char str[DISP_LEN + 1];
    uint8_t dp;
    sprintf(str, "%04d%02d%02d", g_date.y, g_date.m, g_date.d);
    str[DISP_LEN] = '\0';
    dp = (1 << 4) | (1 << 2);
    Display_SetStr(str, dp);
}

static void Format_ShortDateDisplay(void)
{
    char str[DISP_LEN + 1];
    uint8_t dp;
    str[0] = '0' + ((g_date.y / 10) % 10);
    str[1] = '0' + (g_date.y % 10);
    str[2] = '0' + (g_date.m / 10);
    str[3] = '0' + (g_date.m % 10);
    str[4] = '0' + (g_date.d / 10);
    str[5] = '0' + (g_date.d % 10);
    str[6] = ' ';
    str[7] = ' ';
    str[DISP_LEN] = '\0';
    dp = (1 << 6) | (1 << 4);
    Display_SetStr(str, dp);
}

static void Format_AlarmDisplay(void)
{
    char str[DISP_LEN + 1];
    uint8_t dp;
    str[0] = '0' + (g_alarm.t.h / 10);
    str[1] = '0' + (g_alarm.t.h % 10);
    str[2] = '0' + (g_alarm.t.mi / 10);
    str[3] = '0' + (g_alarm.t.mi % 10);
    str[4] = '0' + (g_alarm.t.s / 10);
    str[5] = '0' + (g_alarm.t.s % 10);
    str[6] = ' ';
    str[7] = ' ';
    str[DISP_LEN] = '\0';
    dp = (1 << 6) | (1 << 4);
    Display_SetStr(str, dp);
}

static void Edit_Enter(sys_state_t st)
{
    g_state = st;
    g_edit_pos = 0;
    edit_modified = 0;
    g_last_activity_ms = g_tick_ms;

    if (st == STATE_EDIT_DATE_LONG) {
        edit_field = EF_YEAR;
        disp_blink_mask = 0xF0;
        Format_LongDateDisplay();
    } else if (st == STATE_EDIT_DATE) {
        edit_field = EF_YEAR;
        disp_blink_mask = 0xC0;
        Format_ShortDateDisplay();
    } else if (st == STATE_EDIT_TIME) {
        edit_field = EF_HOUR;
        disp_blink_mask = 0xC0;
        Clock_FormatDisplay();
    } else if (st == STATE_EDIT_ALARM) {
        edit_field = EF_HOUR;
        disp_blink_mask = 0xC0;
        /* Sync current day's slot into g_alarm.t for editing */
        {
            uint8_t idx = ALARM_IDX;
            if (idx < ALARM_SLOTS) {
                g_alarm.t.h  = g_alarm_slot[idx].h;
                g_alarm.t.mi = g_alarm_slot[idx].mi;
                g_alarm.t.s  = g_alarm_slot[idx].s;
                g_alarm.enabled = (g_alarm_slot_enabled_mask & (1 << idx)) ? 1 : 0;
            }
        }
        Format_AlarmDisplay();
    }
}

static void Edit_Exit(uint8_t save)
{
    char buf[48];
    const char *type;
    uint16_t val;

    if (!save) {
        memcpy(&g_time,  &edit_backup_time,  sizeof(time_t_));
        memcpy(&g_date,  &edit_backup_date,  sizeof(date_t));
        memcpy(&g_alarm, &edit_backup_alarm, sizeof(alarm_t));
    }

    if (save && edit_modified) {
        val = 0;
        if (g_state == STATE_EDIT_DATE_LONG || g_state == STATE_EDIT_DATE) {
            type = "DATE";
            val = g_date.y * 10000 + g_date.m * 100 + g_date.d;
        } else if (g_state == STATE_EDIT_TIME) {
            type = "TIME";
            val = g_time.h * 10000 + g_time.mi * 100 + g_time.s;
        } else {
            type = "ALARM";
            val = g_alarm.t.h * 10000 + g_alarm.t.mi * 100 + g_alarm.t.s;
            /* Sync back to current day's multi-alarm slot */
            {
                uint8_t idx = ALARM_IDX;
                if (idx < ALARM_SLOTS) {
                    g_alarm_slot[idx].h  = g_alarm.t.h;
                    g_alarm_slot[idx].mi = g_alarm.t.mi;
                    g_alarm_slot[idx].s  = g_alarm.t.s;
                    if (g_alarm.enabled)
                        g_alarm_slot_enabled_mask |= (1 << idx);
                    else
                        g_alarm_slot_enabled_mask &= ~(1 << idx);
                }
            }
        }
        sprintf(buf, "*EVT:EDIT %s %u\r\n", type, val);
        UART_PutStrNB(buf);
    }
    g_state = STATE_CLOCK;
    disp_blink_mask = 0;
    g_edit_pos = 0;
    Clock_FormatDisplay();
}

void Edit_HandleKey(key_code_t code, key_event_t evt)
{
    uint8_t max_pos;
    uint8_t pos_byte;
    uint8_t max_d;
    uint8_t is_long_date;

    if (evt != KEV_DOWN && evt != KEV_LONG) return;
    g_last_activity_ms = g_tick_ms;
    is_long_date = (g_state == STATE_EDIT_DATE_LONG);

    switch (code) {
    case KEY_FUNC:
        if (evt == KEV_LONG) { Edit_Exit(1); }
        else {
            edit_modified = 0;
            if (g_state == STATE_EDIT_DATE_LONG) Edit_Enter(STATE_EDIT_DATE);
            else if (g_state == STATE_EDIT_DATE) Edit_Enter(STATE_EDIT_TIME);
            else if (g_state == STATE_EDIT_TIME) Edit_Enter(STATE_EDIT_ALARM);
            else { Edit_Exit(0); }
        }
        break;

    case KEY_SHIFT:
        if (is_long_date) {
            max_pos = 3;
            g_edit_pos = (g_edit_pos + 1) % max_pos;
            if (g_edit_pos == 0)      { disp_blink_mask = 0xF0; edit_field = EF_YEAR; }
            else if (g_edit_pos == 1) { disp_blink_mask = 0x0C; edit_field = EF_MONTH; }
            else                      { disp_blink_mask = 0x03; edit_field = EF_DAT; }
        } else {
            max_pos = 6;
            g_edit_pos = (g_edit_pos + 1) % max_pos;
            pos_byte = g_edit_pos / 2;
            disp_blink_mask = (uint8_t)(0xC0 >> (pos_byte * 2));
            if (g_state == STATE_EDIT_DATE) {
                if (pos_byte == 0) edit_field = EF_YEAR;
                else if (pos_byte == 1) edit_field = EF_MONTH;
                else edit_field = EF_DAT;
            } else {
                if (pos_byte == 0) edit_field = EF_HOUR;
                else if (pos_byte == 1) edit_field = EF_MINUTE;
                else edit_field = EF_SECOND;
            }
        }
        break;

    case KEY_ADD:
        edit_modified = 1;
        if (is_long_date && edit_field == EF_YEAR) {
            g_date.y++; if (g_date.y > 2099) g_date.y = 2026;
            Format_LongDateDisplay();
        } else {
            switch (edit_field) {
                case EF_YEAR:
                    g_date.y++; if (g_date.y > 2099) g_date.y = 2026;
                    Format_ShortDateDisplay(); break;
                case EF_MONTH:
                    g_date.m++; if (g_date.m > 12) g_date.m = 1;
                    if (is_long_date) Format_LongDateDisplay();
                    else Format_ShortDateDisplay(); break;
                case EF_DAT:
                    max_d = days_in_month[g_date.m - 1];
                    if (g_date.m == 2 && is_leap_year(g_date.y)) max_d = 29;
                    g_date.d++; if (g_date.d > max_d) g_date.d = 1;
                    if (is_long_date) Format_LongDateDisplay();
                    else Format_ShortDateDisplay(); break;
                case EF_HOUR:
                    if (g_state == STATE_EDIT_TIME) { g_time.h++; if (g_time.h >= 24) g_time.h = 0; Clock_FormatDisplay(); }
                    else { g_alarm.t.h++; if (g_alarm.t.h >= 24) g_alarm.t.h = 0; Format_AlarmDisplay(); }
                    break;
                case EF_MINUTE:
                    if (g_state == STATE_EDIT_TIME) { g_time.mi++; if (g_time.mi >= 60) g_time.mi = 0; Clock_FormatDisplay(); }
                    else { g_alarm.t.mi++; if (g_alarm.t.mi >= 60) g_alarm.t.mi = 0; Format_AlarmDisplay(); }
                    break;
                case EF_SECOND:
                    if (g_state == STATE_EDIT_TIME) { g_time.s++; if (g_time.s >= 60) g_time.s = 0; Clock_FormatDisplay(); }
                    else { g_alarm.t.s++; if (g_alarm.t.s >= 60) g_alarm.t.s = 0; Format_AlarmDisplay(); }
                    break;
                default: break;
            }
        }
        break;

    case KEY_SAVE:
        if (evt == KEV_LONG) Edit_Exit(1);
        break;
    default: break;
    }
}
/* DOW name → alarm slot index. SUN=6, MON=0, ... SAT=5. Returns -1 if not found. */
static int8_t dow_to_slot(const char *s)
{
    if (cmd_match(s, "MON")) return 0;
    if (cmd_match(s, "TUE")) return 1;
    if (cmd_match(s, "WED")) return 2;
    if (cmd_match(s, "THU")) return 3;
    if (cmd_match(s, "FRI")) return 4;
    if (cmd_match(s, "SAT")) return 5;
    if (cmd_match(s, "SUN")) return 6;
    return -1;
}

void ProcessCommand(char *cmd)
{
    char *p;
    char resp[64];

    if (cmd[0] == '\0') return;

    /* *PING */
    if (strncmp(cmd, "*PING", 5) == 0) {
        sprintf(resp, "*PONG %lu\r\n", g_uptime_s);
        UART_PutStrNB(resp);
        return;
    }

    /* *RST */
    if (strncmp(cmd, "*RST", 4) == 0) {
        p = cmd + 4;
        /* sub-reset: check for DATE/TIME/ALARM suffix */
        if (cmd_match(p, "DATE") || cmd_match(p, "YEAR") || cmd_match(p, "MONTH")) {
            g_date.y = 2026; g_date.m = 6; g_date.d = 8;
        } else if (cmd_match(p, "TIME")) {
            g_time.h = 0; g_time.mi = 0; g_time.s = 0;
        } else if (cmd_match(p, "ALARM")) {
            uint8_t ai;
            for (ai = 0; ai < ALARM_SLOTS; ai++) {
                g_alarm_slot[ai].h = 6;
                g_alarm_slot[ai].mi = 0;
                g_alarm_slot[ai].s = 0;
            }
            g_alarm_slot_enabled_mask = 0x1F;
        } else {
            /* full reset */
            g_time.h = 0; g_time.mi = 0; g_time.s = 0;
            g_date.y = 2026; g_date.m = 6; g_date.d = 15; g_date.wday = 1;
            g_format = FMT_LEFT;
            scroll_dir = 0;
            scroll_speed = 0;
            disp_on = 1;
            g_disp_mode = DISP_MODE_TIME;
            g_night_mode = 0;
            g_mode_day = 1;
            g_state   = STATE_CLOCK;
            disp_blink_mask = 0;
            g_led_override = 0;
            g_msg_active   = 0;
            memset(scroll_buf, 0, sizeof(scroll_buf));
            scroll_len    = 0;
            g_scroll_speed_level = 0;
            g_alarm_beep_active = 0;
            remote_beep_active  = 0;
            Beep_Off();
            beep_force_off = 1;   /* *RST 后确保持续静音 */
            {
                uint8_t ai;
                for (ai = 0; ai < ALARM_SLOTS; ai++) {
                    g_alarm_slot[ai].h = 6;
                    g_alarm_slot[ai].mi = 0;
                    g_alarm_slot[ai].s = 0;
                }
            }
            g_alarm_slot_enabled_mask = 0x1F;
        }
        Clock_FormatDisplay();
        UART_PutStrNB("OK\r\n");
        return;
    }

    /* *SET:... */
    if (strncmp(cmd, "*SET:", 5) == 0) {
        p = cmd + 5;

        /* *SET:DATE - 2-pass: Pass1 keywords→kmap, Pass2 vals[], Map by kmap */
        if (cmd_match(p, "DATE")) {
            int yr, mo, dy;
            int kmap;
            int vals[3];
            int vi, wi;
            uint8_t max_d;
            char *q;

            p += 4;
            skip_kw_rest(&p, "");

            /* Pass 1: scan keywords on copy → kmap */
            kmap = 0;
            q = p;
            while (*q) {
                if (cmd_match(q, "YEAR"))       { kmap |= 1; q += 4; skip_kw_rest(&q, ""); }
                else if (cmd_match(q, "MONTH")) { kmap |= 2; q += 5; skip_kw_rest(&q, ""); }
                else if (cmd_match(q, "DATE"))  { kmap |= 4; q += 4; skip_kw_rest(&q, ""); }
                else break;
            }
            /* Any remaining alpha = unknown keyword → error */
            if (*q && ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')))
                { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }

            /* Pass 2: extract integers (YEAR=4digits only if kmap&1, else all 2digits) */
            wi = 0;
            while (*p && wi < 3) {
                if (*p >= '0' && *p <= '9') {
                    int v = 0; uint8_t dc = 0;
                    uint8_t lim = ((wi == 0 && (kmap & 1)) ? 4 : 2);
                    while (*p >= '0' && *p <= '9' && dc < lim) { v = v * 10 + (*p++ - '0'); dc++; }
                    vals[wi++] = v;
                } else { p++; }
            }
            if (wi == 0) { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }

            /* Map by kmap */
            yr = -1; mo = -1; dy = -1;
            vi = 0;
            if (kmap & 1) yr = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) mo = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) dy = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) { if (yr < 0) yr = vals[vi]; else if (mo < 0) mo = vals[vi]; else if (dy < 0) dy = vals[vi]; vi++; }

            /* Apply */
            if (yr < 0) yr = (int)g_date.y;
            if (yr < 2025 || yr > 2099) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            g_date.y = (uint16_t)yr;
            if (mo >= 0) g_date.m = (uint8_t)mo;
            if (dy >= 0) g_date.d = (uint8_t)dy;
            max_d = days_in_month[g_date.m - 1];
            if (g_date.m == 2 && is_leap_year(g_date.y)) max_d = 29;
            if (g_date.d > max_d) g_date.d = max_d;
            {
                uint32_t val;
                val = g_date.y * 10000 + g_date.m * 100 + g_date.d;
                sprintf(resp, "*EVT:EDIT DATE %lu\r\n", val);
                UART_PutStrNB(resp);
            }
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:TIME - 2-pass: Pass1 keywords→kmap, Pass2 vals[], Map by kmap */
        if (cmd_match(p, "TIME")) {
            int h, m, s;
            int kmap;
            int vals[3];
            int vi, wi;
            char *q;

            p += 4;
            skip_kw_rest(&p, "");

            /* OFF check (before 2-pass, keyword-only subcommand) */
            q = p;
            if (cmd_match(q, "OFF")) {
                disp_on = 0;
                UART_PutStrNB("OK\r\n"); return;
            }

            /* Pass 1: scan keywords → kmap */
            kmap = 0;
            q = p;
            while (*q) {
                if (cmd_match(q, "HOUR"))       { kmap |= 1; q += 4; skip_kw_rest(&q, ""); }
                else if (cmd_match(q, "MIN"))   { kmap |= 2; q += 3; skip_kw_rest(&q, "UTE"); }
                else if (cmd_match(q, "SEC"))   { kmap |= 4; q += 3; skip_kw_rest(&q, "OND"); }
                else break;
            }
            /* Any remaining alpha = unknown keyword → error */
            if (*q && ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')))
                { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }

            /* Pass 2: extract integers (max 2 digits each, inline, no strtol) */
            wi = 0;
            while (*p && wi < 3) {
                if (*p >= '0' && *p <= '9') {
                    int v = 0; uint8_t dc = 0;
                    while (*p >= '0' && *p <= '9' && dc < 2) { v = v * 10 + (*p++ - '0'); dc++; }
                    vals[wi++] = v;
                } else { p++; }
            }

            /* Map by kmap */
            h = -1; m = -1; s = -1;
            vi = 0;
            if (kmap & 1) h = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) m = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) s = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) { if (h < 0) h = vals[vi]; else if (m < 0) m = vals[vi]; else if (s < 0) s = vals[vi]; vi++; }

            /* Apply */
            if (h >= 0) { if (h > 23) { UART_PutStrNB("ERROR RANGE\r\n"); return; } g_time.h = (uint8_t)h; }
            if (m >= 0) { if (m > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; } g_time.mi = (uint8_t)m; if (s < 0) g_time.s = 0; }
            if (s >= 0) { if (s > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; } g_time.s = (uint8_t)s; }
            {
                uint32_t val;
                val = g_time.h * 10000 + g_time.mi * 100 + g_time.s;
                sprintf(resp, "*EVT:EDIT TIME %lu\r\n", val);
                UART_PutStrNB(resp);
            }
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:ALARM - 2-pass: Pass1 keywords→kmap, Pass2 vals[], Map by kmap */
        if (cmd_match(p, "ALARM")) {
            int h, m, s;
            int kmap;
            int vals[3];
            int vi, wi;
            int si, ss, se;
            char *q;

            p += 5;
            skip_kw_rest(&p, "");

            ss = (int)ALARM_IDX;
            se = ss + 1;

            /* DOW slot override: *SET:ALARM SUN ... etc */
            q = p;
            { int8_t ds = dow_to_slot(q);
              if (ds >= 0) {
                  ss = ds; se = ds + 1;
                  q += 3; skip_kw_rest(&q, "");
                  p = q;  /* advance main pointer past DOW token */
              }
            }

            /* ON/OFF check (before 2-pass) */
            q = p;
            if (cmd_match(q, "ON")) {
                for (si = ss; si < se; si++)
                    g_alarm_slot_enabled_mask |= (1U << si);
                UART_PutStrNB("OK\r\n"); return;
            }
            if (cmd_match(q, "OFF")) {
                for (si = ss; si < se; si++)
                    g_alarm_slot_enabled_mask &= ~(1U << si);
                g_alarm_beep_active = 0;
                UART_PutStrNB("OK\r\n"); return;
            }

            /* Pass 1: scan keywords → kmap */
            kmap = 0;
            q = p;
            while (*q) {
                if (cmd_match(q, "HOUR"))       { kmap |= 1; q += 4; skip_kw_rest(&q, ""); }
                else if (cmd_match(q, "MIN"))   { kmap |= 2; q += 3; skip_kw_rest(&q, "UTE"); }
                else if (cmd_match(q, "SEC"))   { kmap |= 4; q += 3; skip_kw_rest(&q, "OND"); }
                else break;
            }
            /* Any remaining alpha = unknown keyword → error */
            if (*q && ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')))
                { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }

            /* Pass 2: extract integers (max 2 digits each, inline, no strtol) */
            wi = 0;
            while (*p && wi < 3) {
                if (*p >= '0' && *p <= '9') {
                    int v = 0; uint8_t dc = 0;
                    while (*p >= '0' && *p <= '9' && dc < 2) { v = v * 10 + (*p++ - '0'); dc++; }
                    vals[wi++] = v;
                } else { p++; }
            }

            /* Map by kmap */
            h = -1; m = -1; s = -1;
            vi = 0;
            if (kmap & 1) h = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) m = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) s = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) { if (h < 0) h = vals[vi]; else if (m < 0) m = vals[vi]; else if (s < 0) s = vals[vi]; vi++; }

            /* Apply */
            if (h >= 0) { if (h > 23) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) { g_alarm_slot[si].h = (uint8_t)h; g_alarm_slot_enabled_mask |= (1U << si); } }
            if (m >= 0) { if (m > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) { g_alarm_slot[si].mi = (uint8_t)m; g_alarm_slot_enabled_mask |= (1U << si); } }
            if (s >= 0) { if (s > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) { g_alarm_slot[si].s = (uint8_t)s; g_alarm_slot_enabled_mask |= (1U << si); } }
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:DISPlay ON/OFF */
        if (cmd_match(p, "DISP")) {
            p += 4;
            skip_kw_rest(&p, "LAY");
            if (cmd_match(p, "ON"))  { disp_on = 1; skip_kw_rest(&p, "");
                                       UART_PutStrNB("OK\r\n"); return; }
            if (cmd_match(p, "OFF")) { disp_on = 0; skip_kw_rest(&p, "");
                                       UART_PutStrNB("OK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        /* *SET:FORMAT LEFT/RIGHT */
        if (cmd_match(p, "FORMAT")) {
            p += 6;
            skip_kw_rest(&p, "");
            if (cmd_match(p, "LEFT"))  { g_format = FMT_LEFT;  scroll_dir = 0; skip_kw_rest(&p, "");
                                         UART_PutStrNB("OK\r\n"); return; }
            if (cmd_match(p, "RIGHT")) { g_format = FMT_RIGHT; scroll_dir = 1; skip_kw_rest(&p, "");
                                         UART_PutStrNB("OK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        /* *SET:MSG */
        if (cmd_match(p, "MSG")) {
            char   *msg_text;
            uint8_t len;
            p += 3;
            skip_kw_rest(&p, "");
            while (*p == ' ') p++;
            msg_text = p;
            if (*msg_text == '\0') { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }
            len = (uint8_t)strlen(msg_text);
            if (len > 32) len = 32;
            memset(scroll_buf, 0, sizeof(scroll_buf));
            memcpy(scroll_buf, msg_text, len);
            scroll_len = len;
            scroll_off = 0;
            scroll_speed = 0;
            scroll_dir = 0;
            scroll_dp_bitmap = 0;
            g_msg_active = 1;
            g_msg_end_ms = g_tick_ms + 3000;
            if (len <= 8) {
                Display_SetStr(scroll_buf, 0x00);
                g_state = STATE_MSG_STATIC;
            } else {
                Display_SetStr(scroll_buf, 0x00);
                g_state = STATE_SCROLL;
            }
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:BEEP 10-5000 */
        if (cmd_match(p, "BEEP")) {
            uint16_t ms;
            p += 4;
            skip_kw_rest(&p, "");
            { int vv = 0; while (*p >= '0' && *p <= '9') { vv = vv * 10 + (*p++ - '0'); } ms = (uint16_t)vv; }
            if (ms < 10 || ms > 5000) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            remote_beep_active = 1;
            remote_beep_end_ms = g_tick_ms + ms;
            Beep_On();
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:LED <hex2> */
        if (cmd_match(p, "LED")) {
            uint8_t val;
            p += 3;
            skip_kw_rest(&p, "");
            if (*p == '\0') { UART_PutStrNB("ERROR PARAM\r\n"); return; }
            val = 0;
            { int _hx = 0; while (_hx < 2) { _hx++;
                char ch = *p;
                if (!((ch >= '0' && ch <= '9') ||
                      (ch >= 'A' && ch <= 'F') ||
                      (ch >= 'a' && ch <= 'f'))) break;
                val <<= 4;
                if      (ch >= '0' && ch <= '9') val |= (uint8_t)(ch - '0');
                else if (ch >= 'A' && ch <= 'F') val |= (uint8_t)(ch - 'A' + 10);
                else if (ch >= 'a' && ch <= 'f') val |= (uint8_t)(ch - 'a' + 10);
                p++;
            }}
            UART_PutStrNB("OK\r\n");
            g_led_value = val;
            g_led_override = (g_led_value != 0);
            if (g_led_override) led_override_start_ms = g_tick_ms;
            return;
        }

        /* *SET:KEY <NAME> */
        if (cmd_match(p, "KEY")) {
            key_code_t kc = KEY_NONE;
            p += 3;
            skip_kw_rest(&p, "");
            if (cmd_match(p, "FUNC"))   kc = KEY_FUNC;
            else if (cmd_match(p, "SHIFT"))  kc = KEY_SHIFT;
            else if (cmd_match(p, "ADD"))    kc = KEY_ADD;
            else if (cmd_match(p, "SAVE"))   kc = KEY_SAVE;
            else if (cmd_match(p, "DISP"))   kc = KEY_DISP;
            else if (cmd_match(p, "SPEED"))  kc = KEY_SPEED;
            else if (cmd_match(p, "FORMAT")) kc = KEY_FORMAT;
            else if (cmd_match(p, "EXT"))    kc = KEY_EXT;
            else if (cmd_match(p, "USER1"))  kc = KEY_USER1;
            else if (cmd_match(p, "USER2"))  kc = KEY_USER2;
            if (kc == KEY_NONE) { UART_PutStrNB("ERROR PARAM\r\n"); return; }
            g_suppress_key_evt = 1;
            KeyQueue_Push(kc, KEV_DOWN);
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:MODE DAY/NIGHT */
        if (cmd_match(p, "MODE")) {
            p += 4;
            skip_kw_rest(&p, "");
            if (cmd_match(p, "DAY"))   { g_night_mode = 0; g_mode_day = 1; skip_kw_rest(&p, "");
                                         UART_PutStrNB("*EVT:MODE DAY\r\nOK\r\n"); return; }
            if (cmd_match(p, "NIGHT")) { g_night_mode = 1; g_mode_day = 0; skip_kw_rest(&p, "");
                                         UART_PutStrNB("*EVT:MODE NIGHT\r\nOK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        /* *SET:WEA <temp> <COND> */
        if (cmd_match(p, "WEA")) {
            int t;
            p += 3;
            skip_kw_rest(&p, "THER");
            { t = 0; { int sign = 1; if (*p == '-') { sign = -1; p++; } while (*p >= '0' && *p <= '9') { t = t * 10 + (*p++ - '0'); } t *= sign; } }
            if (t < -40 || t > 50) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            g_weather_temp = (int8_t)t;
            if (cmd_match(p, "SUN"))      { strcpy(g_weather_cond, "SUN"); skip_kw_rest(&p, ""); }
            else if (cmd_match(p, "CLD")) { strcpy(g_weather_cond, "CLD"); skip_kw_rest(&p, ""); }
            else if (cmd_match(p, "OVC")) { strcpy(g_weather_cond, "OVC"); skip_kw_rest(&p, ""); }
            else if (cmd_match(p, "RAI")) { strcpy(g_weather_cond, "RAI"); skip_kw_rest(&p, ""); }
            else if (cmd_match(p, "SNO")) { strcpy(g_weather_cond, "SNO"); skip_kw_rest(&p, ""); }
            else if (cmd_match(p, "FOG")) { strcpy(g_weather_cond, "FOG"); skip_kw_rest(&p, ""); }
            else { strcpy(g_weather_cond, "UNK"); }
            g_weather_valid = 1;
            g_weather_age = 0;
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:GAME START | SCORE nnn | OVER nnn | QUIT */
        if (cmd_match(p, "GAME")) {
            p += 4;
            skip_kw_rest(&p, "");
            if (cmd_match(p, "START")) {
                g_game_active = 1;
                g_state = STATE_GAME;
                Display_SetStr("--------", 0x00);
                UART_PutStrNB("OK\r\n"); return;
            }
            if (cmd_match(p, "SCORE")) {
                { int vv = 0; p += 5; skip_kw_rest(&p, ""); while (*p >= '0' && *p <= '9') { vv = vv * 10 + (*p++ - '0'); } g_game_score = (uint16_t)vv; }
                { char buf[9]; sprintf(buf, "Sc %03d  ", g_game_score); Display_SetStr(buf, 0x00); }
                UART_PutStrNB("OK\r\n"); return;
            }
            if (cmd_match(p, "OVER")) {
                int i;
                { int vv = 0; p += 4; skip_kw_rest(&p, ""); while (*p >= '0' && *p <= '9') { vv = vv * 10 + (*p++ - '0'); } g_game_score = (uint16_t)vv; }
                { char buf[9]; sprintf(buf, "End%03d  ", g_game_score); Display_SetStr(buf, 0x00); }
                for (i = 0; i < 2; i++) {
                    PCA9557_Write(0x00); { volatile int _z=600000; while(_z) _z--; }
                    PCA9557_Write(0xFF); { volatile int _z=600000; while(_z) _z--; }
                }
                g_game_active = 0;
                g_state = STATE_CLOCK;
                Clock_FormatDisplay();
                UART_PutStrNB("OK\r\n"); return;
            }
            if (cmd_match(p, "QUIT")) {
                g_game_active = 0;
                g_state = STATE_CLOCK;
                Clock_FormatDisplay();
                UART_PutStrNB("OK\r\n"); return;
            }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        UART_PutStrNB("ERROR SYNTAX\r\n");
        return;
    }

    /* *GET:... */
    if (strncmp(cmd, "*GET:", 5) == 0) {
        p = cmd + 5;
        if (*p == '\0' || cmd_match(p, "TIME")) {
            if (g_format == FMT_RIGHT) {
                uint8_t rh = (g_time.h % 10) * 10 + (g_time.h / 10);
                uint8_t rm = (g_time.mi % 10) * 10 + (g_time.mi / 10);
                uint8_t rs = (g_time.s % 10) * 10 + (g_time.s / 10);
                sprintf(resp, "OK %02u.%02u.%02u\r\n", rs, rm, rh);
            } else {
                sprintf(resp, "OK %02u.%02u.%02u\r\n", g_time.h, g_time.mi, g_time.s);
            }
            UART_PutStrNB(resp);
            return;
        }
        if (cmd_match(p, "DATE")) {
            if (g_format == FMT_RIGHT) {
                uint8_t rd = (g_date.d % 10) * 10 + (g_date.d / 10);
                uint8_t rm = (g_date.m % 10) * 10 + (g_date.m / 10);
                uint16_t ry = ((g_date.y % 10) * 1000 + ((g_date.y/10) % 10) * 100 +
                              ((g_date.y/100) % 10) * 10 + (g_date.y / 1000));
                sprintf(resp, "OK %02u.%02u.%04u\r\n", rd, rm, ry);
            } else {
                sprintf(resp, "OK %04u.%02u.%02u\r\n", g_date.y, g_date.m, g_date.d);
            }
            UART_PutStrNB(resp);
            return;
        }
        if (cmd_match(p, "FORMAT")) {
            UART_PutStrNB(g_format == FMT_LEFT ? "OK LEFT\r\n" : "OK RIGHT\r\n");
            return;
        }
        if (cmd_match(p, "ALARM")) {
            int off = 0;
            int si;
            off = sprintf(resp, "OK");
            for (si = 0; si < ALARM_SLOTS; si++) {
                off += sprintf(resp + off, " %s,%d,%d,%d,%d",
                    DOW_NAMES[si + 1],
                    g_alarm_slot[si].h, g_alarm_slot[si].mi, g_alarm_slot[si].s,
                    (g_alarm_slot_enabled_mask & (1 << si)) ? 1 : 0);
            }
            sprintf(resp + off, "\r\n");
            UART_PutStrNB(resp);
            return;
        }
        if (cmd_match(p, "DISP")) {
            UART_PutStrNB(disp_on ? "OK ON\r\n" : "OK OFF\r\n");
            return;
        }
        if (cmd_match(p, "MODE")) {
            UART_PutStrNB(g_night_mode ? "OK NIGHT\r\n" : "OK DAY\r\n");
            return;
        }
        UART_PutStrNB("ERROR SYNTAX\r\n");
        return;
    }

    /* *NTP */
    if (strncmp(cmd, "*NTP", 4) == 0) {
        g_ntp_synced = 1;
        g_ntp_last_sync_ms = g_tick_ms;
        UART_PutStrNB("OK\r\n");
        return;
    }

    {
        char err[46];
        sprintf(err, "ERROR '%s'\r\n", cmd);
        UART_PutStrNB(err);
    }
}


static void Report_Display(void)
{
    char str[DISP_LEN + 1];
    uint8_t dp_hex;
    uint8_t i;
    char buf[40];

    dp_hex = 0;
    for (i = 0; i < DISP_LEN; i++) {
        char ch = disp_char[i];
        if (ch == ' ') ch = '_';
        str[i] = ch;
        if (disp_dp[i]) dp_hex |= (uint8_t)(1 << (7 - i));
    }
    str[DISP_LEN] = '\0';
    sprintf(buf, "*EVT:DISP %s %02X\r\n", str, dp_hex);
    UART_PutStrNB(buf);
}

static void Report_LED(void)
{
    char buf[20];
    /* PCA9557 低电平点亮 → 取反后上报 (1=亮, 0=灭) */
    sprintf(buf, "*EVT:LED %02X\r\n", (uint8_t)(~led_pca9557_cache));
    UART_PutStrNB(buf);
}

/* ================================================================
 * 主函数
 * ================================================================ */
int main(void)
{
    uint32_t last_key_scan;
    uint32_t last_disp_scan;
    uint32_t last_led_update;
    uint32_t last_report;
    uint32_t last_scroll;

    last_key_scan     = 0;
    last_disp_scan    = 0;
    last_led_update   = 0;
    last_report       = 0;
    last_scroll       = 0;

    /* 系统时钟 */
    ui32SysClock = SysCtlClockFreqSet(
        (SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480), 20000000);

    /* SysTick 1ms */
    SysTickPeriodSet(ui32SysClock / SYSTICK_FREQUENCY);
    SysTickEnable();
    SysTickIntEnable();

    /* 外设 */
    GPIO_Init();
    I2C_Init();
    UART_Init(115200);
    Display_Init();

    /* UART 中断 */
    IntEnable(INT_UART0);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);

    IntMasterEnable();

    /* 默认值 */
    g_time.h = 12; g_time.mi = 0; g_time.s = 0;
    g_date.y = 2026; g_date.m = 6; g_date.d = 15; g_date.wday = 1;
    g_format = FMT_LEFT;
    disp_on = 1;
    g_mode_day = 1;
    g_ntp_synced = 0;
    g_weather_valid = 0;
    g_night_mode = 0;
    g_disp_mode = DISP_MODE_TIME;
    g_scroll_speed_level = 0;
    g_suppress_key_evt = 0;
    {
        uint8_t ai;
        for (ai = 0; ai < ALARM_SLOTS; ai++) {
            g_alarm_slot[ai].h = 6;
            g_alarm_slot[ai].mi = 0;
            g_alarm_slot[ai].s = 0;
        }
        g_alarm_slot_enabled_mask = 0x1F;
    }

    /* 开机画面 */
    g_state = STATE_BOOT;
    Boot_Sequence();

    g_state = STATE_CLOCK;
    Clock_FormatDisplay();

    UART_PutStr("S800 Ready\r\n");

    /* ================================================================
     * 主循环
     * ================================================================ */
    while (1) {

        /* 10ms 任务 */
        if (flag_10ms) {
            flag_10ms = 0;

            if (Tick_TimedOut(last_key_scan, 10)) {
                last_key_scan = g_tick_ms;
                Key_Scan();
            }

            if (Tick_TimedOut(last_led_update, 100)) {
                last_led_update = g_tick_ms;
                LED_Update();
                if (led_uart_tx_active) {
                    led_uart_tx_active = 0;
                }
                if (led_uart_rx_active) {
                    led_uart_rx_active = 0;
                }
            }

        }

        /* 显示扫描 2ms */
        if (Tick_TimedOut(last_disp_scan, 2)) {
            last_disp_scan = g_tick_ms;
            Display_Refresh();
        }

        /* 100ms 任务 */
        if (flag_100ms) {
            flag_100ms = 0;

            /* 流水/静态显示 — 长短信共用 scroll_buf */
            if (g_state == STATE_SCROLL || g_state == STATE_MSG_STATIC) {
                uint16_t speed_ms = (g_scroll_speed_level == 0) ? 500 : 250;
                if (Tick_TimedOut(last_scroll, speed_ms)) {
                    last_scroll = g_tick_ms;
                    Scroll_Tick();
                }
                /* MSG ≤8字超时退出 */
                if (g_state == STATE_MSG_STATIC && g_tick_ms >= g_msg_end_ms) {
                    memset(scroll_buf, 0, sizeof(scroll_buf));
                    scroll_len = 0;
                    g_msg_active = 0;
                    g_state = STATE_CLOCK;
                    Clock_FormatDisplay();
                }
            }

            /* 闹钟响铃 — PWM0 Gen3 2kHz, 300ms on/300ms off 相位切换 */
            if (g_alarm_beep_active) {
                uint32_t total_ms = 10000 + (uint32_t)g_alarm_weather_beeps * 600;
                uint32_t elapsed = g_tick_ms - beep_start_ms;
                if (elapsed >= total_ms) {
                    Alarm_Stop();
                    g_alarm_weather_beeps = 0;
                    g_alarm_weather_led = 0;
                } else {
                    uint8_t should_be_on = ((elapsed % 600) < 300) ? 1 : 0;
                    if (should_be_on != beep_phase_on) {
                        beep_phase_on = should_be_on;
                        if (beep_phase_on) { Beep_OutOn(); } else { Beep_OutOff(); }
                    }
                }
            }

            /* 远程蜂鸣 — 超时后启动持久静音, 每100ms反复Beep_Off */
            if (remote_beep_active && g_tick_ms >= remote_beep_end_ms) {
                remote_beep_active = 0;
                Beep_Off();  /* 首次关断 + 设置 beep_force_off=1 */
            }
            /* 持久静音循环: 确保蜂鸣器永远死透 */
            if (beep_force_off) {
                Beep_Off();
            }

            /* LED 接管 10s 自动退出 */
            if (g_led_override && Tick_TimedOut(led_override_start_ms, 10000)) {
                g_led_override = 0;
                g_led_value   = 0;
            }

            /* 天气短显超时 5s / 数据过期闪烁 (500ms周期) */
            if (g_state == STATE_WEATHER) {
                if (g_tick_ms >= g_msg_end_ms) {
                    g_state = STATE_CLOCK;
                    disp_blink_mask = 0;
                    Clock_FormatDisplay();
                }
                /* 数据过期 (>120s) 闪烁 */
                if (g_weather_valid && g_weather_age >= 120) {
                    disp_blink_mask = ((g_tick_ms / 500) & 0x01) ? 0xFF : 0x00;
                } else {
                    disp_blink_mask = 0;
                }
            }

        }

        /* 1秒 任务 */
        if (flag_1s) {
            flag_1s = 0;
            g_uptime_s++;

            if (g_state == STATE_CLOCK || g_state == STATE_SCROLL || g_state == STATE_GAME) {
                Time_Tick();
            }

            if (!g_game_active) Alarm_Check();

            if (g_state == STATE_CLOCK || g_state == STATE_GAME) {
                Clock_FormatDisplay();
            }

            if (g_state >= STATE_EDIT_DATE_LONG && g_state <= STATE_EDIT_ALARM) {
                if (Tick_TimedOut(g_last_activity_ms, 5000)) {
                    Edit_Exit(0);
                }
            }

            if (g_ntp_synced == 1 &&
                Tick_TimedOut(g_ntp_last_sync_ms, 86400000UL)) {
                g_ntp_synced = 2;
            }

            if (g_weather_valid && g_weather_age < 255) g_weather_age++;

            if (Tick_TimedOut(last_report, 1000)) {
                last_report = g_tick_ms;
                Report_Display();
                Report_LED();
            }
        }

        /* UART 行接收 */
        if (rx_line_ready) {
            rx_line_ready = 0;
            ExtractLine();
            /* Copy cmd_line to stack — isolate from any ISR/ring-buffer
             * writes that could corrupt the global buffer mid-parse. */
            {
                char cmd_copy[LINE_MAX];
                int ci;
                for (ci = 0; ci < LINE_MAX; ci++) {
                    cmd_copy[ci] = cmd_line[ci];
                    if (cmd_line[ci] == '\0') break;
                }
                cmd_copy[LINE_MAX - 1] = '\0';
                ProcessCommand(cmd_copy);
            }
            /* Beep timeout guard — checked after every command */
            if (remote_beep_active && g_tick_ms >= remote_beep_end_ms) {
                remote_beep_active = 0;
                Beep_Off();
            }
            if (beep_force_off) {
                Beep_Off();
            }
        }

        /* 按键事件处理 */
        {
            key_code_t kc;
            key_event_t ke;
            ke = Key_GetEvent(&kc);
            if (ke != KEV_NONE) {
                char buf[32];

                g_last_activity_ms = g_tick_ms;

                if (g_game_active) {
                    /* 游戏模式: 仅方向键上报，其他键忽略 */
                    if (kc == KEY_ADD || kc == KEY_FUNC ||
                        kc == KEY_SHIFT || kc == KEY_DISP) {
                        sprintf(buf, "*EVT:KEY %s\r\n", keycode_to_name(kc));
                        UART_PutStrNB(buf);
                    }
                }
                else if (kc == KEY_FUNC && g_alarm_beep_active) {
                    Alarm_Stop();
                }
                else if (g_state == STATE_SCROLL || g_state == STATE_MSG_STATIC) {
                    if (ke == KEV_DOWN) {
                        if (kc == KEY_SPEED) {
                            g_scroll_speed_level = !g_scroll_speed_level;
                            UART_PutStrNB(g_scroll_speed_level ? "*EVT:SPEED FAST\r\n" : "*EVT:SPEED SLOW\r\n");
                        } else if (kc == KEY_FORMAT && !g_night_mode) {
                            g_format = (g_format == FMT_LEFT) ? FMT_RIGHT : FMT_LEFT;
                            scroll_dir = !scroll_dir;
                        } else if (kc != KEY_FUNC && kc != KEY_SHIFT &&
                                   kc != KEY_ADD && kc != KEY_SAVE) {
                            memset(scroll_buf, 0, sizeof(scroll_buf));
                            scroll_len = 0;
                            g_state = STATE_CLOCK; g_msg_active = 0; Clock_FormatDisplay();
                        }
                    }
                }
                else if (g_state >= STATE_EDIT_DATE_LONG && g_state <= STATE_EDIT_ALARM) {
                    Edit_HandleKey(kc, ke);
                }
                else if (g_state == STATE_CLOCK) {
                    if (kc == KEY_FUNC && ke == KEV_DOWN) {
                        memcpy(&edit_backup_time,  &g_time,  sizeof(time_t_));
                        memcpy(&edit_backup_date,  &g_date,  sizeof(date_t));
                        memcpy(&edit_backup_alarm, &g_alarm, sizeof(alarm_t));
                        Edit_Enter(STATE_EDIT_DATE_LONG);
                    } else if (kc == KEY_USER2 && ke == KEV_DOWN) {
                        if (g_weather_valid) {
                            char wstr[9];
                            sprintf(wstr, "%2dC %-3s", g_weather_temp, g_weather_cond);
                            Display_SetStr(wstr, 0x00);
                            g_state = STATE_WEATHER;
                            g_msg_end_ms = g_tick_ms + 5000;
                        } else {
                            Display_SetStr("--C  ---", 0x00);
                            g_state = STATE_WEATHER;
                            g_msg_end_ms = g_tick_ms + 3000;
                        }
                    } else if (kc == KEY_SPEED && ke == KEV_DOWN) {
                        g_scroll_speed_level = !g_scroll_speed_level;
                    } else if (kc == KEY_DISP && ke == KEV_DOWN && !g_night_mode) {
                        g_disp_mode = (g_disp_mode + 1) % 3;
                        if (g_state == STATE_SCROLL) { g_state = STATE_CLOCK; }
                        Clock_FormatDisplay();
                    } else if (kc == KEY_FORMAT && ke == KEV_DOWN && !g_night_mode) {
                        g_format = (g_format == FMT_LEFT) ? FMT_RIGHT : FMT_LEFT;
                        Clock_FormatDisplay();
                    } else if (kc == KEY_EXT && ke == KEV_DOWN) {
                        uint8_t idx = ALARM_IDX; uint8_t was_on;
                        if (g_alarm_slot_enabled_mask & (1 << idx)) {
                            g_alarm_slot_enabled_mask &= ~(1 << idx); was_on = 1;
                        } else { g_alarm_slot_enabled_mask |= (1 << idx); was_on = 0; }
                        { char buf[32]; sprintf(buf, "*EVT:ALARM:SET %s %s\r\n", DOW_NAMES[idx + 1], was_on ? "OFF" : "ON"); UART_PutStrNB(buf); }
                        if (!(g_alarm_slot_enabled_mask & (1 << idx))) Alarm_Stop();
                    } else if (kc == KEY_USER1 && ke == KEV_LONG) {
                        /* Long-press USER1: display n.SY.xx
                         * 8-digit layout: [n][.][S][Y][.][x][x][ ]
                         * Dot at pos 1 & 4 as independent chars (FAQ Q12 style) */
                        {
                            char ntp_disp[9];
                            uint8_t hours;
                            const char *st;
                            if (g_ntp_synced == 0) {
                                hours = 0; st = "NO";
                            } else {
                                hours = (uint8_t)((g_tick_ms - g_ntp_last_sync_ms) / 3600000UL);
                                if (hours > 9) hours = 9;
                                st = (g_ntp_synced == 1) ? "OK" : "DR";
                            }
                            if (g_ntp_synced == 0)
                                sprintf(ntp_disp, "_.SY.%s ", st);
                            else
                                sprintf(ntp_disp, "%1u.SY.%s ", hours, st);
                            Display_SetStr(ntp_disp, 0x00);
                            g_state = STATE_WEATHER;
                            g_msg_end_ms = g_tick_ms + 3000;
                        }
                    }
                }

                if (ke == KEV_DOWN && !g_suppress_key_evt) {
                    sprintf(buf, "*EVT:KEY %s\r\n", keycode_to_name(kc));
                    UART_PutStrNB(buf);
                }
                g_suppress_key_evt = 0;
            }
        }
    }
}
