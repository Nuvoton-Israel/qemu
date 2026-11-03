/*
 * NPCM BMC-side KCS channels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/npcm_kcs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/atomic.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "trace.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define KCS_FIRST_CHANNEL 0x0c
#define KCS_CHANNEL_SIZE  0x12

#define KCS_ST           0x00
#define KCS_DO           0x02
#define KCS_DI           0x04
#define KCS_DOC          0x06
#define KCS_DOM          0x08
#define KCS_DIC          0x0a
#define KCS_CTL          0x0c
#define KCS_IC           0x0e
#define KCS_IE           0x10

#define KCS_ST_OBF       BIT(0)
#define KCS_ST_IBF       BIT(1)
#define KCS_ST_CMD       BIT(3)
#define KCS_ST_WRITABLE  0xf4

#define KCS_CTL_IBFIE    BIT(0)
#define KCS_CTL_OBEIE    BIT(1)
#define KCS_CTL_PLMS     0x38
#define KCS_CTL_SCIPOL   BIT(6)
#define KCS_CTL_EME      BIT(7)
#define KCS_CTL_WRITABLE 0x7b

#define KCS_IC_IRQB      BIT(0)
#define KCS_IC_SMIB      BIT(1)
#define KCS_IC_SCIB      BIT(2)
#define KCS_IC_SMIPOL    BIT(6)
#define KCS_IC_SCIIS     BIT(7)
#define KCS_IC_READ_MASK 0x78

#define KCS_IE_IRQE      BIT(0)
#define KCS_IE_SCIE      BIT(1)
#define KCS_IE_SMIE      BIT(2)
#define KCS_IE_HIRQE     BIT(3)
#define KCS_IE_HSCIE     BIT(4)
#define KCS_IE_HSMIE     BIT(5)
#define KCS_IE_HSTA      BIT(6)
#define KCS_IE_MASK      0x7f

/* The pulse mode field sits at bits [5:3] in both CTL and IC. */
#define KCS_PULSE_MODE(reg)  (((reg) & KCS_CTL_PLMS) >> 3)
#define KCS_PULSE_MODE_MAX   5

static bool npcm_kcs_signal_level(NPCMKCSSignal *signal);
static bool npcm_kcs_host_irq(NPCMKCSChannel *c);
static void npcm_kcs_update_irq(NPCMKCSState *s);

static void npcm_kcs_shared_store(NPCMKCSState *s)
{
    int i;

    if (!s->shared) {
        return;
    }
    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        NPCMKCSChannel *c = &s->channels[i];
        NPCMKCSSharedChannel *shared = &s->shared->channels[i];

        shared->status = c->status;
        shared->host_irq = npcm_kcs_host_irq(c);
        shared->sci_level = npcm_kcs_signal_level(&s->sci[i]);
        shared->smi_level = npcm_kcs_signal_level(&s->smi[i]);
        shared->bmc_output = c->output;
    }
}

static void npcm_kcs_shared_update(NPCMKCSState *s)
{
    /* Notify the peer to refresh the shared per-channel snapshots. */
    uint8_t changed = (1 << NPCM_KCS_NR_CHANNELS) - 1;

    if (!s->shared) {
        return;
    }
    npcm_kcs_shared_store(s);
    if (s->doorbell_out_fd >= 0 && s->doorbell_out_path &&
        s->doorbell_out_path[0]) {
        struct sockaddr_un peer = { .sun_family = AF_UNIX };

        g_strlcpy(peer.sun_path, s->doorbell_out_path, sizeof(peer.sun_path));
        if (sendto(s->doorbell_out_fd, &changed, sizeof(changed), 0,
                   (struct sockaddr *)&peer, sizeof(peer)) < 0) {
            trace_npcm_kcs_doorbell_out_failed(errno);
        }
    }
}

/* Returns true if a pending host byte was moved into the input latch. */
static bool npcm_kcs_accept_input(NPCMKCSState *s, unsigned i)
{
    NPCMKCSChannel *c = &s->channels[i];
    NPCMKCSSharedChannel *shared;
    uint32_t seq, ack;

    if (!s->shared) {
        return false;
    }
    shared = &s->shared->channels[i];
    seq = qatomic_load_acquire(&shared->host_input_seq);
    ack = qatomic_read(&shared->bmc_input_ack_seq);

    /* In HSTA mode a full IBF defers the byte until IBF clears. */
    if (seq == ack ||
        ((c->interrupt_enable & KCS_IE_HSTA) && (c->status & KCS_ST_IBF))) {
        return false;
    }
    c->input = shared->host_input;
    c->status |= KCS_ST_IBF;
    c->ibf_sci = false;
    if (shared->host_command) {
        c->status |= KCS_ST_CMD;
    } else {
        c->status &= ~KCS_ST_CMD;
    }
    /* Status must be visible before the ack is. */
    npcm_kcs_shared_store(s);
    qatomic_store_release(&shared->bmc_input_ack_seq, seq);
    trace_npcm_kcs_input_accepted(i, c->input, shared->host_command);
    return true;
}

static void npcm_kcs_doorbell_read(void *opaque)
{
    NPCMKCSState *s = opaque;
    uint8_t events;
    ssize_t n;
    unsigned i;

    while ((n = recv(s->doorbell_in_fd, &events, sizeof(events),
                     MSG_DONTWAIT)) > 0) {
        trace_npcm_kcs_doorbell_in(events);
        for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
            NPCMKCSChannel *c = &s->channels[i];
            NPCMKCSSharedChannel *shared = &s->shared->channels[i];

            if ((events & NPCM_KCS_DB_INPUT(i)) &&
                npcm_kcs_accept_input(s, i)) {
                npcm_kcs_update_irq(s);
            }
            if (events & NPCM_KCS_DB_OUTPUT_ACK(i)) {
                /* Ignore stale acks; only published output may be consumed. */
                uint32_t seq = qatomic_read(&shared->bmc_output_seq);
                uint32_t ack =
                    qatomic_load_acquire(&shared->host_output_ack_seq);
                uint32_t done = qatomic_read(&shared->bmc_output_ack_seq);

                if (seq && ack == seq && done != seq) {
                    c->status &= ~KCS_ST_OBF;
                    c->obf_sci = false;
                    c->obf_smi = false;
                    npcm_kcs_shared_store(s);
                    qatomic_store_release(&shared->bmc_output_ack_seq, seq);
                    npcm_kcs_update_irq(s);
                }
            }
        }
    }
}

static void npcm_kcs_ipc_setup(NPCMKCSState *s, Error **errp)
{
    int fd;

    if (s->shm_path && s->shm_path[0]) {
        fd = open(s->shm_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            error_setg_errno(errp, errno, "%s: cannot open shared memory '%s'",
                             TYPE_NPCM_KCS, s->shm_path);
            return;
        }
        if (ftruncate(fd, sizeof(*s->shared)) < 0) {
            error_setg_errno(errp, errno, "%s: cannot size shared memory '%s'",
                             TYPE_NPCM_KCS, s->shm_path);
            close(fd);
            return;
        }
        s->shared = mmap(NULL, sizeof(*s->shared), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        close(fd);
        if (s->shared == MAP_FAILED) {
            s->shared = NULL;
            error_setg_errno(errp, errno, "%s: cannot map shared memory '%s'",
                             TYPE_NPCM_KCS, s->shm_path);
            return;
        }
        /* IDMAQ owns initialization; the peer must map this region as-is. */
        memset(s->shared, 0, sizeof(*s->shared));
        s->shared->magic = NPCM_KCS_SHM_MAGIC;
        s->shared->version = NPCM_KCS_SHM_VERSION;
        s->shared->channel_count = NPCM_KCS_NR_CHANNELS;
    }

    if ((s->doorbell_in_path || s->doorbell_out_path) && !s->shared) {
        error_setg(errp, "%s: doorbell sockets require shm-path",
                   TYPE_NPCM_KCS);
        return;
    }
    if (s->doorbell_in_path && s->doorbell_in_path[0]) {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };

        if (strlen(s->doorbell_in_path) >= sizeof(addr.sun_path)) {
            error_setg(errp, "%s: doorbell-in path is too long", TYPE_NPCM_KCS);
            return;
        }
        g_strlcpy(addr.sun_path, s->doorbell_in_path, sizeof(addr.sun_path));
        s->doorbell_in_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (s->doorbell_in_fd < 0) {
            error_setg_errno(errp, errno,
                             "%s: cannot create doorbell-in socket",
                             TYPE_NPCM_KCS);
            return;
        }
        unlink(s->doorbell_in_path);
        if (bind(s->doorbell_in_fd, (struct sockaddr *)&addr,
                 sizeof(addr)) < 0) {
            error_setg_errno(errp, errno, "%s: cannot bind doorbell-in '%s'",
                             TYPE_NPCM_KCS, s->doorbell_in_path);
            close(s->doorbell_in_fd);
            s->doorbell_in_fd = -1;
            return;
        }
        qemu_set_fd_handler(s->doorbell_in_fd, npcm_kcs_doorbell_read, NULL, s);
    }
    if (s->doorbell_out_path && s->doorbell_out_path[0]) {
        struct sockaddr_un peer;

        if (strlen(s->doorbell_out_path) >= sizeof(peer.sun_path)) {
            error_setg(errp, "%s: doorbell-out path is too long",
                       TYPE_NPCM_KCS);
            return;
        }
        /* Non-blocking: a slow peer must not stall the vCPU holding the BQL. */
        s->doorbell_out_fd = socket(AF_UNIX,
                                    SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0);
        if (s->doorbell_out_fd < 0) {
            error_setg_errno(errp, errno,
                             "%s: cannot create doorbell-out socket",
                             TYPE_NPCM_KCS);
            return;
        }
    }
}

static void npcm_kcs_ipc_cleanup(NPCMKCSState *s)
{
    if (s->doorbell_in_fd >= 0) {
        qemu_set_fd_handler(s->doorbell_in_fd, NULL, NULL, NULL);
        close(s->doorbell_in_fd);
        s->doorbell_in_fd = -1;
        if (s->doorbell_in_path) {
            unlink(s->doorbell_in_path);
        }
    }
    if (s->doorbell_out_fd >= 0) {
        close(s->doorbell_out_fd);
        s->doorbell_out_fd = -1;
    }
    if (s->shared) {
        munmap(s->shared, sizeof(*s->shared));
        s->shared = NULL;
    }
}

static bool npcm_kcs_signal_level(NPCMKCSSignal *signal)
{
    NPCMKCSChannel *c = &signal->controller->channels[signal->channel];
    uint8_t control = signal->smi ? c->interrupt_control : c->control;
    uint8_t enable = signal->smi ? KCS_IE_SMIE : KCS_IE_SCIE;
    uint8_t hardware = signal->smi ? KCS_IE_HSMIE : KCS_IE_HSCIE;
    uint8_t software = signal->smi ? KCS_IC_SMIB : KCS_IC_SCIB;
    uint8_t polarity = signal->smi ? KCS_IC_SMIPOL : KCS_CTL_SCIPOL;
    bool request;

    /* A disabled output idles high regardless of polarity. */
    if (!(c->interrupt_enable & enable)) {
        return true;
    }
    if (!(c->interrupt_enable & hardware)) {
        request = c->interrupt_control & software;
    } else if (KCS_PULSE_MODE(control)) {
        request = signal->pulse;
    } else {
        request = signal->smi ? c->obf_smi : c->obf_sci || c->ibf_sci;
    }
    return request ^ !!(control & polarity);
}

static void npcm_kcs_update_signals(NPCMKCSState *s)
{
    int i;

    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        qemu_set_irq(s->sci[i].output, npcm_kcs_signal_level(&s->sci[i]));
        qemu_set_irq(s->smi[i].output, npcm_kcs_signal_level(&s->smi[i]));
    }
}

static void npcm_kcs_pulse_end(void *opaque)
{
    NPCMKCSSignal *signal = opaque;

    signal->pulse = false;
    npcm_kcs_update_signals(signal->controller);
    npcm_kcs_shared_update(signal->controller);
}

static void npcm_kcs_trigger(NPCMKCSSignal *signal)
{
    NPCMKCSState *s = signal->controller;
    NPCMKCSChannel *c = &s->channels[signal->channel];
    uint8_t control = signal->smi ? c->interrupt_control : c->control;
    unsigned mode = KCS_PULSE_MODE(control);
    int64_t duration;

    if (!mode) {
        return;
    }
    if (mode > KCS_PULSE_MODE_MAX || !clock_get(s->clock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: cannot generate pulse with mode %u or stopped "
                      "APB clock\n", TYPE_NPCM_KCS, mode);
        return;
    }
    duration = MAX(1, clock_ticks_to_ns(s->clock, 1 << (mode - 1)));
    signal->pulse = true;
    timer_mod(signal->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration);
}

static void npcm_kcs_update_irq(NPCMKCSState *s)
{
    bool level = false;
    int i;

    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        NPCMKCSChannel *c = &s->channels[i];

        level |= ((c->control & KCS_CTL_IBFIE) &&
                  (c->status & KCS_ST_IBF)) ||
                 ((c->control & KCS_CTL_OBEIE) &&
                  !(c->status & KCS_ST_OBF));
    }
    qemu_set_irq(s->irq, level);
    trace_npcm_kcs_irq(level);
    npcm_kcs_update_signals(s);
    npcm_kcs_shared_update(s);
}

static bool npcm_kcs_host_irq(NPCMKCSChannel *c)
{
    return (c->interrupt_enable & KCS_IE_IRQE) &&
           ((c->interrupt_enable & KCS_IE_HIRQE) ?
            (c->status & KCS_ST_OBF) : (c->interrupt_control & KCS_IC_IRQB));
}

static uint64_t npcm_kcs_read(void *opaque, hwaddr offset, unsigned size)
{
    NPCMKCSState *s = NPCM_KCS(opaque);
    NPCMKCSChannel *c;
    unsigned channel, reg;
    uint8_t value;

    if (offset < KCS_FIRST_CHANNEL) {
        goto invalid;
    }
    channel = (offset - KCS_FIRST_CHANNEL) / KCS_CHANNEL_SIZE;
    reg = (offset - KCS_FIRST_CHANNEL) % KCS_CHANNEL_SIZE;
    if (channel >= NPCM_KCS_NR_CHANNELS) {
        goto invalid;
    }
    c = &s->channels[channel];
    switch (reg) {
    case KCS_ST:
        value = c->status;
        break;
    case KCS_DIC:
        c->ibf_sci = true;
        npcm_kcs_trigger(&s->sci[channel]);
        /* fall through */
    case KCS_DI:
        value = c->input;
        c->status &= ~KCS_ST_IBF;
        npcm_kcs_accept_input(s, channel);
        npcm_kcs_update_irq(s);
        break;
    case KCS_CTL:
        value = c->control;
        break;
    case KCS_IC:
        value = (c->interrupt_control & KCS_IC_READ_MASK) |
                (npcm_kcs_signal_level(&s->smi[channel]) ? KCS_IC_SMIB : 0) |
                (npcm_kcs_signal_level(&s->sci[channel]) ? KCS_IC_SCIB : 0) |
                (npcm_kcs_host_irq(c) ? KCS_IC_IRQB : 0);
        break;
    case KCS_IE:
        value = c->interrupt_enable;
        break;
    default:
        goto invalid;
    }
    trace_npcm_kcs_read(channel, reg, value);
    return value;
invalid:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid read at offset 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM_KCS, offset);
    return 0;
}

static void npcm_kcs_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    NPCMKCSState *s = NPCM_KCS(opaque);
    NPCMKCSChannel *c;
    unsigned channel, reg;

    if (offset < KCS_FIRST_CHANNEL) {
        goto invalid;
    }
    channel = (offset - KCS_FIRST_CHANNEL) / KCS_CHANNEL_SIZE;
    reg = (offset - KCS_FIRST_CHANNEL) % KCS_CHANNEL_SIZE;
    if (channel >= NPCM_KCS_NR_CHANNELS) {
        goto invalid;
    }
    c = &s->channels[channel];
    trace_npcm_kcs_write(channel, reg, value);
    switch (reg) {
    case KCS_ST:
        c->status = (c->status & ~KCS_ST_WRITABLE) |
                    (value & KCS_ST_WRITABLE);
        break;
    case KCS_DOC:
    case KCS_DOM:
    case KCS_DO:
        c->output = value;
        c->status |= KCS_ST_OBF;
        if (reg == KCS_DOC) {
            c->obf_sci = true;
            npcm_kcs_trigger(&s->sci[channel]);
        } else if (reg == KCS_DOM) {
            c->obf_smi = true;
            npcm_kcs_trigger(&s->smi[channel]);
        }
        if (s->shared) {
            NPCMKCSSharedChannel *shared = &s->shared->channels[channel];

            /* Publish the sequence only after output and status are stored. */
            npcm_kcs_shared_store(s);
            qatomic_store_release(&shared->bmc_output_seq,
                                  qatomic_read(&shared->bmc_output_seq) + 1);
        }
        break;
    case KCS_CTL:
        if (KCS_PULSE_MODE(value) > KCS_PULSE_MODE_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: reserved SCI pulse mode\n", TYPE_NPCM_KCS);
            return;
        }
        c->control = (value & KCS_CTL_WRITABLE) | KCS_CTL_EME;
        break;
    case KCS_IC:
        if (KCS_PULSE_MODE(value) > KCS_PULSE_MODE_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: reserved SMI pulse mode\n", TYPE_NPCM_KCS);
            return;
        }
        c->interrupt_control = value & ~KCS_IC_SCIIS;
        if (value & KCS_IC_SCIIS) {
            c->ibf_sci = true;
            npcm_kcs_trigger(&s->sci[channel]);
        }
        break;
    case KCS_IE:
        c->interrupt_enable = value & KCS_IE_MASK;
        break;
    default:
        goto invalid;
    }
    npcm_kcs_update_irq(s);
    return;
invalid:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid write at offset 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM_KCS, offset);
}

static bool npcm_kcs_accepts(void *opaque, hwaddr offset, unsigned size,
                             bool is_write, MemTxAttrs attrs)
{
    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid %u-byte access at offset 0x%" HWADDR_PRIx
                      "\n", TYPE_NPCM_KCS, size, offset);
        return false;
    }
    return true;
}

static const MemoryRegionOps npcm_kcs_ops = {
    .read = npcm_kcs_read,
    .write = npcm_kcs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        /* Reject wide transfers before they can consume the input latch. */
        .accepts = npcm_kcs_accepts,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void npcm_kcs_host_status_get(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    NPCMKCSChannel *c = opaque;
    uint32_t value = c->status;

    visit_type_uint32(v, name, &value, errp);
}

static void npcm_kcs_host_irq_get(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    bool irq = npcm_kcs_host_irq(opaque);

    visit_type_bool(v, name, &irq, errp);
}

static void npcm_kcs_host_signal_get(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    bool level = npcm_kcs_signal_level(opaque);

    visit_type_bool(v, name, &level, errp);
}

/* Reading host-data returns the BMC output byte without consuming it. */
static void npcm_kcs_host_data_get(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    NPCMKCSChannel *c = opaque;
    uint32_t value;

    if ((c->interrupt_enable & KCS_IE_HSTA) && !(c->status & KCS_ST_OBF)) {
        error_setg(errp, "KCS output buffer is empty; retry when OBF is set");
        return;
    }
    value = c->output;
    visit_type_uint32(v, name, &value, errp);
}

/* Writing true to host-ack consumes the output byte and clears OBF. */
static void npcm_kcs_host_ack_set(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    NPCMKCSState *s = NPCM_KCS(obj);
    NPCMKCSChannel *c = opaque;
    bool ack;

    if (!visit_type_bool(v, name, &ack, errp)) {
        return;
    }
    if (!ack) {
        error_setg(errp, "KCS host-ack only accepts true");
        return;
    }
    if ((c->interrupt_enable & KCS_IE_HSTA) && !(c->status & KCS_ST_OBF)) {
        error_setg(errp, "KCS output buffer is empty; nothing to acknowledge");
        return;
    }
    c->status &= ~KCS_ST_OBF;
    c->obf_sci = false;
    c->obf_smi = false;
    npcm_kcs_update_irq(s);
}

static void npcm_kcs_host_write(Object *obj, Visitor *v, const char *name,
                                NPCMKCSChannel *c, bool command, Error **errp)
{
    NPCMKCSState *s = NPCM_KCS(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > UINT8_MAX) {
        error_setg(errp, "KCS host writes require a byte (0..255)");
        return;
    }
    if ((c->interrupt_enable & KCS_IE_HSTA) && (c->status & KCS_ST_IBF)) {
        error_setg(errp, "KCS input buffer is full; retry when IBF is cleared");
        return;
    }
    c->input = value;
    c->status |= KCS_ST_IBF;
    c->ibf_sci = false;
    if (command) {
        c->status |= KCS_ST_CMD;
    } else {
        c->status &= ~KCS_ST_CMD;
    }
    npcm_kcs_update_irq(s);
}

static void npcm_kcs_host_data_set(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    npcm_kcs_host_write(obj, v, name, opaque, false, errp);
}

static void npcm_kcs_host_command_set(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    npcm_kcs_host_write(obj, v, name, opaque, true, errp);
}

static void npcm_kcs_reset_enter(Object *obj, ResetType type)
{
    NPCMKCSState *s = NPCM_KCS(obj);
    int i;

    memset(s->channels, 0, sizeof(s->channels));
    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        s->channels[i].control = KCS_CTL_EME | KCS_CTL_SCIPOL;
        s->channels[i].interrupt_control = KCS_IC_SMIPOL;
        timer_del(s->sci[i].timer);
        timer_del(s->smi[i].timer);
        s->sci[i].pulse = false;
        s->smi[i].pulse = false;
        if (s->shared) {
            NPCMKCSSharedChannel *shared = &s->shared->channels[i];

            /* Keep sequences monotonic: drop pending data, don't rewind. */
            qatomic_set(&shared->bmc_input_ack_seq,
                        qatomic_read(&shared->host_input_seq));
            qatomic_set(&shared->bmc_output_ack_seq,
                        qatomic_read(&shared->bmc_output_seq));
        }
    }
}

static void npcm_kcs_reset_hold(Object *obj, ResetType type)
{
    npcm_kcs_update_irq(NPCM_KCS(obj));
}

static int npcm_kcs_post_load(void *opaque, int version_id)
{
    npcm_kcs_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_npcm_kcs_channel = {
    .name = "npcm-kcs/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(status, NPCMKCSChannel),
        VMSTATE_UINT8(input, NPCMKCSChannel),
        VMSTATE_UINT8(output, NPCMKCSChannel),
        VMSTATE_UINT8(control, NPCMKCSChannel),
        VMSTATE_UINT8(interrupt_control, NPCMKCSChannel),
        VMSTATE_UINT8(interrupt_enable, NPCMKCSChannel),
        VMSTATE_BOOL(obf_sci, NPCMKCSChannel),
        VMSTATE_BOOL(obf_smi, NPCMKCSChannel),
        VMSTATE_BOOL(ibf_sci, NPCMKCSChannel),
        VMSTATE_END_OF_LIST(),
    },
};

static const VMStateDescription vmstate_npcm_kcs_signal = {
    .name = "npcm-kcs/signal",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(pulse, NPCMKCSSignal),
        VMSTATE_TIMER_PTR(timer, NPCMKCSSignal),
        VMSTATE_END_OF_LIST(),
    },
};

static const VMStateDescription vmstate_npcm_kcs = {
    .name = TYPE_NPCM_KCS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = npcm_kcs_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channels, NPCMKCSState, NPCM_KCS_NR_CHANNELS, 1,
                             vmstate_npcm_kcs_channel, NPCMKCSChannel),
        VMSTATE_STRUCT_ARRAY(sci, NPCMKCSState, NPCM_KCS_NR_CHANNELS, 1,
                             vmstate_npcm_kcs_signal, NPCMKCSSignal),
        VMSTATE_STRUCT_ARRAY(smi, NPCMKCSState, NPCM_KCS_NR_CHANNELS, 1,
                             vmstate_npcm_kcs_signal, NPCMKCSSignal),
        VMSTATE_CLOCK(clock, NPCMKCSState),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm_kcs_init(Object *obj)
{
    NPCMKCSState *s = NPCM_KCS(obj);
    int i;

    s->doorbell_in_fd = -1;
    s->doorbell_out_fd = -1;
    memory_region_init_io(&s->iomem, obj, &npcm_kcs_ops, s, "regs", 4 * KiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->clock = qdev_init_clock_in(DEVICE(s), "clock", NULL, NULL, 0);

    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        g_autofree char *data = g_strdup_printf("host-data%d", i + 1);
        g_autofree char *command = g_strdup_printf("host-command%d", i + 1);
        g_autofree char *ack = g_strdup_printf("host-ack%d", i + 1);
        g_autofree char *status = g_strdup_printf("host-status%d", i + 1);
        g_autofree char *irq = g_strdup_printf("host-irq%d", i + 1);
        g_autofree char *sci = g_strdup_printf("host-sci%d", i + 1);
        g_autofree char *smi = g_strdup_printf("host-smi%d", i + 1);
        NPCMKCSSignal *signals[] = { &s->sci[i], &s->smi[i] };
        int j;

        for (j = 0; j < ARRAY_SIZE(signals); j++) {
            signals[j]->controller = s;
            signals[j]->channel = i;
            signals[j]->smi = j == 1;
            signals[j]->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                             npcm_kcs_pulse_end, signals[j]);
        }

        object_property_add(obj, data, "uint32", npcm_kcs_host_data_get,
                            npcm_kcs_host_data_set, NULL, &s->channels[i]);
        object_property_add(obj, command, "uint32", NULL,
                            npcm_kcs_host_command_set, NULL, &s->channels[i]);
        object_property_add(obj, ack, "bool", NULL,
                            npcm_kcs_host_ack_set, NULL, &s->channels[i]);
        object_property_add(obj, status, "uint32", npcm_kcs_host_status_get,
                            NULL, NULL, &s->channels[i]);
        object_property_add(obj, irq, "bool", npcm_kcs_host_irq_get,
                            NULL, NULL, &s->channels[i]);
        object_property_add(obj, sci, "bool", npcm_kcs_host_signal_get,
                            NULL, NULL, &s->sci[i]);
        object_property_add(obj, smi, "bool", npcm_kcs_host_signal_get,
                            NULL, NULL, &s->smi[i]);
    }
    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        qdev_init_gpio_out_named(DEVICE(s), &s->sci[i].output, "sci", 1);
        qdev_init_gpio_out_named(DEVICE(s), &s->smi[i].output, "smi", 1);
    }
}

static void npcm_kcs_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    NPCMKCSState *s = NPCM_KCS(dev);

    if (!clock_has_source(s->clock)) {
        error_setg(errp, "NPCM KCS requires a connected APB clock");
        return;
    }
    npcm_kcs_ipc_setup(s, errp);
    if (*errp) {
        npcm_kcs_ipc_cleanup(s);
        return;
    }
    if (s->shared) {
        /* The shared file and doorbells are not part of the migration. */
        error_setg(&s->migration_blocker,
                   "%s: shared-memory IPC does not support migration",
                   TYPE_NPCM_KCS);
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            npcm_kcs_ipc_cleanup(s);
        }
    }
}

static void npcm_kcs_unrealize(DeviceState *dev)
{
    NPCMKCSState *s = NPCM_KCS(dev);

    migrate_del_blocker(&s->migration_blocker);
    npcm_kcs_ipc_cleanup(s);
}

static void npcm_kcs_finalize(Object *obj)
{
    NPCMKCSState *s = NPCM_KCS(obj);
    int i;

    for (i = 0; i < NPCM_KCS_NR_CHANNELS; i++) {
        timer_free(s->sci[i].timer);
        timer_free(s->smi[i].timer);
    }
}

static void npcm_kcs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_STRING("shm-path", NPCMKCSState, shm_path),
        DEFINE_PROP_STRING("doorbell-in", NPCMKCSState, doorbell_in_path),
        DEFINE_PROP_STRING("doorbell-out", NPCMKCSState, doorbell_out_path),
    };

    dc->desc = "NPCM BMC-side KCS channels";
    dc->realize = npcm_kcs_realize;
    dc->unrealize = npcm_kcs_unrealize;
    dc->vmsd = &vmstate_npcm_kcs;
    rc->phases.enter = npcm_kcs_reset_enter;
    rc->phases.hold = npcm_kcs_reset_hold;
    device_class_set_props(dc, properties);
}

static const TypeInfo npcm_kcs_types[] = {
    {
        .name = TYPE_NPCM_KCS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(NPCMKCSState),
        .instance_init = npcm_kcs_init,
        .instance_finalize = npcm_kcs_finalize,
        .class_init = npcm_kcs_class_init,
    },
};
DEFINE_TYPES(npcm_kcs_types)
