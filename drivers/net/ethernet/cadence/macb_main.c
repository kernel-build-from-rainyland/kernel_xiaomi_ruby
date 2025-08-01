/*
 * Cadence MACB/GEM Ethernet Controller driver
 *
 * Copyright (C) 2004-2006 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/clk.h>
#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/circ_buf.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/platform_data/macb.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include "macb.h"

#define MACB_RX_BUFFER_SIZE	128
#define RX_BUFFER_MULTIPLE	64  /* bytes */

#define DEFAULT_RX_RING_SIZE	512 /* must be power of 2 */
#define MIN_RX_RING_SIZE	64
#define MAX_RX_RING_SIZE	8192
#define RX_RING_BYTES(bp)	(macb_dma_desc_get_size(bp)	\
				 * (bp)->rx_ring_size)

#define DEFAULT_TX_RING_SIZE	512 /* must be power of 2 */
#define MIN_TX_RING_SIZE	64
#define MAX_TX_RING_SIZE	4096
#define TX_RING_BYTES(bp)	(macb_dma_desc_get_size(bp)	\
				 * (bp)->tx_ring_size)

/* level of occupied TX descriptors under which we wake up TX process */
#define MACB_TX_WAKEUP_THRESH(bp)	(3 * (bp)->tx_ring_size / 4)

#define MACB_RX_INT_FLAGS	(MACB_BIT(RCOMP) | MACB_BIT(ISR_ROVR))
#define MACB_TX_ERR_FLAGS	(MACB_BIT(ISR_TUND)			\
					| MACB_BIT(ISR_RLE)		\
					| MACB_BIT(TXERR))
#define MACB_TX_INT_FLAGS	(MACB_TX_ERR_FLAGS | MACB_BIT(TCOMP)	\
					| MACB_BIT(TXUBR))

/* Max length of transmit frame must be a multiple of 8 bytes */
#define MACB_TX_LEN_ALIGN	8
#define MACB_MAX_TX_LEN		((unsigned int)((1 << MACB_TX_FRMLEN_SIZE) - 1) & ~((unsigned int)(MACB_TX_LEN_ALIGN - 1)))
/* Limit maximum TX length as per Cadence TSO errata. This is to avoid a
 * false amba_error in TX path from the DMA assuming there is not enough
 * space in the SRAM (16KB) even when there is.
 */
#define GEM_MAX_TX_LEN		(unsigned int)(0x3FC0)

#define GEM_MTU_MIN_SIZE	ETH_MIN_MTU
#define MACB_NETIF_LSO		NETIF_F_TSO

#define MACB_WOL_HAS_MAGIC_PACKET	(0x1 << 0)
#define MACB_WOL_ENABLED		(0x1 << 1)

/* Graceful stop timeouts in us. We should allow up to
 * 1 frame time (10 Mbits/s, full-duplex, ignoring collisions)
 */
#define MACB_HALT_TIMEOUT	1230

/* DMA buffer descriptor might be different size
 * depends on hardware configuration:
 *
 * 1. dma address width 32 bits:
 *    word 1: 32 bit address of Data Buffer
 *    word 2: control
 *
 * 2. dma address width 64 bits:
 *    word 1: 32 bit address of Data Buffer
 *    word 2: control
 *    word 3: upper 32 bit address of Data Buffer
 *    word 4: unused
 *
 * 3. dma address width 32 bits with hardware timestamping:
 *    word 1: 32 bit address of Data Buffer
 *    word 2: control
 *    word 3: timestamp word 1
 *    word 4: timestamp word 2
 *
 * 4. dma address width 64 bits with hardware timestamping:
 *    word 1: 32 bit address of Data Buffer
 *    word 2: control
 *    word 3: upper 32 bit address of Data Buffer
 *    word 4: unused
 *    word 5: timestamp word 1
 *    word 6: timestamp word 2
 */
static unsigned int macb_dma_desc_get_size(struct macb *bp)
{
#ifdef MACB_EXT_DESC
	unsigned int desc_size;

	switch (bp->hw_dma_cap) {
	case HW_DMA_CAP_64B:
		desc_size = sizeof(struct macb_dma_desc)
			+ sizeof(struct macb_dma_desc_64);
		break;
	case HW_DMA_CAP_PTP:
		desc_size = sizeof(struct macb_dma_desc)
			+ sizeof(struct macb_dma_desc_ptp);
		break;
	case HW_DMA_CAP_64B_PTP:
		desc_size = sizeof(struct macb_dma_desc)
			+ sizeof(struct macb_dma_desc_64)
			+ sizeof(struct macb_dma_desc_ptp);
		break;
	default:
		desc_size = sizeof(struct macb_dma_desc);
	}
	return desc_size;
#endif
	return sizeof(struct macb_dma_desc);
}

static unsigned int macb_adj_dma_desc_idx(struct macb *bp, unsigned int desc_idx)
{
#ifdef MACB_EXT_DESC
	switch (bp->hw_dma_cap) {
	case HW_DMA_CAP_64B:
	case HW_DMA_CAP_PTP:
		desc_idx <<= 1;
		break;
	case HW_DMA_CAP_64B_PTP:
		desc_idx *= 3;
		break;
	default:
		break;
	}
#endif
	return desc_idx;
}

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
static struct macb_dma_desc_64 *macb_64b_desc(struct macb *bp, struct macb_dma_desc *desc)
{
	if (bp->hw_dma_cap & HW_DMA_CAP_64B)
		return (struct macb_dma_desc_64 *)((void *)desc + sizeof(struct macb_dma_desc));
	return NULL;
}
#endif

/* Ring buffer accessors */
static unsigned int macb_tx_ring_wrap(struct macb *bp, unsigned int index)
{
	return index & (bp->tx_ring_size - 1);
}

static struct macb_dma_desc *macb_tx_desc(struct macb_queue *queue,
					  unsigned int index)
{
	index = macb_tx_ring_wrap(queue->bp, index);
	index = macb_adj_dma_desc_idx(queue->bp, index);
	return &queue->tx_ring[index];
}

static struct macb_tx_skb *macb_tx_skb(struct macb_queue *queue,
				       unsigned int index)
{
	return &queue->tx_skb[macb_tx_ring_wrap(queue->bp, index)];
}

static dma_addr_t macb_tx_dma(struct macb_queue *queue, unsigned int index)
{
	dma_addr_t offset;

	offset = macb_tx_ring_wrap(queue->bp, index) *
			macb_dma_desc_get_size(queue->bp);

	return queue->tx_ring_dma + offset;
}

static unsigned int macb_rx_ring_wrap(struct macb *bp, unsigned int index)
{
	return index & (bp->rx_ring_size - 1);
}

static struct macb_dma_desc *macb_rx_desc(struct macb_queue *queue, unsigned int index)
{
	index = macb_rx_ring_wrap(queue->bp, index);
	index = macb_adj_dma_desc_idx(queue->bp, index);
	return &queue->rx_ring[index];
}

static void *macb_rx_buffer(struct macb_queue *queue, unsigned int index)
{
	return queue->rx_buffers + queue->bp->rx_buffer_size *
	       macb_rx_ring_wrap(queue->bp, index);
}

/* I/O accessors */
static u32 hw_readl_native(struct macb *bp, int offset)
{
	return __raw_readl(bp->regs + offset);
}

static void hw_writel_native(struct macb *bp, int offset, u32 value)
{
	__raw_writel(value, bp->regs + offset);
}

static u32 hw_readl(struct macb *bp, int offset)
{
	return readl_relaxed(bp->regs + offset);
}

static void hw_writel(struct macb *bp, int offset, u32 value)
{
	writel_relaxed(value, bp->regs + offset);
}

/* Find the CPU endianness by using the loopback bit of NCR register. When the
 * CPU is in big endian we need to program swapped mode for management
 * descriptor access.
 */
static bool hw_is_native_io(void __iomem *addr)
{
	u32 value = MACB_BIT(LLB);

	__raw_writel(value, addr + MACB_NCR);
	value = __raw_readl(addr + MACB_NCR);

	/* Write 0 back to disable everything */
	__raw_writel(0, addr + MACB_NCR);

	return value == MACB_BIT(LLB);
}

static bool hw_is_gem(void __iomem *addr, bool native_io)
{
	u32 id;

	if (native_io)
		id = __raw_readl(addr + MACB_MID);
	else
		id = readl_relaxed(addr + MACB_MID);

	return MACB_BFEXT(IDNUM, id) >= 0x2;
}

static void macb_set_hwaddr(struct macb *bp)
{
	u32 bottom;
	u16 top;

	bottom = cpu_to_le32(*((u32 *)bp->dev->dev_addr));
	macb_or_gem_writel(bp, SA1B, bottom);
	top = cpu_to_le16(*((u16 *)(bp->dev->dev_addr + 4)));
	macb_or_gem_writel(bp, SA1T, top);

	/* Clear unused address register sets */
	macb_or_gem_writel(bp, SA2B, 0);
	macb_or_gem_writel(bp, SA2T, 0);
	macb_or_gem_writel(bp, SA3B, 0);
	macb_or_gem_writel(bp, SA3T, 0);
	macb_or_gem_writel(bp, SA4B, 0);
	macb_or_gem_writel(bp, SA4T, 0);
}

static void macb_get_hwaddr(struct macb *bp)
{
	struct macb_platform_data *pdata;
	u32 bottom;
	u16 top;
	u8 addr[6];
	int i;

	pdata = dev_get_platdata(&bp->pdev->dev);

	/* Check all 4 address register for valid address */
	for (i = 0; i < 4; i++) {
		bottom = macb_or_gem_readl(bp, SA1B + i * 8);
		top = macb_or_gem_readl(bp, SA1T + i * 8);

		if (pdata && pdata->rev_eth_addr) {
			addr[5] = bottom & 0xff;
			addr[4] = (bottom >> 8) & 0xff;
			addr[3] = (bottom >> 16) & 0xff;
			addr[2] = (bottom >> 24) & 0xff;
			addr[1] = top & 0xff;
			addr[0] = (top & 0xff00) >> 8;
		} else {
			addr[0] = bottom & 0xff;
			addr[1] = (bottom >> 8) & 0xff;
			addr[2] = (bottom >> 16) & 0xff;
			addr[3] = (bottom >> 24) & 0xff;
			addr[4] = top & 0xff;
			addr[5] = (top >> 8) & 0xff;
		}

		if (is_valid_ether_addr(addr)) {
			memcpy(bp->dev->dev_addr, addr, sizeof(addr));
			return;
		}
	}

	dev_info(&bp->pdev->dev, "invalid hw address, using random\n");
	eth_hw_addr_random(bp->dev);
}

static int macb_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct macb *bp = bus->priv;
	int value;

	macb_writel(bp, MAN, (MACB_BF(SOF, MACB_MAN_SOF)
			      | MACB_BF(RW, MACB_MAN_READ)
			      | MACB_BF(PHYA, mii_id)
			      | MACB_BF(REGA, regnum)
			      | MACB_BF(CODE, MACB_MAN_CODE)));

	/* wait for end of transfer */
	while (!MACB_BFEXT(IDLE, macb_readl(bp, NSR)))
		cpu_relax();

	value = MACB_BFEXT(DATA, macb_readl(bp, MAN));

	return value;
}

static int macb_mdio_write(struct mii_bus *bus, int mii_id, int regnum,
			   u16 value)
{
	struct macb *bp = bus->priv;

	macb_writel(bp, MAN, (MACB_BF(SOF, MACB_MAN_SOF)
			      | MACB_BF(RW, MACB_MAN_WRITE)
			      | MACB_BF(PHYA, mii_id)
			      | MACB_BF(REGA, regnum)
			      | MACB_BF(CODE, MACB_MAN_CODE)
			      | MACB_BF(DATA, value)));

	/* wait for end of transfer */
	while (!MACB_BFEXT(IDLE, macb_readl(bp, NSR)))
		cpu_relax();

	return 0;
}

/**
 * macb_set_tx_clk() - Set a clock to a new frequency
 * @clk		Pointer to the clock to change
 * @rate	New frequency in Hz
 * @dev		Pointer to the struct net_device
 */
static void macb_set_tx_clk(struct clk *clk, int speed, struct net_device *dev)
{
	long ferr, rate, rate_rounded;

	if (!clk)
		return;

	switch (speed) {
	case SPEED_10:
		rate = 2500000;
		break;
	case SPEED_100:
		rate = 25000000;
		break;
	case SPEED_1000:
		rate = 125000000;
		break;
	default:
		return;
	}

	rate_rounded = clk_round_rate(clk, rate);
	if (rate_rounded < 0)
		return;

	/* RGMII allows 50 ppm frequency error. Test and warn if this limit
	 * is not satisfied.
	 */
	ferr = abs(rate_rounded - rate);
	ferr = DIV_ROUND_UP(ferr, rate / 100000);
	if (ferr > 5)
		netdev_warn(dev, "unable to generate target frequency: %ld Hz\n",
			    rate);

	if (clk_set_rate(clk, rate_rounded))
		netdev_err(dev, "adjusting tx_clk failed.\n");
}

static void macb_handle_link_change(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	struct phy_device *phydev = dev->phydev;
	unsigned long flags;
	int status_change = 0;

	spin_lock_irqsave(&bp->lock, flags);

	if (phydev->link) {
		if ((bp->speed != phydev->speed) ||
		    (bp->duplex != phydev->duplex)) {
			u32 reg;

			reg = macb_readl(bp, NCFGR);
			reg &= ~(MACB_BIT(SPD) | MACB_BIT(FD));
			if (macb_is_gem(bp))
				reg &= ~GEM_BIT(GBE);

			if (phydev->duplex)
				reg |= MACB_BIT(FD);
			if (phydev->speed == SPEED_100)
				reg |= MACB_BIT(SPD);
			if (phydev->speed == SPEED_1000 &&
			    bp->caps & MACB_CAPS_GIGABIT_MODE_AVAILABLE)
				reg |= GEM_BIT(GBE);

			macb_or_gem_writel(bp, NCFGR, reg);

			bp->speed = phydev->speed;
			bp->duplex = phydev->duplex;
			status_change = 1;
		}
	}

	if (phydev->link != bp->link) {
		if (!phydev->link) {
			bp->speed = 0;
			bp->duplex = -1;
		}
		bp->link = phydev->link;

		status_change = 1;
	}

	spin_unlock_irqrestore(&bp->lock, flags);

	if (status_change) {
		if (phydev->link) {
			/* Update the TX clock rate if and only if the link is
			 * up and there has been a link change.
			 */
			macb_set_tx_clk(bp->tx_clk, phydev->speed, dev);

			netif_carrier_on(dev);
			netdev_info(dev, "link up (%d/%s)\n",
				    phydev->speed,
				    phydev->duplex == DUPLEX_FULL ?
				    "Full" : "Half");
		} else {
			netif_carrier_off(dev);
			netdev_info(dev, "link down\n");
		}
	}
}

/* based on au1000_eth. c*/
static int macb_mii_probe(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	struct macb_platform_data *pdata;
	struct phy_device *phydev;
	struct device_node *np;
	int phy_irq, ret, i;

	pdata = dev_get_platdata(&bp->pdev->dev);
	np = bp->pdev->dev.of_node;
	ret = 0;

	if (np) {
		if (of_phy_is_fixed_link(np)) {
			bp->phy_node = of_node_get(np);
		} else {
			bp->phy_node = of_parse_phandle(np, "phy-handle", 0);
			/* fallback to standard phy registration if no
			 * phy-handle was found nor any phy found during
			 * dt phy registration
			 */
			if (!bp->phy_node && !phy_find_first(bp->mii_bus)) {
				for (i = 0; i < PHY_MAX_ADDR; i++) {
					struct phy_device *phydev;

					phydev = mdiobus_scan(bp->mii_bus, i);
					if (IS_ERR(phydev) &&
					    PTR_ERR(phydev) != -ENODEV) {
						ret = PTR_ERR(phydev);
						break;
					}
				}

				if (ret)
					return -ENODEV;
			}
		}
	}

	if (bp->phy_node) {
		phydev = of_phy_connect(dev, bp->phy_node,
					&macb_handle_link_change, 0,
					bp->phy_interface);
		if (!phydev)
			return -ENODEV;
	} else {
		phydev = phy_find_first(bp->mii_bus);
		if (!phydev) {
			netdev_err(dev, "no PHY found\n");
			return -ENXIO;
		}

		if (pdata) {
			if (gpio_is_valid(pdata->phy_irq_pin)) {
				ret = devm_gpio_request(&bp->pdev->dev,
							pdata->phy_irq_pin, "phy int");
				if (!ret) {
					phy_irq = gpio_to_irq(pdata->phy_irq_pin);
					phydev->irq = (phy_irq < 0) ? PHY_POLL : phy_irq;
				}
			} else {
				phydev->irq = PHY_POLL;
			}
		}

		/* attach the mac to the phy */
		ret = phy_connect_direct(dev, phydev, &macb_handle_link_change,
					 bp->phy_interface);
		if (ret) {
			netdev_err(dev, "Could not attach to PHY\n");
			return ret;
		}
	}

	/* mask with MAC supported features */
	if (macb_is_gem(bp) && bp->caps & MACB_CAPS_GIGABIT_MODE_AVAILABLE)
		phydev->supported &= PHY_GBIT_FEATURES;
	else
		phydev->supported &= PHY_BASIC_FEATURES;

	if (bp->caps & MACB_CAPS_NO_GIGABIT_HALF)
		phydev->supported &= ~SUPPORTED_1000baseT_Half;

	phydev->advertising = phydev->supported;

	bp->link = 0;
	bp->speed = 0;
	bp->duplex = -1;

	return 0;
}

static int macb_mii_init(struct macb *bp)
{
	struct macb_platform_data *pdata;
	struct device_node *np;
	int err = -ENXIO;

	/* Enable management port */
	macb_writel(bp, NCR, MACB_BIT(MPE));

	bp->mii_bus = mdiobus_alloc();
	if (!bp->mii_bus) {
		err = -ENOMEM;
		goto err_out;
	}

	bp->mii_bus->name = "MACB_mii_bus";
	bp->mii_bus->read = &macb_mdio_read;
	bp->mii_bus->write = &macb_mdio_write;
	snprintf(bp->mii_bus->id, MII_BUS_ID_SIZE, "%s-%x",
		 bp->pdev->name, bp->pdev->id);
	bp->mii_bus->priv = bp;
	bp->mii_bus->parent = &bp->pdev->dev;
	pdata = dev_get_platdata(&bp->pdev->dev);

	dev_set_drvdata(&bp->dev->dev, bp->mii_bus);

	np = bp->pdev->dev.of_node;
	if (np && of_phy_is_fixed_link(np)) {
		if (of_phy_register_fixed_link(np) < 0) {
			dev_err(&bp->pdev->dev,
				"broken fixed-link specification %pOF\n", np);
			goto err_out_free_mdiobus;
		}

		err = mdiobus_register(bp->mii_bus);
	} else {
		if (pdata)
			bp->mii_bus->phy_mask = pdata->phy_mask;

		err = of_mdiobus_register(bp->mii_bus, np);
	}

	if (err)
		goto err_out_free_fixed_link;

	err = macb_mii_probe(bp->dev);
	if (err)
		goto err_out_unregister_bus;

	return 0;

err_out_unregister_bus:
	mdiobus_unregister(bp->mii_bus);
err_out_free_fixed_link:
	if (np && of_phy_is_fixed_link(np))
		of_phy_deregister_fixed_link(np);
err_out_free_mdiobus:
	of_node_put(bp->phy_node);
	mdiobus_free(bp->mii_bus);
err_out:
	return err;
}

static void macb_update_stats(struct macb *bp)
{
	u32 *p = &bp->hw_stats.macb.rx_pause_frames;
	u32 *end = &bp->hw_stats.macb.tx_pause_frames + 1;
	int offset = MACB_PFR;

	WARN_ON((unsigned long)(end - p - 1) != (MACB_TPF - MACB_PFR) / 4);

	for (; p < end; p++, offset += 4)
		*p += bp->macb_reg_readl(bp, offset);
}

static int macb_halt_tx(struct macb *bp)
{
	unsigned long	halt_time, timeout;
	u32		status;

	macb_writel(bp, NCR, macb_readl(bp, NCR) | MACB_BIT(THALT));

	timeout = jiffies + usecs_to_jiffies(MACB_HALT_TIMEOUT);
	do {
		halt_time = jiffies;
		status = macb_readl(bp, TSR);
		if (!(status & MACB_BIT(TGO)))
			return 0;

		udelay(250);
	} while (time_before(halt_time, timeout));

	return -ETIMEDOUT;
}

static void macb_tx_unmap(struct macb *bp, struct macb_tx_skb *tx_skb)
{
	if (tx_skb->mapping) {
		if (tx_skb->mapped_as_page)
			dma_unmap_page(&bp->pdev->dev, tx_skb->mapping,
				       tx_skb->size, DMA_TO_DEVICE);
		else
			dma_unmap_single(&bp->pdev->dev, tx_skb->mapping,
					 tx_skb->size, DMA_TO_DEVICE);
		tx_skb->mapping = 0;
	}

	if (tx_skb->skb) {
		dev_kfree_skb_any(tx_skb->skb);
		tx_skb->skb = NULL;
	}
}

static void macb_set_addr(struct macb *bp, struct macb_dma_desc *desc, dma_addr_t addr)
{
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	struct macb_dma_desc_64 *desc_64;

	if (bp->hw_dma_cap & HW_DMA_CAP_64B) {
		desc_64 = macb_64b_desc(bp, desc);
		desc_64->addrh = upper_32_bits(addr);
		/* The low bits of RX address contain the RX_USED bit, clearing
		 * of which allows packet RX. Make sure the high bits are also
		 * visible to HW at that point.
		 */
		dma_wmb();
	}
#endif
	desc->addr = lower_32_bits(addr);
}

static dma_addr_t macb_get_addr(struct macb *bp, struct macb_dma_desc *desc)
{
	dma_addr_t addr = 0;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	struct macb_dma_desc_64 *desc_64;

	if (bp->hw_dma_cap & HW_DMA_CAP_64B) {
		desc_64 = macb_64b_desc(bp, desc);
		addr = ((u64)(desc_64->addrh) << 32);
	}
#endif
	addr |= MACB_BF(RX_WADDR, MACB_BFEXT(RX_WADDR, desc->addr));
#ifdef CONFIG_MACB_USE_HWSTAMP
	if (bp->hw_dma_cap & HW_DMA_CAP_PTP)
		addr &= ~GEM_BIT(DMA_RXVALID);
#endif
	return addr;
}

static void macb_tx_error_task(struct work_struct *work)
{
	struct macb_queue	*queue = container_of(work, struct macb_queue,
						      tx_error_task);
	struct macb		*bp = queue->bp;
	struct macb_tx_skb	*tx_skb;
	struct macb_dma_desc	*desc;
	struct sk_buff		*skb;
	unsigned int		tail;
	unsigned long		flags;

	netdev_vdbg(bp->dev, "macb_tx_error_task: q = %u, t = %u, h = %u\n",
		    (unsigned int)(queue - bp->queues),
		    queue->tx_tail, queue->tx_head);

	/* Prevent the queue IRQ handlers from running: each of them may call
	 * macb_tx_interrupt(), which in turn may call netif_wake_subqueue().
	 * As explained below, we have to halt the transmission before updating
	 * TBQP registers so we call netif_tx_stop_all_queues() to notify the
	 * network engine about the macb/gem being halted.
	 */
	spin_lock_irqsave(&bp->lock, flags);

	/* Make sure nobody is trying to queue up new packets */
	netif_tx_stop_all_queues(bp->dev);

	/* Stop transmission now
	 * (in case we have just queued new packets)
	 * macb/gem must be halted to write TBQP register
	 */
	if (macb_halt_tx(bp))
		/* Just complain for now, reinitializing TX path can be good */
		netdev_err(bp->dev, "BUG: halt tx timed out\n");

	/* Treat frames in TX queue including the ones that caused the error.
	 * Free transmit buffers in upper layer.
	 */
	for (tail = queue->tx_tail; tail != queue->tx_head; tail++) {
		u32	ctrl;

		desc = macb_tx_desc(queue, tail);
		ctrl = desc->ctrl;
		tx_skb = macb_tx_skb(queue, tail);
		skb = tx_skb->skb;

		if (ctrl & MACB_BIT(TX_USED)) {
			/* skb is set for the last buffer of the frame */
			while (!skb) {
				macb_tx_unmap(bp, tx_skb);
				tail++;
				tx_skb = macb_tx_skb(queue, tail);
				skb = tx_skb->skb;
			}

			/* ctrl still refers to the first buffer descriptor
			 * since it's the only one written back by the hardware
			 */
			if (!(ctrl & MACB_BIT(TX_BUF_EXHAUSTED))) {
				netdev_vdbg(bp->dev, "txerr skb %u (data %p) TX complete\n",
					    macb_tx_ring_wrap(bp, tail),
					    skb->data);
				bp->dev->stats.tx_packets++;
				queue->stats.tx_packets++;
				bp->dev->stats.tx_bytes += skb->len;
				queue->stats.tx_bytes += skb->len;
			}
		} else {
			/* "Buffers exhausted mid-frame" errors may only happen
			 * if the driver is buggy, so complain loudly about
			 * those. Statistics are updated by hardware.
			 */
			if (ctrl & MACB_BIT(TX_BUF_EXHAUSTED))
				netdev_err(bp->dev,
					   "BUG: TX buffers exhausted mid-frame\n");

			desc->ctrl = ctrl | MACB_BIT(TX_USED);
		}

		macb_tx_unmap(bp, tx_skb);
	}

	/* Set end of TX queue */
	desc = macb_tx_desc(queue, 0);
	macb_set_addr(bp, desc, 0);
	desc->ctrl = MACB_BIT(TX_USED);

	/* Make descriptor updates visible to hardware */
	wmb();

	/* Reinitialize the TX desc queue */
	queue_writel(queue, TBQP, lower_32_bits(queue->tx_ring_dma));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	if (bp->hw_dma_cap & HW_DMA_CAP_64B)
		queue_writel(queue, TBQPH, upper_32_bits(queue->tx_ring_dma));
#endif
	/* Make TX ring reflect state of hardware */
	queue->tx_head = 0;
	queue->tx_tail = 0;

	/* Housework before enabling TX IRQ */
	macb_writel(bp, TSR, macb_readl(bp, TSR));
	queue_writel(queue, IER, MACB_TX_INT_FLAGS);

	/* Now we are ready to start transmission again */
	netif_tx_start_all_queues(bp->dev);
	macb_writel(bp, NCR, macb_readl(bp, NCR) | MACB_BIT(TSTART));

	spin_unlock_irqrestore(&bp->lock, flags);
}

static void macb_tx_interrupt(struct macb_queue *queue)
{
	unsigned int tail;
	unsigned int head;
	u32 status;
	struct macb *bp = queue->bp;
	u16 queue_index = queue - bp->queues;

	status = macb_readl(bp, TSR);
	macb_writel(bp, TSR, status);

	if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
		queue_writel(queue, ISR, MACB_BIT(TCOMP));

	netdev_vdbg(bp->dev, "macb_tx_interrupt status = 0x%03lx\n",
		    (unsigned long)status);

	head = queue->tx_head;
	for (tail = queue->tx_tail; tail != head; tail++) {
		struct macb_tx_skb	*tx_skb;
		struct sk_buff		*skb;
		struct macb_dma_desc	*desc;
		u32			ctrl;

		desc = macb_tx_desc(queue, tail);

		/* Make hw descriptor updates visible to CPU */
		rmb();

		ctrl = desc->ctrl;

		/* TX_USED bit is only set by hardware on the very first buffer
		 * descriptor of the transmitted frame.
		 */
		if (!(ctrl & MACB_BIT(TX_USED)))
			break;

		/* Process all buffers of the current transmitted frame */
		for (;; tail++) {
			tx_skb = macb_tx_skb(queue, tail);
			skb = tx_skb->skb;

			/* First, update TX stats if needed */
			if (skb) {
				if (unlikely(skb_shinfo(skb)->tx_flags &
					     SKBTX_HW_TSTAMP) &&
				    gem_ptp_do_txstamp(queue, skb, desc) == 0) {
					/* skb now belongs to timestamp buffer
					 * and will be removed later
					 */
					tx_skb->skb = NULL;
				}
				netdev_vdbg(bp->dev, "skb %u (data %p) TX complete\n",
					    macb_tx_ring_wrap(bp, tail),
					    skb->data);
				bp->dev->stats.tx_packets++;
				queue->stats.tx_packets++;
				bp->dev->stats.tx_bytes += skb->len;
				queue->stats.tx_bytes += skb->len;
			}

			/* Now we can safely release resources */
			macb_tx_unmap(bp, tx_skb);

			/* skb is set only for the last buffer of the frame.
			 * WARNING: at this point skb has been freed by
			 * macb_tx_unmap().
			 */
			if (skb)
				break;
		}
	}

	queue->tx_tail = tail;
	if (__netif_subqueue_stopped(bp->dev, queue_index) &&
	    CIRC_CNT(queue->tx_head, queue->tx_tail,
		     bp->tx_ring_size) <= MACB_TX_WAKEUP_THRESH(bp))
		netif_wake_subqueue(bp->dev, queue_index);
}

static void gem_rx_refill(struct macb_queue *queue)
{
	unsigned int		entry;
	struct sk_buff		*skb;
	dma_addr_t		paddr;
	struct macb *bp = queue->bp;
	struct macb_dma_desc *desc;

	while (CIRC_SPACE(queue->rx_prepared_head, queue->rx_tail,
			bp->rx_ring_size) > 0) {
		entry = macb_rx_ring_wrap(bp, queue->rx_prepared_head);

		/* Make hw descriptor updates visible to CPU */
		rmb();

		desc = macb_rx_desc(queue, entry);

		if (!queue->rx_skbuff[entry]) {
			/* allocate sk_buff for this free entry in ring */
			skb = netdev_alloc_skb(bp->dev, bp->rx_buffer_size);
			if (unlikely(!skb)) {
				netdev_err(bp->dev,
					   "Unable to allocate sk_buff\n");
				break;
			}

			/* now fill corresponding descriptor entry */
			paddr = dma_map_single(&bp->pdev->dev, skb->data,
					       bp->rx_buffer_size,
					       DMA_FROM_DEVICE);
			if (dma_mapping_error(&bp->pdev->dev, paddr)) {
				dev_kfree_skb(skb);
				break;
			}

			queue->rx_skbuff[entry] = skb;

			if (entry == bp->rx_ring_size - 1)
				paddr |= MACB_BIT(RX_WRAP);
			desc->ctrl = 0;
			/* Setting addr clears RX_USED and allows reception,
			 * make sure ctrl is cleared first to avoid a race.
			 */
			dma_wmb();
			macb_set_addr(bp, desc, paddr);

			/* properly align Ethernet header */
			skb_reserve(skb, NET_IP_ALIGN);
		} else {
			desc->ctrl = 0;
			dma_wmb();
			desc->addr &= ~MACB_BIT(RX_USED);
		}
		queue->rx_prepared_head++;
	}

	/* Make descriptor updates visible to hardware */
	wmb();

	netdev_vdbg(bp->dev, "rx ring: queue: %p, prepared head %d, tail %d\n",
			queue, queue->rx_prepared_head, queue->rx_tail);
}

/* Mark DMA descriptors from begin up to and not including end as unused */
static void discard_partial_frame(struct macb_queue *queue, unsigned int begin,
				  unsigned int end)
{
	unsigned int frag;

	for (frag = begin; frag != end; frag++) {
		struct macb_dma_desc *desc = macb_rx_desc(queue, frag);

		desc->addr &= ~MACB_BIT(RX_USED);
	}

	/* Make descriptor updates visible to hardware */
	wmb();

	/* When this happens, the hardware stats registers for
	 * whatever caused this is updated, so we don't have to record
	 * anything.
	 */
}

static int gem_rx(struct macb_queue *queue, int budget)
{
	struct macb *bp = queue->bp;
	unsigned int		len;
	unsigned int		entry;
	struct sk_buff		*skb;
	struct macb_dma_desc	*desc;
	int			count = 0;

	while (count < budget) {
		u32 ctrl;
		dma_addr_t addr;
		bool rxused;

		entry = macb_rx_ring_wrap(bp, queue->rx_tail);
		desc = macb_rx_desc(queue, entry);

		/* Make hw descriptor updates visible to CPU */
		rmb();

		rxused = (desc->addr & MACB_BIT(RX_USED)) ? true : false;
		addr = macb_get_addr(bp, desc);

		if (!rxused)
			break;

		/* Ensure ctrl is at least as up-to-date as rxused */
		dma_rmb();

		ctrl = desc->ctrl;

		queue->rx_tail++;
		count++;

		if (!(ctrl & MACB_BIT(RX_SOF) && ctrl & MACB_BIT(RX_EOF))) {
			netdev_err(bp->dev,
				   "not whole frame pointed by descriptor\n");
			bp->dev->stats.rx_dropped++;
			queue->stats.rx_dropped++;
			break;
		}
		skb = queue->rx_skbuff[entry];
		if (unlikely(!skb)) {
			netdev_err(bp->dev,
				   "inconsistent Rx descriptor chain\n");
			bp->dev->stats.rx_dropped++;
			queue->stats.rx_dropped++;
			break;
		}
		/* now everything is ready for receiving packet */
		queue->rx_skbuff[entry] = NULL;
		len = ctrl & bp->rx_frm_len_mask;

		netdev_vdbg(bp->dev, "gem_rx %u (len %u)\n", entry, len);

		skb_put(skb, len);
		dma_unmap_single(&bp->pdev->dev, addr,
				 bp->rx_buffer_size, DMA_FROM_DEVICE);

		skb->protocol = eth_type_trans(skb, bp->dev);
		skb_checksum_none_assert(skb);
		if (bp->dev->features & NETIF_F_RXCSUM &&
		    !(bp->dev->flags & IFF_PROMISC) &&
		    GEM_BFEXT(RX_CSUM, ctrl) & GEM_RX_CSUM_CHECKED_MASK)
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		bp->dev->stats.rx_packets++;
		queue->stats.rx_packets++;
		bp->dev->stats.rx_bytes += skb->len;
		queue->stats.rx_bytes += skb->len;

		gem_ptp_do_rxstamp(bp, skb, desc);

#if defined(DEBUG) && defined(VERBOSE_DEBUG)
		netdev_vdbg(bp->dev, "received skb of length %u, csum: %08x\n",
			    skb->len, skb->csum);
		print_hex_dump(KERN_DEBUG, " mac: ", DUMP_PREFIX_ADDRESS, 16, 1,
			       skb_mac_header(skb), 16, true);
		print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_ADDRESS, 16, 1,
			       skb->data, 32, true);
#endif

		netif_receive_skb(skb);
	}

	gem_rx_refill(queue);

	return count;
}

static int macb_rx_frame(struct macb_queue *queue, unsigned int first_frag,
			 unsigned int last_frag)
{
	unsigned int len;
	unsigned int frag;
	unsigned int offset;
	struct sk_buff *skb;
	struct macb_dma_desc *desc;
	struct macb *bp = queue->bp;

	desc = macb_rx_desc(queue, last_frag);
	len = desc->ctrl & bp->rx_frm_len_mask;

	netdev_vdbg(bp->dev, "macb_rx_frame frags %u - %u (len %u)\n",
		macb_rx_ring_wrap(bp, first_frag),
		macb_rx_ring_wrap(bp, last_frag), len);

	/* The ethernet header starts NET_IP_ALIGN bytes into the
	 * first buffer. Since the header is 14 bytes, this makes the
	 * payload word-aligned.
	 *
	 * Instead of calling skb_reserve(NET_IP_ALIGN), we just copy
	 * the two padding bytes into the skb so that we avoid hitting
	 * the slowpath in memcpy(), and pull them off afterwards.
	 */
	skb = netdev_alloc_skb(bp->dev, len + NET_IP_ALIGN);
	if (!skb) {
		bp->dev->stats.rx_dropped++;
		for (frag = first_frag; ; frag++) {
			desc = macb_rx_desc(queue, frag);
			desc->addr &= ~MACB_BIT(RX_USED);
			if (frag == last_frag)
				break;
		}

		/* Make descriptor updates visible to hardware */
		wmb();

		return 1;
	}

	offset = 0;
	len += NET_IP_ALIGN;
	skb_checksum_none_assert(skb);
	skb_put(skb, len);

	for (frag = first_frag; ; frag++) {
		unsigned int frag_len = bp->rx_buffer_size;

		if (offset + frag_len > len) {
			if (unlikely(frag != last_frag)) {
				dev_kfree_skb_any(skb);
				return -1;
			}
			frag_len = len - offset;
		}
		skb_copy_to_linear_data_offset(skb, offset,
					       macb_rx_buffer(queue, frag),
					       frag_len);
		offset += bp->rx_buffer_size;
		desc = macb_rx_desc(queue, frag);
		desc->addr &= ~MACB_BIT(RX_USED);

		if (frag == last_frag)
			break;
	}

	/* Make descriptor updates visible to hardware */
	wmb();

	__skb_pull(skb, NET_IP_ALIGN);
	skb->protocol = eth_type_trans(skb, bp->dev);

	bp->dev->stats.rx_packets++;
	bp->dev->stats.rx_bytes += skb->len;
	netdev_vdbg(bp->dev, "received skb of length %u, csum: %08x\n",
		    skb->len, skb->csum);
	netif_receive_skb(skb);

	return 0;
}

static inline void macb_init_rx_ring(struct macb_queue *queue)
{
	struct macb *bp = queue->bp;
	dma_addr_t addr;
	struct macb_dma_desc *desc = NULL;
	int i;

	addr = queue->rx_buffers_dma;
	for (i = 0; i < bp->rx_ring_size; i++) {
		desc = macb_rx_desc(queue, i);
		macb_set_addr(bp, desc, addr);
		desc->ctrl = 0;
		addr += bp->rx_buffer_size;
	}
	desc->addr |= MACB_BIT(RX_WRAP);
	queue->rx_tail = 0;
}

static int macb_rx(struct macb_queue *queue, int budget)
{
	struct macb *bp = queue->bp;
	bool reset_rx_queue = false;
	int received = 0;
	unsigned int tail;
	int first_frag = -1;

	for (tail = queue->rx_tail; budget > 0; tail++) {
		struct macb_dma_desc *desc = macb_rx_desc(queue, tail);
		u32 ctrl;

		/* Make hw descriptor updates visible to CPU */
		rmb();

		if (!(desc->addr & MACB_BIT(RX_USED)))
			break;

		/* Ensure ctrl is at least as up-to-date as addr */
		dma_rmb();

		ctrl = desc->ctrl;

		if (ctrl & MACB_BIT(RX_SOF)) {
			if (first_frag != -1)
				discard_partial_frame(queue, first_frag, tail);
			first_frag = tail;
		}

		if (ctrl & MACB_BIT(RX_EOF)) {
			int dropped;

			if (unlikely(first_frag == -1)) {
				reset_rx_queue = true;
				continue;
			}

			dropped = macb_rx_frame(queue, first_frag, tail);
			first_frag = -1;
			if (unlikely(dropped < 0)) {
				reset_rx_queue = true;
				continue;
			}
			if (!dropped) {
				received++;
				budget--;
			}
		}
	}

	if (unlikely(reset_rx_queue)) {
		unsigned long flags;
		u32 ctrl;

		netdev_err(bp->dev, "RX queue corruption: reset it\n");

		spin_lock_irqsave(&bp->lock, flags);

		ctrl = macb_readl(bp, NCR);
		macb_writel(bp, NCR, ctrl & ~MACB_BIT(RE));

		macb_init_rx_ring(queue);
		queue_writel(queue, RBQP, queue->rx_ring_dma);

		macb_writel(bp, NCR, ctrl | MACB_BIT(RE));

		spin_unlock_irqrestore(&bp->lock, flags);
		return received;
	}

	if (first_frag != -1)
		queue->rx_tail = first_frag;
	else
		queue->rx_tail = tail;

	return received;
}

static int macb_poll(struct napi_struct *napi, int budget)
{
	struct macb_queue *queue = container_of(napi, struct macb_queue, napi);
	struct macb *bp = queue->bp;
	int work_done;
	u32 status;

	status = macb_readl(bp, RSR);
	macb_writel(bp, RSR, status);

	netdev_vdbg(bp->dev, "poll: status = %08lx, budget = %d\n",
		    (unsigned long)status, budget);

	work_done = bp->macbgem_ops.mog_rx(queue, budget);
	if (work_done < budget) {
		napi_complete_done(napi, work_done);

		/* RSR bits only seem to propagate to raise interrupts when
		 * interrupts are enabled at the time, so if bits are already
		 * set due to packets received while interrupts were disabled,
		 * they will not cause another interrupt to be generated when
		 * interrupts are re-enabled.
		 * Check for this case here. This has been seen to happen
		 * around 30% of the time under heavy network load.
		 */
		status = macb_readl(bp, RSR);
		if (status) {
			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_BIT(RCOMP));
			napi_reschedule(napi);
		} else {
			queue_writel(queue, IER, bp->rx_intr_mask);

			/* In rare cases, packets could have been received in
			 * the window between the check above and re-enabling
			 * interrupts. Therefore, a double-check is required
			 * to avoid losing a wakeup. This can potentially race
			 * with the interrupt handler doing the same actions
			 * if an interrupt is raised just after enabling them,
			 * but this should be harmless.
			 */
			status = macb_readl(bp, RSR);
			if (unlikely(status)) {
				queue_writel(queue, IDR, bp->rx_intr_mask);
				if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
					queue_writel(queue, ISR, MACB_BIT(RCOMP));
				napi_schedule(napi);
			}
		}
	}

	/* TODO: Handle errors */

	return work_done;
}

static void macb_hresp_error_task(unsigned long data)
{
	struct macb *bp = (struct macb *)data;
	struct net_device *dev = bp->dev;
	struct macb_queue *queue = bp->queues;
	unsigned int q;
	u32 ctrl;

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		queue_writel(queue, IDR, bp->rx_intr_mask |
					 MACB_TX_INT_FLAGS |
					 MACB_BIT(HRESP));
	}
	ctrl = macb_readl(bp, NCR);
	ctrl &= ~(MACB_BIT(RE) | MACB_BIT(TE));
	macb_writel(bp, NCR, ctrl);

	netif_tx_stop_all_queues(dev);
	netif_carrier_off(dev);

	bp->macbgem_ops.mog_init_rings(bp);

	/* Initialize TX and RX buffers */
	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		queue_writel(queue, RBQP, lower_32_bits(queue->rx_ring_dma));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		if (bp->hw_dma_cap & HW_DMA_CAP_64B)
			queue_writel(queue, RBQPH,
				     upper_32_bits(queue->rx_ring_dma));
#endif
		queue_writel(queue, TBQP, lower_32_bits(queue->tx_ring_dma));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		if (bp->hw_dma_cap & HW_DMA_CAP_64B)
			queue_writel(queue, TBQPH,
				     upper_32_bits(queue->tx_ring_dma));
#endif

		/* Enable interrupts */
		queue_writel(queue, IER,
			     bp->rx_intr_mask |
			     MACB_TX_INT_FLAGS |
			     MACB_BIT(HRESP));
	}

	ctrl |= MACB_BIT(RE) | MACB_BIT(TE);
	macb_writel(bp, NCR, ctrl);

	netif_carrier_on(dev);
	netif_tx_start_all_queues(dev);
}

static void macb_tx_restart(struct macb_queue *queue)
{
	unsigned int head = queue->tx_head;
	unsigned int tail = queue->tx_tail;
	struct macb *bp = queue->bp;
	unsigned int head_idx, tbqp;

	if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
		queue_writel(queue, ISR, MACB_BIT(TXUBR));

	if (head == tail)
		return;

	tbqp = queue_readl(queue, TBQP) / macb_dma_desc_get_size(bp);
	tbqp = macb_adj_dma_desc_idx(bp, macb_tx_ring_wrap(bp, tbqp));
	head_idx = macb_adj_dma_desc_idx(bp, macb_tx_ring_wrap(bp, head));

	if (tbqp == head_idx)
		return;

	macb_writel(bp, NCR, macb_readl(bp, NCR) | MACB_BIT(TSTART));
}

static irqreturn_t macb_interrupt(int irq, void *dev_id)
{
	struct macb_queue *queue = dev_id;
	struct macb *bp = queue->bp;
	struct net_device *dev = bp->dev;
	u32 status, ctrl;

	status = queue_readl(queue, ISR);

	if (unlikely(!status))
		return IRQ_NONE;

	spin_lock(&bp->lock);

	while (status) {
		/* close possible race with dev_close */
		if (unlikely(!netif_running(dev))) {
			queue_writel(queue, IDR, -1);
			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, -1);
			break;
		}

		netdev_vdbg(bp->dev, "queue = %u, isr = 0x%08lx\n",
			    (unsigned int)(queue - bp->queues),
			    (unsigned long)status);

		if (status & bp->rx_intr_mask) {
			/* There's no point taking any more interrupts
			 * until we have processed the buffers. The
			 * scheduling call may fail if the poll routine
			 * is already scheduled, so disable interrupts
			 * now.
			 */
			queue_writel(queue, IDR, bp->rx_intr_mask);
			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_BIT(RCOMP));

			if (napi_schedule_prep(&queue->napi)) {
				netdev_vdbg(bp->dev, "scheduling RX softirq\n");
				__napi_schedule(&queue->napi);
			}
		}

		if (unlikely(status & (MACB_TX_ERR_FLAGS))) {
			queue_writel(queue, IDR, MACB_TX_INT_FLAGS);
			schedule_work(&queue->tx_error_task);

			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_TX_ERR_FLAGS);

			break;
		}

		if (status & MACB_BIT(TCOMP))
			macb_tx_interrupt(queue);

		if (status & MACB_BIT(TXUBR))
			macb_tx_restart(queue);

		/* Link change detection isn't possible with RMII, so we'll
		 * add that if/when we get our hands on a full-blown MII PHY.
		 */

		/* There is a hardware issue under heavy load where DMA can
		 * stop, this causes endless "used buffer descriptor read"
		 * interrupts but it can be cleared by re-enabling RX. See
		 * the at91rm9200 manual, section 41.3.1 or the Zynq manual
		 * section 16.7.4 for details. RXUBR is only enabled for
		 * these two versions.
		 */
		if (status & MACB_BIT(RXUBR)) {
			ctrl = macb_readl(bp, NCR);
			macb_writel(bp, NCR, ctrl & ~MACB_BIT(RE));
			wmb();
			macb_writel(bp, NCR, ctrl | MACB_BIT(RE));

			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_BIT(RXUBR));
		}

		if (status & MACB_BIT(ISR_ROVR)) {
			/* We missed at least one packet */
			spin_lock(&bp->stats_lock);
			if (macb_is_gem(bp))
				bp->hw_stats.gem.rx_overruns++;
			else
				bp->hw_stats.macb.rx_overruns++;
			spin_unlock(&bp->stats_lock);

			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_BIT(ISR_ROVR));
		}

		if (status & MACB_BIT(HRESP)) {
			tasklet_schedule(&bp->hresp_err_tasklet);
			netdev_err(dev, "DMA bus error: HRESP not OK\n");

			if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
				queue_writel(queue, ISR, MACB_BIT(HRESP));
		}
		status = queue_readl(queue, ISR);
	}

	spin_unlock(&bp->lock);

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/* Polling receive - used by netconsole and other diagnostic tools
 * to allow network i/o with interrupts disabled.
 */
static void macb_poll_controller(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	struct macb_queue *queue;
	unsigned long flags;
	unsigned int q;

	local_irq_save(flags);
	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue)
		macb_interrupt(dev->irq, queue);
	local_irq_restore(flags);
}
#endif

static unsigned int macb_tx_map(struct macb *bp,
				struct macb_queue *queue,
				struct sk_buff *skb,
				unsigned int hdrlen)
{
	dma_addr_t mapping;
	unsigned int len, entry, i, tx_head = queue->tx_head;
	struct macb_tx_skb *tx_skb = NULL;
	struct macb_dma_desc *desc;
	unsigned int offset, size, count = 0;
	unsigned int f, nr_frags = skb_shinfo(skb)->nr_frags;
	unsigned int eof = 1, mss_mfs = 0;
	u32 ctrl, lso_ctrl = 0, seq_ctrl = 0;

	/* LSO */
	if (skb_shinfo(skb)->gso_size != 0) {
		if (ip_hdr(skb)->protocol == IPPROTO_UDP)
			/* UDP - UFO */
			lso_ctrl = MACB_LSO_UFO_ENABLE;
		else
			/* TCP - TSO */
			lso_ctrl = MACB_LSO_TSO_ENABLE;
	}

	/* First, map non-paged data */
	len = skb_headlen(skb);

	/* first buffer length */
	size = hdrlen;

	offset = 0;
	while (len) {
		entry = macb_tx_ring_wrap(bp, tx_head);
		tx_skb = &queue->tx_skb[entry];

		mapping = dma_map_single(&bp->pdev->dev,
					 skb->data + offset,
					 size, DMA_TO_DEVICE);
		if (dma_mapping_error(&bp->pdev->dev, mapping))
			goto dma_error;

		/* Save info to properly release resources */
		tx_skb->skb = NULL;
		tx_skb->mapping = mapping;
		tx_skb->size = size;
		tx_skb->mapped_as_page = false;

		len -= size;
		offset += size;
		count++;
		tx_head++;

		size = min(len, bp->max_tx_length);
	}

	/* Then, map paged data from fragments */
	for (f = 0; f < nr_frags; f++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[f];

		len = skb_frag_size(frag);
		offset = 0;
		while (len) {
			size = min(len, bp->max_tx_length);
			entry = macb_tx_ring_wrap(bp, tx_head);
			tx_skb = &queue->tx_skb[entry];

			mapping = skb_frag_dma_map(&bp->pdev->dev, frag,
						   offset, size, DMA_TO_DEVICE);
			if (dma_mapping_error(&bp->pdev->dev, mapping))
				goto dma_error;

			/* Save info to properly release resources */
			tx_skb->skb = NULL;
			tx_skb->mapping = mapping;
			tx_skb->size = size;
			tx_skb->mapped_as_page = true;

			len -= size;
			offset += size;
			count++;
			tx_head++;
		}
	}

	/* Should never happen */
	if (unlikely(!tx_skb)) {
		netdev_err(bp->dev, "BUG! empty skb!\n");
		return 0;
	}

	/* This is the last buffer of the frame: save socket buffer */
	tx_skb->skb = skb;

	/* Update TX ring: update buffer descriptors in reverse order
	 * to avoid race condition
	 */

	/* Set 'TX_USED' bit in buffer descriptor at tx_head position
	 * to set the end of TX queue
	 */
	i = tx_head;
	entry = macb_tx_ring_wrap(bp, i);
	ctrl = MACB_BIT(TX_USED);
	desc = macb_tx_desc(queue, entry);
	desc->ctrl = ctrl;

	if (lso_ctrl) {
		if (lso_ctrl == MACB_LSO_UFO_ENABLE)
			/* include header and FCS in value given to h/w */
			mss_mfs = skb_shinfo(skb)->gso_size +
					skb_transport_offset(skb) +
					ETH_FCS_LEN;
		else /* TSO */ {
			mss_mfs = skb_shinfo(skb)->gso_size;
			/* TCP Sequence Number Source Select
			 * can be set only for TSO
			 */
			seq_ctrl = 0;
		}
	}

	do {
		i--;
		entry = macb_tx_ring_wrap(bp, i);
		tx_skb = &queue->tx_skb[entry];
		desc = macb_tx_desc(queue, entry);

		ctrl = (u32)tx_skb->size;
		if (eof) {
			ctrl |= MACB_BIT(TX_LAST);
			eof = 0;
		}
		if (unlikely(entry == (bp->tx_ring_size - 1)))
			ctrl |= MACB_BIT(TX_WRAP);

		/* First descriptor is header descriptor */
		if (i == queue->tx_head) {
			ctrl |= MACB_BF(TX_LSO, lso_ctrl);
			ctrl |= MACB_BF(TX_TCP_SEQ_SRC, seq_ctrl);
			if ((bp->dev->features & NETIF_F_HW_CSUM) &&
			    skb->ip_summed != CHECKSUM_PARTIAL && !lso_ctrl)
				ctrl |= MACB_BIT(TX_NOCRC);
		} else
			/* Only set MSS/MFS on payload descriptors
			 * (second or later descriptor)
			 */
			ctrl |= MACB_BF(MSS_MFS, mss_mfs);

		/* Set TX buffer descriptor */
		macb_set_addr(bp, desc, tx_skb->mapping);
		/* desc->addr must be visible to hardware before clearing
		 * 'TX_USED' bit in desc->ctrl.
		 */
		wmb();
		desc->ctrl = ctrl;
	} while (i != queue->tx_head);

	queue->tx_head = tx_head;

	return count;

dma_error:
	netdev_err(bp->dev, "TX DMA map failed\n");

	for (i = queue->tx_head; i != tx_head; i++) {
		tx_skb = macb_tx_skb(queue, i);

		macb_tx_unmap(bp, tx_skb);
	}

	return 0;
}

static netdev_features_t macb_features_check(struct sk_buff *skb,
					     struct net_device *dev,
					     netdev_features_t features)
{
	unsigned int nr_frags, f;
	unsigned int hdrlen;

	/* Validate LSO compatibility */

	/* there is only one buffer or protocol is not UDP */
	if (!skb_is_nonlinear(skb) || (ip_hdr(skb)->protocol != IPPROTO_UDP))
		return features;

	/* length of header */
	hdrlen = skb_transport_offset(skb);

	/* For UFO only:
	 * When software supplies two or more payload buffers all payload buffers
	 * apart from the last must be a multiple of 8 bytes in size.
	 */
	if (!IS_ALIGNED(skb_headlen(skb) - hdrlen, MACB_TX_LEN_ALIGN))
		return features & ~MACB_NETIF_LSO;

	nr_frags = skb_shinfo(skb)->nr_frags;
	/* No need to check last fragment */
	nr_frags--;
	for (f = 0; f < nr_frags; f++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[f];

		if (!IS_ALIGNED(skb_frag_size(frag), MACB_TX_LEN_ALIGN))
			return features & ~MACB_NETIF_LSO;
	}
	return features;
}

static inline int macb_clear_csum(struct sk_buff *skb)
{
	/* no change for packets without checksum offloading */
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	/* make sure we can modify the header */
	if (unlikely(skb_cow_head(skb, 0)))
		return -1;

	/* initialize checksum field
	 * This is required - at least for Zynq, which otherwise calculates
	 * wrong UDP header checksums for UDP packets with UDP data len <=2
	 */
	*(__sum16 *)(skb_checksum_start(skb) + skb->csum_offset) = 0;
	return 0;
}

static int macb_pad_and_fcs(struct sk_buff **skb, struct net_device *ndev)
{
	bool cloned = skb_cloned(*skb) || skb_header_cloned(*skb) ||
		      skb_is_nonlinear(*skb);
	int padlen = ETH_ZLEN - (*skb)->len;
	int tailroom = skb_tailroom(*skb);
	struct sk_buff *nskb;
	u32 fcs;

	if (!(ndev->features & NETIF_F_HW_CSUM) ||
	    !((*skb)->ip_summed != CHECKSUM_PARTIAL) ||
	    skb_shinfo(*skb)->gso_size)	/* Not available for GSO */
		return 0;

	if (padlen <= 0) {
		/* FCS could be appeded to tailroom. */
		if (tailroom >= ETH_FCS_LEN)
			goto add_fcs;
		/* No room for FCS, need to reallocate skb. */
		else
			padlen = ETH_FCS_LEN;
	} else {
		/* Add room for FCS. */
		padlen += ETH_FCS_LEN;
	}

	if (cloned || tailroom < padlen) {
		nskb = skb_copy_expand(*skb, 0, padlen, GFP_ATOMIC);
		if (!nskb)
			return -ENOMEM;

		dev_kfree_skb_any(*skb);
		*skb = nskb;
	}

	if (padlen) {
		if (padlen >= ETH_FCS_LEN)
			skb_put_zero(*skb, padlen - ETH_FCS_LEN);
		else
			skb_trim(*skb, ETH_FCS_LEN - padlen);
	}

add_fcs:
	/* set FCS to packet */
	fcs = crc32_le(~0, (*skb)->data, (*skb)->len);
	fcs = ~fcs;

	skb_put_u8(*skb, fcs		& 0xff);
	skb_put_u8(*skb, (fcs >> 8)	& 0xff);
	skb_put_u8(*skb, (fcs >> 16)	& 0xff);
	skb_put_u8(*skb, (fcs >> 24)	& 0xff);

	return 0;
}

static netdev_tx_t macb_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	u16 queue_index = skb_get_queue_mapping(skb);
	struct macb *bp = netdev_priv(dev);
	struct macb_queue *queue = &bp->queues[queue_index];
	unsigned long flags;
	unsigned int desc_cnt, nr_frags, frag_size, f;
	unsigned int hdrlen;
	bool is_lso, is_udp = 0;
	netdev_tx_t ret = NETDEV_TX_OK;

	if (macb_clear_csum(skb)) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	if (macb_pad_and_fcs(&skb, dev)) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	is_lso = (skb_shinfo(skb)->gso_size != 0);

	if (is_lso) {
		is_udp = !!(ip_hdr(skb)->protocol == IPPROTO_UDP);

		/* length of headers */
		if (is_udp)
			/* only queue eth + ip headers separately for UDP */
			hdrlen = skb_transport_offset(skb);
		else
			hdrlen = skb_transport_offset(skb) + tcp_hdrlen(skb);
		if (skb_headlen(skb) < hdrlen) {
			netdev_err(bp->dev, "Error - LSO headers fragmented!!!\n");
			/* if this is required, would need to copy to single buffer */
			return NETDEV_TX_BUSY;
		}
	} else
		hdrlen = min(skb_headlen(skb), bp->max_tx_length);

#if defined(DEBUG) && defined(VERBOSE_DEBUG)
	netdev_vdbg(bp->dev,
		    "start_xmit: queue %hu len %u head %p data %p tail %p end %p\n",
		    queue_index, skb->len, skb->head, skb->data,
		    skb_tail_pointer(skb), skb_end_pointer(skb));
	print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_OFFSET, 16, 1,
		       skb->data, 16, true);
#endif

	/* Count how many TX buffer descriptors are needed to send this
	 * socket buffer: skb fragments of jumbo frames may need to be
	 * split into many buffer descriptors.
	 */
	if (is_lso && (skb_headlen(skb) > hdrlen))
		/* extra header descriptor if also payload in first buffer */
		desc_cnt = DIV_ROUND_UP((skb_headlen(skb) - hdrlen), bp->max_tx_length) + 1;
	else
		desc_cnt = DIV_ROUND_UP(skb_headlen(skb), bp->max_tx_length);
	nr_frags = skb_shinfo(skb)->nr_frags;
	for (f = 0; f < nr_frags; f++) {
		frag_size = skb_frag_size(&skb_shinfo(skb)->frags[f]);
		desc_cnt += DIV_ROUND_UP(frag_size, bp->max_tx_length);
	}

	spin_lock_irqsave(&bp->lock, flags);

	/* This is a hard error, log it. */
	if (CIRC_SPACE(queue->tx_head, queue->tx_tail,
		       bp->tx_ring_size) < desc_cnt) {
		netif_stop_subqueue(dev, queue_index);
		spin_unlock_irqrestore(&bp->lock, flags);
		netdev_dbg(bp->dev, "tx_head = %u, tx_tail = %u\n",
			   queue->tx_head, queue->tx_tail);
		return NETDEV_TX_BUSY;
	}

	/* Map socket buffer for DMA transfer */
	if (!macb_tx_map(bp, queue, skb, hdrlen)) {
		dev_kfree_skb_any(skb);
		goto unlock;
	}

	/* Make newly initialized descriptor visible to hardware */
	wmb();
	skb_tx_timestamp(skb);

	macb_writel(bp, NCR, macb_readl(bp, NCR) | MACB_BIT(TSTART));

	if (CIRC_SPACE(queue->tx_head, queue->tx_tail, bp->tx_ring_size) < 1)
		netif_stop_subqueue(dev, queue_index);

unlock:
	spin_unlock_irqrestore(&bp->lock, flags);

	return ret;
}

static void macb_init_rx_buffer_size(struct macb *bp, size_t size)
{
	if (!macb_is_gem(bp)) {
		bp->rx_buffer_size = MACB_RX_BUFFER_SIZE;
	} else {
		bp->rx_buffer_size = size;

		if (bp->rx_buffer_size % RX_BUFFER_MULTIPLE) {
			netdev_dbg(bp->dev,
				   "RX buffer must be multiple of %d bytes, expanding\n",
				   RX_BUFFER_MULTIPLE);
			bp->rx_buffer_size =
				roundup(bp->rx_buffer_size, RX_BUFFER_MULTIPLE);
		}
	}

	netdev_dbg(bp->dev, "mtu [%u] rx_buffer_size [%zu]\n",
		   bp->dev->mtu, bp->rx_buffer_size);
}

static void gem_free_rx_buffers(struct macb *bp)
{
	struct sk_buff		*skb;
	struct macb_dma_desc	*desc;
	struct macb_queue *queue;
	dma_addr_t		addr;
	unsigned int q;
	int i;

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		if (!queue->rx_skbuff)
			continue;

		for (i = 0; i < bp->rx_ring_size; i++) {
			skb = queue->rx_skbuff[i];

			if (!skb)
				continue;

			desc = macb_rx_desc(queue, i);
			addr = macb_get_addr(bp, desc);

			dma_unmap_single(&bp->pdev->dev, addr, bp->rx_buffer_size,
					DMA_FROM_DEVICE);
			dev_kfree_skb_any(skb);
			skb = NULL;
		}

		kfree(queue->rx_skbuff);
		queue->rx_skbuff = NULL;
	}
}

static void macb_free_rx_buffers(struct macb *bp)
{
	struct macb_queue *queue = &bp->queues[0];

	if (queue->rx_buffers) {
		dma_free_coherent(&bp->pdev->dev,
				  bp->rx_ring_size * bp->rx_buffer_size,
				  queue->rx_buffers, queue->rx_buffers_dma);
		queue->rx_buffers = NULL;
	}
}

static void macb_free_consistent(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int q;
	int size;

	bp->macbgem_ops.mog_free_rx_buffers(bp);

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		kfree(queue->tx_skb);
		queue->tx_skb = NULL;
		if (queue->tx_ring) {
			size = TX_RING_BYTES(bp) + bp->tx_bd_rd_prefetch;
			dma_free_coherent(&bp->pdev->dev, size,
					  queue->tx_ring, queue->tx_ring_dma);
			queue->tx_ring = NULL;
		}
		if (queue->rx_ring) {
			size = RX_RING_BYTES(bp) + bp->rx_bd_rd_prefetch;
			dma_free_coherent(&bp->pdev->dev, size,
					  queue->rx_ring, queue->rx_ring_dma);
			queue->rx_ring = NULL;
		}
	}
}

static int gem_alloc_rx_buffers(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int q;
	int size;

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		size = bp->rx_ring_size * sizeof(struct sk_buff *);
		queue->rx_skbuff = kzalloc(size, GFP_KERNEL);
		if (!queue->rx_skbuff)
			return -ENOMEM;
		else
			netdev_dbg(bp->dev,
				   "Allocated %d RX struct sk_buff entries at %p\n",
				   bp->rx_ring_size, queue->rx_skbuff);
	}
	return 0;
}

static int macb_alloc_rx_buffers(struct macb *bp)
{
	struct macb_queue *queue = &bp->queues[0];
	int size;

	size = bp->rx_ring_size * bp->rx_buffer_size;
	queue->rx_buffers = dma_alloc_coherent(&bp->pdev->dev, size,
					    &queue->rx_buffers_dma, GFP_KERNEL);
	if (!queue->rx_buffers)
		return -ENOMEM;

	netdev_dbg(bp->dev,
		   "Allocated RX buffers of %d bytes at %08lx (mapped %p)\n",
		   size, (unsigned long)queue->rx_buffers_dma, queue->rx_buffers);
	return 0;
}

static int macb_alloc_consistent(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int q;
	int size;

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		size = TX_RING_BYTES(bp) + bp->tx_bd_rd_prefetch;
		queue->tx_ring = dma_alloc_coherent(&bp->pdev->dev, size,
						    &queue->tx_ring_dma,
						    GFP_KERNEL);
		if (!queue->tx_ring)
			goto out_err;
		netdev_dbg(bp->dev,
			   "Allocated TX ring for queue %u of %d bytes at %08lx (mapped %p)\n",
			   q, size, (unsigned long)queue->tx_ring_dma,
			   queue->tx_ring);

		size = bp->tx_ring_size * sizeof(struct macb_tx_skb);
		queue->tx_skb = kmalloc(size, GFP_KERNEL);
		if (!queue->tx_skb)
			goto out_err;

		size = RX_RING_BYTES(bp) + bp->rx_bd_rd_prefetch;
		queue->rx_ring = dma_alloc_coherent(&bp->pdev->dev, size,
						 &queue->rx_ring_dma, GFP_KERNEL);
		if (!queue->rx_ring)
			goto out_err;
		netdev_dbg(bp->dev,
			   "Allocated RX ring of %d bytes at %08lx (mapped %p)\n",
			   size, (unsigned long)queue->rx_ring_dma, queue->rx_ring);
	}
	if (bp->macbgem_ops.mog_alloc_rx_buffers(bp))
		goto out_err;

	return 0;

out_err:
	macb_free_consistent(bp);
	return -ENOMEM;
}

static void gem_init_rings(struct macb *bp)
{
	struct macb_queue *queue;
	struct macb_dma_desc *desc = NULL;
	unsigned int q;
	int i;

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		for (i = 0; i < bp->tx_ring_size; i++) {
			desc = macb_tx_desc(queue, i);
			macb_set_addr(bp, desc, 0);
			desc->ctrl = MACB_BIT(TX_USED);
		}
		desc->ctrl |= MACB_BIT(TX_WRAP);
		queue->tx_head = 0;
		queue->tx_tail = 0;

		queue->rx_tail = 0;
		queue->rx_prepared_head = 0;

		gem_rx_refill(queue);
	}

}

static void macb_init_rings(struct macb *bp)
{
	int i;
	struct macb_dma_desc *desc = NULL;

	macb_init_rx_ring(&bp->queues[0]);

	for (i = 0; i < bp->tx_ring_size; i++) {
		desc = macb_tx_desc(&bp->queues[0], i);
		macb_set_addr(bp, desc, 0);
		desc->ctrl = MACB_BIT(TX_USED);
	}
	bp->queues[0].tx_head = 0;
	bp->queues[0].tx_tail = 0;
	desc->ctrl |= MACB_BIT(TX_WRAP);
}

static void macb_reset_hw(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int q;
	u32 ctrl = macb_readl(bp, NCR);

	/* Disable RX and TX (XXX: Should we halt the transmission
	 * more gracefully?)
	 */
	ctrl &= ~(MACB_BIT(RE) | MACB_BIT(TE));

	/* Clear the stats registers (XXX: Update stats first?) */
	ctrl |= MACB_BIT(CLRSTAT);

	macb_writel(bp, NCR, ctrl);

	/* Clear all status flags */
	macb_writel(bp, TSR, -1);
	macb_writel(bp, RSR, -1);

	/* Disable all interrupts */
	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		queue_writel(queue, IDR, -1);
		queue_readl(queue, ISR);
		if (bp->caps & MACB_CAPS_ISR_CLEAR_ON_WRITE)
			queue_writel(queue, ISR, -1);
	}
}

static u32 gem_mdc_clk_div(struct macb *bp)
{
	u32 config;
	unsigned long pclk_hz = clk_get_rate(bp->pclk);

	if (pclk_hz <= 20000000)
		config = GEM_BF(CLK, GEM_CLK_DIV8);
	else if (pclk_hz <= 40000000)
		config = GEM_BF(CLK, GEM_CLK_DIV16);
	else if (pclk_hz <= 80000000)
		config = GEM_BF(CLK, GEM_CLK_DIV32);
	else if (pclk_hz <= 120000000)
		config = GEM_BF(CLK, GEM_CLK_DIV48);
	else if (pclk_hz <= 160000000)
		config = GEM_BF(CLK, GEM_CLK_DIV64);
	else
		config = GEM_BF(CLK, GEM_CLK_DIV96);

	return config;
}

static u32 macb_mdc_clk_div(struct macb *bp)
{
	u32 config;
	unsigned long pclk_hz;

	if (macb_is_gem(bp))
		return gem_mdc_clk_div(bp);

	pclk_hz = clk_get_rate(bp->pclk);
	if (pclk_hz <= 20000000)
		config = MACB_BF(CLK, MACB_CLK_DIV8);
	else if (pclk_hz <= 40000000)
		config = MACB_BF(CLK, MACB_CLK_DIV16);
	else if (pclk_hz <= 80000000)
		config = MACB_BF(CLK, MACB_CLK_DIV32);
	else
		config = MACB_BF(CLK, MACB_CLK_DIV64);

	return config;
}

/* Get the DMA bus width field of the network configuration register that we
 * should program.  We find the width from decoding the design configuration
 * register to find the maximum supported data bus width.
 */
static u32 macb_dbw(struct macb *bp)
{
	if (!macb_is_gem(bp))
		return 0;

	switch (GEM_BFEXT(DBWDEF, gem_readl(bp, DCFG1))) {
	case 4:
		return GEM_BF(DBW, GEM_DBW128);
	case 2:
		return GEM_BF(DBW, GEM_DBW64);
	case 1:
	default:
		return GEM_BF(DBW, GEM_DBW32);
	}
}

/* Configure the receive DMA engine
 * - use the correct receive buffer size
 * - set best burst length for DMA operations
 *   (if not supported by FIFO, it will fallback to default)
 * - set both rx/tx packet buffers to full memory size
 * These are configurable parameters for GEM.
 */
static void macb_configure_dma(struct macb *bp)
{
	struct macb_queue *queue;
	u32 buffer_size;
	unsigned int q;
	u32 dmacfg;

	buffer_size = bp->rx_buffer_size / RX_BUFFER_MULTIPLE;
	if (macb_is_gem(bp)) {
		dmacfg = gem_readl(bp, DMACFG) & ~GEM_BF(RXBS, -1L);
		for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
			if (q)
				queue_writel(queue, RBQS, buffer_size);
			else
				dmacfg |= GEM_BF(RXBS, buffer_size);
		}
		if (bp->dma_burst_length)
			dmacfg = GEM_BFINS(FBLDO, bp->dma_burst_length, dmacfg);
		dmacfg |= GEM_BIT(TXPBMS) | GEM_BF(RXBMS, -1L);
		dmacfg &= ~GEM_BIT(ENDIA_PKT);

		if (bp->native_io)
			dmacfg &= ~GEM_BIT(ENDIA_DESC);
		else
			dmacfg |= GEM_BIT(ENDIA_DESC); /* CPU in big endian */

		if (bp->dev->features & NETIF_F_HW_CSUM)
			dmacfg |= GEM_BIT(TXCOEN);
		else
			dmacfg &= ~GEM_BIT(TXCOEN);

		dmacfg &= ~GEM_BIT(ADDR64);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		if (bp->hw_dma_cap & HW_DMA_CAP_64B)
			dmacfg |= GEM_BIT(ADDR64);
#endif
#ifdef CONFIG_MACB_USE_HWSTAMP
		if (bp->hw_dma_cap & HW_DMA_CAP_PTP)
			dmacfg |= GEM_BIT(RXEXT) | GEM_BIT(TXEXT);
#endif
		netdev_dbg(bp->dev, "Cadence configure DMA with 0x%08x\n",
			   dmacfg);
		gem_writel(bp, DMACFG, dmacfg);
	}
}

static void macb_init_hw(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int q;

	u32 config;

	macb_reset_hw(bp);
	macb_set_hwaddr(bp);

	config = macb_mdc_clk_div(bp);
	if (bp->phy_interface == PHY_INTERFACE_MODE_SGMII)
		config |= GEM_BIT(SGMIIEN) | GEM_BIT(PCSSEL);
	config |= MACB_BF(RBOF, NET_IP_ALIGN);	/* Make eth data aligned */
	config |= MACB_BIT(PAE);		/* PAuse Enable */
	config |= MACB_BIT(DRFCS);		/* Discard Rx FCS */
	if (bp->caps & MACB_CAPS_JUMBO)
		config |= MACB_BIT(JFRAME);	/* Enable jumbo frames */
	else
		config |= MACB_BIT(BIG);	/* Receive oversized frames */
	if (bp->dev->flags & IFF_PROMISC)
		config |= MACB_BIT(CAF);	/* Copy All Frames */
	else if (macb_is_gem(bp) && bp->dev->features & NETIF_F_RXCSUM)
		config |= GEM_BIT(RXCOEN);
	if (!(bp->dev->flags & IFF_BROADCAST))
		config |= MACB_BIT(NBC);	/* No BroadCast */
	config |= macb_dbw(bp);
	macb_writel(bp, NCFGR, config);
	if ((bp->caps & MACB_CAPS_JUMBO) && bp->jumbo_max_len)
		gem_writel(bp, JML, bp->jumbo_max_len);
	bp->speed = SPEED_10;
	bp->duplex = DUPLEX_HALF;
	bp->rx_frm_len_mask = MACB_RX_FRMLEN_MASK;
	if (bp->caps & MACB_CAPS_JUMBO)
		bp->rx_frm_len_mask = MACB_RX_JFRMLEN_MASK;

	macb_configure_dma(bp);

	/* Initialize TX and RX buffers */
	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
		queue_writel(queue, RBQP, lower_32_bits(queue->rx_ring_dma));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		if (bp->hw_dma_cap & HW_DMA_CAP_64B)
			queue_writel(queue, RBQPH, upper_32_bits(queue->rx_ring_dma));
#endif
		queue_writel(queue, TBQP, lower_32_bits(queue->tx_ring_dma));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		if (bp->hw_dma_cap & HW_DMA_CAP_64B)
			queue_writel(queue, TBQPH, upper_32_bits(queue->tx_ring_dma));
#endif

		/* Enable interrupts */
		queue_writel(queue, IER,
			     bp->rx_intr_mask |
			     MACB_TX_INT_FLAGS |
			     MACB_BIT(HRESP));
	}

	/* Enable TX and RX */
	macb_writel(bp, NCR, macb_readl(bp, NCR) | MACB_BIT(RE) | MACB_BIT(TE));
}

/* The hash address register is 64 bits long and takes up two
 * locations in the memory map.  The least significant bits are stored
 * in EMAC_HSL and the most significant bits in EMAC_HSH.
 *
 * The unicast hash enable and the multicast hash enable bits in the
 * network configuration register enable the reception of hash matched
 * frames. The destination address is reduced to a 6 bit index into
 * the 64 bit hash register using the following hash function.  The
 * hash function is an exclusive or of every sixth bit of the
 * destination address.
 *
 * hi[5] = da[5] ^ da[11] ^ da[17] ^ da[23] ^ da[29] ^ da[35] ^ da[41] ^ da[47]
 * hi[4] = da[4] ^ da[10] ^ da[16] ^ da[22] ^ da[28] ^ da[34] ^ da[40] ^ da[46]
 * hi[3] = da[3] ^ da[09] ^ da[15] ^ da[21] ^ da[27] ^ da[33] ^ da[39] ^ da[45]
 * hi[2] = da[2] ^ da[08] ^ da[14] ^ da[20] ^ da[26] ^ da[32] ^ da[38] ^ da[44]
 * hi[1] = da[1] ^ da[07] ^ da[13] ^ da[19] ^ da[25] ^ da[31] ^ da[37] ^ da[43]
 * hi[0] = da[0] ^ da[06] ^ da[12] ^ da[18] ^ da[24] ^ da[30] ^ da[36] ^ da[42]
 *
 * da[0] represents the least significant bit of the first byte
 * received, that is, the multicast/unicast indicator, and da[47]
 * represents the most significant bit of the last byte received.  If
 * the hash index, hi[n], points to a bit that is set in the hash
 * register then the frame will be matched according to whether the
 * frame is multicast or unicast.  A multicast match will be signalled
 * if the multicast hash enable bit is set, da[0] is 1 and the hash
 * index points to a bit set in the hash register.  A unicast match
 * will be signalled if the unicast hash enable bit is set, da[0] is 0
 * and the hash index points to a bit set in the hash register.  To
 * receive all multicast frames, the hash register should be set with
 * all ones and the multicast hash enable bit should be set in the
 * network configuration register.
 */

static inline int hash_bit_value(int bitnr, __u8 *addr)
{
	if (addr[bitnr / 8] & (1 << (bitnr % 8)))
		return 1;
	return 0;
}

/* Return the hash index value for the specified address. */
static int hash_get_index(__u8 *addr)
{
	int i, j, bitval;
	int hash_index = 0;

	for (j = 0; j < 6; j++) {
		for (i = 0, bitval = 0; i < 8; i++)
			bitval ^= hash_bit_value(i * 6 + j, addr);

		hash_index |= (bitval << j);
	}

	return hash_index;
}

/* Add multicast addresses to the internal multicast-hash table. */
static void macb_sethashtable(struct net_device *dev)
{
	struct netdev_hw_addr *ha;
	unsigned long mc_filter[2];
	unsigned int bitnr;
	struct macb *bp = netdev_priv(dev);

	mc_filter[0] = 0;
	mc_filter[1] = 0;

	netdev_for_each_mc_addr(ha, dev) {
		bitnr = hash_get_index(ha->addr);
		mc_filter[bitnr >> 5] |= 1 << (bitnr & 31);
	}

	macb_or_gem_writel(bp, HRB, mc_filter[0]);
	macb_or_gem_writel(bp, HRT, mc_filter[1]);
}

/* Enable/Disable promiscuous and multicast modes. */
static void macb_set_rx_mode(struct net_device *dev)
{
	unsigned long cfg;
	struct macb *bp = netdev_priv(dev);

	cfg = macb_readl(bp, NCFGR);

	if (dev->flags & IFF_PROMISC) {
		/* Enable promiscuous mode */
		cfg |= MACB_BIT(CAF);

		/* Disable RX checksum offload */
		if (macb_is_gem(bp))
			cfg &= ~GEM_BIT(RXCOEN);
	} else {
		/* Disable promiscuous mode */
		cfg &= ~MACB_BIT(CAF);

		/* Enable RX checksum offload only if requested */
		if (macb_is_gem(bp) && dev->features & NETIF_F_RXCSUM)
			cfg |= GEM_BIT(RXCOEN);
	}

	if (dev->flags & IFF_ALLMULTI) {
		/* Enable all multicast mode */
		macb_or_gem_writel(bp, HRB, -1);
		macb_or_gem_writel(bp, HRT, -1);
		cfg |= MACB_BIT(NCFGR_MTI);
	} else if (!netdev_mc_empty(dev)) {
		/* Enable specific multicasts */
		macb_sethashtable(dev);
		cfg |= MACB_BIT(NCFGR_MTI);
	} else if (dev->flags & (~IFF_ALLMULTI)) {
		/* Disable all multicast mode */
		macb_or_gem_writel(bp, HRB, 0);
		macb_or_gem_writel(bp, HRT, 0);
		cfg &= ~MACB_BIT(NCFGR_MTI);
	}

	macb_writel(bp, NCFGR, cfg);
}

static int macb_open(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	size_t bufsz = dev->mtu + ETH_HLEN + ETH_FCS_LEN + NET_IP_ALIGN;
	struct macb_queue *queue;
	unsigned int q;
	int err;

	netdev_dbg(bp->dev, "open\n");

	/* carrier starts down */
	netif_carrier_off(dev);

	/* if the phy is not yet register, retry later*/
	if (!dev->phydev)
		return -EAGAIN;

	/* RX buffers initialization */
	macb_init_rx_buffer_size(bp, bufsz);

	err = macb_alloc_consistent(bp);
	if (err) {
		netdev_err(dev, "Unable to allocate DMA memory (error %d)\n",
			   err);
		return err;
	}

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue)
		napi_enable(&queue->napi);

	bp->macbgem_ops.mog_init_rings(bp);
	macb_init_hw(bp);

	/* schedule a link state check */
	phy_start(dev->phydev);

	netif_tx_start_all_queues(dev);

	if (bp->ptp_info)
		bp->ptp_info->ptp_init(dev);

	return 0;
}

static int macb_close(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	struct macb_queue *queue;
	unsigned long flags;
	unsigned int q;

	netif_tx_stop_all_queues(dev);

	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue)
		napi_disable(&queue->napi);

	if (dev->phydev)
		phy_stop(dev->phydev);

	spin_lock_irqsave(&bp->lock, flags);
	macb_reset_hw(bp);
	netif_carrier_off(dev);
	spin_unlock_irqrestore(&bp->lock, flags);

	macb_free_consistent(bp);

	if (bp->ptp_info)
		bp->ptp_info->ptp_remove(dev);

	return 0;
}

static int macb_change_mtu(struct net_device *dev, int new_mtu)
{
	if (netif_running(dev))
		return -EBUSY;

	dev->mtu = new_mtu;

	return 0;
}

static void gem_update_stats(struct macb *bp)
{
	struct macb_queue *queue;
	unsigned int i, q, idx;
	unsigned long *stat;

	u32 *p = &bp->hw_stats.gem.tx_octets_31_0;

	for (i = 0; i < GEM_STATS_LEN; ++i, ++p) {
		u32 offset = gem_statistics[i].offset;
		u64 val = bp->macb_reg_readl(bp, offset);

		bp->ethtool_stats[i] += val;
		*p += val;

		if (offset == GEM_OCTTXL || offset == GEM_OCTRXL) {
			/* Add GEM_OCTTXH, GEM_OCTRXH */
			val = bp->macb_reg_readl(bp, offset + 4);
			bp->ethtool_stats[i] += ((u64)val) << 32;
			*(++p) += val;
		}
	}

	idx = GEM_STATS_LEN;
	for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue)
		for (i = 0, stat = &queue->stats.first; i < QUEUE_STATS_LEN; ++i, ++stat)
			bp->ethtool_stats[idx++] = *stat;
}

static struct net_device_stats *gem_get_stats(struct macb *bp)
{
	struct gem_stats *hwstat = &bp->hw_stats.gem;
	struct net_device_stats *nstat = &bp->dev->stats;

	if (!netif_running(bp->dev))
		return nstat;

	spin_lock_irq(&bp->stats_lock);
	gem_update_stats(bp);

	nstat->rx_errors = (hwstat->rx_frame_check_sequence_errors +
			    hwstat->rx_alignment_errors +
			    hwstat->rx_resource_errors +
			    hwstat->rx_overruns +
			    hwstat->rx_oversize_frames +
			    hwstat->rx_jabbers +
			    hwstat->rx_undersized_frames +
			    hwstat->rx_length_field_frame_errors);
	nstat->tx_errors = (hwstat->tx_late_collisions +
			    hwstat->tx_excessive_collisions +
			    hwstat->tx_underrun +
			    hwstat->tx_carrier_sense_errors);
	nstat->multicast = hwstat->rx_multicast_frames;
	nstat->collisions = (hwstat->tx_single_collision_frames +
			     hwstat->tx_multiple_collision_frames +
			     hwstat->tx_excessive_collisions);
	nstat->rx_length_errors = (hwstat->rx_oversize_frames +
				   hwstat->rx_jabbers +
				   hwstat->rx_undersized_frames +
				   hwstat->rx_length_field_frame_errors);
	nstat->rx_over_errors = hwstat->rx_resource_errors;
	nstat->rx_crc_errors = hwstat->rx_frame_check_sequence_errors;
	nstat->rx_frame_errors = hwstat->rx_alignment_errors;
	nstat->rx_fifo_errors = hwstat->rx_overruns;
	nstat->tx_aborted_errors = hwstat->tx_excessive_collisions;
	nstat->tx_carrier_errors = hwstat->tx_carrier_sense_errors;
	nstat->tx_fifo_errors = hwstat->tx_underrun;
	spin_unlock_irq(&bp->stats_lock);

	return nstat;
}

static void gem_get_ethtool_stats(struct net_device *dev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct macb *bp = netdev_priv(dev);

	spin_lock_irq(&bp->stats_lock);
	gem_update_stats(bp);
	memcpy(data, &bp->ethtool_stats, sizeof(u64)
			* (GEM_STATS_LEN + QUEUE_STATS_LEN * MACB_MAX_QUEUES));
	spin_unlock_irq(&bp->stats_lock);
}

static int gem_get_sset_count(struct net_device *dev, int sset)
{
	struct macb *bp = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return GEM_STATS_LEN + bp->num_queues * QUEUE_STATS_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void gem_get_ethtool_strings(struct net_device *dev, u32 sset, u8 *p)
{
	char stat_string[ETH_GSTRING_LEN];
	struct macb *bp = netdev_priv(dev);
	struct macb_queue *queue;
	unsigned int i;
	unsigned int q;

	switch (sset) {
	case ETH_SS_STATS:
		for (i = 0; i < GEM_STATS_LEN; i++, p += ETH_GSTRING_LEN)
			memcpy(p, gem_statistics[i].stat_string,
			       ETH_GSTRING_LEN);

		for (q = 0, queue = bp->queues; q < bp->num_queues; ++q, ++queue) {
			for (i = 0; i < QUEUE_STATS_LEN; i++, p += ETH_GSTRING_LEN) {
				snprintf(stat_string, ETH_GSTRING_LEN, "q%d_%s",
						q, queue_statistics[i].stat_string);
				memcpy(p, stat_string, ETH_GSTRING_LEN);
			}
		}
		break;
	}
}

static struct net_device_stats *macb_get_stats(struct net_device *dev)
{
	struct macb *bp = netdev_priv(dev);
	struct net_device_stats *nstat = &bp->dev->stats;
	struct macb_stats *hwstat = &bp->hw_stats.macb;

	if (macb_is_gem(bp))
		return gem_get_stats(bp);

	/* read stats from hardware */
	spin_lock_irq(&bp->stats_lock);
	macb_update_stats(bp);

	/* Convert HW stats into netdevice stats */
	nstat->rx_errors = (hwstat->rx_fcs_errors +
			    hwstat->rx_align_errors +
			    hwstat->rx_resource_errors +
			    hwstat->rx_overruns +
			    hwstat->rx_oversize_pkts +
			    hwstat->rx_jabbers +
			    hwstat->rx_undersize_pkts +
			    hwstat->rx_length_mismatch);
	nstat->tx_errors = (hwstat->tx_late_cols +
			    hwstat->tx_excessive_cols +
			    hwstat->tx_underruns +
			    hwstat->tx_carrier_errors +
			    hwstat->sqe_test_errors);
	nstat->collisions = (hwstat->tx_single_cols +
			     hwstat->tx_multiple_cols +
			     hwstat->tx_excessive_cols);
	nstat->rx_length_errors = (hwstat->rx_oversize_pkts +
				   hwstat->rx_jabbers +
				   hwstat->rx_undersize_pkts +
				   hwstat->rx_length_mismatch);
	nstat->rx_over_errors = hwstat->rx_resource_errors +
				   hwstat->rx_overruns;
	nstat->rx_crc_errors = hwstat->rx_fcs_errors;
	nstat->rx_frame_errors = hwstat->rx_align_errors;
	nstat->rx_fifo_errors = hwstat->rx_overruns;
	/* XXX: What does "missed" mean? */
	nstat->tx_aborted_errors = hwstat->tx_excessive_cols;
	nstat->tx_carrier_errors = hwstat->tx_carrier_errors;
	nstat->tx_fifo_errors = hwstat->tx_underruns;
	/* Don't know about heartbeat or window errors... */
	spin_unlock_irq(&bp->stats_lock);

	return nstat;
}

static int macb_get_regs_len(struct net_device *netdev)
{
	return MACB_GREGS_NBR * sizeof(u32);
}

static void macb_get_regs(struct net_device *dev, struct ethtool_regs *regs,
			  void *p)
{
	struct macb *bp = netdev_priv(dev);
	unsigned int tail, head;
	u32 *regs_buff = p;

	regs->version = (macb_readl(bp, MID) & ((1 << MACB_REV_SIZE) - 1))
			| MACB_GREGS_VERSION;

	tail = macb_tx_ring_wrap(bp, bp->queues[0].tx_tail);
	head = macb_tx_ring_wrap(bp, bp->queues[0].tx_head);

	regs_buff[0]  = macb_readl(bp, NCR);
	regs_buff[1]  = macb_or_gem_readl(bp, NCFGR);
	regs_buff[2]  = macb_readl(bp, NSR);
	regs_buff[3]  = macb_readl(bp, TSR);
	regs_buff[4]  = macb_readl(bp, RBQP);
	regs_buff[5]  = macb_readl(bp, TBQP);
	regs_buff[6]  = macb_readl(bp, RSR);
	regs_buff[7]  = macb_readl(bp, IMR);

	regs_buff[8]  = tail;
	regs_buff[9]  = head;
	regs_buff[10] = macb_tx_dma(&bp->queues[0], tail);
	regs_buff[11] = macb_tx_dma(&bp->queues[0], head);

	if (!(bp->caps & MACB_CAPS_USRIO_DISABLED))
		regs_buff[12] = macb_or_gem_readl(bp, USRIO);
	if (macb_is_gem(bp))
		regs_buff[13] = gem_readl(bp, DMACFG);
}

static void macb_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct macb *bp = netdev_priv(netdev);

	wol->supported = 0;
	wol->wolopts = 0;

	if (bp->wol & MACB_WOL_HAS_MAGIC_PACKET) {
		wol->supported = WAKE_MAGIC;

		if (bp->wol & MACB_WOL_ENABLED)
			wol->wolopts |= WAKE_MAGIC;
	}
}

static int macb_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct macb *bp = netdev_priv(netdev);

	if (!(bp->wol & MACB_WOL_HAS_MAGIC_PACKET) ||
	    (wol->wolopts & ~WAKE_MAGIC))
		return -EOPNOTSUPP;

	if (wol->wolopts & WAKE_MAGIC)
		bp->wol |= MACB_WOL_ENABLED;
	else
		bp->wol &= ~MACB_WOL_ENABLED;

	device_set_wakeup_enable(&bp->pdev->dev, bp->wol & MACB_WOL_ENABLED);

	return 0;
}

static void macb_get_ringparam(struct net_device *netdev,
			       struct ethtool_ringparam *ring)
{
	struct macb *bp = netdev_priv(netdev);

	ring->rx_max_pending = MAX_RX_RING_SIZE;
	ring->tx_max_pending = MAX_TX_RING_SIZE;

	ring->rx_pending = bp->rx_ring_size;
	ring->tx_pending = bp->tx_ring_size;
}

static int macb_set_ringparam(struct net_device *netdev,
			      struct ethtool_ringparam *ring)
{
	struct macb *bp = netdev_priv(netdev);
	u32 new_rx_size, new_tx_size;
	unsigned int reset = 0;

	if ((ring->rx_mini_pending) || (ring->rx_jumbo_pending))
		return -EINVAL;

	new_rx_size = clamp_t(u32, ring->rx_pending,
			      MIN_RX_RING_SIZE, MAX_RX_RING_SIZE);
	new_rx_size = roundup_pow_of_two(new_rx_size);

	new_tx_size = clamp_t(u32, ring->tx_pending,
			      MIN_TX_RING_SIZE, MAX_TX_RING_SIZE);
	new_tx_size = roundup_pow_of_two(new_tx_size);

	if ((new_tx_size == bp->tx_ring_size) &&
	    (new_rx_size == bp->rx_ring_size)) {
		/* nothing to do */
		return 0;
	}

	if (netif_running(bp->dev)) {
		reset = 1;
		macb_close(bp->dev);
	}

	bp->rx_ring_size = new_rx_size;
	bp->tx_ring_size = new_tx_size;

	if (reset)
		macb_open(bp->dev);

	return 0;
}

#ifdef CONFIG_MACB_USE_HWSTAMP
static unsigned int gem_get_tsu_rate(struct macb *bp)
{
	struct clk *tsu_clk;
	unsigned int tsu_rate;

	tsu_clk = devm_clk_get(&bp->pdev->dev, "tsu_clk");
	if (!IS_ERR(tsu_clk))
		tsu_rate = clk_get_rate(tsu_clk);
	/* try pclk instead */
	else if (!IS_ERR(bp->pclk)) {
		tsu_clk = bp->pclk;
		tsu_rate = clk_get_rate(tsu_clk);
	} else
		return -ENOTSUPP;
	return tsu_rate;
}

static s32 gem_get_ptp_max_adj(void)
{
	return 64000000;
}

static int gem_get_ts_info(struct net_device *dev,
			   struct ethtool_ts_info *info)
{
	struct macb *bp = netdev_priv(dev);

	if ((bp->hw_dma_cap & HW_DMA_CAP_PTP) == 0) {
		ethtool_op_get_ts_info(dev, info);
		return 0;
	}

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_SOFTWARE |
		SOF_TIMESTAMPING_RX_SOFTWARE |
		SOF_TIMESTAMPING_SOFTWARE |
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types =
		(1 << HWTSTAMP_TX_ONESTEP_SYNC) |
		(1 << HWTSTAMP_TX_OFF) |
		(1 << HWTSTAMP_TX_ON);
	info->rx_filters =
		(1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_ALL);

	info->phc_index = bp->ptp_clock ? ptp_clock_index(bp->ptp_clock) : -1;

	return 0;
}

static struct macb_ptp_info gem_ptp_info = {
	.ptp_init	 = gem_ptp_init,
	.ptp_remove	 = gem_ptp_remove,
	.get_ptp_max_adj = gem_get_ptp_max_adj,
	.get_tsu_rate	 = gem_get_tsu_rate,
	.get_ts_info	 = gem_get_ts_info,
	.get_hwtst	 = gem_get_hwtst,
	.set_hwtst	 = gem_set_hwtst,
};
#endif

static int macb_get_ts_info(struct net_device *netdev,
			    struct ethtool_ts_info *info)
{
	struct macb *bp = netdev_priv(netdev);

	if (bp->ptp_info)
		return bp->ptp_info->get_ts_info(netdev, info);

	return ethtool_op_get_ts_info(netdev, info);
}

static void gem_enable_flow_filters(struct macb *bp, bool enable)
{
	struct ethtool_rx_fs_item *item;
	u32 t2_scr;
	int num_t2_scr;

	num_t2_scr = GEM_BFEXT(T2SCR, gem_readl(bp, DCFG8));

	list_for_each_entry(item, &bp->rx_fs_list.list, list) {
		struct ethtool_rx_flow_spec *fs = &item->fs;
		struct ethtool_tcpip4_spec *tp4sp_m;

		if (fs->location >= num_t2_scr)
			continue;

		t2_scr = gem_readl_n(bp, SCRT2, fs->location);

		/* enable/disable screener regs for the flow entry */
		t2_scr = GEM_BFINS(ETHTEN, enable, t2_scr);

		/* only enable fields with no masking */
		tp4sp_m = &(fs->m_u.tcp_ip4_spec);

		if (enable && (tp4sp_m->ip4src == 0xFFFFFFFF))
			t2_scr = GEM_BFINS(CMPAEN, 1, t2_scr);
		else
			t2_scr = GEM_BFINS(CMPAEN, 0, t2_scr);

		if (enable && (tp4sp_m->ip4dst == 0xFFFFFFFF))
			t2_scr = GEM_BFINS(CMPBEN, 1, t2_scr);
		else
			t2_scr = GEM_BFINS(CMPBEN, 0, t2_scr);

		if (enable && ((tp4sp_m->psrc == 0xFFFF) || (tp4sp_m->pdst == 0xFFFF)))
			t2_scr = GEM_BFINS(CMPCEN, 1, t2_scr);
		else
			t2_scr = GEM_BFINS(CMPCEN, 0, t2_scr);

		gem_writel_n(bp, SCRT2, fs->location, t2_scr);
	}
}

static void gem_prog_cmp_regs(struct macb *bp, struct ethtool_rx_flow_spec *fs)
{
	struct ethtool_tcpip4_spec *tp4sp_v, *tp4sp_m;
	uint16_t index = fs->location;
	u32 w0, w1, t2_scr;
	bool cmp_a = false;
	bool cmp_b = false;
	bool cmp_c = false;

	tp4sp_v = &(fs->h_u.tcp_ip4_spec);
	tp4sp_m = &(fs->m_u.tcp_ip4_spec);

	/* ignore field if any masking set */
	if (tp4sp_m->ip4src == 0xFFFFFFFF) {
		/* 1st compare reg - IP source address */
		w0 = 0;
		w1 = 0;
		w0 = tp4sp_v->ip4src;
		w1 = GEM_BFINS(T2DISMSK, 1, w1); /* 32-bit compare */
		w1 = GEM_BFINS(T2CMPOFST, GEM_T2COMPOFST_ETYPE, w1);
		w1 = GEM_BFINS(T2OFST, ETYPE_SRCIP_OFFSET, w1);
		gem_writel_n(bp, T2CMPW0, T2CMP_OFST(GEM_IP4SRC_CMP(index)), w0);
		gem_writel_n(bp, T2CMPW1, T2CMP_OFST(GEM_IP4SRC_CMP(index)), w1);
		cmp_a = true;
	}

	/* ignore field if any masking set */
	if (tp4sp_m->ip4dst == 0xFFFFFFFF) {
		/* 2nd compare reg - IP destination address */
		w0 = 0;
		w1 = 0;
		w0 = tp4sp_v->ip4dst;
		w1 = GEM_BFINS(T2DISMSK, 1, w1); /* 32-bit compare */
		w1 = GEM_BFINS(T2CMPOFST, GEM_T2COMPOFST_ETYPE, w1);
		w1 = GEM_BFINS(T2OFST, ETYPE_DSTIP_OFFSET, w1);
		gem_writel_n(bp, T2CMPW0, T2CMP_OFST(GEM_IP4DST_CMP(index)), w0);
		gem_writel_n(bp, T2CMPW1, T2CMP_OFST(GEM_IP4DST_CMP(index)), w1);
		cmp_b = true;
	}

	/* ignore both port fields if masking set in both */
	if ((tp4sp_m->psrc == 0xFFFF) || (tp4sp_m->pdst == 0xFFFF)) {
		/* 3rd compare reg - source port, destination port */
		w0 = 0;
		w1 = 0;
		w1 = GEM_BFINS(T2CMPOFST, GEM_T2COMPOFST_IPHDR, w1);
		if (tp4sp_m->psrc == tp4sp_m->pdst) {
			w0 = GEM_BFINS(T2MASK, tp4sp_v->psrc, w0);
			w0 = GEM_BFINS(T2CMP, tp4sp_v->pdst, w0);
			w1 = GEM_BFINS(T2DISMSK, 1, w1); /* 32-bit compare */
			w1 = GEM_BFINS(T2OFST, IPHDR_SRCPORT_OFFSET, w1);
		} else {
			/* only one port definition */
			w1 = GEM_BFINS(T2DISMSK, 0, w1); /* 16-bit compare */
			w0 = GEM_BFINS(T2MASK, 0xFFFF, w0);
			if (tp4sp_m->psrc == 0xFFFF) { /* src port */
				w0 = GEM_BFINS(T2CMP, tp4sp_v->psrc, w0);
				w1 = GEM_BFINS(T2OFST, IPHDR_SRCPORT_OFFSET, w1);
			} else { /* dst port */
				w0 = GEM_BFINS(T2CMP, tp4sp_v->pdst, w0);
				w1 = GEM_BFINS(T2OFST, IPHDR_DSTPORT_OFFSET, w1);
			}
		}
		gem_writel_n(bp, T2CMPW0, T2CMP_OFST(GEM_PORT_CMP(index)), w0);
		gem_writel_n(bp, T2CMPW1, T2CMP_OFST(GEM_PORT_CMP(index)), w1);
		cmp_c = true;
	}

	t2_scr = 0;
	t2_scr = GEM_BFINS(QUEUE, (fs->ring_cookie) & 0xFF, t2_scr);
	t2_scr = GEM_BFINS(ETHT2IDX, SCRT2_ETHT, t2_scr);
	if (cmp_a)
		t2_scr = GEM_BFINS(CMPA, GEM_IP4SRC_CMP(index), t2_scr);
	if (cmp_b)
		t2_scr = GEM_BFINS(CMPB, GEM_IP4DST_CMP(index), t2_scr);
	if (cmp_c)
		t2_scr = GEM_BFINS(CMPC, GEM_PORT_CMP(index), t2_scr);
	gem_writel_n(bp, SCRT2, index, t2_scr);
}

static int gem_add_flow_filter(struct net_device *netdev,
		struct ethtool_rxnfc *cmd)
{
	struct macb *bp = netdev_priv(netdev);
	struct ethtool_rx_flow_spec *fs = &cmd->fs;
	struct ethtool_rx_fs_item *item, *newfs;
	unsigned long flags;
	int ret = -EINVAL;
	bool added = false;

	newfs = kmalloc(sizeof(*newfs), GFP_KERNEL);
	if (newfs == NULL)
		return -ENOMEM;
	memcpy(&newfs->fs, fs, sizeof(newfs->fs));

	netdev_dbg(netdev,
			"Adding flow filter entry,type=%u,queue=%u,loc=%u,src=%08X,dst=%08X,ps=%u,pd=%u\n",
			fs->flow_type, (int)fs->ring_cookie, fs->location,
			htonl(fs->h_u.tcp_ip4_spec.ip4src),
			htonl(fs->h_u.tcp_ip4_spec.ip4dst),
			htons(fs->h_u.tcp_ip4_spec.psrc), htons(fs->h_u.tcp_ip4_spec.pdst));

	spin_lock_irqsave(&bp->rx_fs_lock, flags);

	/* find correct place to add in list */
	list_for_each_entry(item, &bp->rx_fs_list.list, list) {
		if (item->fs.location > newfs->fs.location) {
			list_add_tail(&newfs->list, &item->list);
			added = true;
			break;
		} else if (item->fs.location == fs->location) {
			netdev_err(netdev, "Rule not added: location %d not free!\n",
					fs->location);
			ret = -EBUSY;
			goto err;
		}
	}
	if (!added)
		list_add_tail(&newfs->list, &bp->rx_fs_list.list);

	gem_prog_cmp_regs(bp, fs);
	bp->rx_fs_list.count++;
	/* enable filtering if NTUPLE on */
	if (netdev->features & NETIF_F_NTUPLE)
		gem_enable_flow_filters(bp, 1);

	spin_unlock_irqrestore(&bp->rx_fs_lock, flags);
	return 0;

err:
	spin_unlock_irqrestore(&bp->rx_fs_lock, flags);
	kfree(newfs);
	return ret;
}

static int gem_del_flow_filter(struct net_device *netdev,
		struct ethtool_rxnfc *cmd)
{
	struct macb *bp = netdev_priv(netdev);
	struct ethtool_rx_fs_item *item;
	struct ethtool_rx_flow_spec *fs;
	unsigned long flags;

	spin_lock_irqsave(&bp->rx_fs_lock, flags);

	list_for_each_entry(item, &bp->rx_fs_list.list, list) {
		if (item->fs.location == cmd->fs.location) {
			/* disable screener regs for the flow entry */
			fs = &(item->fs);
			netdev_dbg(netdev,
					"Deleting flow filter entry,type=%u,queue=%u,loc=%u,src=%08X,dst=%08X,ps=%u,pd=%u\n",
					fs->flow_type, (int)fs->ring_cookie, fs->location,
					htonl(fs->h_u.tcp_ip4_spec.ip4src),
					htonl(fs->h_u.tcp_ip4_spec.ip4dst),
					htons(fs->h_u.tcp_ip4_spec.psrc),
					htons(fs->h_u.tcp_ip4_spec.pdst));

			gem_writel_n(bp, SCRT2, fs->location, 0);

			list_del(&item->list);
			bp->rx_fs_list.count--;
			spin_unlock_irqrestore(&bp->rx_fs_lock, flags);
			kfree(item);
			return 0;
		}
	}

	spin_unlock_irqrestore(&bp->rx_fs_lock, flags);
	return -EINVAL;
}

static int gem_get_flow_entry(struct net_device *netdev,
		struct ethtool_rxnfc *cmd)
{
	struct macb *bp = netdev_priv(netdev);
	struct ethtool_rx_fs_item *item;

	list_for_each_entry(item, &bp->rx_fs_list.list, list) {
		if (item->fs.location == cmd->fs.location) {
			memcpy(&cmd->fs, &item->fs, sizeof(cmd->fs));
			return 0;
		}
	}
	return -EINVAL;
}

static int gem_get_all_flow_entries(struct net_device *netdev,
		struct ethtool_rxnfc *cmd, u32 *rule_locs)
{
	struct macb *bp = netdev_priv(netdev);
	struct ethtool_rx_fs_item *item;
	uint32_t cnt = 0;

	list_for_each_entry(item, &bp->rx_fs_list.list, list) {
		if (cnt == cmd->rule_cnt)
			return -EMSGSIZE;
		rule_locs[cnt] = item->fs.location;
		cnt++;
	}
	cmd->data = bp->max_tuples;
	cmd->rule_cnt = cnt;

	return 0;
}

static int gem_get_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd,
		u32 *rule_locs)
{
	struct macb *bp = netdev_priv(netdev);
	int ret = 0;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = bp->num_queues;
		break;
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = bp->rx_fs_list.count;
		break;
	case ETHTOOL_GRXCLSRULE:
		ret = gem_get_flow_entry(netdev, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		ret = gem_get_all_flow_entries(netdev, cmd, rule_locs);
		break;
	default:
		netdev_err(netdev,
			  "Command parameter %d is not supported\n", cmd->cmd);
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int gem_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct macb *bp = netdev_priv(netdev);
	int ret;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		if ((cmd->fs.location >= bp->max_tuples)
				|| (cmd->fs.ring_cookie >= bp->num_queues)) {
			ret = -EINVAL;
			break;
		}
		ret = gem_add_flow_filter(netdev, cmd);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		ret = gem_del_flow_filter(netdev, cmd);
		break;
	default:
		netdev_err(netdev,
			  "Command parameter %d is not supported\n", cmd->cmd);
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static const struct ethtool_ops macb_ethtool_ops = {
	.get_regs_len		= macb_get_regs_len,
	.get_regs		= macb_get_regs,
	.get_link		= ethtool_op_get_link,
	.get_ts_info		= ethtool_op_get_ts_info,
	.get_wol		= macb_get_wol,
	.set_wol		= macb_set_wol,
	.get_link_ksettings     = phy_ethtool_get_link_ksettings,
	.set_link_ksettings     = phy_ethtool_set_link_ksettings,
	.get_ringparam		= macb_get_ringparam,
	.set_ringparam		= macb_set_ringparam,
};

static const struct ethtool_ops gem_ethtool_ops = {
	.get_regs_len		= macb_get_regs_len,
	.get_regs		= macb_get_regs,
	.get_link		= ethtool_op_get_link,
	.get_ts_info		= macb_get_ts_info,
	.get_ethtool_stats	= gem_get_ethtool_stats,
	.get_strings		= gem_get_ethtool_strings,
	.get_sset_count		= gem_get_sset_count,
	.get_link_ksettings     = phy_ethtool_get_link_ksettings,
	.set_link_ksettings     = phy_ethtool_set_link_ksettings,
	.get_ringparam		= macb_get_ringparam,
	.set_ringparam		= macb_set_ringparam,
	.get_rxnfc			= gem_get_rxnfc,
	.set_rxnfc			= gem_set_rxnfc,
};

static int macb_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct phy_device *phydev = dev->phydev;
	struct macb *bp = netdev_priv(dev);

	if (!netif_running(dev))
		return -EINVAL;

	if (!phydev)
		return -ENODEV;

	if (!bp->ptp_info)
		return phy_mii_ioctl(phydev, rq, cmd);

	switch (cmd) {
	case SIOCSHWTSTAMP:
		return bp->ptp_info->set_hwtst(dev, rq, cmd);
	case SIOCGHWTSTAMP:
		return bp->ptp_info->get_hwtst(dev, rq);
	default:
		return phy_mii_ioctl(phydev, rq, cmd);
	}
}

static int macb_set_features(struct net_device *netdev,
			     netdev_features_t features)
{
	struct macb *bp = netdev_priv(netdev);
	netdev_features_t changed = features ^ netdev->features;

	/* TX checksum offload */
	if ((changed & NETIF_F_HW_CSUM) && macb_is_gem(bp)) {
		u32 dmacfg;

		dmacfg = gem_readl(bp, DMACFG);
		if (features & NETIF_F_HW_CSUM)
			dmacfg |= GEM_BIT(TXCOEN);
		else
			dmacfg &= ~GEM_BIT(TXCOEN);
		gem_writel(bp, DMACFG, dmacfg);
	}

	/* RX checksum offload */
	if ((changed & NETIF_F_RXCSUM) && macb_is_gem(bp)) {
		u32 netcfg;

		netcfg = gem_readl(bp, NCFGR);
		if (features & NETIF_F_RXCSUM &&
		    !(netdev->flags & IFF_PROMISC))
			netcfg |= GEM_BIT(RXCOEN);
		else
			netcfg &= ~GEM_BIT(RXCOEN);
		gem_writel(bp, NCFGR, netcfg);
	}

	/* RX Flow Filters */
	if ((changed & NETIF_F_NTUPLE) && macb_is_gem(bp)) {
		bool turn_on = features & NETIF_F_NTUPLE;

		gem_enable_flow_filters(bp, turn_on);
	}
	return 0;
}

static const struct net_device_ops macb_netdev_ops = {
	.ndo_open		= macb_open,
	.ndo_stop		= macb_close,
	.ndo_start_xmit		= macb_start_xmit,
	.ndo_set_rx_mode	= macb_set_rx_mode,
	.ndo_get_stats		= macb_get_stats,
	.ndo_do_ioctl		= macb_ioctl,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_change_mtu		= macb_change_mtu,
	.ndo_set_mac_address	= eth_mac_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= macb_poll_controller,
#endif
	.ndo_set_features	= macb_set_features,
	.ndo_features_check	= macb_features_check,
};

/* Configure peripheral capabilities according to device tree
 * and integration options used
 */
static void macb_configure_caps(struct macb *bp,
				const struct macb_config *dt_conf)
{
	u32 dcfg;

	if (dt_conf)
		bp->caps = dt_conf->caps;

	if (hw_is_gem(bp->regs, bp->native_io)) {
		bp->caps |= MACB_CAPS_MACB_IS_GEM;

		dcfg = gem_readl(bp, DCFG1);
		if (GEM_BFEXT(IRQCOR, dcfg) == 0)
			bp->caps |= MACB_CAPS_ISR_CLEAR_ON_WRITE;
		dcfg = gem_readl(bp, DCFG2);
		if ((dcfg & (GEM_BIT(RX_PKT_BUFF) | GEM_BIT(TX_PKT_BUFF))) == 0)
			bp->caps |= MACB_CAPS_FIFO_MODE;
#ifdef CONFIG_MACB_USE_HWSTAMP
		if (gem_has_ptp(bp)) {
			if (!GEM_BFEXT(TSU, gem_readl(bp, DCFG5)))
				pr_err("GEM doesn't support hardware ptp.\n");
			else {
				bp->hw_dma_cap |= HW_DMA_CAP_PTP;
				bp->ptp_info = &gem_ptp_info;
			}
		}
#endif
	}

	dev_dbg(&bp->pdev->dev, "Cadence caps 0x%08x\n", bp->caps);
}

static void macb_probe_queues(void __iomem *mem,
			      bool native_io,
			      unsigned int *queue_mask,
			      unsigned int *num_queues)
{
	unsigned int hw_q;

	*queue_mask = 0x1;
	*num_queues = 1;

	/* is it macb or gem ?
	 *
	 * We need to read directly from the hardware here because
	 * we are early in the probe process and don't have the
	 * MACB_CAPS_MACB_IS_GEM flag positioned
	 */
	if (!hw_is_gem(mem, native_io))
		return;

	/* bit 0 is never set but queue 0 always exists */
	*queue_mask = readl_relaxed(mem + GEM_DCFG6) & 0xff;

	*queue_mask |= 0x1;

	for (hw_q = 1; hw_q < MACB_MAX_QUEUES; ++hw_q)
		if (*queue_mask & (1 << hw_q))
			(*num_queues)++;
}

static int macb_clk_init(struct platform_device *pdev, struct clk **pclk,
			 struct clk **hclk, struct clk **tx_clk,
			 struct clk **rx_clk)
{
	struct macb_platform_data *pdata;
	int err;

	pdata = dev_get_platdata(&pdev->dev);
	if (pdata) {
		*pclk = pdata->pclk;
		*hclk = pdata->hclk;
	} else {
		*pclk = devm_clk_get(&pdev->dev, "pclk");
		*hclk = devm_clk_get(&pdev->dev, "hclk");
	}

	if (IS_ERR_OR_NULL(*pclk)) {
		err = PTR_ERR(*pclk);
		if (!err)
			err = -ENODEV;

		dev_err(&pdev->dev, "failed to get macb_clk (%d)\n", err);
		return err;
	}

	if (IS_ERR_OR_NULL(*hclk)) {
		err = PTR_ERR(*hclk);
		if (!err)
			err = -ENODEV;

		dev_err(&pdev->dev, "failed to get hclk (%d)\n", err);
		return err;
	}

	*tx_clk = devm_clk_get(&pdev->dev, "tx_clk");
	if (IS_ERR(*tx_clk))
		*tx_clk = NULL;

	*rx_clk = devm_clk_get(&pdev->dev, "rx_clk");
	if (IS_ERR(*rx_clk))
		*rx_clk = NULL;

	err = clk_prepare_enable(*pclk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable pclk (%d)\n", err);
		return err;
	}

	err = clk_prepare_enable(*hclk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable hclk (%d)\n", err);
		goto err_disable_pclk;
	}

	err = clk_prepare_enable(*tx_clk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable tx_clk (%d)\n", err);
		goto err_disable_hclk;
	}

	err = clk_prepare_enable(*rx_clk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable rx_clk (%d)\n", err);
		goto err_disable_txclk;
	}

	return 0;

err_disable_txclk:
	clk_disable_unprepare(*tx_clk);

err_disable_hclk:
	clk_disable_unprepare(*hclk);

err_disable_pclk:
	clk_disable_unprepare(*pclk);

	return err;
}

static int macb_init(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	unsigned int hw_q, q;
	struct macb *bp = netdev_priv(dev);
	struct macb_queue *queue;
	int err;
	u32 val, reg;

	bp->tx_ring_size = DEFAULT_TX_RING_SIZE;
	bp->rx_ring_size = DEFAULT_RX_RING_SIZE;

	/* set the queue register mapping once for all: queue0 has a special
	 * register mapping but we don't want to test the queue index then
	 * compute the corresponding register offset at run time.
	 */
	for (hw_q = 0, q = 0; hw_q < MACB_MAX_QUEUES; ++hw_q) {
		if (!(bp->queue_mask & (1 << hw_q)))
			continue;

		queue = &bp->queues[q];
		queue->bp = bp;
		netif_napi_add(dev, &queue->napi, macb_poll, 64);
		if (hw_q) {
			queue->ISR  = GEM_ISR(hw_q - 1);
			queue->IER  = GEM_IER(hw_q - 1);
			queue->IDR  = GEM_IDR(hw_q - 1);
			queue->IMR  = GEM_IMR(hw_q - 1);
			queue->TBQP = GEM_TBQP(hw_q - 1);
			queue->RBQP = GEM_RBQP(hw_q - 1);
			queue->RBQS = GEM_RBQS(hw_q - 1);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
			if (bp->hw_dma_cap & HW_DMA_CAP_64B) {
				queue->TBQPH = GEM_TBQPH(hw_q - 1);
				queue->RBQPH = GEM_RBQPH(hw_q - 1);
			}
#endif
		} else {
			/* queue0 uses legacy registers */
			queue->ISR  = MACB_ISR;
			queue->IER  = MACB_IER;
			queue->IDR  = MACB_IDR;
			queue->IMR  = MACB_IMR;
			queue->TBQP = MACB_TBQP;
			queue->RBQP = MACB_RBQP;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
			if (bp->hw_dma_cap & HW_DMA_CAP_64B) {
				queue->TBQPH = MACB_TBQPH;
				queue->RBQPH = MACB_RBQPH;
			}
#endif
		}

		/* get irq: here we use the linux queue index, not the hardware
		 * queue index. the queue irq definitions in the device tree
		 * must remove the optional gaps that could exist in the
		 * hardware queue mask.
		 */
		queue->irq = platform_get_irq(pdev, q);
		err = devm_request_irq(&pdev->dev, queue->irq, macb_interrupt,
				       IRQF_SHARED, dev->name, queue);
		if (err) {
			dev_err(&pdev->dev,
				"Unable to request IRQ %d (error %d)\n",
				queue->irq, err);
			return err;
		}

		INIT_WORK(&queue->tx_error_task, macb_tx_error_task);
		q++;
	}

	dev->netdev_ops = &macb_netdev_ops;

	/* setup appropriated routines according to adapter type */
	if (macb_is_gem(bp)) {
		bp->max_tx_length = GEM_MAX_TX_LEN;
		bp->macbgem_ops.mog_alloc_rx_buffers = gem_alloc_rx_buffers;
		bp->macbgem_ops.mog_free_rx_buffers = gem_free_rx_buffers;
		bp->macbgem_ops.mog_init_rings = gem_init_rings;
		bp->macbgem_ops.mog_rx = gem_rx;
		dev->ethtool_ops = &gem_ethtool_ops;
	} else {
		bp->max_tx_length = MACB_MAX_TX_LEN;
		bp->macbgem_ops.mog_alloc_rx_buffers = macb_alloc_rx_buffers;
		bp->macbgem_ops.mog_free_rx_buffers = macb_free_rx_buffers;
		bp->macbgem_ops.mog_init_rings = macb_init_rings;
		bp->macbgem_ops.mog_rx = macb_rx;
		dev->ethtool_ops = &macb_ethtool_ops;
	}

	/* Set features */
	dev->hw_features = NETIF_F_SG;

	/* Check LSO capability */
	if (GEM_BFEXT(PBUF_LSO, gem_readl(bp, DCFG6)))
		dev->hw_features |= MACB_NETIF_LSO;

	/* Checksum offload is only available on gem with packet buffer */
	if (macb_is_gem(bp) && !(bp->caps & MACB_CAPS_FIFO_MODE))
		dev->hw_features |= NETIF_F_HW_CSUM | NETIF_F_RXCSUM;
	if (bp->caps & MACB_CAPS_SG_DISABLED)
		dev->hw_features &= ~NETIF_F_SG;
	dev->features = dev->hw_features;

	/* Check RX Flow Filters support.
	 * Max Rx flows set by availability of screeners & compare regs:
	 * each 4-tuple define requires 1 T2 screener reg + 3 compare regs
	 */
	reg = gem_readl(bp, DCFG8);
	bp->max_tuples = min((GEM_BFEXT(SCR2CMP, reg) / 3),
			GEM_BFEXT(T2SCR, reg));
	if (bp->max_tuples > 0) {
		/* also needs one ethtype match to check IPv4 */
		if (GEM_BFEXT(SCR2ETH, reg) > 0) {
			/* program this reg now */
			reg = 0;
			reg = GEM_BFINS(ETHTCMP, (uint16_t)ETH_P_IP, reg);
			gem_writel_n(bp, ETHT, SCRT2_ETHT, reg);
			/* Filtering is supported in hw but don't enable it in kernel now */
			dev->hw_features |= NETIF_F_NTUPLE;
			/* init Rx flow definitions */
			INIT_LIST_HEAD(&bp->rx_fs_list.list);
			bp->rx_fs_list.count = 0;
			spin_lock_init(&bp->rx_fs_lock);
		} else
			bp->max_tuples = 0;
	}

	if (!(bp->caps & MACB_CAPS_USRIO_DISABLED)) {
		val = 0;
		if (bp->phy_interface == PHY_INTERFACE_MODE_RGMII)
			val = GEM_BIT(RGMII);
		else if (bp->phy_interface == PHY_INTERFACE_MODE_RMII &&
			 (bp->caps & MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII))
			val = MACB_BIT(RMII);
		else if (!(bp->caps & MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII))
			val = MACB_BIT(MII);

		if (bp->caps & MACB_CAPS_USRIO_HAS_CLKEN)
			val |= MACB_BIT(CLKEN);

		macb_or_gem_writel(bp, USRIO, val);
	}

	/* Set MII management clock divider */
	val = macb_mdc_clk_div(bp);
	val |= macb_dbw(bp);
	if (bp->phy_interface == PHY_INTERFACE_MODE_SGMII)
		val |= GEM_BIT(SGMIIEN) | GEM_BIT(PCSSEL);
	macb_writel(bp, NCFGR, val);

	return 0;
}

#if defined(CONFIG_OF)
/* 1518 rounded up */
#define AT91ETHER_MAX_RBUFF_SZ	0x600
/* max number of receive buffers */
#define AT91ETHER_MAX_RX_DESCR	9

/* Initialize and start the Receiver and Transmit subsystems */
static int at91ether_start(struct net_device *dev)
{
	struct macb *lp = netdev_priv(dev);
	struct macb_queue *q = &lp->queues[0];
	struct macb_dma_desc *desc;
	dma_addr_t addr;
	u32 ctl;
	int i;

	q->rx_ring = dma_alloc_coherent(&lp->pdev->dev,
					 (AT91ETHER_MAX_RX_DESCR *
					  macb_dma_desc_get_size(lp)),
					 &q->rx_ring_dma, GFP_KERNEL);
	if (!q->rx_ring)
		return -ENOMEM;

	q->rx_buffers = dma_alloc_coherent(&lp->pdev->dev,
					    AT91ETHER_MAX_RX_DESCR *
					    AT91ETHER_MAX_RBUFF_SZ,
					    &q->rx_buffers_dma, GFP_KERNEL);
	if (!q->rx_buffers) {
		dma_free_coherent(&lp->pdev->dev,
				  AT91ETHER_MAX_RX_DESCR *
				  macb_dma_desc_get_size(lp),
				  q->rx_ring, q->rx_ring_dma);
		q->rx_ring = NULL;
		return -ENOMEM;
	}

	addr = q->rx_buffers_dma;
	for (i = 0; i < AT91ETHER_MAX_RX_DESCR; i++) {
		desc = macb_rx_desc(q, i);
		macb_set_addr(lp, desc, addr);
		desc->ctrl = 0;
		addr += AT91ETHER_MAX_RBUFF_SZ;
	}

	/* Set the Wrap bit on the last descriptor */
	desc->addr |= MACB_BIT(RX_WRAP);

	/* Reset buffer index */
	q->rx_tail = 0;

	/* Program address of descriptor list in Rx Buffer Queue register */
	macb_writel(lp, RBQP, q->rx_ring_dma);

	/* Enable Receive and Transmit */
	ctl = macb_readl(lp, NCR);
	macb_writel(lp, NCR, ctl | MACB_BIT(RE) | MACB_BIT(TE));

	return 0;
}

/* Open the ethernet interface */
static int at91ether_open(struct net_device *dev)
{
	struct macb *lp = netdev_priv(dev);
	u32 ctl;
	int ret;

	/* Clear internal statistics */
	ctl = macb_readl(lp, NCR);
	macb_writel(lp, NCR, ctl | MACB_BIT(CLRSTAT));

	macb_set_hwaddr(lp);

	ret = at91ether_start(dev);
	if (ret)
		return ret;

	/* Enable MAC interrupts */
	macb_writel(lp, IER, MACB_BIT(RCOMP)	|
			     MACB_BIT(RXUBR)	|
			     MACB_BIT(ISR_TUND)	|
			     MACB_BIT(ISR_RLE)	|
			     MACB_BIT(TCOMP)	|
			     MACB_BIT(ISR_ROVR)	|
			     MACB_BIT(HRESP));

	/* schedule a link state check */
	phy_start(dev->phydev);

	netif_start_queue(dev);

	return 0;
}

/* Close the interface */
static int at91ether_close(struct net_device *dev)
{
	struct macb *lp = netdev_priv(dev);
	struct macb_queue *q = &lp->queues[0];
	u32 ctl;

	/* Disable Receiver and Transmitter */
	ctl = macb_readl(lp, NCR);
	macb_writel(lp, NCR, ctl & ~(MACB_BIT(TE) | MACB_BIT(RE)));

	/* Disable MAC interrupts */
	macb_writel(lp, IDR, MACB_BIT(RCOMP)	|
			     MACB_BIT(RXUBR)	|
			     MACB_BIT(ISR_TUND)	|
			     MACB_BIT(ISR_RLE)	|
			     MACB_BIT(TCOMP)	|
			     MACB_BIT(ISR_ROVR) |
			     MACB_BIT(HRESP));

	netif_stop_queue(dev);

	dma_free_coherent(&lp->pdev->dev,
			  AT91ETHER_MAX_RX_DESCR *
			  macb_dma_desc_get_size(lp),
			  q->rx_ring, q->rx_ring_dma);
	q->rx_ring = NULL;

	dma_free_coherent(&lp->pdev->dev,
			  AT91ETHER_MAX_RX_DESCR * AT91ETHER_MAX_RBUFF_SZ,
			  q->rx_buffers, q->rx_buffers_dma);
	q->rx_buffers = NULL;

	return 0;
}

/* Transmit packet */
static netdev_tx_t at91ether_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct macb *lp = netdev_priv(dev);

	if (macb_readl(lp, TSR) & MACB_BIT(RM9200_BNQ)) {
		netif_stop_queue(dev);

		/* Store packet information (to free when Tx completed) */
		lp->skb = skb;
		lp->skb_length = skb->len;
		lp->skb_physaddr = dma_map_single(NULL, skb->data, skb->len,
							DMA_TO_DEVICE);
		if (dma_mapping_error(NULL, lp->skb_physaddr)) {
			dev_kfree_skb_any(skb);
			dev->stats.tx_dropped++;
			netdev_err(dev, "%s: DMA mapping error\n", __func__);
			return NETDEV_TX_OK;
		}

		/* Set address of the data in the Transmit Address register */
		macb_writel(lp, TAR, lp->skb_physaddr);
		/* Set length of the packet in the Transmit Control register */
		macb_writel(lp, TCR, skb->len);

	} else {
		netdev_err(dev, "%s called, but device is busy!\n", __func__);
		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
}

/* Extract received frame from buffer descriptors and sent to upper layers.
 * (Called from interrupt context)
 */
static void at91ether_rx(struct net_device *dev)
{
	struct macb *lp = netdev_priv(dev);
	struct macb_queue *q = &lp->queues[0];
	struct macb_dma_desc *desc;
	unsigned char *p_recv;
	struct sk_buff *skb;
	unsigned int pktlen;

	desc = macb_rx_desc(q, q->rx_tail);
	while (desc->addr & MACB_BIT(RX_USED)) {
		p_recv = q->rx_buffers + q->rx_tail * AT91ETHER_MAX_RBUFF_SZ;
		pktlen = MACB_BF(RX_FRMLEN, desc->ctrl);
		skb = netdev_alloc_skb(dev, pktlen + 2);
		if (skb) {
			skb_reserve(skb, 2);
			skb_put_data(skb, p_recv, pktlen);

			skb->protocol = eth_type_trans(skb, dev);
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += pktlen;
			netif_rx(skb);
		} else {
			dev->stats.rx_dropped++;
		}

		if (desc->ctrl & MACB_BIT(RX_MHASH_MATCH))
			dev->stats.multicast++;

		/* reset ownership bit */
		desc->addr &= ~MACB_BIT(RX_USED);

		/* wrap after last buffer */
		if (q->rx_tail == AT91ETHER_MAX_RX_DESCR - 1)
			q->rx_tail = 0;
		else
			q->rx_tail++;

		desc = macb_rx_desc(q, q->rx_tail);
	}
}

/* MAC interrupt handler */
static irqreturn_t at91ether_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct macb *lp = netdev_priv(dev);
	u32 intstatus, ctl;

	/* MAC Interrupt Status register indicates what interrupts are pending.
	 * It is automatically cleared once read.
	 */
	intstatus = macb_readl(lp, ISR);

	/* Receive complete */
	if (intstatus & MACB_BIT(RCOMP))
		at91ether_rx(dev);

	/* Transmit complete */
	if (intstatus & MACB_BIT(TCOMP)) {
		/* The TCOM bit is set even if the transmission failed */
		if (intstatus & (MACB_BIT(ISR_TUND) | MACB_BIT(ISR_RLE)))
			dev->stats.tx_errors++;

		if (lp->skb) {
			dev_kfree_skb_irq(lp->skb);
			lp->skb = NULL;
			dma_unmap_single(NULL, lp->skb_physaddr,
					 lp->skb_length, DMA_TO_DEVICE);
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += lp->skb_length;
		}
		netif_wake_queue(dev);
	}

	/* Work-around for EMAC Errata section 41.3.1 */
	if (intstatus & MACB_BIT(RXUBR)) {
		ctl = macb_readl(lp, NCR);
		macb_writel(lp, NCR, ctl & ~MACB_BIT(RE));
		wmb();
		macb_writel(lp, NCR, ctl | MACB_BIT(RE));
	}

	if (intstatus & MACB_BIT(ISR_ROVR))
		netdev_err(dev, "ROVR error\n");

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void at91ether_poll_controller(struct net_device *dev)
{
	unsigned long flags;

	local_irq_save(flags);
	at91ether_interrupt(dev->irq, dev);
	local_irq_restore(flags);
}
#endif

static const struct net_device_ops at91ether_netdev_ops = {
	.ndo_open		= at91ether_open,
	.ndo_stop		= at91ether_close,
	.ndo_start_xmit		= at91ether_start_xmit,
	.ndo_get_stats		= macb_get_stats,
	.ndo_set_rx_mode	= macb_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_do_ioctl		= macb_ioctl,
	.ndo_validate_addr	= eth_validate_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= at91ether_poll_controller,
#endif
};

static int at91ether_clk_init(struct platform_device *pdev, struct clk **pclk,
			      struct clk **hclk, struct clk **tx_clk,
			      struct clk **rx_clk)
{
	int err;

	*hclk = NULL;
	*tx_clk = NULL;
	*rx_clk = NULL;

	*pclk = devm_clk_get(&pdev->dev, "ether_clk");
	if (IS_ERR(*pclk))
		return PTR_ERR(*pclk);

	err = clk_prepare_enable(*pclk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable pclk (%d)\n", err);
		return err;
	}

	return 0;
}

static int at91ether_init(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct macb *bp = netdev_priv(dev);
	int err;
	u32 reg;

	bp->queues[0].bp = bp;

	dev->netdev_ops = &at91ether_netdev_ops;
	dev->ethtool_ops = &macb_ethtool_ops;

	err = devm_request_irq(&pdev->dev, dev->irq, at91ether_interrupt,
			       0, dev->name, dev);
	if (err)
		return err;

	macb_writel(bp, NCR, 0);

	reg = MACB_BF(CLK, MACB_CLK_DIV32) | MACB_BIT(BIG);
	if (bp->phy_interface == PHY_INTERFACE_MODE_RMII)
		reg |= MACB_BIT(RM9200_RMII);

	macb_writel(bp, NCFGR, reg);

	return 0;
}

static const struct macb_config at91sam9260_config = {
	.caps = MACB_CAPS_USRIO_HAS_CLKEN | MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config sama5d3macb_config = {
	.caps = MACB_CAPS_SG_DISABLED
	      | MACB_CAPS_USRIO_HAS_CLKEN | MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config pc302gem_config = {
	.caps = MACB_CAPS_SG_DISABLED | MACB_CAPS_GIGABIT_MODE_AVAILABLE,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config sama5d2_config = {
	.caps = MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config sama5d3_config = {
	.caps = MACB_CAPS_SG_DISABLED | MACB_CAPS_GIGABIT_MODE_AVAILABLE
	      | MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII | MACB_CAPS_JUMBO,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
	.jumbo_max_len = 10240,
};

static const struct macb_config sama5d4_config = {
	.caps = MACB_CAPS_USRIO_DEFAULT_IS_MII_GMII,
	.dma_burst_length = 4,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config emac_config = {
	.caps = MACB_CAPS_NEEDS_RSTONUBR,
	.clk_init = at91ether_clk_init,
	.init = at91ether_init,
};

static const struct macb_config np4_config = {
	.caps = MACB_CAPS_USRIO_DISABLED,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct macb_config zynqmp_config = {
	.caps = MACB_CAPS_GIGABIT_MODE_AVAILABLE |
			MACB_CAPS_JUMBO |
			MACB_CAPS_GEM_HAS_PTP | MACB_CAPS_BD_RD_PREFETCH,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
	.jumbo_max_len = 10240,
};

static const struct macb_config zynq_config = {
	.caps = MACB_CAPS_GIGABIT_MODE_AVAILABLE | MACB_CAPS_NO_GIGABIT_HALF |
		MACB_CAPS_NEEDS_RSTONUBR,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
};

static const struct of_device_id macb_dt_ids[] = {
	{ .compatible = "cdns,at32ap7000-macb" },
	{ .compatible = "cdns,at91sam9260-macb", .data = &at91sam9260_config },
	{ .compatible = "cdns,macb" },
	{ .compatible = "cdns,np4-macb", .data = &np4_config },
	{ .compatible = "cdns,pc302-gem", .data = &pc302gem_config },
	{ .compatible = "cdns,gem", .data = &pc302gem_config },
	{ .compatible = "atmel,sama5d2-gem", .data = &sama5d2_config },
	{ .compatible = "atmel,sama5d3-gem", .data = &sama5d3_config },
	{ .compatible = "atmel,sama5d3-macb", .data = &sama5d3macb_config },
	{ .compatible = "atmel,sama5d4-gem", .data = &sama5d4_config },
	{ .compatible = "cdns,at91rm9200-emac", .data = &emac_config },
	{ .compatible = "cdns,emac", .data = &emac_config },
	{ .compatible = "cdns,zynqmp-gem", .data = &zynqmp_config},
	{ .compatible = "cdns,zynq-gem", .data = &zynq_config },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, macb_dt_ids);
#endif /* CONFIG_OF */

static const struct macb_config default_gem_config = {
	.caps = MACB_CAPS_GIGABIT_MODE_AVAILABLE |
			MACB_CAPS_JUMBO |
			MACB_CAPS_GEM_HAS_PTP,
	.dma_burst_length = 16,
	.clk_init = macb_clk_init,
	.init = macb_init,
	.jumbo_max_len = 10240,
};

static int macb_probe(struct platform_device *pdev)
{
	const struct macb_config *macb_config = &default_gem_config;
	int (*clk_init)(struct platform_device *, struct clk **,
			struct clk **, struct clk **,  struct clk **)
					      = macb_config->clk_init;
	int (*init)(struct platform_device *) = macb_config->init;
	struct device_node *np = pdev->dev.of_node;
	struct clk *pclk, *hclk = NULL, *tx_clk = NULL, *rx_clk = NULL;
	unsigned int queue_mask, num_queues;
	struct macb_platform_data *pdata;
	bool native_io;
	struct phy_device *phydev;
	struct net_device *dev;
	struct resource *regs;
	void __iomem *mem;
	const char *mac;
	struct macb *bp;
	int err, val;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mem = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	if (np) {
		const struct of_device_id *match;

		match = of_match_node(macb_dt_ids, np);
		if (match && match->data) {
			macb_config = match->data;
			clk_init = macb_config->clk_init;
			init = macb_config->init;
		}
	}

	err = clk_init(pdev, &pclk, &hclk, &tx_clk, &rx_clk);
	if (err)
		return err;

	native_io = hw_is_native_io(mem);

	macb_probe_queues(mem, native_io, &queue_mask, &num_queues);
	dev = alloc_etherdev_mq(sizeof(*bp), num_queues);
	if (!dev) {
		err = -ENOMEM;
		goto err_disable_clocks;
	}

	dev->base_addr = regs->start;

	SET_NETDEV_DEV(dev, &pdev->dev);

	bp = netdev_priv(dev);
	bp->pdev = pdev;
	bp->dev = dev;
	bp->regs = mem;
	bp->native_io = native_io;
	if (native_io) {
		bp->macb_reg_readl = hw_readl_native;
		bp->macb_reg_writel = hw_writel_native;
	} else {
		bp->macb_reg_readl = hw_readl;
		bp->macb_reg_writel = hw_writel;
	}
	bp->num_queues = num_queues;
	bp->queue_mask = queue_mask;
	if (macb_config)
		bp->dma_burst_length = macb_config->dma_burst_length;
	bp->pclk = pclk;
	bp->hclk = hclk;
	bp->tx_clk = tx_clk;
	bp->rx_clk = rx_clk;
	if (macb_config)
		bp->jumbo_max_len = macb_config->jumbo_max_len;

	bp->wol = 0;
	if (of_get_property(np, "magic-packet", NULL))
		bp->wol |= MACB_WOL_HAS_MAGIC_PACKET;
	device_set_wakeup_capable(&pdev->dev, bp->wol & MACB_WOL_HAS_MAGIC_PACKET);

	spin_lock_init(&bp->lock);
	spin_lock_init(&bp->stats_lock);

	/* setup capabilities */
	macb_configure_caps(bp, macb_config);

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	if (GEM_BFEXT(DAW64, gem_readl(bp, DCFG6))) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(44));
		if (err) {
			dev_err(&pdev->dev, "failed to set DMA mask\n");
			goto err_out_free_netdev;
		}
		bp->hw_dma_cap |= HW_DMA_CAP_64B;
	}
#endif
	platform_set_drvdata(pdev, dev);

	dev->irq = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		err = dev->irq;
		goto err_out_free_netdev;
	}

	/* MTU range: 68 - 1500 or 10240 */
	dev->min_mtu = GEM_MTU_MIN_SIZE;
	if (bp->caps & MACB_CAPS_JUMBO)
		dev->max_mtu = gem_readl(bp, JML) - ETH_HLEN - ETH_FCS_LEN;
	else
		dev->max_mtu = ETH_DATA_LEN;

	if (bp->caps & MACB_CAPS_BD_RD_PREFETCH) {
		val = GEM_BFEXT(RXBD_RDBUFF, gem_readl(bp, DCFG10));
		if (val)
			bp->rx_bd_rd_prefetch = (2 << (val - 1)) *
						macb_dma_desc_get_size(bp);

		val = GEM_BFEXT(TXBD_RDBUFF, gem_readl(bp, DCFG10));
		if (val)
			bp->tx_bd_rd_prefetch = (2 << (val - 1)) *
						macb_dma_desc_get_size(bp);
	}

	bp->rx_intr_mask = MACB_RX_INT_FLAGS;
	if (bp->caps & MACB_CAPS_NEEDS_RSTONUBR)
		bp->rx_intr_mask |= MACB_BIT(RXUBR);

	mac = of_get_mac_address(np);
	if (mac) {
		ether_addr_copy(bp->dev->dev_addr, mac);
	} else {
		err = of_get_nvmem_mac_address(np, bp->dev->dev_addr);
		if (err) {
			if (err == -EPROBE_DEFER)
				goto err_out_free_netdev;
			macb_get_hwaddr(bp);
		}
	}

	err = of_get_phy_mode(np);
	if (err < 0) {
		pdata = dev_get_platdata(&pdev->dev);
		if (pdata && pdata->is_rmii)
			bp->phy_interface = PHY_INTERFACE_MODE_RMII;
		else
			bp->phy_interface = PHY_INTERFACE_MODE_MII;
	} else {
		bp->phy_interface = err;
	}

	/* IP specific init */
	err = init(pdev);
	if (err)
		goto err_out_free_netdev;

	err = macb_mii_init(bp);
	if (err)
		goto err_out_free_netdev;

	phydev = dev->phydev;

	netif_carrier_off(dev);

	err = register_netdev(dev);
	if (err) {
		dev_err(&pdev->dev, "Cannot register net device, aborting.\n");
		goto err_out_unregister_mdio;
	}

	tasklet_init(&bp->hresp_err_tasklet, macb_hresp_error_task,
		     (unsigned long)bp);

	phy_attached_info(phydev);

	netdev_info(dev, "Cadence %s rev 0x%08x at 0x%08lx irq %d (%pM)\n",
		    macb_is_gem(bp) ? "GEM" : "MACB", macb_readl(bp, MID),
		    dev->base_addr, dev->irq, dev->dev_addr);

	return 0;

err_out_unregister_mdio:
	phy_disconnect(dev->phydev);
	mdiobus_unregister(bp->mii_bus);
	of_node_put(bp->phy_node);
	if (np && of_phy_is_fixed_link(np))
		of_phy_deregister_fixed_link(np);
	mdiobus_free(bp->mii_bus);

err_out_free_netdev:
	free_netdev(dev);

err_disable_clocks:
	clk_disable_unprepare(tx_clk);
	clk_disable_unprepare(hclk);
	clk_disable_unprepare(pclk);
	clk_disable_unprepare(rx_clk);

	return err;
}

static int macb_remove(struct platform_device *pdev)
{
	struct net_device *dev;
	struct macb *bp;
	struct device_node *np = pdev->dev.of_node;

	dev = platform_get_drvdata(pdev);

	if (dev) {
		bp = netdev_priv(dev);
		if (dev->phydev)
			phy_disconnect(dev->phydev);
		mdiobus_unregister(bp->mii_bus);
		if (np && of_phy_is_fixed_link(np))
			of_phy_deregister_fixed_link(np);
		dev->phydev = NULL;
		mdiobus_free(bp->mii_bus);

		unregister_netdev(dev);
		tasklet_kill(&bp->hresp_err_tasklet);
		clk_disable_unprepare(bp->tx_clk);
		clk_disable_unprepare(bp->hclk);
		clk_disable_unprepare(bp->pclk);
		clk_disable_unprepare(bp->rx_clk);
		of_node_put(bp->phy_node);
		free_netdev(dev);
	}

	return 0;
}

static int __maybe_unused macb_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct net_device *netdev = platform_get_drvdata(pdev);
	struct macb *bp = netdev_priv(netdev);

	netif_carrier_off(netdev);
	netif_device_detach(netdev);

	if (bp->wol & MACB_WOL_ENABLED) {
		macb_writel(bp, IER, MACB_BIT(WOL));
		macb_writel(bp, WOL, MACB_BIT(MAG));
		enable_irq_wake(bp->queues[0].irq);
	} else {
		clk_disable_unprepare(bp->tx_clk);
		clk_disable_unprepare(bp->hclk);
		clk_disable_unprepare(bp->pclk);
		clk_disable_unprepare(bp->rx_clk);
	}

	return 0;
}

static int __maybe_unused macb_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct net_device *netdev = platform_get_drvdata(pdev);
	struct macb *bp = netdev_priv(netdev);

	if (bp->wol & MACB_WOL_ENABLED) {
		macb_writel(bp, IDR, MACB_BIT(WOL));
		macb_writel(bp, WOL, 0);
		disable_irq_wake(bp->queues[0].irq);
	} else {
		clk_prepare_enable(bp->pclk);
		clk_prepare_enable(bp->hclk);
		clk_prepare_enable(bp->tx_clk);
		clk_prepare_enable(bp->rx_clk);
	}

	netif_device_attach(netdev);

	return 0;
}

static SIMPLE_DEV_PM_OPS(macb_pm_ops, macb_suspend, macb_resume);

static struct platform_driver macb_driver = {
	.probe		= macb_probe,
	.remove		= macb_remove,
	.driver		= {
		.name		= "macb",
		.of_match_table	= of_match_ptr(macb_dt_ids),
		.pm	= &macb_pm_ops,
	},
};

module_platform_driver(macb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence MACB/GEM Ethernet driver");
MODULE_AUTHOR("Haavard Skinnemoen (Atmel)");
MODULE_ALIAS("platform:macb");
