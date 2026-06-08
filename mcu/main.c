#include <stdint.h>
#include <stdbool.h>
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
#include "string.h"
#include <stdio.h>
#include <ctype.h>

/************************** ??__NOP????? **************************/
#ifndef __NOP
#define __NOP() __asm volatile ("nop")
#endif

#define SYSTICK_FREQUENCY		1000			//1000hz
#define	I2C_FLASHTIME				500				//500mS
#define GPIO_FLASHTIME			300				//300mS

// I2C??????
#define TCA6424_I2CADDR 					0x22
#define PCA9557_I2CADDR						0x18

#define PCA9557_INPUT							0x00
#define	PCA9557_OUTPUT						0x01
#define PCA9557_POLINVERT					0x02
#define PCA9557_CONFIG						0x03

#define TCA6424_CONFIG_PORT0			0x0c
#define TCA6424_CONFIG_PORT1			0x0d
#define TCA6424_CONFIG_PORT2			0x0e

#define TCA6424_INPUT_PORT0				0x00
#define TCA6424_INPUT_PORT1				0x01
#define TCA6424_INPUT_PORT2				0x02

#define TCA6424_OUTPUT_PORT0			0x04
#define TCA6424_OUTPUT_PORT1			0x05
#define TCA6424_OUTPUT_PORT2			0x06

/************************** ??????(????????) **************************/
#define DP_SEG              0x80    // ?????????0x40
#define SCAN_REVERSE        0       // ????????1
#define SEPARATOR_STYLE     0       // ?????????1
#define BRIGHTNESS          1       // ???????2-5

/************************** ??????(?????) **************************/
// ????(??: 10ms)
#define BOOT_FULL_BRIGHT		100		// ????: 100×10ms = 1?
#define BOOT_FULL_OFF			100		// ????: 1?
#define BOOT_STUDENT_ID		300		// ????: 3?
#define BOOT_NAME				300		// ????: 3?
#define BOOT_VERSION			200		// ?????: 2?

// ????(????????????)
#define STUDENT_ID			"20260001"	// ???8?
#define STUDENT_NAME		"HUZHENYE"	// ????(??8?)
#define SOFTWARE_VERSION	"V1.0    "	// ?????

/************************** ??????? **************************/
// ??????
#define DISP_MODE_TIME		0	// HH.MM.SS
#define DISP_MODE_DATE_SHORT	1	// YY.MM.DD
#define DISP_MODE_DATE_LONG	2	// YYYY.MMDD

/************************** ?????? **************************/
void 		Delay(uint32_t value);
void        Delay_us(uint32_t us);
void 		S800_GPIO_Init(void);
uint8_t 	I2C0_WriteByte(uint8_t DevAddr, uint8_t RegAddr, uint8_t WriteData);
uint8_t 	I2C0_ReadByte(uint8_t DevAddr, uint8_t RegAddr);
void		S800_I2C0_Init(void);
void 		S800_UART_Init(void);
void        Boot_Sequence(void);        // ????
uint8_t     CharToSeg7(char c);         // ??????
void        ShowString(const char *str, uint32_t duration, uint8_t led_state); // ?????

// ???????
uint8_t     is_leap_year(uint16_t year);
uint8_t     get_days_in_month(uint16_t year, uint8_t month);
void        update_time(void);
void        update_display(void);
void        key_scan(void);
void        process_uart_command(void);

/************************** ???? **************************/
// ?????????(0x079 ? 0x79)
uint8_t seg7[] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f,
                  0x77,0x7c,0x58,0x5e,0x79,0x71,0x5c,0x00,0x40};

// ??????
volatile uint16_t systick_10ms_couter,systick_100ms_couter;
volatile uint8_t	systick_10ms_status,systick_100ms_status;
volatile uint8_t result,cnt,key_value,gpio_status;
volatile uint8_t rightshift = 0x01;
uint32_t ui32SysClock,ui32IntPriorityMask;
uint8_t uart_receive_char;
int i=0;
char receive[50];
volatile uint8_t uart_cmd_ready = 0;
const char *ATCLASS="AT+CLASS#";
const char *ATCODE="AT+STUDENTCODE#";
const char *CLASS="CLASSF17XXXXX";
const char *CODE="CODE517XXXXXXX";

/************************** ????????? **************************/
// ????
volatile uint8_t hour = 12, minute = 34, second = 56;
volatile uint16_t year = 2026;
volatile uint8_t month = 6, day = 8;

// ????
uint8_t disp_mode = DISP_MODE_TIME;    // ??????
uint8_t disp_on = 1;                   // ????
static uint8_t sec_counter = 0;        // 100ms???

// ????
static uint8_t key_prev_state = 0xFF;
static uint32_t key_press_time = 0;
static uint32_t sys_tick = 0;          // ???????

// ???????
uint8_t disp_buf[8] = {0};
uint8_t dp_buf[8] = {0};
volatile uint8_t scan_cnt = 0;         // ?????

/************************** ???????(????) **************************/
void Delay_us(uint32_t us) {
    uint32_t loops = us * (ui32SysClock / 1000000) / 3;
    while (loops--) {
        __NOP(); __NOP(); __NOP();
    }
}

/************************** ???????? **************************/
void ShowString(const char *str, uint32_t duration, uint8_t led_state) {
    int j, k;
    uint8_t seg_data[8];
    
    // ??????????
    for (j = 0; j < 8; j++) {
        if (str[j] == '\0') break;
        seg_data[j] = CharToSeg7(str[j]);
    }
    for (; j < 8; j++) {
        seg_data[j] = 0x00; // ??????
    }
    
    // ??????
    for (k = 0; k < duration; k++) {
        for (j = 0; j < 8; j++) {
            // ???????(??????)
            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
            Delay_us(10); // ??TCA6424????
            
            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, seg_data[j]);
            Delay_us(10); // ??????
            
            uint8_t bit_pos = SCAN_REVERSE ? (7 - j) : j;
            result = I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 1 << bit_pos);
            
            // ??LED??
            result = I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, ~led_state);
            
            Delay_us(100 * BRIGHTNESS); // ????
        }
    }
}

void Boot_Sequence(void) {
    // ??SysTick??,??????
    SysTickIntDisable();
    
    // 1. ??????1?
    ShowString("88888888", BOOT_FULL_BRIGHT, 0xFF); // ???+LED??
    ShowString("        ", BOOT_FULL_OFF, 0x00); // ???+LED??
    
    // 2. ????(??1?)
    ShowString(STUDENT_ID, BOOT_STUDENT_ID/2, 0xFF);
    ShowString("        ", 50, 0x00); // ??500ms
    ShowString(STUDENT_ID, BOOT_STUDENT_ID/2, 0xFF);
    
    // 3. ????
    ShowString("        ", BOOT_FULL_OFF, 0x00);
    
    // 4. ????(??1?)
    ShowString(STUDENT_NAME, BOOT_NAME/2, 0xFF);
    ShowString("        ", 50, 0x00); // ??500ms
    ShowString(STUDENT_NAME, BOOT_NAME/2, 0xFF);
    
    // 5. ????
    ShowString("        ", BOOT_FULL_OFF, 0x00);
    
    // 6. ?????
    ShowString(SOFTWARE_VERSION, BOOT_VERSION, 0x00); // LED??
    
    // 7. ????
    ShowString("        ", 50, 0x00);
    
    // ??????
    memset(disp_buf, 0, sizeof(disp_buf));
    
    // ????SysTick??
    SysTickIntEnable();
}

/************************** ???????? **************************/
uint8_t CharToSeg7(char c) {
    if (c >= '0' && c <= '9') {
        return seg7[c - '0'];
    } else if (c >= 'A' && c <= 'Z') {
        switch (c) {
            case 'A': return 0x77;
            case 'B': return 0x7C;
            case 'C': return 0x39;
            case 'D': return 0x5E;
            case 'E': return 0x79;
            case 'F': return 0x71;
            case 'G': return 0x3D;
            case 'H': return 0x76;
            case 'I': return 0x06;
            case 'J': return 0x0E;
            case 'K': return 0x76;
            case 'L': return 0x38;
            case 'M': return 0x55;
            case 'N': return 0x54;
            case 'O': return 0x5C;
            case 'P': return 0x73;
            case 'Q': return 0x67;
            case 'R': return 0x50;
            case 'S': return 0x6D;
            case 'T': return 0x78;
            case 'U': return 0x3E;
            case 'V': return 0x1C;
            case 'W': return 0x7E;
            case 'X': return 0x76;
            case 'Y': return 0x6E;
            case 'Z': return 0x5B;
            default: return 0x00;
        }
    } else if (c >= 'a' && c <= 'z') {
        return CharToSeg7(c - 'a' + 'A'); // ?????
    } else if (c == '.') {
        return DP_SEG; // ???
    } else if (c == '-') {
        return 0x40; // ??
    } else {
        return 0x00; // ????????
    }
}

/************************** ????????? **************************/
// ????
uint8_t is_leap_year(uint16_t year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

// ??????
uint8_t get_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month-1];
}

// ??????
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

// ??????
void update_display(void) {
    char str[9] = {0};
    int j;
    
    if (!disp_on) {
        for (j = 0; j < 8; j++) {
            disp_buf[j] = 0x00;
            dp_buf[j] = 0;
        }
        return;
    }
    
    memset(dp_buf, 0, sizeof(dp_buf));
    
    switch (disp_mode) {
        case DISP_MODE_TIME:
            if (SEPARATOR_STYLE == 0) {
                // ??????:HH.MM.SS
                sprintf(str, "%02d%02d%02d  ", hour, minute, second);
                dp_buf[1] = 1; // ?2?????
                dp_buf[3] = 1; // ?4?????
            } else {
                // ?????:HH-MM-SS
                sprintf(str, "%02d-%02d-%02d", hour, minute, second);
            }
            break;
            
        case DISP_MODE_DATE_SHORT:
            if (SEPARATOR_STYLE == 0) {
                // ??????:YY.MM.DD
                sprintf(str, "%02d%02d%02d  ", year % 100, month, day);
                dp_buf[1] = 1;
                dp_buf[3] = 1;
            } else {
                // ?????:YY-MM-DD
                sprintf(str, "%02d-%02d-%02d", year % 100, month, day);
            }
            break;
            
        case DISP_MODE_DATE_LONG:
            if (SEPARATOR_STYLE == 0) {
                // ??????:YYYY.MMDD
                sprintf(str, "%04d%02d%02d", year, month, day);
                dp_buf[3] = 1; // ?4?????
            } else {
                // ?????:YYYY-MMDD
                sprintf(str, "%04d-%02d%02d", year, month, day);
            }
            break;
    }
    
    // ????????????
    for (j = 0; j < 8; j++) {
        disp_buf[j] = CharToSeg7(str[j]);
        if (dp_buf[j] && SEPARATOR_STYLE == 0) {
            disp_buf[j] |= DP_SEG;
        }
    }
}

// ????(USER1=PJ0??????)
void key_scan(void) {
    uint8_t key_state = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0);
    uint32_t now = sys_tick;
    
    // ????(?????)
    if (!key_state && key_prev_state) {
        key_press_time = now;
    }
    // ????
    else if (key_state && !key_prev_state) {
        if (now - key_press_time > 20 && now - key_press_time < 1000) {
            // ????????
            disp_mode++;
            if (disp_mode > DISP_MODE_DATE_LONG) {
                disp_mode = DISP_MODE_TIME;
            }
            update_display();
        }
    }
    
    key_prev_state = key_state;
}

// ??????
void process_uart_command(void) {
    char cmd[64];
    strncpy(cmd, receive, 64);
    cmd[63] = '\0';
    
    // ??????????
    char *p = cmd;
    while (*p && isspace((unsigned char)*p)) p++;
    int len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len-1])) {
        p[len-1] = '\0';
        len--;
    }
    for (int j = 0; p[j]; j++) {
        p[j] = toupper((unsigned char)p[j]);
    }
    
    if (strcmp(p, "AT+CLASS#") == 0) {
        UARTStringPut((uint8_t *)CLASS);
        UARTStringPut((uint8_t *)"\r\n");
    }
    else if (strcmp(p, "AT+STUDENTCODE#") == 0) {
        UARTStringPut((uint8_t *)CODE);
        UARTStringPut((uint8_t *)"\r\n");
    }
    else if (strncmp(p, "*SET:DISP", 9) == 0) {
        char *param = p + 9;
        while (*param && isspace((unsigned char)*param)) param++;
        
        if (strcmp(param, "ON") == 0) {
            disp_on = 1;
            UARTStringPut((uint8_t *)"OK\r\n");
        }
        else if (strcmp(param, "OFF") == 0) {
            disp_on = 0;
            UARTStringPut((uint8_t *)"OK\r\n");
        }
        else {
            UARTStringPut((uint8_t *)"ERROR PARAM\r\n");
        }
        update_display();
    }
    
    memset(receive, 0, sizeof(receive));
    uart_cmd_ready = 0;
}

/************************** ??? **************************/
int main(void)
{
	volatile uint16_t	gpio_flash_cnt;
	
	// ??????(????????)
	ui32SysClock = SysCtlClockFreqSet((SYSCTL_XTAL_16MHZ |SYSCTL_OSC_INT | SYSCTL_USE_PLL |SYSCTL_CFG_VCO_480), 20000000);
	
    SysTickPeriodSet(ui32SysClock/SYSTICK_FREQUENCY);
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
	IntPrioritySet(INT_UART0,0x00);
	IntPrioritySet(FAULT_SYSTICK,0xe0);
	
	/************************** ?????? **************************/
	Boot_Sequence();
	
	// ?????
    update_display();
	
	// ???(???)
	while (1)
	{
		if (systick_10ms_status)
		{
			systick_10ms_status		= 0;
            sys_tick += 10;
            
			if (++gpio_flash_cnt	>= GPIO_FLASHTIME/10)
			{
				gpio_flash_cnt			= 0;
				if (gpio_status)
					GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0,GPIO_PIN_0 );
				else
					GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0,0);
				gpio_status					= !gpio_status;
			}
            
            // ????(10ms??)
            key_scan();
		}
		if (systick_100ms_status)
		{
			systick_100ms_status	= 0;
            
			if (++sec_counter >= 10)   // 10 * 100ms = 1?
			{
				sec_counter = 0;
				update_time();        // ????
				update_display();     // ????
			}
		}
        
        // ??????
        if (uart_cmd_ready)
        {
            process_uart_command();
        }
	}
}

/************************** ??????????? **************************/
void Delay(uint32_t value)
{
	uint32_t ui32Loop;
	for(ui32Loop = 0; ui32Loop < value; ui32Loop++){};
}

void UARTStringPut(uint8_t *cMessage)
{
	while(*cMessage!='\0')
		UARTCharPut(UART0_BASE,*(cMessage++));
}

void UARTStringPutNonBlocking(const char *cMessage)
{
	while(*cMessage!='\0')
		UARTCharPutNonBlocking(UART0_BASE,*(cMessage++));
}

void UARTStringGetNonBlocking(char *cMessage,int32_t uart0_int_status)
{
	if(uart0_int_status==0x10)
	{
		while(UARTCharsAvail(UART0_BASE))    											
		{
			cMessage[i]=UARTCharGetNonBlocking(UART0_BASE); 									
			i++;  
		}
		if(cMessage[i-1]=='#'){
			cMessage[i]='\0';
			i=0;
			uart_cmd_ready = 1;
		}
	}
	if(uart0_int_status==0x40)
	{
		while(UARTCharsAvail(UART0_BASE))                  
		{
			cMessage[i]=UARTCharGetNonBlocking(UART0_BASE); 									
			i++;
		}
	    cMessage[i]='\0';
		i=0;
		uart_cmd_ready = 1;
	}
}

void S800_UART_Init(void)
{
	SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
	while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA));

	GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);    			

    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, ui32SysClock,115200,(UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |UART_CONFIG_PAR_NONE));
	UARTFIFOLevelSet(UART0_BASE,UART_FIFO_TX6_8,UART_FIFO_RX6_8);

	UARTStringPut((uint8_t *)"\r\nClock System Ready\r\n");
}

void S800_GPIO_Init(void)
{
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF));
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
	while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOJ));
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
	while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPION));		
	
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_0);
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0);
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_1);

	GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE,GPIO_PIN_0 | GPIO_PIN_1);
	GPIOPadConfigSet(GPIO_PORTJ_BASE,GPIO_PIN_0 | GPIO_PIN_1,GPIO_STRENGTH_2MA,GPIO_PIN_TYPE_STD_WPU);
}

void S800_I2C0_Init(void)
{
	uint8_t result;
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
	GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

	I2CMasterInitExpClk(I2C0_BASE,ui32SysClock, true);
	I2CMasterEnable(I2C0_BASE);	

	result = I2C0_WriteByte(TCA6424_I2CADDR,TCA6424_CONFIG_PORT0,0x0ff);
	result = I2C0_WriteByte(TCA6424_I2CADDR,TCA6424_CONFIG_PORT1,0x0);
	result = I2C0_WriteByte(TCA6424_I2CADDR,TCA6424_CONFIG_PORT2,0x0);

	result = I2C0_WriteByte(PCA9557_I2CADDR,PCA9557_CONFIG,0x00);
	result = I2C0_WriteByte(PCA9557_I2CADDR,PCA9557_OUTPUT,0x0ff);
}

uint8_t I2C0_WriteByte(uint8_t DevAddr, uint8_t RegAddr, uint8_t WriteData)
{
	uint8_t rop;
	while(I2CMasterBusy(I2C0_BASE)){};
	I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, false);
	I2CMasterDataPut(I2C0_BASE, RegAddr);
	I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
	while(I2CMasterBusy(I2C0_BASE)){};
	rop = (uint8_t)I2CMasterErr(I2C0_BASE);

	I2CMasterDataPut(I2C0_BASE, WriteData);
	I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
	while(I2CMasterBusy(I2C0_BASE)){};

	rop = (uint8_t)I2CMasterErr(I2C0_BASE);
	return rop;
}

uint8_t I2C0_ReadByte(uint8_t DevAddr, uint8_t RegAddr)
{
	uint8_t value,rop;
	while(I2CMasterBusy(I2C0_BASE)){};	
	I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, false);
	I2CMasterDataPut(I2C0_BASE, RegAddr);
	I2CMasterControl(I2C0_BASE,I2C_MASTER_CMD_SINGLE_SEND);
	while(I2CMasterBusBusy(I2C0_BASE));
	rop = (uint8_t)I2CMasterErr(I2C0_BASE);
	Delay_us(10);
	I2CMasterSlaveAddrSet(I2C0_BASE, DevAddr, true);
	I2CMasterControl(I2C0_BASE,I2C_MASTER_CMD_SINGLE_RECEIVE);
	while(I2CMasterBusBusy(I2C0_BASE));
	value=I2CMasterDataGet(I2C0_BASE);
	Delay_us(10);
	return value;
}

/************************** ????SysTick??(125Hz??) **************************/
void SysTick_Handler(void)
{
    // ==================== ???????(1ms??,??????) ====================
    // ???????
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
    Delay_us(5);
    
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, disp_buf[scan_cnt]);
    Delay_us(5);
    
    uint8_t bit_pos = SCAN_REVERSE ? (7 - scan_cnt) : scan_cnt;
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 1 << bit_pos);
    
    scan_cnt++;
    if (scan_cnt >= 8) scan_cnt = 0;

    // ==================== ??????? ====================
    if (systick_100ms_couter	!= 0)
        systick_100ms_couter--;
    else
    {
        systick_100ms_couter	= SYSTICK_FREQUENCY/10;
        systick_100ms_status 	= 1;
    }
    
    if (systick_10ms_couter	!= 0)
        systick_10ms_couter--;
    else
    {
        systick_10ms_couter		= SYSTICK_FREQUENCY/100;
        systick_10ms_status 	= 1;
    }
    if (GPIOPinRead(GPIO_PORTJ_BASE,GPIO_PIN_0) == 0)
    {
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0,GPIO_PIN_0);		
    }
    else
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0,0);		
}

void UART0_Handler(void)
{
	int32_t uart0_int_status;
	uart0_int_status 		= UARTIntStatus(UART0_BASE, true);

	UARTIntClear(UART0_BASE, uart0_int_status);
	UARTStringGetNonBlocking(receive,uart0_int_status);
}