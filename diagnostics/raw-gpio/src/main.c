/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <string.h>

#define ID "MONA2-DIAG-v3"
#define C0 NRF_GPIO_PIN_MAP(1,15) /* XIAO D10 / P1.15 */
static const uint32_t col[]={C0,46,45,44,10};
static const uint32_t row[]={3,28,29,43};
static const uint32_t analog_pin[]={2,3,28,29,4,5};
static const struct device *const uart=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static atomic_t command;
static uint8_t selected;
static bool active,batch;
static uint8_t phase;
static int64_t deadline,next_report;
static uint32_t frames,ones[4][5],mask[4];
static const char *mode="IDLE";

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart));

static bool nfc_gpio(void){ return (NRF_UICR->NFCPINS & 1u)==0; }
static void release_all(void){ for(int i=0;i<5;i++) nrf_gpio_cfg_default(col[i]); }
static void rows_input(void){ for(int i=0;i<4;i++) nrf_gpio_cfg_input(row[i],NRF_GPIO_PIN_PULLDOWN); }
static void drive(uint32_t p,bool high){
    nrf_gpio_pin_write(p,high?1:0);
    nrf_gpio_cfg(p,NRF_GPIO_PIN_DIR_OUTPUT,NRF_GPIO_PIN_INPUT_CONNECT,NRF_GPIO_PIN_NOPULL,NRF_GPIO_PIN_S0S1,NRF_GPIO_PIN_NOSENSE);
}
static void configure(uint8_t c,bool others_low){
    release_all();
    if(others_low) for(int i=0;i<5;i++) if(i!=c) drive(col[i],false);
    drive(col[c],true);
}
static uint8_t read_rows(void){
    uint8_t b=0; for(int r=0;r<4;r++) if(nrf_gpio_pin_read(row[r])) b|=1u<<r; return b;
}
static bool output_ok(uint8_t c,bool low){
    bool bad=!nrf_gpio_pin_read(col[c]);
    if(low) for(int i=0;i<5;i++) if(i!=c) bad|=nrf_gpio_pin_read(col[i]);
    return !bad;
}
static void reset_stats(void){ frames=0; for(int r=0;r<4;r++){mask[r]=0;for(int c=0;c<5;c++)ones[r][c]=0;} }
static void collect(uint8_t c){ uint8_t b=read_rows(); for(int r=0;r<4;r++) if(b&(1u<<r)){mask[r]|=1u<<c;ones[r][c]++;} }
static void summary(const char *tag){
    printk("%s MODE=%s SEL=C%u frames=%u mask=R0:%02x R1:%02x R2:%02x R3:%02x\n",tag,mode,selected,frames,mask[0],mask[1],mask[2],mask[3]);
    for(int r=0;r<4;r++) printk("COUNTS R%d C0=%u C1=%u C2=%u C3=%u C4=%u\n",r,ones[r][0],ones[r][1],ones[r][2],ones[r][3],ones[r][4]);
}
static int wait_us_for_mode(void){ if(strcmp(mode,"SCAN100")==0) return 100; if(strcmp(mode,"SCAN1000")==0) return 1000; return 0; }
static bool scan_frame(bool low){
    int us=wait_us_for_mode();
    if(strstr(mode,"FIX")){ if(!output_ok(selected,low)) return false; collect(selected); }
    else for(int c=0;c<5;c++){ drive(col[c],true); if(us) k_busy_wait(us); if(!output_ok(c,low)) return false; collect(c); drive(col[c],false); }
    frames++; return true;
}
static void stop(const char *why){ release_all(); if(active) summary("END"); active=false; batch=false; mode="IDLE"; printk("STOP %s; all columns high impedance\n",why); }

static void gpio_info(void){
    printk("GPIO NFC_GPIO=%u P0_IN=%08x P0_OUT=%08x P1_IN=%08x P1_OUT=%08x\n",nfc_gpio(),(unsigned)NRF_P0->IN,(unsigned)NRF_P0->OUT,(unsigned)NRF_P1->IN,(unsigned)NRF_P1->OUT);
    for(int i=0;i<5;i++) printk("COL C%d pin=%u CNF=%08x\n",i,(unsigned)col[i],(unsigned)(col[i]<32?NRF_P0->PIN_CNF[col[i]]:NRF_P1->PIN_CNF[col[i]-32]));
    for(int i=0;i<4;i++) printk("ROW R%d pin=%u CNF=%08x\n",i,(unsigned)row[i],(unsigned)(row[i]<32?NRF_P0->PIN_CNF[row[i]]:NRF_P1->PIN_CNF[row[i]-32]));
}
static void adc_report(void){ printk("ADC: not sampled in this GPIO-safe build; use the GPIO 0/1 logs and an external DMM for voltage.\n"); }
static void menu(void){ printk("\n%s READY (right board only)\n? menu i GPIO a ADC 0-4 select s HIZ l LOW f scan0 m scan100 t scan1000 z scanHIZ b batch x stop\nRows: R0=D1/P0.03 R1=H=D2/P0.28 R2=N=D3/P0.29 R3=D6/P1.11; columns C0=D10/P1.15 C1=D9 C2=D8 C3=D7 C4=NFC2/P0.10\n",ID); }
static void begin(const char *m,int sec,bool low){ if(!nfc_gpio() && (low||m[0]!='F')){printk("REFUSED NFC pin is not GPIO; no UICR writes\n");return;} release_all();rows_input();reset_stats();mode=m;configure(selected,low);active=true;deadline=k_uptime_get()+sec*1000;next_report=k_uptime_get()+1000;printk("BEGIN MODE=%s SEL=C%u duration=%us; hold N for selected tests\n",mode,selected,sec);}
static void rx(const struct device *d,void *u){ uint8_t c;ARG_UNUSED(u);if(!uart_irq_update(d))return;while(uart_irq_rx_ready(d)&&uart_fifo_read(d,&c,1)==1){if(c=='?'||c=='x'||c=='X')atomic_set(&command,c=='?'?'?':'x');else if(c>='0'&&c<='4')atomic_set(&command,c);else if(c=='b'||c=='B'||c=='s'||c=='S'||c=='l'||c=='L'||c=='f'||c=='F'||c=='m'||c=='M'||c=='t'||c=='T'||c=='z'||c=='Z'||c=='i'||c=='I'||c=='a'||c=='A'||c=='p'||c=='P')atomic_cas(&command,0,c|0x20);}}

int main(void){ release_all();rows_input();for(int i=0;i<6;i++)nrf_gpio_cfg_default(analog_pin[i]);if(!device_is_ready(uart)||usb_enable(NULL)!=0)return 0;uart_irq_callback_user_data_set(uart,rx,NULL);uart_irq_rx_enable(uart);bool connected=false;while(1){uint32_t dtr=0;uart_line_ctrl_get(uart,UART_LINE_CTRL_DTR,&dtr);if(!dtr){release_all();active=false;connected=false;atomic_set(&command,0);k_msleep(50);continue;}if(!connected){menu();gpio_info();adc_report();connected=true;}atomic_val_t c=atomic_set(&command,0);int64_t now=k_uptime_get();if(c=='x')stop("command");else if(c=='?')menu();else if(c=='i')gpio_info();else if(c=='a'&&!active)adc_report();else if(c>='0'&&c<='4'&&!active){selected=c-'0';printk("SELECT C%u\n",selected);}else if(c=='b'&&!active){selected=0;batch=true;phase=0;begin("FIX_HIZ",5,false);}else if(!active&&c){if(c=='p'){printk("PMW3610 probe is intentionally omitted: no SPI writes; use normal firmware for sensor diagnostics.\n");}else if(c=='s')begin("FIX_HIZ",60,false);else if(c=='l')begin("FIX_LOW",15,true);else if(c=='f')begin("SCAN0",60,true);else if(c=='m')begin("SCAN100",60,true);else if(c=='t')begin("SCAN1000",60,true);else if(c=='z')begin("SCANHIZ",60,false);}if(active){if(now>=deadline){summary("PHASE_END");if(batch&&phase<5){phase++;const char *pm[]={"FIX_LOW","SCAN0","SCAN100","SCAN1000","SCANHIZ"};begin(pm[phase-1],5,phase==1||phase>=2);}else stop("timeout");}else if(!scan_frame(strstr(mode,"HIZ")==NULL)){stop("output readback fault");}else if(now>=next_report){summary("REPORT");next_report=now+1000;}}k_msleep(5);}}
