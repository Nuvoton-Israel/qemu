/*
 * Microchip LAN7800 USB 2.0 to 10/100/1000 Ethernet controller
 *
 * Models the parts of the device the Linux lan78xx driver relies on:
 * vendor register access, EEPROM, the internal LAN88xx PHY, and the
 * LAN78xx bulk framing with checksum, TSO and VLAN offloads.
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/usb/usb.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/eth.h"
#include "system/dma.h"
#include "system/system.h"
#include "hw/net/net_tx_pkt.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "desc.h"

#define TYPE_USB_LAN78XX "usb-lan78xx"
OBJECT_DECLARE_SIMPLE_TYPE(USBLan78xxState, USB_LAN78XX)

/* vendor requests */
#define LAN78XX_REQ_WRITE_REG       0xa0
#define LAN78XX_REQ_READ_REG        0xa1
#define LAN78XX_REQ_GET_STATS       0xa2

/* registers */
#define ID_REV                      0x000
#define INT_STS                     0x00c
#define HW_CFG                      0x010
#define PMT_CTL                     0x014
#define DP_SEL                      0x024
#define E2P_CMD                     0x040
#define E2P_DATA                    0x044
#define INT_EP_CTL                  0x098
#define RFE_CTL                     0x0b0
#define FCT_RX_CTL                  0x0c0
#define FCT_TX_CTL                  0x0c4
#define MAC_CR                      0x100
#define MAC_RX                      0x104
#define MAC_TX                      0x108
#define RX_ADDRH                    0x118
#define RX_ADDRL                    0x11c
#define MII_ACC                     0x120
#define MII_DATA                    0x124
#define MAF_BASE                    0x400
#define MAF_ENTRIES                 33
#define LAN78XX_REG_SPACE           0x1100

#define ID_REV_LAN7800              0x78000002

#define INT_STS_PHY_INT             BIT(17)
#define HW_CFG_SRST                 BIT(0)
#define HW_CFG_LRST                 BIT(1)
#define PMT_CTL_PHY_RST             BIT(4)
#define PMT_CTL_READY               BIT(7)
#define DP_SEL_DPRDY                BIT(31)
#define E2P_CMD_BUSY                BIT(31)
#define E2P_CMD_CMD_MASK            (7u << 28)
#define E2P_CMD_READ                (0u << 28)
#define E2P_CMD_EWDS                (1u << 28)
#define E2P_CMD_EWEN                (2u << 28)
#define E2P_CMD_WRITE               (3u << 28)
#define E2P_CMD_ADDR_MASK           0x1ff
#define RFE_CTL_MCAST_HASH          BIT(3)
#define RFE_CTL_VLAN_STRIP          BIT(7)
#define RFE_CTL_UCAST_EN            BIT(8)
#define RFE_CTL_MCAST_EN            BIT(9)
#define RFE_CTL_BCAST_EN            BIT(10)
#define FCT_CTL_DIS                 BIT(20)
#define FCT_CTL_EMPTY               BIT(21)
#define FCT_CTL_RST                 BIT(30)
#define FCT_CTL_EN                  BIT(31)
#define MAC_CR_RST                  BIT(0)
#define MAC_RXTX_EN                 BIT(0)
#define MAC_RXTX_DIS                BIT(1)
#define MII_ACC_BUSY                BIT(0)
#define MII_ACC_WRITE               BIT(1)
#define MII_ACC_REG(v)              (((v) >> 6) & 0x1f)
#define MII_ACC_PHY(v)              (((v) >> 11) & 0x1f)
#define MAF_HI_VALID                BIT(31)

/* bulk framing */
#define TX_CMD_LEN                  8
#define TX_CMD_A_LEN_MASK           0x000fffff
#define TX_CMD_A_IVTG               BIT(24)
#define TX_CMD_A_TPE                BIT(25)
#define TX_CMD_A_IPE                BIT(26)
#define TX_CMD_A_LSO                BIT(27)
#define TX_CMD_B_MSS(b)             (((b) >> 16) & 0x3fff)
#define TX_CMD_B_VTAG(b)            ((b) & 0xffff)
#define RX_CMD_LEN                  10
#define RX_CMD_A_ICSM               BIT(14)
#define RX_CMD_A_FVTG               BIT(23)
#define RX_CMD_A_MAM                BIT(24)
#define RX_CMD_A_BAM                BIT(25)

#define LAN78XX_MAX_FRAME           9022
#define LAN78XX_RX_BUF              (RX_CMD_LEN + LAN78XX_MAX_FRAME + 4)
#define LAN78XX_TX_BUF              (16 * KiB + 512)
#define LAN78XX_EEPROM_SIZE         512

/* internal LAN88xx PHY */
#define PHY_ADDR                    1
#define PHY_ID1                     0x0007
#define PHY_ID2                     0xc132
#define PHY_PAGE_REG                0x1f
#define PHY_PAGE_TR                 0x52b5
#define PHY_TR_CR                   0x10
#define PHY_TR_CR_DONE              BIT(15)
#define PHY_MMD_CTRL                0x0d
#define PHY_MMD_DATA                0x0e
#define PHY_PAGES                   3

#define BMCR_ANRESTART              BIT(9)
#define BMCR_RESET                  BIT(15)
#define BMSR_LINK                   BIT(2)
#define BMSR_ANEG_DONE              BIT(5)

struct USBLan78xxState {
    USBDevice dev;

    NICConf conf;
    NICState *nic;
    USBEndpoint *bulk_in;
    USBEndpoint *intr;
    struct NetTxPkt *tx_pkt;

    uint32_t regs[LAN78XX_REG_SPACE / 4];
    uint8_t eeprom[LAN78XX_EEPROM_SIZE];
    bool eeprom_wen;

    uint16_t phy[32];
    uint16_t phy_ext[PHY_PAGES][32];
    uint16_t phy_page;
    uint16_t phy_tr_cr;

    uint32_t int_pending;

    uint8_t rx_buf[LAN78XX_RX_BUF];
    uint32_t rx_len;
    uint32_t rx_pos;
    bool rx_zlp;

    uint8_t tx_buf[LAN78XX_TX_BUF];
    uint32_t tx_len;
};

static const USBDescIface desc_iface_hs = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 3,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0xff,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 512,
        }, {
            .bEndpointAddress = USB_DIR_OUT | 0x02,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 512,
        }, {
            .bEndpointAddress = USB_DIR_IN | 0x03,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = 16,
            .bInterval        = 4,
        },
    },
};

static const USBDescIface desc_iface_fs = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 3,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0xff,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 64,
        }, {
            .bEndpointAddress = USB_DIR_OUT | 0x02,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 64,
        }, {
            .bEndpointAddress = USB_DIR_IN | 0x03,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = 16,
            .bInterval        = 1,
        },
    },
};

static const USBDescDevice desc_device_hs = {
    .bcdUSB             = 0x0210,
    .bDeviceClass       = USB_CLASS_VENDOR_SPEC,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0xff,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |
                                   USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 1,
            .nif                 = 1,
            .ifs                 = &desc_iface_hs,
        },
    },
};

static const USBDescDevice desc_device_fs = {
    .bcdUSB             = 0x0210,
    .bDeviceClass       = USB_CLASS_VENDOR_SPEC,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0xff,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |
                                   USB_CFG_ATT_WAKEUP,
            .bMaxPower           = 1,
            .nif                 = 1,
            .ifs                 = &desc_iface_fs,
        },
    },
};

static const USBDescStrings desc_strings = {
    [0] = "",
};

static const USBDesc desc_lan78xx = {
    .id = {
        .idVendor  = 0x0424,
        .idProduct = 0x7800,
        .bcdDevice = 0x0300,
    },
    .full = &desc_device_fs,
    .high = &desc_device_hs,
    .str  = desc_strings,
};

static inline uint32_t reg_get(USBLan78xxState *s, uint32_t addr)
{
    return s->regs[addr / 4];
}

static inline void reg_set(USBLan78xxState *s, uint32_t addr, uint32_t val)
{
    s->regs[addr / 4] = val;
}

static bool lan78xx_link_up(USBLan78xxState *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);

    return nc->peer && !nc->link_down;
}

static void lan78xx_phy_reset(USBLan78xxState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    memset(s->phy_ext, 0, sizeof(s->phy_ext));
    s->phy[0x00] = 0x1140;      /* aneg enabled, full duplex, 1000 */
    s->phy[0x04] = 0x01e1;      /* advertise 10/100 half/full */
    s->phy[0x09] = 0x0300;      /* advertise 1000 half/full */
    s->phy_page = 0;
    s->phy_tr_cr = 0;
}

static uint16_t lan78xx_phy_read(USBLan78xxState *s, unsigned reg)
{
    bool link = lan78xx_link_up(s);

    if (s->phy_page == PHY_PAGE_TR && reg == PHY_TR_CR) {
        return s->phy_tr_cr;
    }
    if (reg != PHY_PAGE_REG && s->phy_page > 0 && s->phy_page < PHY_PAGES &&
        reg >= 0x10) {
        return s->phy_ext[s->phy_page][reg];
    }

    switch (reg) {
    case 0x01:  /* BMSR: 10/100 half/full, extended status, aneg able */
        return 0x7909 | (link ? BMSR_LINK | BMSR_ANEG_DONE : 0);
    case 0x02:
        return PHY_ID1;
    case 0x03:
        return PHY_ID2;
    case 0x05:  /* link partner: 10/100 half/full, ack */
        return link ? 0xc1e1 : 0;
    case 0x06:  /* link partner is aneg able */
        return link ? 0x0001 : 0;
    case 0x0a:  /* link partner: 1000 half/full, local/remote rx ok */
        return link ? 0x3c00 : 0;
    case 0x0f:  /* extended status: 1000BASE-T half/full */
        return 0x3000;
    case PHY_MMD_DATA:
        return 0;
    case PHY_PAGE_REG:
        return s->phy_page;
    default:
        return s->phy[reg];
    }
}

static void lan78xx_phy_write(USBLan78xxState *s, unsigned reg, uint16_t val)
{
    if (reg == PHY_PAGE_REG) {
        s->phy_page = val;
        return;
    }
    if (s->phy_page == PHY_PAGE_TR && reg == PHY_TR_CR) {
        s->phy_tr_cr = val | PHY_TR_CR_DONE;
        return;
    }
    if (s->phy_page > 0 && s->phy_page < PHY_PAGES && reg >= 0x10) {
        s->phy_ext[s->phy_page][reg] = val;
        return;
    }

    switch (reg) {
    case 0x00:
        if (val & BMCR_RESET) {
            lan78xx_phy_reset(s);
            return;
        }
        s->phy[reg] = val & ~BMCR_ANRESTART;
        break;
    case 0x01: case 0x02: case 0x03: case 0x05:
    case 0x06: case 0x0a: case 0x0f: case PHY_MMD_DATA:
        break;
    default:
        s->phy[reg] = val;
        break;
    }
}

static void lan78xx_reset_regs(USBLan78xxState *s)
{
    const uint8_t *mac = s->conf.macaddr.a;

    memset(s->regs, 0, sizeof(s->regs));
    reg_set(s, ID_REV, ID_REV_LAN7800);
    reg_set(s, PMT_CTL, PMT_CTL_READY);
    /* the MAC address the chip would have auto-loaded at power on */
    reg_set(s, RX_ADDRL, ldl_le_p(mac));
    reg_set(s, RX_ADDRH, lduw_le_p(mac + 4));
    s->int_pending = 0;
    s->eeprom_wen = false;
    s->rx_len = s->rx_pos = 0;
    s->rx_zlp = false;
    s->tx_len = 0;
    lan78xx_phy_reset(s);
}

static bool lan78xx_rx_enabled(USBLan78xxState *s)
{
    return (reg_get(s, MAC_RX) & MAC_RXTX_EN) &&
           (reg_get(s, FCT_RX_CTL) & FCT_CTL_EN);
}

static uint32_t lan78xx_read_reg(USBLan78xxState *s, uint32_t addr)
{
    uint32_t val;

    if (addr >= LAN78XX_REG_SPACE || (addr & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad register 0x%x\n",
                      __func__, addr);
        return 0;
    }
    val = reg_get(s, addr);

    switch (addr) {
    case MAC_RX:
    case MAC_TX:
        return (val & MAC_RXTX_EN) ? val & ~MAC_RXTX_DIS : val | MAC_RXTX_DIS;
    case FCT_RX_CTL:
        val |= (s->rx_len - s->rx_pos) ? 0 : FCT_CTL_EMPTY;
        return (val & FCT_CTL_EN) ? val : val | FCT_CTL_DIS;
    case FCT_TX_CTL:
        val |= FCT_CTL_EMPTY;
        return (val & FCT_CTL_EN) ? val : val | FCT_CTL_DIS;
    case DP_SEL:
        return val | DP_SEL_DPRDY;
    case PMT_CTL:
        return val | PMT_CTL_READY;
    default:
        return val;
    }
}

static void lan78xx_eeprom_cmd(USBLan78xxState *s, uint32_t cmd)
{
    unsigned addr = cmd & E2P_CMD_ADDR_MASK;

    switch (cmd & E2P_CMD_CMD_MASK) {
    case E2P_CMD_READ:
        reg_set(s, E2P_DATA, s->eeprom[addr]);
        break;
    case E2P_CMD_WRITE:
        if (s->eeprom_wen) {
            s->eeprom[addr] = reg_get(s, E2P_DATA);
        }
        break;
    case E2P_CMD_EWEN:
        s->eeprom_wen = true;
        break;
    case E2P_CMD_EWDS:
        s->eeprom_wen = false;
        break;
    default:
        break;
    }
    reg_set(s, E2P_CMD, cmd & ~E2P_CMD_BUSY);
}

static void lan78xx_mii_cmd(USBLan78xxState *s, uint32_t cmd)
{
    unsigned reg = MII_ACC_REG(cmd);
    bool present = MII_ACC_PHY(cmd) == PHY_ADDR;

    if (cmd & MII_ACC_WRITE) {
        if (present) {
            lan78xx_phy_write(s, reg, reg_get(s, MII_DATA));
        }
    } else {
        reg_set(s, MII_DATA, present ? lan78xx_phy_read(s, reg) : 0xffff);
    }
    reg_set(s, MII_ACC, cmd & ~MII_ACC_BUSY);
}

static void lan78xx_write_reg(USBLan78xxState *s, uint32_t addr, uint32_t val)
{
    if (addr >= LAN78XX_REG_SPACE || (addr & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad register 0x%x\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case ID_REV:
        return;
    case INT_STS:
        reg_set(s, INT_STS, reg_get(s, INT_STS) & ~val);
        return;
    case HW_CFG:
        reg_set(s, HW_CFG, val & ~(HW_CFG_LRST | HW_CFG_SRST));
        return;
    case PMT_CTL:
        if (val & PMT_CTL_PHY_RST) {
            lan78xx_phy_reset(s);
        }
        reg_set(s, PMT_CTL, (val & ~PMT_CTL_PHY_RST) | PMT_CTL_READY);
        return;
    case E2P_CMD:
        lan78xx_eeprom_cmd(s, val);
        return;
    case MII_ACC:
        lan78xx_mii_cmd(s, val);
        return;
    case MAC_CR:
        reg_set(s, MAC_CR, val & ~MAC_CR_RST);
        return;
    case FCT_RX_CTL:
        if (val & FCT_CTL_RST) {
            s->rx_len = s->rx_pos = 0;
            s->rx_zlp = false;
        }
        reg_set(s, addr, val & ~FCT_CTL_RST);
        break;
    case FCT_TX_CTL:
        if (val & FCT_CTL_RST) {
            s->tx_len = 0;
        }
        reg_set(s, addr, val & ~FCT_CTL_RST);
        return;
    default:
        reg_set(s, addr, val);
        break;
    }

    if ((addr == MAC_RX || addr == FCT_RX_CTL) && lan78xx_rx_enabled(s)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void lan78xx_handle_control(USBDevice *dev, USBPacket *p, int request,
                                   int value, int index, int length,
                                   uint8_t *data)
{
    USBLan78xxState *s = USB_LAN78XX(dev);

    if (usb_desc_handle_control(dev, p, request, value, index, length,
                                data) >= 0) {
        return;
    }

    switch (request) {
    case VendorDeviceRequest | LAN78XX_REQ_READ_REG:
        if (length < 4) {
            break;
        }
        stl_le_p(data, lan78xx_read_reg(s, index));
        p->actual_length = 4;
        return;
    case VendorDeviceOutRequest | LAN78XX_REQ_WRITE_REG:
        if (length < 4) {
            break;
        }
        lan78xx_write_reg(s, index, ldl_le_p(data));
        return;
    case VendorDeviceRequest | LAN78XX_REQ_GET_STATS:
        memset(data, 0, length);
        p->actual_length = length;
        return;
    default:
        break;
    }
    p->status = USB_RET_STALL;
}

static void lan78xx_free_frag(void *context, void *base, size_t len)
{
}

static void lan78xx_tx_frame(USBLan78xxState *s, uint32_t cmd_a,
                             uint32_t cmd_b, uint8_t *frame, size_t len)
{
    struct NetTxPkt *pkt = s->tx_pkt;

    net_tx_pkt_add_raw_fragment(pkt, frame, len);
    if (net_tx_pkt_parse(pkt)) {
        if (cmd_a & TX_CMD_A_IVTG) {
            net_tx_pkt_setup_vlan_header(pkt, TX_CMD_B_VTAG(cmd_b));
        }
        if (cmd_a & TX_CMD_A_LSO) {
            if (net_tx_pkt_build_vheader(pkt, true, true,
                                         TX_CMD_B_MSS(cmd_b))) {
                net_tx_pkt_update_ip_checksums(pkt);
            }
        } else {
            if (cmd_a & TX_CMD_A_TPE) {
                net_tx_pkt_build_vheader(pkt, false, true, 0);
            }
            if (cmd_a & TX_CMD_A_IPE) {
                net_tx_pkt_update_ip_hdr_checksum(pkt);
            }
        }
        net_tx_pkt_send(pkt, qemu_get_queue(s->nic));
    }
    net_tx_pkt_reset(pkt, lan78xx_free_frag, s);
}

/* A bulk OUT transfer carries one or more [cmd_a][cmd_b][frame] records */
static void lan78xx_tx_transfer(USBLan78xxState *s)
{
    uint32_t pos = 0;
    bool enabled = (reg_get(s, MAC_TX) & MAC_RXTX_EN) &&
                   (reg_get(s, FCT_TX_CTL) & FCT_CTL_EN);

    while (enabled) {
        uint32_t cmd_a, cmd_b, len;

        pos = ROUND_UP(pos, 4);
        if (pos + TX_CMD_LEN > s->tx_len) {
            break;
        }
        cmd_a = ldl_le_p(s->tx_buf + pos);
        cmd_b = ldl_le_p(s->tx_buf + pos + 4);
        len = cmd_a & TX_CMD_A_LEN_MASK;
        pos += TX_CMD_LEN;
        if (len == 0 || pos + len > s->tx_len) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad frame length %u\n",
                          __func__, len);
            break;
        }
        lan78xx_tx_frame(s, cmd_a, cmd_b, s->tx_buf + pos, len);
        pos += len;
    }
    s->tx_len = 0;
}

static void lan78xx_handle_bulk_out(USBLan78xxState *s, USBPacket *p)
{
    size_t size = p->iov.size;

    if (s->tx_len + size > sizeof(s->tx_buf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: transfer overflow\n", __func__);
        s->tx_len = 0;
        p->status = USB_RET_STALL;
        return;
    }
    usb_packet_copy(p, s->tx_buf + s->tx_len, size);
    s->tx_len += size;

    /* a short or zero-length packet ends the transfer */
    if (size % p->ep->max_packet_size || size == 0) {
        lan78xx_tx_transfer(s);
    }
}

static void lan78xx_handle_bulk_in(USBLan78xxState *s, USBPacket *p)
{
    uint32_t avail = s->rx_len - s->rx_pos;
    size_t n;

    if (!avail) {
        if (s->rx_zlp) {
            s->rx_zlp = false;
            /* can_receive() is true again; redeliver anything queued */
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
            return;
        }
        p->status = USB_RET_NAK;
        return;
    }

    n = MIN(avail, p->iov.size);
    usb_packet_copy(p, s->rx_buf + s->rx_pos, n);
    s->rx_pos += n;
    if (s->rx_pos == s->rx_len) {
        /* end the transfer explicitly if it filled whole packets */
        s->rx_zlp = !(s->rx_len % p->ep->max_packet_size);
        s->rx_len = s->rx_pos = 0;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void lan78xx_handle_intr(USBLan78xxState *s, USBPacket *p)
{
    uint32_t sts = s->int_pending & reg_get(s, INT_EP_CTL);
    uint8_t buf[4];

    if (!sts) {
        p->status = USB_RET_NAK;
        return;
    }
    stl_le_p(buf, sts);
    usb_packet_copy(p, buf, MIN(sizeof(buf), p->iov.size));
    s->int_pending &= ~sts;
}

static void lan78xx_handle_data(USBDevice *dev, USBPacket *p)
{
    USBLan78xxState *s = USB_LAN78XX(dev);

    switch (p->pid) {
    case USB_TOKEN_IN:
        switch (p->ep->nr) {
        case 1:
            lan78xx_handle_bulk_in(s, p);
            return;
        case 3:
            lan78xx_handle_intr(s, p);
            return;
        }
        break;
    case USB_TOKEN_OUT:
        if (p->ep->nr == 2) {
            lan78xx_handle_bulk_out(s, p);
            return;
        }
        break;
    }
    p->status = USB_RET_STALL;
}

static bool lan78xx_addr_accepted(USBLan78xxState *s, const uint8_t *da,
                                  uint32_t *flags)
{
    uint32_t rfe = reg_get(s, RFE_CTL);
    uint32_t lo = ldl_le_p(da), hi = lduw_le_p(da + 4);
    int i;

    if (is_broadcast_ether_addr(da)) {
        *flags |= RX_CMD_A_BAM;
        return rfe & RFE_CTL_BCAST_EN;
    }
    if (is_multicast_ether_addr(da)) {
        *flags |= RX_CMD_A_MAM;
        return rfe & (RFE_CTL_MCAST_EN | RFE_CTL_MCAST_HASH);
    }
    if (rfe & RFE_CTL_UCAST_EN) {
        return true;
    }
    if (lo == reg_get(s, RX_ADDRL) && hi == (reg_get(s, RX_ADDRH) & 0xffff)) {
        return true;
    }
    for (i = 0; i < MAF_ENTRIES; i++) {
        uint32_t maf_hi = reg_get(s, MAF_BASE + 8 * i);

        if ((maf_hi & MAF_HI_VALID) && hi == (maf_hi & 0xffff) &&
            lo == reg_get(s, MAF_BASE + 8 * i + 4)) {
            return true;
        }
    }
    return false;
}

static bool lan78xx_can_receive(NetClientState *nc)
{
    USBLan78xxState *s = qemu_get_nic_opaque(nc);

    if (!s->dev.config || !lan78xx_rx_enabled(s)) {
        return true;    /* dropped in receive */
    }
    return s->rx_len == 0 && !s->rx_zlp;
}

static ssize_t lan78xx_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    USBLan78xxState *s = qemu_get_nic_opaque(nc);
    uint32_t cmd_a = RX_CMD_A_ICSM, cmd_b = 0;
    uint8_t *dst = s->rx_buf + RX_CMD_LEN;
    size_t len = size;

    if (!s->dev.config || !lan78xx_rx_enabled(s) || size < ETH_HLEN) {
        return size;
    }
    if (s->rx_len || s->rx_zlp) {
        return 0;
    }
    if (size > LAN78XX_MAX_FRAME || !lan78xx_addr_accepted(s, buf, &cmd_a)) {
        return size;
    }

    if ((reg_get(s, RFE_CTL) & RFE_CTL_VLAN_STRIP) &&
        lduw_be_p(buf + 12) == ETH_P_VLAN && size >= ETH_HLEN + 4) {
        cmd_a |= RX_CMD_A_FVTG;
        cmd_b = lduw_be_p(buf + 14);
        memcpy(dst, buf, 12);
        memcpy(dst + 12, buf + 16, size - 16);
        len = size - 4;
    } else {
        memcpy(dst, buf, size);
    }

    /* the frame is followed by its FCS, which the driver drops */
    memset(dst + len, 0, 4);
    len += 4;
    stl_le_p(s->rx_buf, cmd_a | len);
    stl_le_p(s->rx_buf + 4, cmd_b);
    stw_le_p(s->rx_buf + 8, 0);
    s->rx_len = RX_CMD_LEN + len;
    s->rx_pos = 0;
    usb_wakeup(s->bulk_in, 0);
    return size;
}

static void lan78xx_set_link_status(NetClientState *nc)
{
    USBLan78xxState *s = qemu_get_nic_opaque(nc);

    reg_set(s, INT_STS, reg_get(s, INT_STS) | INT_STS_PHY_INT);
    s->int_pending |= INT_STS_PHY_INT;
    usb_wakeup(s->intr, 0);
}

static NetClientInfo net_lan78xx_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = lan78xx_can_receive,
    .receive = lan78xx_receive,
    .link_status_changed = lan78xx_set_link_status,
};

static void lan78xx_handle_reset(USBDevice *dev)
{
    lan78xx_reset_regs(USB_LAN78XX(dev));
}

static void lan78xx_realize(USBDevice *dev, Error **errp)
{
    USBLan78xxState *s = USB_LAN78XX(dev);

    usb_desc_init(dev);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 3);

    /* blank EEPROM: no 0xa5 signature, so the MAC comes from RX_ADDR */
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    net_tx_pkt_init(&s->tx_pkt, 1);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_lan78xx_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->qdev.id,
                          &dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    lan78xx_reset_regs(s);
}

static void lan78xx_unrealize(USBDevice *dev)
{
    USBLan78xxState *s = USB_LAN78XX(dev);

    net_tx_pkt_uninit(s->tx_pkt);
    qemu_del_nic(s->nic);
}

static void lan78xx_instance_init(Object *obj)
{
    USBLan78xxState *s = USB_LAN78XX(obj);

    device_add_bootindex_property(obj, &s->conf.bootindex, "bootindex",
                                  "/ethernet-phy@0", DEVICE(obj));
}

static const VMStateDescription vmstate_lan78xx = {
    .name = TYPE_USB_LAN78XX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBLan78xxState),
        VMSTATE_UINT32_ARRAY(regs, USBLan78xxState, LAN78XX_REG_SPACE / 4),
        VMSTATE_UINT8_ARRAY(eeprom, USBLan78xxState, LAN78XX_EEPROM_SIZE),
        VMSTATE_BOOL(eeprom_wen, USBLan78xxState),
        VMSTATE_UINT16_ARRAY(phy, USBLan78xxState, 32),
        VMSTATE_UINT16_2DARRAY(phy_ext, USBLan78xxState, PHY_PAGES, 32),
        VMSTATE_UINT16(phy_page, USBLan78xxState),
        VMSTATE_UINT16(phy_tr_cr, USBLan78xxState),
        VMSTATE_UINT32(int_pending, USBLan78xxState),
        VMSTATE_UINT8_ARRAY(rx_buf, USBLan78xxState, LAN78XX_RX_BUF),
        VMSTATE_UINT32(rx_len, USBLan78xxState),
        VMSTATE_UINT32(rx_pos, USBLan78xxState),
        VMSTATE_BOOL(rx_zlp, USBLan78xxState),
        VMSTATE_UINT8_ARRAY(tx_buf, USBLan78xxState, LAN78XX_TX_BUF),
        VMSTATE_UINT32(tx_len, USBLan78xxState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property lan78xx_properties[] = {
    DEFINE_NIC_PROPERTIES(USBLan78xxState, conf),
};

static void lan78xx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = lan78xx_realize;
    uc->unrealize      = lan78xx_unrealize;
    uc->product_desc   = "Microchip LAN7800 USB Ethernet";
    uc->usb_desc       = &desc_lan78xx;
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = lan78xx_handle_reset;
    uc->handle_control = lan78xx_handle_control;
    uc->handle_data    = lan78xx_handle_data;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->fw_name = "network";
    dc->vmsd = &vmstate_lan78xx;
    device_class_set_props(dc, lan78xx_properties);
}

static const TypeInfo lan78xx_info = {
    .name          = TYPE_USB_LAN78XX,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBLan78xxState),
    .class_init    = lan78xx_class_init,
    .instance_init = lan78xx_instance_init,
};

static void lan78xx_register_types(void)
{
    type_register_static(&lan78xx_info);
}

type_init(lan78xx_register_types)
