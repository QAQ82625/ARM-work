/**
 * 智能时钟系统 — MCU 端主程序
 * 基于 TM4C1294NCPDT (S800 板)
 * ARM Compiler 5 (C89) 兼容
 */

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

/* Debug: marker set at each processing stage — inspect with Keil debugger if hung */
volatile uint8_t  g_dbg;
volatile uint8_t  g_dbg2;
volatile uint16_t g_dbg_len;

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

/* 远程蜂鸣（非阻塞） */
static uint8_t  remote_beep_active = 0;
static uint32_t remote_beep_end_ms = 0;

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

static uint8_t match_abbrev(const char *input, const char *full)
{
    while (*input && *full) {
        char a = *input, b = *full;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
        input++; full++;
    }
    if (*input) return 0;  /* input longer than full */
    /* remaining full chars: uppercase = mandatory, lowercase = optional */
    while (*full) {
        if (*full >= 'A' && *full <= 'Z') return 0;
        full++;
    }
    return 1;
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
 * Beeper Timer (PN1, 2kHz via Timer0A ISR)
 * ================================================================ */
void Beeper_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER0)) {}
    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER0_BASE, TIMER_A, ui32SysClock / 4000);
    IntEnable(INT_TIMER0A_TM4C123);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    TimerEnable(TIMER0_BASE, TIMER_A);
}

void TIMER0A_Handler(void)
{
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    if (g_alarm_beep_active || remote_beep_active) {
        static uint8_t phase = 0;
        phase = !phase;
        if (phase)
            GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, GPIO_PIN_1);
        else
            GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, 0);
    }
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
    int i, n = (int)strlen(msg);
    for (i = 0; i < n; i++) {
        UARTCharPut(UART0_BASE, msg[i]);
    }
}

void UART_PutStrNB(const char *msg)
{
    int i, n = (int)strlen(msg);
    g_dbg = 0xF0;
    for (i = 0; i < n; i++) {
        /* 使用阻塞版本：等待 FIFO 有空间再写入，避免字符丢弃 */
        UARTCharPut(UART0_BASE, msg[i]);
    }
    g_dbg = 0xF1;
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
            /* Normalize spaces: one space between tokens */
            if (idx > 0 && cmd_line[idx-1] != ' ')
                cmd_line[idx++] = ' ';
            continue;
        }

        if (!is_msg && ch >= 'a' && ch <= 'z') {
            ch -= 32;
        }

        cmd_line[idx++] = ch;
    }

    cmd_line[idx] = '\0';
    g_dbg = (uint8_t)(0xE0 | (idx & 0x0F));
    g_dbg_len = idx;

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
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, 0);
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
#define MATCH_CMD(tok, full, minlen) \
    ((int)strlen(tok) >= (minlen) && strncmp(tok, full, (size_t)(minlen)) == 0)

static char *skip_to_next(char *s)
{
    s += strcspn(s, " ");
    s += strspn(s, " ");
    return s;
}

void ProcessCommand(char *cmd)
{
    char *p;

    g_dbg = 0x10;
    g_dbg_len = (uint16_t)strlen(cmd);
    /* Verify null terminator: if cmd_line[idx] != 0, stack was corrupted */
    g_dbg2 = (uint8_t)cmd[g_dbg_len];
    if (cmd[0] == '\0') return;

    /* *PING */
    if (strncmp(cmd, "*PING", 5) == 0) {
        char resp[32];
        sprintf(resp, "*PONG %lu\r\n", g_uptime_s);
        UART_PutStrNB(resp);
        return;
    }

    /* *RST [DATE|TIME|ALARM] */
    if (strncmp(cmd, "*RST", 4) == 0) {
        g_dbg = 0x11;
        p = cmd + 4;
        p += strspn(p, " ");
        if (*p == '\0') {
            g_dbg = 0x12;
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
            {
                uint8_t ai;
                for (ai = 0; ai < ALARM_SLOTS; ai++) {
                    g_alarm_slot[ai].h = 6;
                    g_alarm_slot[ai].mi = 0;
                    g_alarm_slot[ai].s = 0;
                }
            }
            g_alarm_slot_enabled_mask = 0x1F;
            g_dbg = 0x13;
        } else {
            g_dbg = 0x14;
            char  tok2[16];
            int   pass;
            int   word_len;

            for (pass = 0; pass < 3; pass++) {
                p += strspn(p, " ");
                if (*p == '\0') break;
                word_len = (int)strcspn(p, " ");
                if (word_len >= 15) word_len = 15;
                memcpy(tok2, p, (size_t)word_len);
                tok2[word_len] = '\0';
                p += word_len;
                if (match_abbrev(tok2, "DATE") || match_abbrev(tok2, "YEAR") || match_abbrev(tok2, "MONTH"))
                    { g_date.y = 2026; g_date.m = 6; g_date.d = 8; }
                else if (match_abbrev(tok2, "TIME"))
                    { g_time.h = 0; g_time.mi = 0; g_time.s = 0; }
                else if (match_abbrev(tok2, "ALARM")) {
                    uint8_t ai;
                    for (ai = 0; ai < ALARM_SLOTS; ai++) {
                        g_alarm_slot[ai].h = 6;
                        g_alarm_slot[ai].mi = 0;
                        g_alarm_slot[ai].s = 0;
                    }
                    g_alarm_slot_enabled_mask = 0x1F;
                }
            }
        }
        g_dbg = 0x15;
        Clock_FormatDisplay();
        g_dbg = 0x16;
        UART_PutStrNB("OK\r\n");
        g_dbg = 0x17;
        return;
    }

    /* *SET:... */
    if (strncmp(cmd, "*SET:", 5) == 0) {
        g_dbg = 0x20;
        p = cmd + 5;
        p += strspn(p, " ");  /* skip space between *SET: and sub-command */

/* *SET:DATE - 2-pass keyword-aware parser */
        if (MATCH_CMD(p, "DATE", 4)) {
            g_dbg = 0x30;
            char  *t;
            char   wbuf[8];
            int    vals[3];
            int    wlen;
            int    wi;
            int    kmap;
            int    yr_val;
            int    mo_val;
            int    dy_val;
            int    vi;
            uint8_t max_d;

            t = (char *)(p + 4);

            /* Pass 1: scan keywords only - build kmap */
            g_dbg = 0x31;
            kmap = 0;
            { int _p = 0; while (_p < 10) { _p++;
                t += strspn(t, " ");
                if (!*t) break;
                wlen = (int)strcspn(t, " ");
                if (wlen >= 7) wlen = 7;
                if (strspn(t, "0123456789") > 0) {
                    t += wlen;
                } else {
                    g_dbg = (uint8_t)(0xA0 | (wlen & 0x0F));
                    memcpy(wbuf, t, (size_t)wlen);
                    wbuf[wlen] = '\0';
                    t += wlen;
                    if (match_abbrev(wbuf, "YEAR"))       kmap |= 1;
                    else if (match_abbrev(wbuf, "MONTH"))  kmap |= 2;
                    else if (match_abbrev(wbuf, "DATE"))   kmap |= 4;
                    else { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }
                }
            }
            }

            /* Pass 2: extract all integers at once */
            t = (char *)(p + 4);
            g_dbg = 0x32;
            g_dbg_len = (uint16_t)(t - cmd);   /* DIAG: offset of t from cmd start */
            wi = 0;
            vals[0] = -1; vals[1] = -1; vals[2] = -1;
            while (wi < 3) {
                g_dbg = 0x50;
                t += strcspn(t, "0123456789");
                g_dbg = 0x51;
                if (!*t) break;
                vals[wi] = (int)strtol(t, &t, 10);
                g_dbg = 0x52;
                wi++;
            }

            if (wi == 0) { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }
            g_dbg = 0x33;

            /* Map vals by kmap order, then fill remaining */
            yr_val = -1; mo_val = -1; dy_val = -1;
            vi = 0;
            if (kmap & 1) yr_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) mo_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) dy_val = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) {
                if (yr_val < 0) yr_val = vals[vi];
                else if (mo_val < 0) mo_val = vals[vi];
                else if (dy_val < 0) dy_val = vals[vi];
                vi++;
            }

            if (yr_val < 0) yr_val = (int)g_date.y;
            if (yr_val < 2025 || yr_val > 2099) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            g_date.y = (uint16_t)yr_val;
            if (mo_val >= 0) {
                if (mo_val < 1 || mo_val > 12) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                g_date.m = (uint8_t)mo_val;
            }
            if (dy_val >= 0) {
                if (dy_val < 1 || dy_val > 31) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                g_date.d = (uint8_t)dy_val;
            }
            max_d = days_in_month[g_date.m - 1];
            if (g_date.m == 2 && is_leap_year(g_date.y)) max_d = 29;
            if (g_date.d > max_d) g_date.d = max_d;
            {
                char dbg_line[64];
                sprintf(dbg_line, "*DBG D off=%u dbg=%02X kmap=%d v=%d,%d,%d\r\n",
                        (unsigned int)g_dbg_len, (unsigned int)g_dbg,
                        kmap, vals[0], vals[1], vals[2]);
                UART_PutStrNB(dbg_line);
            }
            g_dbg = 0x34;
            UART_PutStrNB("OK\r\n");
            g_dbg = 0x35;
            return;
        }

        /* *SET:TIME - 2-pass keyword-aware parser */
        if (MATCH_CMD(p, "TIME", 4)) {
            g_dbg = 0x41;
            char  *t;
            char   wbuf[8];
            int    vals[3];
            int    wlen;
            int    wi;
            int    kmap;
            int    h_val;
            int    m_val;
            int    s_val;
            int    vi;

            t = (char *)(p + 4);
            t += strspn(t, " ");

            if ((t[0] == 'O' || t[0] == 'o') &&
                (t[1] == 'F' || t[1] == 'f') &&
                (t[2] == 'F' || t[2] == 'f')) {
                disp_on = 0;
                UART_PutStrNB("OK\r\n"); return;
            }

            /* Pass 1: scan keywords */
            g_dbg = 0x42;
            kmap = 0;
            { int _p = 0; while (_p < 10) { _p++;
                t += strspn(t, " ");
                if (!*t) break;
                wlen = (int)strcspn(t, " ");
                if (wlen >= 7) wlen = 7;
                if (strspn(t, "0123456789") > 0) {
                    t += wlen;
                } else {
                    g_dbg = (uint8_t)(0xA0 | (wlen & 0x0F));
                    memcpy(wbuf, t, (size_t)wlen);
                    wbuf[wlen] = '\0';
                    t += wlen;
                    if (match_abbrev(wbuf, "HOUR"))        kmap |= 1;
                    else if (match_abbrev(wbuf, "MINute"))  kmap |= 2;
                    else if (match_abbrev(wbuf, "SECond"))  kmap |= 4;
                    else { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }
                }
            }
            }

            /* Pass 2: extract integers */
            t = (char *)(p + 4);
            t += strspn(t, " ");
            g_dbg = 0x43;
            g_dbg_len = (uint16_t)(t - cmd);   /* DIAG: offset of t from cmd start */
            wi = 0;
            vals[0] = -1; vals[1] = -1; vals[2] = -1;
            while (wi < 3) {
                g_dbg = 0x60;
                t += strcspn(t, "0123456789");
                g_dbg = 0x61;
                if (!*t) break;
                vals[wi] = (int)strtol(t, &t, 10);
                g_dbg = 0x62;
                wi++;
            }

            /* Map by kmap */
            h_val = -1; m_val = -1; s_val = -1;
            vi = 0;
            if (kmap & 1) h_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) m_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) s_val = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) {
                if (h_val < 0) h_val = vals[vi];
                else if (m_val < 0) m_val = vals[vi];
                else if (s_val < 0) s_val = vals[vi];
                vi++;
            }

            if (h_val >= 0) {
                if (h_val > 23) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                g_time.h = (uint8_t)h_val;
            }
            if (m_val >= 0) {
                if (m_val > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                g_time.mi = (uint8_t)m_val;
                if (s_val < 0) g_time.s = 0;
            }
            if (s_val >= 0) {
                if (s_val > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                g_time.s = (uint8_t)s_val;
            }
            {
                char dbg_line[64];
                sprintf(dbg_line, "*DBG T off=%u dbg=%02X kmap=%d v=%d,%d,%d\r\n",
                        (unsigned int)g_dbg_len, (unsigned int)g_dbg,
                        kmap, vals[0], vals[1], vals[2]);
                UART_PutStrNB(dbg_line);
            }
            g_dbg = 0x44;
            UART_PutStrNB("OK\r\n");
            g_dbg = 0x45;
            return;
        }

        /* *SET:ALARM - 2-pass keyword-aware parser */
        if (MATCH_CMD(p, "ALARM", 5)) {
            char  *t;
            char   wbuf[8];
            int    vals[3];
            int    wlen;
            int    wi;
            int    kmap;
            int    h_val;
            int    m_val;
            int    s_val;
            int    vi;
            int    si;
            int    ss;
            int    se;

            t = (char *)(p + 5);
            t += strspn(t, " ");

            ss = (int)ALARM_IDX;
            se = ss + 1;

            /* Check ON/OFF */
            if ((t[0] == 'O' || t[0] == 'o')) {
                if ((t[1] == 'N' || t[1] == 'n')) {
                    for (si = ss; si < se; si++)
                        g_alarm_slot_enabled_mask |= (1U << si);
                    UART_PutStrNB("OK\r\n"); return;
                }
                if ((t[1] == 'F' || t[1] == 'f') &&
                    (t[2] == 'F' || t[2] == 'f')) {
                    for (si = ss; si < se; si++)
                        g_alarm_slot_enabled_mask &= ~(1U << si);
                    g_alarm_beep_active = 0;
                    UART_PutStrNB("OK\r\n"); return;
                }
            }

            /* Pass 1: scan keywords */
            kmap = 0;
            { int _p = 0; while (_p < 10) { _p++;
                t += strspn(t, " ");
                if (!*t) break;
                wlen = (int)strcspn(t, " ");
                if (wlen >= 7) wlen = 7;
                if (strspn(t, "0123456789") > 0) {
                    t += wlen;
                } else {
                    memcpy(wbuf, t, (size_t)wlen);
                    wbuf[wlen] = '\0';
                    t += wlen;
                    if (match_abbrev(wbuf, "HOUR"))        kmap |= 1;
                    else if (match_abbrev(wbuf, "MINute"))  kmap |= 2;
                    else if (match_abbrev(wbuf, "SECond"))  kmap |= 4;
                    else { UART_PutStrNB("ERROR SYNTAX\r\n"); return; }
                }
            }
            }

            /* Pass 2: extract integers */
            t = (char *)(p + 5);
            t += strspn(t, " ");
            wi = 0;
            vals[0] = -1; vals[1] = -1; vals[2] = -1;
            while (wi < 3) {
                t += strcspn(t, "0123456789");
                if (!*t) break;
                vals[wi] = (int)strtol(t, &t, 10);
                wi++;
            }

            /* Map by kmap */
            h_val = -1; m_val = -1; s_val = -1;
            vi = 0;
            if (kmap & 1) h_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 2) m_val = (vi < wi) ? vals[vi++] : -1;
            if (kmap & 4) s_val = (vi < wi) ? vals[vi++] : -1;
            while (vi < wi) {
                if (h_val < 0) h_val = vals[vi];
                else if (m_val < 0) m_val = vals[vi];
                else if (s_val < 0) s_val = vals[vi];
                vi++;
            }

            if (h_val >= 0) {
                if (h_val > 23) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) {
                    g_alarm_slot[si].h = (uint8_t)h_val;
                    g_alarm_slot_enabled_mask |= (1U << si);
                }
            }
            if (m_val >= 0) {
                if (m_val > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) {
                    g_alarm_slot[si].mi = (uint8_t)m_val;
                    g_alarm_slot_enabled_mask |= (1U << si);
                }
            }
            if (s_val >= 0) {
                if (s_val > 59) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
                for (si = ss; si < se; si++) {
                    g_alarm_slot[si].s = (uint8_t)s_val;
                    g_alarm_slot_enabled_mask |= (1U << si);
                }
            }
            UART_PutStrNB("OK\r\n");
            return;
        }
        /* *SET:DISPlay ON/OFF */
        if (MATCH_CMD(p, "DISPLAY", 4) || MATCH_CMD(p, "DISP", 4)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (strncmp(p, "ON", 2) == 0 || strncmp(p, "on", 2) == 0)
                { disp_on = 1; UART_PutStrNB("OK\r\n"); return; }
            if (strncmp(p, "OFF", 3) == 0 || strncmp(p, "off", 3) == 0)
                { disp_on = 0; UART_PutStrNB("OK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        /* *SET:FORMAT LEFT/RIGHT */
        if (MATCH_CMD(p, "FORMAT", 6)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (strncmp(p, "LEFT", 4) == 0)  { g_format = FMT_LEFT;  scroll_dir = 0; UART_PutStrNB("OK\r\n"); return; }
            if (strncmp(p, "RIGHT", 5) == 0) { g_format = FMT_RIGHT; scroll_dir = 1; UART_PutStrNB("OK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        if (MATCH_CMD(p, "MSG", 3)) {
            char   *msg_text;
            uint8_t len;

            /* cmd_line for MSG preserves original case (is_msg=1 in ExtractLine) */
            msg_text = p + 3;
            msg_text += strspn(msg_text, " ");
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
        if (MATCH_CMD(p, "BEEP", 4)) {
            uint16_t ms;
            p = skip_to_next(p);
            p += strspn(p, " ");
            ms = (uint16_t)atoi(p);
            if (ms < 10 || ms > 5000) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            remote_beep_active = 1;
            remote_beep_end_ms = g_tick_ms + ms;
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:LED <hex2> */
        if (MATCH_CMD(p, "LED", 3)) {
            char   *ep;
            uint8_t val;
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (*p == '\0') { UART_PutStrNB("ERROR PARAM\r\n"); return; }
            val = 0; ep = p;
            { int _hx = 0; while (_hx < 2) { _hx++;
                char ch = *ep;
                if (!((ch >= '0' && ch <= '9') ||
                      (ch >= 'A' && ch <= 'F') ||
                      (ch >= 'a' && ch <= 'f'))) break;
                val <<= 4;
                if      (ch >= '0' && ch <= '9') val |= (uint8_t)(ch - '0');
                else if (ch >= 'A' && ch <= 'F') val |= (uint8_t)(ch - 'A' + 10);
                else if (ch >= 'a' && ch <= 'f') val |= (uint8_t)(ch - 'a' + 10);
                ep++;
            }}
            if (ep == p) { UART_PutStrNB("ERROR PARAM\r\n"); return; }
            g_led_value = val;
            g_led_override = (g_led_value != 0);
            if (g_led_override) led_override_start_ms = g_tick_ms;
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:KEY <NAME> */
        if (MATCH_CMD(p, "KEY", 3)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            key_code_t kc = KEY_NONE;
            if (strncmp(p, "FUNC", 4) == 0 || strncmp(p, "func", 4) == 0)   kc = KEY_FUNC;
            else if (strncmp(p, "SHIFT", 5) == 0) kc = KEY_SHIFT;
            else if (strncmp(p, "ADD", 3) == 0)    kc = KEY_ADD;
            else if (strncmp(p, "SAVE", 4) == 0)   kc = KEY_SAVE;
            else if (strncmp(p, "DISP", 4) == 0)   kc = KEY_DISP;
            else if (strncmp(p, "SPEED", 5) == 0) kc = KEY_SPEED;
            else if (strncmp(p, "FORMAT", 6) == 0) kc = KEY_FORMAT;
            else if (strncmp(p, "EXT", 3) == 0)    kc = KEY_EXT;
            else if (strncmp(p, "USER1", 5) == 0) kc = KEY_USER1;
            else if (strncmp(p, "USER2", 5) == 0) kc = KEY_USER2;
            if (kc == KEY_NONE) { UART_PutStrNB("ERROR PARAM\r\n"); return; }
            g_suppress_key_evt = 1;
            KeyQueue_Push(kc, KEV_DOWN);
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:MODE DAY/NIGHT */
        if (MATCH_CMD(p, "MODE", 4)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (strncmp(p, "DAY", 3) == 0 || strncmp(p, "day", 3) == 0)
                { g_night_mode = 0; g_mode_day = 1;
                  UART_PutStrNB("*EVT:MODE DAY\r\nOK\r\n"); return; }
            if (strncmp(p, "NIGHT", 5) == 0 || strncmp(p, "night", 5) == 0)
                { g_night_mode = 1; g_mode_day = 0;
                  UART_PutStrNB("*EVT:MODE NIGHT\r\nOK\r\n"); return; }
            UART_PutStrNB("ERROR PARAM\r\n"); return;
        }

        /* *SET:WEA <temp> <COND> */
        if (MATCH_CMD(p, "WEA", 3)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            int t = atoi(p);
            if (t < -40 || t > 50) { UART_PutStrNB("ERROR RANGE\r\n"); return; }
            g_weather_temp = (int8_t)t;
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (match_abbrev(p, "SUN"))      strcpy(g_weather_cond, "SUN");
            else if (match_abbrev(p, "CLD")) strcpy(g_weather_cond, "CLD");
            else if (match_abbrev(p, "OVC")) strcpy(g_weather_cond, "OVC");
            else if (match_abbrev(p, "RAI")) strcpy(g_weather_cond, "RAI");
            else if (match_abbrev(p, "SNO")) strcpy(g_weather_cond, "SNO");
            else if (match_abbrev(p, "FOG")) strcpy(g_weather_cond, "FOG");
            else { strcpy(g_weather_cond, "UNK"); }
            g_weather_valid = 1;
            g_weather_age = 0;
            UART_PutStrNB("OK\r\n");
            return;
        }

        /* *SET:GAME START | SCORE nnn | OVER nnn | QUIT */
        if (MATCH_CMD(p, "GAME", 4)) {
            p = skip_to_next(p);
            p += strspn(p, " ");
            if (strncmp(p, "START", 5) == 0) {
                g_game_active = 1;
                g_state = STATE_GAME;
                Display_SetStr("--------", 0x00);
                UART_PutStrNB("OK\r\n"); return;
            }
            if (strncmp(p, "SCORE", 5) == 0) {
                p = skip_to_next(p); p += strspn(p, " ");
                g_game_score = (uint16_t)atoi(p);
                { char buf[9]; sprintf(buf, "Sc %03d  ", g_game_score); Display_SetStr(buf, 0x00); }
                UART_PutStrNB("OK\r\n"); return;
            }
            if (strncmp(p, "OVER", 4) == 0) {
                int i; p = skip_to_next(p); p += strspn(p, " ");
                g_game_score = (uint16_t)atoi(p);
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
            if (strncmp(p, "QUIT", 4) == 0) {
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
        p += strspn(p, " ");
        if (*p == '\0' || match_abbrev(p, "TIME")) {
            char resp[48];
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
        if (match_abbrev(p, "DATE")) {
            char resp[48];
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
        if (match_abbrev(p, "FORMAT")) {
            UART_PutStrNB(g_format == FMT_LEFT ? "OK LEFT\r\n" : "OK RIGHT\r\n");
            return;
        }
        if (match_abbrev(p, "ALARM")) {
            char resp[160]; int off = 0;
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
        if (match_abbrev(p, "DISP")) {
            UART_PutStrNB(disp_on ? "OK ON\r\n" : "OK OFF\r\n");
            return;
        }
        if (match_abbrev(p, "MODE")) {
            UART_PutStrNB(g_night_mode ? "OK NIGHT\r\n" : "OK DAY\r\n");
            return;
        }
        if (match_abbrev(p, "SPEED")) {
            UART_PutStrNB(g_scroll_speed_level ? "OK FAST\r\n" : "OK SLOW\r\n");
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
    #undef MATCH_CMD
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

    /* Beeper Timer (PN1, 2kHz) — init AFTER IntMasterEnable */
    Beeper_Init();

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

            /* 远程蜂鸣 — 10ms 粒度实现 ±20ms 精度 */
            if (remote_beep_active && g_tick_ms >= remote_beep_end_ms) {
                remote_beep_active = 0;
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

            /* 闹钟响铃 — Timer0A ISR toggles PN1 at 2kHz */
            if (g_alarm_beep_active) {
                uint32_t total_ms = 10000 + (uint32_t)g_alarm_weather_beeps * 400;
                if (Tick_TimedOut(beep_start_ms, total_ms)) {
                    Alarm_Stop();
                    g_alarm_weather_beeps = 0;
                    g_alarm_weather_led = 0;
                }
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
            g_dbg = 0x01;
            rx_line_ready = 0;
            ExtractLine();
            g_dbg = 0x02;
            ProcessCommand(cmd_line);
            g_dbg = 0x03;
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
