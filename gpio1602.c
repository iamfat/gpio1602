/* 
2011-04-18. version 0.1

TTY DRIVER TO CONNECT LCM 1602 VIA BTPLUG GPIO

本源码以MIT-LICENSE授权发布

Copyright (c) 2011 Jia Huang, jia.huang@geneegroup.com

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#define DRV_NAME "gpio1602"
#define DEV_NAME "ttyG"

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/io.h>

#define BYTE unsigned char
#define UINT unsigned int
#define BOOL unsigned char

#define TRUE 1
#define FALSE 0

#define FLAG_RS 0x01
#define FLAG_RW 0x02
#define FLAG_E  0x04

#define DATA_BUSY 0x80

#define DATA_CTL	0x98
#define DATA		0x78

#define FLAG_CTL	0x99
#define FLAG		0x79

#define CMD_SET_OPT				0x20
#define	CMD_OPT_8BITS			0x10
#define CMD_OPT_2LINES			0x08
#define CMD_OPT_F5x10			0x04

#define CMD_SET_DISP			0x08
#define CMD_DISP_ON				0x04
#define CMD_DISP_CURSOR_ON		0x02
#define CMD_DISP_CURSOR_BLINK	0x01

#define CMD_CLEAR		 		0x01
#define CMD_RESET_CURSOR		0x02
#define CMD_SET_MODE	 		0x04
#define CMD_MODE_SHIFT_RIGHT	0x02
#define CMD_MODE_SHIFT_SCREEN	0x01

#define CMD_SET_MOVE			0x10
#define CMD_MOVE_CURSOR			0x00
#define CMD_MOVE_SCREEN			0x08
#define CMD_MOVE_RIGHT			0x04
#define CMD_MOVE_LEFT			0x00

//#define FLAG_LINE1 0x00  			//第一行显示位置0~15  0x00~0x0F
//#define FLAG_LINE1 0x40			//第二行显示位置0~15  0x41~0x4F 
#define CMD_LOCATE				0x80
#define CMD_LOCATE_LINE2 		0x40

#define MAX_X 16
#define MAX_Y 2

#define OUT(x,y) outb(y, x) 
#define IN(x)	inb(x)
#define DELAY(x) udelay(x)


/*
   GPIO与1602连接：
   DB0 = GPIO 00		
   |
   DB7 = GPIO 07

   RS = GPIO 10;
   RW = GPIO 11;
   E  = GPIO 12;
   */

static char g1602_buffer[MAX_Y][MAX_X];

static BYTE g1602_busy(void)
{
	BYTE is_busy;

	//	rs = 0; rw = 1;	e = 1;	检测忙标志
	OUT(FLAG, FLAG_RW|FLAG_E);
	OUT(DATA_CTL, 0);	// set all pins to input mode
	DELAY(5);

	is_busy = (IN(DATA) & DATA_BUSY);

	//	e = 0;
	OUT(FLAG, FLAG_RW);

	OUT(DATA_CTL, 0xff);	// set all pins to OUTPut mode

	return is_busy;
}

static void g1602_command(BYTE cmd, int usec)
{
	while (g1602_busy()) DELAY(5);

	//	rs = 0; rw = 0;	e = 0;
	OUT(FLAG, 0); 
	OUT(DATA, cmd);

	//	e = 1;	
	OUT(FLAG, FLAG_E); 
	DELAY(5);

	//	e = 0;
	OUT(FLAG, 0);

	DELAY(usec);
}

static int g1602_cur_x = 0, g1602_cur_y = 0;

static void g1602_locate(int x, int y)
{
	BYTE flag = 0;
	
	if (y > 0) flag |= CMD_LOCATE_LINE2;
	flag |= (x >= MAX_X ? MAX_X - 1 : x); 

	g1602_command(CMD_LOCATE|flag, 50);
}

static void g1602_clear(void) {
	g1602_cur_x = 0;
	g1602_cur_y = 0;
	g1602_command(CMD_CLEAR, 2000);
}

static void g1602_reset_cursor(void) {
	g1602_cur_x = 0;
	g1602_cur_y = 0;
	g1602_command(CMD_RESET_CURSOR, 2000);
}

static void g1602_putc(BYTE ch)
{
	while(g1602_busy()) DELAY(5);

	//	rs = 1; rw = 0;	e = 0;
	OUT(FLAG, FLAG_RS); 
	DELAY(5);

	OUT(DATA, ch); 

	//	rs = 1; rw = 0; e = 1;
	OUT(FLAG, FLAG_RS|FLAG_E);
	DELAY(5);

	//	rs = 1; rw = 0; e = 0;	
	OUT(FLAG, FLAG_RS);
}

static void g1602_raw_write(const char *s, int slen)
{
	g1602_command(CMD_SET_MODE|CMD_MODE_SHIFT_RIGHT, 50);
	while (slen--) {
		g1602_putc(*s);
		++s;
	}
}

static void g1602_scrollup(void) {

	BYTE y=0;
	for (y=1; y<MAX_Y; y++) {
		memcpy(g1602_buffer[y-1], g1602_buffer[y], MAX_X);
	}

	memset(&g1602_buffer[MAX_Y-1], ' ', MAX_X);

	// 更新整个屏幕
	g1602_clear();
	for (y=0; y<MAX_Y-1; y++) {
		g1602_locate(0,0);
		g1602_raw_write(g1602_buffer[y], MAX_X);
	}
}

static void g1602_relocate(BOOL force) {
	BOOL altered = FALSE;

	if (g1602_cur_x == MAX_X) {
		g1602_cur_x = 0;
		g1602_cur_y ++;
		altered = TRUE;
	}

	if (g1602_cur_y == MAX_Y) {
		g1602_scrollup();
		g1602_cur_x = 0;
		g1602_cur_y = MAX_Y - 1;
		altered = TRUE;
	}
	
	if (altered || force) {
		g1602_locate(g1602_cur_x, g1602_cur_y);
	}
}	

static void g1602_show_cursor(BOOL show) {
	if (show) {
		g1602_command(CMD_SET_DISP|CMD_DISP_ON|CMD_DISP_CURSOR_ON|CMD_DISP_CURSOR_BLINK, 50);
	}
	else {
		g1602_command(CMD_SET_DISP|CMD_DISP_ON, 50);
	}
}

typedef enum {PARSE_NORMAL, PARSE_ESC, PARSE_CSI, PARSE_SPEC} PARSE_STATUS;

static void g1602_write(const char *s, int slen)
{
	char ch;
	int i;
	static PARSE_STATUS status = PARSE_NORMAL;
	static int codes[2] = {0, 0};
	static int code_i = 0;

	for (i=0; i < slen; i++) {
		ch = s[i];
		switch (status) {
		case PARSE_SPEC:
			if (ch >= '0' && ch <= '9') {
				codes[code_i] = codes[code_i] * 10 + (ch - '0');
			}
			else switch (ch) {
				case ';':
					code_i++;
					codes[code_i] = 0;
					break;
				case 'l':
					if (codes[0] == 25) {
						g1602_show_cursor(TRUE);
					}
					status = PARSE_NORMAL;
					break;
				case 'h':
					if (codes[0] == 25) {
						g1602_show_cursor(FALSE);
					}
					status = PARSE_NORMAL;
					break;
				default:
					status = PARSE_NORMAL;
			}
			break;
		case PARSE_CSI:
			if (ch >= '0' && ch <= '9') {
				codes[code_i] = codes[code_i] * 10 + (ch - '0');
			}
			else switch (ch) {
			case ';':	// sep
				code_i++;
				codes[code_i] = 0;
				break;
			case '?':	//cursor ?
				code_i = 0;
				codes[0] = 0;
				status = PARSE_SPEC;
				break;
			case 'J':	//clear screen
				g1602_clear();
				status = PARSE_NORMAL;
				break;
			case 'K':	//clean to end
				g1602_relocate(FALSE);
				{
					int n = MAX_X - g1602_cur_x;
					while (n--) g1602_putc(' ');
				}
				g1602_relocate(TRUE);
				status = PARSE_NORMAL;
				break;
			case 'H':	//locate to x:y  *[x:yH
				g1602_cur_x=codes[0];
				g1602_cur_y=codes[1];
				g1602_relocate(TRUE);
				status = PARSE_NORMAL;
				break;
			default:
				status = PARSE_NORMAL;
			}
			break;
		case PARSE_ESC:
			switch (ch) {
				case '[':
					status = PARSE_CSI;
					code_i = 0;
					codes[0] = 0;
					break;
				case 27:
					break;
				default:
					status = PARSE_NORMAL;
					i--; // 更新状态重新处理该字符
			}
			break;
		default:
			if (ch >= ' ') {	// 空格
				g1602_relocate(FALSE);
				g1602_buffer[g1602_cur_y][g1602_cur_x] = ch;
				g1602_putc(ch);
				g1602_cur_x++;
			}
			else switch(ch) {
				case '\e': // ESC
					status = PARSE_ESC;
					break;
				case '\n':
					g1602_cur_x = 0;
					g1602_cur_y ++;
					g1602_relocate(TRUE);
					break;
				/* default:  skip */
			}
		}
	}
}

static void g1602_puts(const char *s) {
	g1602_write(s, strlen(s));
}

static void g1602_init(void)
{

	memset(g1602_buffer, ' ', MAX_Y * MAX_X);

	/* set GPIO port0[7-0] as OUTPut mode */
	OUT(DATA_CTL, 0xff); 
	/* set GPIO port1[2-0] as OUTPut mode */
	OUT(FLAG_CTL, FLAG_RS|FLAG_RW|FLAG_E); 

	g1602_command(CMD_SET_OPT|CMD_OPT_8BITS|CMD_OPT_2LINES, 50);
	g1602_command(CMD_SET_DISP|CMD_DISP_ON, 50);
	g1602_command(CMD_SET_MODE|CMD_MODE_SHIFT_RIGHT, 50);

	g1602_clear();

}

#define pr_init(fmt, args...) ({ static const __initconst char __fmt[] = fmt; printk(__fmt, ## args); })

static struct tty_driver *gpio_driver;
static struct tty_struct * volatile gpio_tty;
static unsigned long gpio_count;
static DEFINE_MUTEX(gpio_tty_mutex);

static int
gpio_open(struct tty_struct *tty, struct file *filp)
{
	mutex_lock(&gpio_tty_mutex);
	pr_debug("open %lu\n", gpio_count);
	++gpio_count;
	gpio_tty = tty;
	mutex_unlock(&gpio_tty_mutex);
	return 0;
}

static void
gpio_close(struct tty_struct *tty, struct file *filp)
{
	mutex_lock(&gpio_tty_mutex);
	pr_debug("close %lu\n", gpio_count);
	if (--gpio_count == 0)
		gpio_tty = NULL;
	mutex_unlock(&gpio_tty_mutex);
}

#ifndef CONFIG_BFIN_JTAG_COMM_CONSOLE
# define acquire_console_sem()
# define release_console_sem()
#endif
static int gpio_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	acquire_console_sem();
	g1602_write(buf, count);
	release_console_sem();
	return count;
}

static void gpio_flush_chars(struct tty_struct *tty)
{
}

static int gpio_write_room(struct tty_struct *tty)
{
	if (tty->stopped)
		return 0;
	return 32768;
}

static int gpio_chars_in_buffer(struct tty_struct *tty)
{
	return 0;
}

static struct tty_operations gpio_ops = {
	.open            = gpio_open,
	.close           = gpio_close,
	.write           = gpio_write,
	/*.put_char        = gpio_put_char,*/
	.flush_chars     = gpio_flush_chars,
	.write_room      = gpio_write_room,
	.chars_in_buffer = gpio_chars_in_buffer,
	/*.wait_until_sent = gpio_wait_until_sent,*/
};

static int __init gpio_init(void)
{
	int ret;

	ret = -ENOMEM;

	gpio_driver = alloc_tty_driver(1);
	if (!gpio_driver)
		goto err;

	gpio_driver->owner        = THIS_MODULE;
	gpio_driver->driver_name  = DRV_NAME;
	gpio_driver->name         = DEV_NAME;
	gpio_driver->type         = TTY_DRIVER_TYPE_SERIAL;
	gpio_driver->subtype      = SERIAL_TYPE_NORMAL;
	gpio_driver->init_termios = tty_std_termios;
	tty_set_operations(gpio_driver, &gpio_ops);

	ret = tty_register_driver(gpio_driver);
	if (ret)
		goto err;

	g1602_init();

	pr_init(KERN_INFO DRV_NAME ": initialized\n");

	return 0;

 err:
	put_tty_driver(gpio_driver);
	return ret;
}

static void __exit gpio_exit(void)
{
	tty_unregister_driver(gpio_driver);
	put_tty_driver(gpio_driver);
}

static void gpio_console_write(struct console *co, const char *buf, unsigned count)
{
	g1602_write(buf, count);
}

static struct tty_driver *gpio_console_device(struct console *co, int *index)
{
	*index = co->index;
	return gpio_driver;
}

static struct console gpio_console = {
	.name    = DEV_NAME,
	.write   = gpio_console_write,
	.device  = gpio_console_device,
	.flags   = CON_ANYTIME | CON_PRINTBUFFER,
	.index   = -1,
};

static int __init gpio_console_init(void)
{
	register_console(&gpio_console);
	return 0;
}

MODULE_AUTHOR("Jia Huang <jia.huang@geneegroup.com>");
MODULE_DESCRIPTION("TTY over GPIO for LCM1602");
MODULE_LICENSE("MIT");

module_init(gpio_init);
module_exit(gpio_exit);
console_initcall(gpio_console_init);
