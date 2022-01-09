/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2021 Aidan MacDonald
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "axp-pmu.h"
#include "power.h"
#include "system.h"
#include "i2c-async.h"
#include <string.h>

/* Headers for the debug menu */
#ifndef BOOTLOADER
# include "action.h"
# include "list.h"
# include <stdio.h>
#endif

struct axp_adc_info {
    uint8_t reg;
    uint8_t en_reg;
    uint8_t en_bit;
};

struct axp_supply_info {
    uint8_t volt_reg;
    uint8_t volt_reg_mask;
    uint8_t en_reg;
    uint8_t en_bit;
    int min_mV;
    int max_mV;
    int step_mV;
};

static const struct axp_adc_info axp_adc_info[NUM_ADC_CHANNELS] = {
    {0x56, AXP_REG_ADCENABLE1, 5}, /* ACIN_VOLTAGE */
    {0x58, AXP_REG_ADCENABLE1, 4}, /* ACIN_CURRENT */
    {0x5a, AXP_REG_ADCENABLE1, 3}, /* VBUS_VOLTAGE */
    {0x5c, AXP_REG_ADCENABLE1, 2}, /* VBUS_CURRENT */
    {0x5e, AXP_REG_ADCENABLE2, 7}, /* INTERNAL_TEMP */
    {0x62, AXP_REG_ADCENABLE1, 1}, /* TS_INPUT */
    {0x78, AXP_REG_ADCENABLE1, 7}, /* BATTERY_VOLTAGE */
    {0x7a, AXP_REG_ADCENABLE1, 6}, /* CHARGE_CURRENT */
    {0x7c, AXP_REG_ADCENABLE1, 6}, /* DISCHARGE_CURRENT */
    {0x7e, AXP_REG_ADCENABLE1, 1}, /* APS_VOLTAGE */
    {0x70, 0xff, 0},               /* BATTERY_POWER */
};

static const struct axp_supply_info axp_supply_info[AXP_NUM_SUPPLIES] = {
#if HAVE_AXP_PMU == 192
    [AXP_SUPPLY_DCDC1] = {
        .volt_reg = 0x26,
        .volt_reg_mask = 0x7f,
        .en_reg = 0x12,
        .en_bit = 0,
        .min_mV = 700,
        .max_mV = 3500,
        .step_mV = 25,
    },
    [AXP_SUPPLY_DCDC2] = {
        .volt_reg = 0x23,
        .volt_reg_mask = 0x3f,
        .en_reg = 0x10,
        .en_bit = 0,
        .min_mV = 700,
        .max_mV = 2275,
        .step_mV = 25,
    },
    [AXP_SUPPLY_DCDC3] = {
        .volt_reg = 0x27,
        .volt_reg_mask = 0x7f,
        .en_reg = 0x12,
        .en_bit = 1,
        .min_mV = 700,
        .max_mV = 3500,
        .step_mV = 25,
    },
    /*
     * NOTE: LDO1 is always on, and we can't query it or change voltages
     */
    [AXP_SUPPLY_LDO2] = {
        .volt_reg = 0x28,
        .volt_reg_mask = 0xf0,
        .en_reg = 0x12,
        .en_bit = 2,
        .min_mV = 1800,
        .max_mV = 3300,
        .step_mV = 100,
    },
    [AXP_SUPPLY_LDO3] = {
        .volt_reg = 0x28,
        .volt_reg_mask = 0x0f,
        .en_reg = 0x12,
        .en_bit = 3,
        .min_mV = 1800,
        .max_mV = 3300,
        .step_mV = 100,
    },
    [AXP_SUPPLY_LDO_IO0] = {
        .volt_reg = 0x91,
        .volt_reg_mask = 0xf0,
        .en_reg = 0x90,
        .en_bit = 0xff, /* this one requires special handling */
        .min_mV = 1800,
        .max_mV = 3300,
        .step_mV = 100,
    },
#else
# error "Untested AXP chip"
#endif
};

static struct axp_driver {
    int adc_enable;
    int chargecurrent_setting;
    int chip_id;
} axp;

static void axp_init_enabled_adcs(void)
{
    axp.adc_enable = 0;

    /* Read chip ID, so we can display it on the debug screen.
     * This is undocumented but there's Linux driver code floating around
     * which suggests this should work for many AXP chips. */
    axp.chip_id = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_CHIP_ID);

    /* Read enabled ADCs from the hardware */
    uint8_t regs[2];
    int rc = i2c_reg_read(AXP_PMU_BUS, AXP_PMU_ADDR,
                          AXP_REG_ADCENABLE1, 2, &regs[0]);
    if(rc != I2C_STATUS_OK)
        return;

    /* Parse registers to set ADC enable bits */
    const struct axp_adc_info* info = axp_adc_info;
    for(int i = 0; i < NUM_ADC_CHANNELS; ++i) {
        if(info[i].en_reg == 0xff)
            continue;

        if(regs[info[i].en_reg - AXP_REG_ADCENABLE1] & info[i].en_bit)
            axp.adc_enable |= 1 << i;
    }

    /* Handle battery power ADC */
    if((axp.adc_enable & (1 << ADC_BATTERY_VOLTAGE)) &&
       (axp.adc_enable & (1 << ADC_DISCHARGE_CURRENT))) {
        axp.adc_enable |= (1 << ADC_BATTERY_POWER);
    }
}

void axp_init(void)
{
    axp_init_enabled_adcs();

    /* We need discharge current ADC to reliably poll for a full battery */
    int bits = axp.adc_enable;
    bits |= (1 << ADC_DISCHARGE_CURRENT);
    axp_adc_set_enabled(bits);

    /* Read the maximum charging current */
    int value = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_CHARGECONTROL1);
    axp.chargecurrent_setting = (value < 0) ? -1 : (value & 0xf);
}

void axp_supply_set_voltage(int supply, int voltage)
{
    const struct axp_supply_info* info = &axp_supply_info[supply];
    if(info->volt_reg == 0 || info->volt_reg_mask == 0)
        return;

    if(voltage > 0 && info->step_mV != 0) {
        if(voltage < info->min_mV || voltage > info->max_mV)
            return;

        int regval = (voltage - info->min_mV) / info->step_mV;
        i2c_reg_modify1(AXP_PMU_BUS, AXP_PMU_ADDR, info->volt_reg,
                        info->volt_reg_mask, regval, NULL);
    }

    if(info->en_bit != 0xff) {
        i2c_reg_setbit1(AXP_PMU_BUS, AXP_PMU_ADDR,
                        info->en_reg, info->en_bit,
                        voltage > 0 ? 1 : 0, NULL);
    }
}

int axp_supply_get_voltage(int supply)
{
    const struct axp_supply_info* info = &axp_supply_info[supply];
    if(info->volt_reg == 0)
        return AXP_SUPPLY_NOT_PRESENT;

    if(info->en_reg != 0) {
        int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, info->en_reg);
        if(r < 0)
            return AXP_SUPPLY_DISABLED;

#if HAVE_AXP_PMU == 192
        if(supply == AXP_SUPPLY_LDO_IO0) {
            if((r & 7) != 2)
                return AXP_SUPPLY_DISABLED;
        } else
#endif
        {
            if(r & (1 << info->en_bit) == 0)
                return AXP_SUPPLY_DISABLED;
        }
    }

    /* Hack, avoid undefined shift below. Can be useful too... */
    if(info->volt_reg_mask == 0)
        return info->min_mV;

    int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, info->volt_reg);
    if(r < 0)
        return 0;

    int bit = find_first_set_bit(info->volt_reg_mask);
    int val = (r & info->volt_reg_mask) >> bit;
    return info->min_mV + (val * info->step_mV);
}

/* TODO: this can STILL indicate some false positives! */
int axp_battery_status(void)
{
    int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_POWERSTATUS);
    if(r >= 0) {
        /* Charging bit indicates we're currently charging */
        if((r & 0x04) != 0)
            return AXP_BATT_CHARGING;

        /* Not plugged in means we're discharging */
        if((r & 0xf0) == 0)
            return AXP_BATT_DISCHARGING;
    } else {
        /* Report discharging if we can't find out power status */
        return AXP_BATT_DISCHARGING;
    }

    /* If the battery is full and not in use, the charging bit will be 0,
     * there will be an external power source, AND the discharge current
     * will be zero. Seems to rule out all false positives. */
    int d = axp_adc_read_raw(ADC_DISCHARGE_CURRENT);
    if(d == 0)
        return AXP_BATT_FULL;

    return AXP_BATT_DISCHARGING;
}

int axp_input_status(void)
{
#ifdef HAVE_BATTERY_SWITCH
    int input_status = 0;
#else
    int input_status = AXP_INPUT_BATTERY;
#endif

    int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_POWERSTATUS);
    if(r < 0)
        return input_status;

    /* Check for AC input */
    if(r & 0x80)
        input_status |= AXP_INPUT_AC;

    /* Only report USB if ACIN and VBUS are not shorted */
    if((r & 0x20) != 0 && (r & 0x02) == 0)
        input_status |= AXP_INPUT_USB;

#ifdef HAVE_BATTERY_SWITCH
    /* Check for battery presence if target defines it as removable */
    r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_CHARGESTATUS);
    if(r >= 0 && (r & 0x20) != 0)
        input_status |= AXP_INPUT_BATTERY;
#endif

    return input_status;
}

int axp_adc_read(int adc)
{
    int value = axp_adc_read_raw(adc);
    if(value == INT_MIN)
        return INT_MIN;

    return axp_adc_conv_raw(adc, value);
}

int axp_adc_read_raw(int adc)
{
    /* Don't give a reading if the ADC is not enabled */
    if((axp.adc_enable & (1 << adc)) == 0)
        return INT_MIN;

    /* Read the ADC */
    uint8_t buf[3];
    int count = (adc == ADC_BATTERY_POWER) ? 3 : 2;
    uint8_t reg = axp_adc_info[adc].reg;
    int rc = i2c_reg_read(AXP_PMU_BUS, AXP_PMU_ADDR, reg, count, &buf[0]);
    if(rc != I2C_STATUS_OK)
        return INT_MIN;

    /* Parse the value */
    if(adc == ADC_BATTERY_POWER)
        return (buf[0] << 16) | (buf[1] << 8) | buf[2];
    else if(adc == ADC_CHARGE_CURRENT || adc == ADC_DISCHARGE_CURRENT)
        return (buf[0] << 5) | (buf[1] & 0x1f);
    else
        return (buf[0] << 4) | (buf[1] & 0xf);
}

int axp_adc_conv_raw(int adc, int value)
{
    switch(adc) {
    case ADC_ACIN_VOLTAGE:
    case ADC_VBUS_VOLTAGE:
        /* 0 mV ... 6.9615 mV, step 1.7 mV */
        return value * 17 / 10;
    case ADC_ACIN_CURRENT:
        /* 0 mA ... 2.5594 A, step 0.625 mA */
        return value * 5 / 8;
    case ADC_VBUS_CURRENT:
        /* 0 mA ... 1.5356 A, step 0.375 mA */
        return value * 3 / 8;
    case ADC_INTERNAL_TEMP:
        /* -144.7 C ... 264.8 C, step 0.1 C */
        return value - 1447;
    case ADC_TS_INPUT:
        /* 0 mV ... 3.276 V, step 0.8 mV */
        return value * 4 / 5;
    case ADC_BATTERY_VOLTAGE:
        /* 0 mV ... 4.5045 V, step 1.1 mV */
        return value * 11 / 10;
    case ADC_CHARGE_CURRENT:
    case ADC_DISCHARGE_CURRENT:
        /* 0 mA to 4.095 A, step 0.5 mA */
        return value / 2;
    case ADC_APS_VOLTAGE:
        /* 0 mV to 5.733 V, step 1.4 mV */
        return value * 7 / 5;
    case ADC_BATTERY_POWER:
        /* 0 uW to 23.6404 W, step 0.55 uW */
        return value * 11 / 20;
    default:
        /* Shouldn't happen */
        return INT_MIN;
    }
}

int axp_adc_get_enabled(void)
{
    return axp.adc_enable;
}

void axp_adc_set_enabled(int adc_bits)
{
    /* Ignore no-op */
    if(adc_bits == axp.adc_enable)
        return;

    /* Compute the new register values */
    const struct axp_adc_info* info = axp_adc_info;
    uint8_t regs[2] = {0, 0};
    for(int i = 0; i < NUM_ADC_CHANNELS; ++i) {
        if(info[i].en_reg == 0xff)
            continue;

        if(adc_bits & (1 << i))
            regs[info[i].en_reg - 0x82] |= 1 << info[i].en_bit;
    }

    /* These ADCs share an enable bit */
    if(adc_bits & ((1 << ADC_CHARGE_CURRENT)|(1 << ADC_DISCHARGE_CURRENT))) {
        adc_bits |= (1 << ADC_CHARGE_CURRENT);
        adc_bits |= (1 << ADC_DISCHARGE_CURRENT);
    }

    /* Enable required bits for battery power ADC */
    if(adc_bits & (1 << ADC_BATTERY_POWER)) {
        regs[0] |= 1 << info[ADC_DISCHARGE_CURRENT].en_bit;
        regs[0] |= 1 << info[ADC_BATTERY_VOLTAGE].en_bit;
    }

    /* Update the configuration */
    i2c_reg_write(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_ADCENABLE1, 2, &regs[0]);
    axp.adc_enable = adc_bits;
}

int axp_adc_get_rate(void)
{
    int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_ADCSAMPLERATE);
    if(r < 0)
        return AXP_ADC_RATE_100HZ; /* an arbitrary value */

    return (r >> 6) & 3;
}

void axp_adc_set_rate(int rate)
{
    i2c_reg_modify1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP_REG_ADCSAMPLERATE,
                    0xc0, (rate & 3) << 6, NULL);
}

static uint32_t axp_cc_parse(const uint8_t* buf)
{
    return ((uint32_t)buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

void axp_cc_read(uint32_t* charge, uint32_t* discharge)
{
    uint8_t buf[8];
    int rc = i2c_reg_read(AXP_PMU_BUS, AXP_PMU_ADDR,
                          AXP_REG_COULOMBCOUNTERBASE, 8, &buf[0]);
    if(rc != I2C_STATUS_OK) {
        if(charge)
            *charge = 0;
        if(discharge)
            *discharge = 0;
        return;
    }

    if(charge)
        *charge = axp_cc_parse(&buf[0]);
    if(discharge)
        *discharge = axp_cc_parse(&buf[4]);
}

void axp_cc_clear(void)
{
    i2c_reg_setbit1(AXP_PMU_BUS, AXP_PMU_ADDR,
                    AXP_REG_COULOMBCOUNTERCTRL, 5, 1, NULL);
}

void axp_cc_enable(bool en)
{
    i2c_reg_setbit1(AXP_PMU_BUS, AXP_PMU_ADDR,
                    AXP_REG_COULOMBCOUNTERCTRL, 7, en ? 1 : 0, NULL);
}

bool axp_cc_is_enabled(void)
{
    int reg = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR,
                            AXP_REG_COULOMBCOUNTERCTRL);
    return reg >= 0 && (reg & 0x40) != 0;
}

static const int chargecurrent_tbl[] = {
    100,  190,  280,  360,
    450,  550,  630,  700,
    780,  880,  960,  1000,
    1080, 1160, 1240, 1320,
};

static const int chargecurrent_tblsz = sizeof(chargecurrent_tbl)/sizeof(int);

void axp_set_charge_current(int maxcurrent)
{
    /* Find the charge current just higher than maxcurrent */
    int value = 0;
    while(value < chargecurrent_tblsz &&
          chargecurrent_tbl[value] <= maxcurrent)
        ++value;

    /* Select the next lower current, the greatest current <= maxcurrent */
    if(value >= chargecurrent_tblsz)
        value = chargecurrent_tblsz - 1;
    else if(value > 0)
        --value;

    /* Don't issue i2c write if desired setting is already in use */
    if(value == axp.chargecurrent_setting)
        return;

    /* Update register */
    i2c_reg_modify1(AXP_PMU_BUS, AXP_PMU_ADDR,
                    AXP_REG_CHARGECONTROL1, 0x0f, value, NULL);
    axp.chargecurrent_setting = value;
}

int axp_get_charge_current(void)
{
    if(axp.chargecurrent_setting < 0)
        return chargecurrent_tbl[0];
    else
        return chargecurrent_tbl[axp.chargecurrent_setting];
}

void axp_power_off(void)
{
    /* Set the shutdown bit */
    i2c_reg_setbit1(AXP_PMU_BUS, AXP_PMU_ADDR,
                    AXP_REG_SHUTDOWNLEDCTRL, 7, 1, NULL);
}

#ifndef BOOTLOADER
enum {
    AXP_DEBUG_CHIP_ID,
    AXP_DEBUG_BATTERY_STATUS,
    AXP_DEBUG_INPUT_STATUS,
    AXP_DEBUG_CHARGE_CURRENT,
    AXP_DEBUG_COULOMB_COUNTERS,
    AXP_DEBUG_ADC_RATE,
    AXP_DEBUG_FIRST_ADC,
    AXP_DEBUG_FIRST_SUPPLY = AXP_DEBUG_FIRST_ADC + NUM_ADC_CHANNELS,
    AXP_DEBUG_NUM_ENTRIES = AXP_DEBUG_FIRST_SUPPLY + AXP_NUM_SUPPLIES,
};

static int axp_debug_menu_cb(int action, struct gui_synclist* lists)
{
    (void)lists;

    if(action == ACTION_NONE)
        action = ACTION_REDRAW;

    return action;
}

static const char* axp_debug_menu_get_name(int item, void* data,
                                           char* buf, size_t buflen)
{
    (void)data;

    static const char* const adc_names[] = {
        "V_acin", "I_acin", "V_vbus", "I_vbus", "T_int",
        "V_ts", "V_batt", "I_chrg", "I_dchg", "V_aps", "P_batt"
    };

    static const char* const adc_units[] = {
        "mV", "mA", "mV", "mA", "C", "mV", "mV", "mA", "mA", "mV", "uW",
    };

    static const char* const supply_names[] = {
        "DCDC1", "DCDC2", "DCDC3",
        "LDO1", "LDO2", "LDO3", "LDO_IO0",
    };

    int adc = item - AXP_DEBUG_FIRST_ADC;
    if(item >= AXP_DEBUG_FIRST_ADC && adc < NUM_ADC_CHANNELS) {
        int raw_value = axp_adc_read_raw(adc);
        if(raw_value == INT_MIN) {
            snprintf(buf, buflen, "%s: [Disabled]", adc_names[adc]);
            return buf;
        }

        int value = axp_adc_conv_raw(adc, raw_value);
        if(adc == ADC_INTERNAL_TEMP) {
            snprintf(buf, buflen, "%s: %d.%d %s", adc_names[adc],
                     value/10, value%10, adc_units[adc]);
        } else {
            snprintf(buf, buflen, "%s: %d %s", adc_names[adc],
                     value, adc_units[adc]);
        }

        return buf;
    }

    int supply = item - AXP_DEBUG_FIRST_SUPPLY;
    if(item >= AXP_DEBUG_FIRST_SUPPLY && supply < AXP_NUM_SUPPLIES) {
        int voltage = axp_supply_get_voltage(supply);
        if(voltage == AXP_SUPPLY_NOT_PRESENT)
            snprintf(buf, buflen, "%s: [Not Present]", supply_names[supply]);
        else if(voltage == AXP_SUPPLY_DISABLED)
            snprintf(buf, buflen, "%s: [Disabled]", supply_names[supply]);
        else
            snprintf(buf, buflen, "%s: %d mV", supply_names[supply], voltage);

        return buf;
    }

    switch(item) {
    case AXP_DEBUG_CHIP_ID: {
        snprintf(buf, buflen, "Chip ID: %d (%02x) [Driver: AXP%d]",
                 axp.chip_id, axp.chip_id, HAVE_AXP_PMU);
        return buf;
    } break;

    case AXP_DEBUG_BATTERY_STATUS: {
        switch(axp_battery_status()) {
        case AXP_BATT_FULL:
            return "Battery: Full";
        case AXP_BATT_CHARGING:
            return "Battery: Charging";
        case AXP_BATT_DISCHARGING:
            return "Battery: Discharging";
        default:
            return "Battery: Unknown";
        }
    } break;

    case AXP_DEBUG_INPUT_STATUS: {
        int s = axp_input_status();
        const char* ac = (s & AXP_INPUT_AC) ? " AC" : "";
        const char* usb = (s & AXP_INPUT_USB) ? " USB" : "";
        const char* batt = (s & AXP_INPUT_BATTERY) ? " Battery" : "";
        snprintf(buf, buflen, "Inputs:%s%s%s", ac, usb, batt);
        return buf;
    } break;

    case AXP_DEBUG_CHARGE_CURRENT: {
        int current = axp_get_charge_current();
        snprintf(buf, buflen, "Max charge current: %d mA", current);
        return buf;
    } break;

    case AXP_DEBUG_COULOMB_COUNTERS: {
        uint32_t charge, discharge;
        axp_cc_read(&charge, &discharge);

        snprintf(buf, buflen, "Coulomb counters: +%lu / -%lu",
                 (unsigned long)charge, (unsigned long)discharge);
        return buf;
    } break;

    case AXP_DEBUG_ADC_RATE: {
        int rate = 25 << axp_adc_get_rate();
        snprintf(buf, buflen, "ADC sample rate: %d Hz", rate);
        return buf;
    } break;

    default:
        return "---";
    }
}

bool axp_debug_menu(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "AXP debug", AXP_DEBUG_NUM_ENTRIES, NULL);
    info.action_callback = axp_debug_menu_cb;
    info.get_name = axp_debug_menu_get_name;
    return simplelist_show_list(&info);
}
#endif /* !BOOTLOADER */

/* This is basically the only valid implementation, so define it here */
unsigned int power_input_status(void)
{
    unsigned int state = 0;
    int input_status = axp_input_status();

    if(input_status & AXP_INPUT_AC)
        state |= POWER_INPUT_MAIN_CHARGER;

    if(input_status & AXP_INPUT_USB)
        state |= POWER_INPUT_USB_CHARGER;

#ifdef HAVE_BATTERY_SWITCH
    if(input_status & AXP_INPUT_BATTERY)
        state |= POWER_INPUT_BATTERY;
#endif

    return state;
}
