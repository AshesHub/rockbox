/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 * $Id$
 *
 * Copyright (C) 2002 by Bj�rn Stenberg <bjorn@haxx.se>
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <time.h>
#include <stdbool.h>

#include "debug.h"

#include "screens.h"
#include "button.h"
#include "menu.h"

#include "string.h"
#include "lcd.h"

extern char having_new_lcd;


void backlight_on(void)
{
  /* we could do something better here! */
}

void backlight_time(int dummy)
{
    (void)dummy;
}

int fat_startsector(void)
{
    return 63;
}

int ata_write_sectors(unsigned long start,
                      unsigned char count,
                      void* buf)
{
    int i;
    
    for (i=0; i<count; i++ ) {
        FILE* f;
        char name[32];

        DEBUGF("Writing sector %X\n",start+i);
        sprintf(name,"sector%lX.bin",start+i);
        f=fopen(name,"w");
        if (f) {
            fwrite(buf,512,1,f);
            fclose(f);
        }
    }
    return 1;
}

int ata_read_sectors(unsigned long start,
                     unsigned char count,
                     void* buf)
{
    int i;
    
    for (i=0; i<count; i++ ) {
        FILE* f;
        char name[32];

        DEBUGF("Reading sector %X\n",start+i);
        sprintf(name,"sector%lX.bin",start+i);
        f=fopen(name,"r");
        if (f) {
            fread(buf,512,1,f);
            fclose(f);
        }
    }
    return 1;
}

void ata_delayed_write(unsigned long sector, void* buf)
{
    ata_write_sectors(sector,1,buf);
}

void ata_flush(void)
{
}

void ata_spin(void)
{
}

void ata_spindown(int s)
{
    (void)s;
}

bool simulate_usb(void)
{
    usb_display_info();
    while (button_get(true) & BUTTON_REL);
    return false;
}

void backlight_set_timeout(int index)
{
  (void)index;
}

void backlight_set_on_when_charging(bool beep)
{
  (void)beep;
}

/* original is in firmware/common/timefuncs.c */
struct tm *get_time(void)
{
    time_t now = time(NULL);
    return localtime(&now);
}

int rtc_read(int address)
{
  time_t now = time(NULL);
  struct tm *teem = localtime(&now);

  switch(address) {
  case 3: /* hour */
    return (teem->tm_hour%10) | ((teem->tm_hour/10) << 4);

  case 2: /* minute */
    return (teem->tm_min%10) | ((teem->tm_min/10) << 4);

  case 1: /* seconds */
    return (teem->tm_sec%10) | ((teem->tm_sec/10) << 4);

  case 7: /* year */
    return ((teem->tm_year-100)%10) | (((teem->tm_year-100)/10) << 4);

  case 6: /* month */
    return ((teem->tm_mon+1)%10) | (((teem->tm_mon+1)/10) << 4);

  case 5: /* day */
    return (teem->tm_mday%10) | ((teem->tm_mday/10) << 4);
  }

  return address ^ 0x55;
}

int rtc_write(int address, int value)
{
  DEBUGF("write %x to address %x\n", value, address);
  return 0;
}

bool has_new_lcd(void)
{
    return having_new_lcd;
}

void lcd_set_contrast( int x )
{
    (void)x;
}

void mpeg_set_pitch(int pitch)
{
    (void)pitch;
}

void mpeg_set_buffer_margin(int seconds)
{
    (void)seconds;
}

static int sleeptime;
void set_sleep_timer(int seconds)
{
    sleeptime = seconds;
}

int get_sleep_timer(void)
{
    return sleeptime;
}

#ifdef HAVE_LCD_CHARCELLS
void lcd_clearrect (int x, int y, int nx, int ny)
{
  /* Reprint char if you want to change anything */
  (void)x;
  (void)y;
  (void)nx;
  (void)ny;
}

void lcd_fillrect (int x, int y, int nx, int ny)
{
  /* Reprint char if you want to change display anything */
  (void)x;
  (void)y;
  (void)nx;
  (void)ny;
}
#endif

void cpu_sleep(bool enabled)
{
    (void)enabled;
}

void button_set_flip(bool yesno)
{
    (void)yesno;
}

int talk_buffer_steal(void)
{
    return 0;
}

int talk_id(int id, bool block)
{
    (void)id;
    (void)block;
    return 0;
}

int talk_file(char* filename, bool block)
{
    (void)filename;
    (void)block;
    return 0;
}
