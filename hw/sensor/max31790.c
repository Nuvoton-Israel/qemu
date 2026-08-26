/*
 * Maxim MAX31790 6-channel PWM fan controller
 *
 * The device is a plain SMBus register file: byte registers below 0x18 and
 * big-endian word registers at and above it. Tachometer counts are derived
 * from the commanded duty cycle so that a guest driver reading fanN_input
 * sees the fan respond to what it wrote.
 *
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_MAX31790 "max31790"
#define MAX31790(obj) OBJECT_CHECK(MAX31790State, (obj), TYPE_MAX31790)

#define MAX31790_NUM_CHANNELS       6
#define MAX31790_NUM_REGS           0x60

#define MAX31790_REG_GLOBAL_CONFIG  0x00
#define MAX31790_REG_PWM_FREQ       0x01
#define MAX31790_REG_FAN_CONFIG(ch)     (0x02 + (ch))
#define MAX31790_REG_FAN_DYNAMICS(ch)   (0x08 + (ch))
#define MAX31790_REG_FAN_FAULT_STATUS2  0x10
#define MAX31790_REG_FAN_FAULT_STATUS1  0x11
#define MAX31790_REG_TACH_COUNT(ch)     (0x18 + (ch) * 2)
#define MAX31790_REG_PWM_DUTY_CYCLE(ch) (0x30 + (ch) * 2)
#define MAX31790_REG_PWMOUT(ch)         (0x40 + (ch) * 2)
#define MAX31790_REG_TARGET_COUNT(ch)   (0x50 + (ch) * 2)

/* first register that is accessed as a 16 bit big endian quantity */
#define MAX31790_FIRST_WORD_REG     0x18

/* Fan Dynamics: speed range field, 5:7 */
#define MAX31790_FAN_DYN_SR_SHIFT   5

/* power on defaults from the datasheet */
#define MAX31790_DEFAULT_GLOBAL_CONFIG  0x20
#define MAX31790_DEFAULT_PWM_FREQ       0x00
#define MAX31790_DEFAULT_FAN_CONFIG     0x08
#define MAX31790_DEFAULT_FAN_DYNAMICS   0x4c

typedef struct MAX31790State {
    I2CSlave parent_obj;

    uint8_t regs[MAX31790_NUM_REGS];

    uint8_t cmd;        /* register pointer for the current transfer */
    bool cmd_pending;   /* true until the command byte has been consumed */
    int byte_idx;       /* which byte of a word register is next */
} MAX31790State;

static bool max31790_is_word_reg(uint8_t reg)
{
    return reg >= MAX31790_FIRST_WORD_REG;
}

/*
 * Derive a plausible tach count from the commanded duty cycle so that the
 * guest sees a non-zero, duty-proportional fan speed. The Linux driver
 * computes rpm = (60 * sr * 8192) / (count >> 4), so the count has to shrink
 * as the duty grows.
 */
static void max31790_update_tach(MAX31790State *s, int ch)
{
    uint16_t pwmout;
    uint32_t count;

    pwmout = (s->regs[MAX31790_REG_PWMOUT(ch)] << 8) |
              s->regs[MAX31790_REG_PWMOUT(ch) + 1];

    /* PWMOUT is a 9 bit value left aligned in a 16 bit register */
    pwmout >>= 7;
    if (pwmout == 0) {
        /* stopped: report the maximum count, which the driver maps to 0 rpm */
        count = 0xffe0;
    } else {
        /* full scale 511 gives ~0x0800, i.e. a fast but legal count */
        count = (0x0800 * 511) / pwmout;
        if (count > 0xffe0) {
            count = 0xffe0;
        }
    }

    s->regs[MAX31790_REG_TACH_COUNT(ch)] = count >> 8;
    s->regs[MAX31790_REG_TACH_COUNT(ch) + 1] = count & 0xe0;
}

static uint8_t max31790_read_byte(MAX31790State *s)
{
    uint8_t reg = s->cmd;
    uint8_t val;

    if (max31790_is_word_reg(reg)) {
        reg += s->byte_idx;
    }

    if (reg >= MAX31790_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from invalid register 0x%02x\n",
                      __func__, reg);
        return 0xff;
    }

    val = s->regs[reg];

    if (max31790_is_word_reg(s->cmd)) {
        s->byte_idx = (s->byte_idx + 1) & 1;
    }

    return val;
}

static void max31790_write_byte(MAX31790State *s, uint8_t data)
{
    uint8_t reg = s->cmd;
    int ch;

    if (max31790_is_word_reg(reg)) {
        reg += s->byte_idx;
    }

    if (reg >= MAX31790_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to invalid register 0x%02x\n",
                      __func__, reg);
        return;
    }

    /* fault status registers are read only */
    if (reg == MAX31790_REG_FAN_FAULT_STATUS1 ||
        reg == MAX31790_REG_FAN_FAULT_STATUS2) {
        return;
    }
    /* so are the tach counters */
    if (reg >= MAX31790_REG_TACH_COUNT(0) &&
        reg < MAX31790_REG_TACH_COUNT(0) + MAX31790_NUM_CHANNELS * 2) {
        return;
    }

    s->regs[reg] = data;

    if (max31790_is_word_reg(s->cmd)) {
        s->byte_idx = (s->byte_idx + 1) & 1;
    }

    /* a duty cycle change is reflected in the readback and the tachometer */
    if (reg >= MAX31790_REG_PWMOUT(0) &&
        reg < MAX31790_REG_PWMOUT(0) + MAX31790_NUM_CHANNELS * 2) {
        ch = (reg - MAX31790_REG_PWMOUT(0)) / 2;
        s->regs[MAX31790_REG_PWM_DUTY_CYCLE(ch)] =
            s->regs[MAX31790_REG_PWMOUT(ch)];
        s->regs[MAX31790_REG_PWM_DUTY_CYCLE(ch) + 1] =
            s->regs[MAX31790_REG_PWMOUT(ch) + 1];
        max31790_update_tach(s, ch);
    }
}

static int max31790_event(I2CSlave *i2c, enum i2c_event event)
{
    MAX31790State *s = MAX31790(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->cmd_pending = true;
        s->byte_idx = 0;
        break;
    case I2C_START_RECV:
        s->byte_idx = 0;
        break;
    case I2C_FINISH:
    case I2C_NACK:
        s->cmd_pending = false;
        s->byte_idx = 0;
        break;
    default:
        break;
    }

    return 0;
}

static uint8_t max31790_recv(I2CSlave *i2c)
{
    return max31790_read_byte(MAX31790(i2c));
}

static int max31790_send(I2CSlave *i2c, uint8_t data)
{
    MAX31790State *s = MAX31790(i2c);

    if (s->cmd_pending) {
        s->cmd = data;
        s->cmd_pending = false;
        s->byte_idx = 0;
        return 0;
    }

    max31790_write_byte(s, data);
    return 0;
}

static void max31790_reset_enter(Object *obj, ResetType type)
{
    MAX31790State *s = MAX31790(obj);
    int ch;

    memset(s->regs, 0, sizeof(s->regs));

    s->regs[MAX31790_REG_GLOBAL_CONFIG] = MAX31790_DEFAULT_GLOBAL_CONFIG;
    /*
     * The board straps the start frequency to 30Hz; the guest driver is
     * expected to override this. Keeping the reset value at the datasheet
     * default is what makes that write observable.
     */
    s->regs[MAX31790_REG_PWM_FREQ] = MAX31790_DEFAULT_PWM_FREQ;

    for (ch = 0; ch < MAX31790_NUM_CHANNELS; ch++) {
        s->regs[MAX31790_REG_FAN_CONFIG(ch)] = MAX31790_DEFAULT_FAN_CONFIG;
        s->regs[MAX31790_REG_FAN_DYNAMICS(ch)] = MAX31790_DEFAULT_FAN_DYNAMICS;
        max31790_update_tach(s, ch);
    }

    s->cmd = 0;
    s->cmd_pending = false;
    s->byte_idx = 0;
}

static const VMStateDescription vmstate_max31790 = {
    .name = "MAX31790",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MAX31790State, MAX31790_NUM_REGS),
        VMSTATE_UINT8(cmd, MAX31790State),
        VMSTATE_BOOL(cmd_pending, MAX31790State),
        VMSTATE_INT32(byte_idx, MAX31790State),
        VMSTATE_I2C_SLAVE(parent_obj, MAX31790State),
        VMSTATE_END_OF_LIST()
    }
};

static void max31790_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Maxim MAX31790 6-channel PWM fan controller";
    dc->vmsd = &vmstate_max31790;
    k->event = max31790_event;
    k->recv = max31790_recv;
    k->send = max31790_send;
    rc->phases.enter = max31790_reset_enter;
}

static const TypeInfo max31790_types[] = {
    {
        .name          = TYPE_MAX31790,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(MAX31790State),
        .class_init    = max31790_class_init,
    },
};

DEFINE_TYPES(max31790_types)
