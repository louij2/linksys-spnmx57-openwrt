// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2015, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2020 Sartura Ltd. */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#define MDIO_MODE_REG				0x40
#define   MDIO_MODE_MDC_MODE			BIT(12)
/* 0 = Clause 22, 1 = Clause 45 */
#define   MDIO_MODE_C45				BIT(8)
#define   MDIO_MODE_DIV_MASK			GENMASK(7, 0)
#define     MDIO_MODE_DIV(x)			FIELD_PREP(MDIO_MODE_DIV_MASK, (x) - 1)
#define     MDIO_MODE_DIV_1			0x0
#define     MDIO_MODE_DIV_2			0x1
#define     MDIO_MODE_DIV_4			0x3
#define     MDIO_MODE_DIV_8			0x7
#define     MDIO_MODE_DIV_16			0xf
#define     MDIO_MODE_DIV_32			0x1f
#define     MDIO_MODE_DIV_64			0x3f
#define     MDIO_MODE_DIV_128			0x7f
#define     MDIO_MODE_DIV_256			0xff
#define MDIO_ADDR_REG				0x44
#define MDIO_DATA_WRITE_REG			0x48
#define MDIO_DATA_READ_REG			0x4c
#define MDIO_CMD_REG				0x50
#define MDIO_CMD_ACCESS_BUSY		BIT(16)
#define MDIO_CMD_ACCESS_START		BIT(8)
#define MDIO_CMD_ACCESS_CODE_READ	0
#define MDIO_CMD_ACCESS_CODE_WRITE	1
#define MDIO_CMD_ACCESS_CODE_C45_ADDR	0
#define MDIO_CMD_ACCESS_CODE_C45_WRITE	1
#define MDIO_CMD_ACCESS_CODE_C45_READ	2

#define IPQ4019_MDIO_TIMEOUT	10000
#define IPQ4019_MDIO_SLEEP		10

/* MDIO clock source frequency is fixed to 100M */
#define IPQ_MDIO_CLK_RATE	100000000

#define IPQ_PHY_SET_DELAY_US	100000

/*
 * QCA8084/QCA8386 preinit, reached only when the mdio node carries
 * qcom,qca8084-preinit in the DTS. Reverse engineered from the vendor
 * U-Boot's APPSBL partition (Ghidra decompilation, not a datasheet) --
 * see docs/uboot-qca8084-protocol.md in the linksys-spnmx57-openwrt repo.
 *
 * The QCA8386 switch exposes its internal 32-bit register space over plain
 * Clause 22 MDIO, indirectly, via a fixed pseudo-PHY pair:
 *   - write the page (bits [23:8] of the logical address) to phy 0x18,
 *     reg 0x0c, then wait ~100us for it to settle
 *   - the 32-bit value then lives at phy 0x10, two consecutive 16-bit
 *     registers (logical_addr & 0x1c) and +2, low half then high half
 *
 * This first cut is deliberately READ-ONLY. It reads EPHY_CFG (the register
 * holding each EPHY's current strap address as a 5-bit field) and logs it,
 * to settle whether the addresses already default to 1,2,3,4 in hardware
 * before anything writes to a switch-internal register at all. The vendor's
 * own U-Boot never programs EPHY_CFG either, only reads it, which is the
 * whole reason this question is open.
 */
#define QCA8084_PAGE_PHY	0x18
#define QCA8084_PAGE_REG	0x0c
#define QCA8084_DATA_PHY	0x10
#define QCA8084_EPHY_CFG	0x0c90f018u

static int qca8084_reg_read32(struct mii_bus *bus, u32 addr, u32 *val)
{
	int lo, hi, ret;

	/* bus->read/write directly, NOT mdiobus_read/write: this now runs
	 * BEFORE of_mdiobus_register(), and bus->mdio_lock is only
	 * initialised INSIDE __mdiobus_register() (mutex_init(&bus->mdio_lock)
	 * happens there, not at allocation -- checked against the actual
	 * kernel source after an earlier version of this patch silently read
	 * back 0xffffffff from every transaction, which is consistent with
	 * mdiobus_read() taking an uninitialised mutex). At this point in
	 * probe() the bus is not yet visible to anything else, so there is no
	 * concurrent access to guard against and no lock is needed yet.
	 */
	ret = bus->write(bus, QCA8084_PAGE_PHY, QCA8084_PAGE_REG,
			  (addr & 0xffffff) >> 8);
	if (ret < 0)
		return ret;
	udelay(100);

	lo = bus->read(bus, QCA8084_DATA_PHY, addr & 0x1c);
	if (lo < 0)
		return lo;
	hi = bus->read(bus, QCA8084_DATA_PHY, (addr & 0x1c) + 2);
	if (hi < 0)
		return hi;

	*val = ((u32)(u16)lo) | (((u32)(u16)hi) << 16);
	return 0;
}

static int qca8084_reg_write32(struct mii_bus *bus, u32 addr, u32 val)
{
	int ret;
	u32 slot = addr & 0x1c;

	ret = bus->write(bus, QCA8084_PAGE_PHY, QCA8084_PAGE_REG,
			  (addr & 0xffffff) >> 8);
	if (ret < 0)
		return ret;
	udelay(100);

	ret = bus->write(bus, QCA8084_DATA_PHY, slot, val & 0xffff);
	if (ret < 0)
		return ret;
	return bus->write(bus, QCA8084_DATA_PHY, slot + 2, val >> 16);
}

/* Set bit 0 (transcribed from FUN_4a94c568; meaning inferred as
 * "clock enable" from context, not confirmed against a datasheet).
 */
static int qca8084_clk_enable(struct mii_bus *bus, u32 reg)
{
	u32 val;
	int ret = qca8084_reg_read32(bus, reg, &val);

	if (ret)
		return ret;
	return qca8084_reg_write32(bus, reg, val | 1);
}

/* Assert bit 2, wait ~21ms, deassert (transcribed from FUN_4a94c57e;
 * meaning inferred as "reset pulse" from the assert/wait/deassert shape).
 */
static int qca8084_reset_pulse(struct mii_bus *bus, u32 reg)
{
	u32 val;
	int ret = qca8084_reg_read32(bus, reg, &val);

	if (ret)
		return ret;
	ret = qca8084_reg_write32(bus, reg, val | 4);
	if (ret)
		return ret;
	usleep_range(21000, 22000);
	return qca8084_reg_write32(bus, reg, val & ~4u);
}

/* Standard Qualcomm PHY "debug register" access: write the sub-register
 * address to MDIO reg 0x1d, then the value is read/written via reg 0x1e.
 * The same convention mainline's at803x.c already uses for other Qualcomm
 * PHYs, applied here to each EPHY's own MDIO address.
 */
static int qca8084_debug_write(struct mii_bus *bus, int phy_addr,
				u16 debug_reg, u16 mask, u16 set)
{
	int val, ret;

	ret = bus->write(bus, phy_addr, 0x1d, debug_reg);
	if (ret < 0)
		return ret;
	val = bus->read(bus, phy_addr, 0x1e);
	if (val < 0)
		return val;
	ret = bus->write(bus, phy_addr, 0x1d, debug_reg);
	if (ret < 0)
		return ret;
	return bus->write(bus, phy_addr, 0x1e, (val & ~mask) | set);
}

/* Per-port calibration source registers and the bitfield extraction each
 * one needs -- transcribed exactly from the decompiled calibration loop
 * (FUN_4a94c630), not re-derived. Ports 1 and 2 share the same extraction
 * shape and only differ in which register they read.
 */
struct qca8084_calib_src {
	u32 reg;
	u32 trim_a_shift, trim_a_mask;
	u32 trim_b_shift, trim_b_mask;
};

static const struct qca8084_calib_src qca8084_calib_srcs[4] = {
	/* port 0 */ { 0x0c900048, 0x12, 0x3fffff, 0x16, 0x7ffffff },
	/* port 1 */ { 0x0c900060, 0x17, 0x7ffffff, 0x1b, 0xffffffff },
	/* port 2 */ { 0x0c900068, 0x17, 0x7ffffff, 0x1b, 0xffffffff },
	/* port 3 */ { 0x0c90005c, 0x0e, 0x3ffff,   0x12, 0x7fffff },
};

static void qca8084_preinit(struct mii_bus *bus)
{
	static const u32 clk_a = 0x0c8001a8, clk_b = 0x0c8001ac;
	static const u32 ephy_clk[4] = { 0x0c8001b0, 0x0c8001b4,
					  0x0c8001b8, 0x0c8001bc };
	u32 ephy_cfg, gate_reg;
	u32 reg;
	int port, ret;

	/* Now called before of_mdiobus_register(), so the MDIO controller's
	 * own reset/clock-divider setup (normally run as part of bus
	 * registration) has NOT happened yet at this point -- without it,
	 * every transaction on this bus reads back all-1s (confirmed: the
	 * first attempt at this ordering read EPHY_CFG as 0xffffffff, not a
	 * real value). bus->reset is already assigned by this point in
	 * probe() and only touches priv fields (membase, mdio_clk) set up
	 * earlier in probe() too, so it is safe to call directly here
	 * without the bus being registered.
	 */
	if (bus->reset)
		bus->reset(bus);

	ret = qca8084_reg_read32(bus, QCA8084_EPHY_CFG, &ephy_cfg);
	if (ret) {
		dev_warn(bus->parent,
			 "qca8084: EPHY_CFG read failed (%d), aborting preinit\n",
			 ret);
		return;
	}
	dev_info(bus->parent, "qca8084: EPHY_CFG = 0x%08x before preinit\n",
		 ephy_cfg);
	for (port = 0; port < 4; port++)
		dev_info(bus->parent, "qca8084: port %d strap addr = 0x%x\n",
			 port, (ephy_cfg >> (port * 5)) & 0x1f);

	/* clock enable, then reset-pulse, both shared blocks first */
	qca8084_clk_enable(bus, clk_a);
	qca8084_clk_enable(bus, clk_b);
	qca8084_reset_pulse(bus, clk_a);
	qca8084_reset_pulse(bus, clk_b);

	/* clear bit 0 across 8 registers 0x058..0x138 step 0x20 -- transcribed
	 * as-is from FUN_4a94c630; meaning not confirmed (isolate/bypass?).
	 */
	for (reg = 0x0c800058; reg != 0x0c800158; reg += 0x20) {
		u32 val;

		ret = qca8084_reg_read32(bus, reg, &val);
		if (ret) {
			dev_warn(bus->parent,
				 "qca8084: isolate-clear read 0x%x failed (%d)\n",
				 reg, ret);
			continue;
		}
		qca8084_reg_write32(bus, reg, val & ~1u);
	}

	/* per-EPHY clock enable, then reset-pulse, all 4 shared blocks first */
	for (port = 0; port < 4; port++)
		qca8084_clk_enable(bus, ephy_clk[port]);
	for (port = 0; port < 4; port++)
		qca8084_reset_pulse(bus, ephy_clk[port]);

	/* Calibration loop is gated: only runs if this status field is 1 or 2.
	 * Meaning not confirmed (chip variant / package mode?), transcribed
	 * as-is.
	 */
	ret = qca8084_reg_read32(bus, 0x0c900014, &gate_reg);
	if (ret) {
		dev_warn(bus->parent, "qca8084: gate register read failed (%d)\n",
			 ret);
	} else {
		bool vendor_gate = (((gate_reg & 0xffffff) >> 0x10) - 1) < 2;

		dev_info(bus->parent,
			 "qca8084: gate register 0x%08x (vendor condition %s) -- calibrating anyway to test whether it is a hard requirement\n",
			 gate_reg, vendor_gate ? "true" : "false");
	}
	if (!ret) {
		for (port = 0; port < 4; port++) {
			const struct qca8084_calib_src *src = &qca8084_calib_srcs[port];
			u32 srcval, ephy_cfg_now;
			u32 trim_a, trim_b;
			int phy_addr;

			ret = qca8084_reg_read32(bus, QCA8084_EPHY_CFG, &ephy_cfg_now);
			if (ret)
				break;
			phy_addr = (ephy_cfg_now >> (port * 5)) & 0x1f;

			ret = qca8084_reg_read32(bus, src->reg, &srcval);
			if (ret)
				break;
			trim_a = (srcval & src->trim_a_mask) >> src->trim_a_shift;
			/* AND with 0xffffffff is a no-op; ports 1/2 use that as the
			 * mask because the decompiled source applies no mask there
			 * at all before the shift -- kept explicit rather than
			 * special-cased, so this line means the same thing for
			 * every port.
			 */
			trim_b = (srcval & src->trim_b_mask) >> src->trim_b_shift;

			qca8084_debug_write(bus, phy_addr, 0x180, 0x00f0,
					     (trim_a & 0xf) << 4);
			qca8084_debug_write(bus, phy_addr, 0x280, 0x001f,
					     trim_b & 0x1f);

			dev_info(bus->parent,
				 "qca8084: port %d (phy 0x%x) calibrated, trim_a=0x%x trim_b=0x%x\n",
				 port, phy_addr, trim_a, trim_b);
		}
	} else {
		dev_info(bus->parent,
			 "qca8084: gate register 0x%08x, calibration loop skipped\n",
			 gate_reg);
	}

	/* final step: clear bits [21:20] of EPHY_CFG and settle */
	ret = qca8084_reg_read32(bus, QCA8084_EPHY_CFG, &ephy_cfg);
	if (!ret) {
		qca8084_reg_write32(bus, QCA8084_EPHY_CFG, ephy_cfg & 0xffcfffffu);
		usleep_range(11000, 12000);
	}

	ret = qca8084_reg_read32(bus, QCA8084_EPHY_CFG, &ephy_cfg);
	if (!ret)
		dev_info(bus->parent, "qca8084: EPHY_CFG = 0x%08x after preinit\n",
			 ephy_cfg);
}

struct ipq4019_mdio_data {
	void __iomem	*membase;
	void __iomem *eth_ldo_rdy;
	struct clk *mdio_clk;
	unsigned int mdc_rate;
};

static int ipq4019_mdio_wait_busy(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int busy;

	return readl_poll_timeout(priv->membase + MDIO_CMD_REG, busy,
				  (busy & MDIO_CMD_ACCESS_BUSY) == 0,
				  IPQ4019_MDIO_SLEEP, IPQ4019_MDIO_TIMEOUT);
}

static int ipq4019_mdio_read_c45(struct mii_bus *bus, int mii_id, int mmd,
				 int reg)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data |= MDIO_MODE_C45;

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and mmd */
	writel((mii_id << 8) | mmd, priv->membase + MDIO_ADDR_REG);

	/* issue reg */
	writel(reg, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_ADDR;

	/* issue read command */
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait read complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_READ;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Read and return data */
	return readl(priv->membase + MDIO_DATA_READ_REG);
}

static int ipq4019_mdio_read_c22(struct mii_bus *bus, int mii_id, int regnum)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data &= ~MDIO_MODE_C45;

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and reg */
	writel((mii_id << 8) | regnum, priv->membase + MDIO_ADDR_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_READ;

	/* issue read command */
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait read complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Read and return data */
	return readl(priv->membase + MDIO_DATA_READ_REG);
}

static int ipq4019_mdio_write_c45(struct mii_bus *bus, int mii_id, int mmd,
				  int reg, u16 value)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data |= MDIO_MODE_C45;

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and mmd */
	writel((mii_id << 8) | mmd, priv->membase + MDIO_ADDR_REG);

	/* issue reg */
	writel(reg, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_ADDR;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* issue write data */
	writel(value, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_WRITE;
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait write complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	return 0;
}

static int ipq4019_mdio_write_c22(struct mii_bus *bus, int mii_id, int regnum,
				  u16 value)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Enter Clause 22 mode */
	data = readl(priv->membase + MDIO_MODE_REG);

	data &= ~MDIO_MODE_C45;

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and reg */
	writel((mii_id << 8) | regnum, priv->membase + MDIO_ADDR_REG);

	/* issue write data */
	writel(value, priv->membase + MDIO_DATA_WRITE_REG);

	/* issue write command */
	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_WRITE;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait write complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	return 0;
}

static int ipq4019_mdio_set_div(struct ipq4019_mdio_data *priv)
{
	unsigned long ahb_rate;
	int div;
	u32 val;

	/* If we don't have a clock for AHB use the fixed value */
	ahb_rate = IPQ_MDIO_CLK_RATE;
	if (priv->mdio_clk)
		ahb_rate = clk_get_rate(priv->mdio_clk);

	/* MDC rate is ahb_rate/(MDIO_MODE_DIV + 1)
	 * While supported, internal documentation doesn't
	 * assure correct functionality of the MDIO bus
	 * with divider of 1, 2 or 4.
	 */
	for (div = 8; div <= 256; div *= 2) {
		/* The requested rate is supported by the div */
		if (priv->mdc_rate == DIV_ROUND_UP(ahb_rate, div)) {
			val = readl(priv->membase + MDIO_MODE_REG);
			val &= ~MDIO_MODE_DIV_MASK;
			val |= MDIO_MODE_DIV(div);
			writel(val, priv->membase + MDIO_MODE_REG);

			return 0;
		}
	}

	/* The requested rate is not supported */
	return -EINVAL;
}

static int ipq_mdio_reset(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	/* To indicate CMN_PLL that ethernet_ldo has been ready if platform resource 1
	 * is specified in the device tree.
	 */
	if (priv->eth_ldo_rdy) {
		val = readl(priv->eth_ldo_rdy);
		val |= BIT(0);
		writel(val, priv->eth_ldo_rdy);
		fsleep(IPQ_PHY_SET_DELAY_US);
	}

	/* Configure MDIO clock source frequency if clock is specified in the device tree */
	ret = clk_set_rate(priv->mdio_clk, IPQ_MDIO_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->mdio_clk);
	if (ret)
		return ret;

	mdelay(10);

	/* Restore MDC rate */
	return ipq4019_mdio_set_div(priv);
}

static void ipq4019_mdio_select_mdc_rate(struct platform_device *pdev,
					 struct ipq4019_mdio_data *priv)
{
	unsigned long ahb_rate;
	int div;
	u32 val;

	/* MDC rate defined in DT, we don't have to decide a default value */
	if (!of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				  &priv->mdc_rate))
		return;

	/* If we don't have a clock for AHB use the fixed value */
	ahb_rate = IPQ_MDIO_CLK_RATE;
	if (priv->mdio_clk)
		ahb_rate = clk_get_rate(priv->mdio_clk);

	/* Check what is the current div set */
	val = readl(priv->membase + MDIO_MODE_REG);
	div = FIELD_GET(MDIO_MODE_DIV_MASK, val);

	/* div is not set to the default value of /256
	 * Probably someone changed that (bootloader, other drivers)
	 * Keep this and don't overwrite it.
	 */
	if (div != MDIO_MODE_DIV_256) {
		priv->mdc_rate = DIV_ROUND_UP(ahb_rate, div + 1);
		return;
	}

	/* If div is /256 assume nobody have set this value and
	 * try to find one MDC rate that is close the 802.3 spec of
	 * 2.5MHz
	 */
	for (div = 256; div >= 8; div /= 2) {
		/* Stop as soon as we found a divider that
		 * reached the closest value to 2.5MHz
		 */
		if (DIV_ROUND_UP(ahb_rate, div) > 2500000)
			break;

		priv->mdc_rate = DIV_ROUND_UP(ahb_rate, div);
	}
}

static int ipq4019_mdio_probe(struct platform_device *pdev)
{
	struct ipq4019_mdio_data *priv;
	struct mii_bus *bus;
	struct resource *res;
	int ret;

	bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;

	priv->membase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->membase))
		return PTR_ERR(priv->membase);

	priv->mdio_clk = devm_clk_get_optional(&pdev->dev, "gcc_mdio_ahb_clk");
	if (IS_ERR(priv->mdio_clk))
		return PTR_ERR(priv->mdio_clk);

	ipq4019_mdio_select_mdc_rate(pdev, priv);
	ret = ipq4019_mdio_set_div(priv);
	if (ret)
		return ret;

	/* The platform resource is provided on the chipset IPQ5018 */
	/* This resource is optional */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		priv->eth_ldo_rdy = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->eth_ldo_rdy))
			return PTR_ERR(priv->eth_ldo_rdy);
	}

	bus->name = "ipq4019_mdio";
	bus->read = ipq4019_mdio_read_c22;
	bus->write = ipq4019_mdio_write_c22;
	bus->read_c45 = ipq4019_mdio_read_c45;
	bus->write_c45 = ipq4019_mdio_write_c45;
	bus->reset = ipq_mdio_reset;
	bus->parent = &pdev->dev;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s%d", pdev->name, pdev->id);

	/* ORDERING BUG, fixed: this must run BEFORE of_mdiobus_register(),
	 * not after. of_mdiobus_register() is what walks the DTS's
	 * explicit-reg ethernet-phy@N children and probes each address
	 * synchronously as part of registering the bus -- that is the source
	 * of the "MDIO device at address N is missing" messages. Calling
	 * preinit afterwards means the chip gets clocked/reset/calibrated
	 * only after the scan that needed it has already run and failed.
	 *
	 * Two consequences of running this early, both handled inside
	 * qca8084_preinit() itself: (1) bus->mdio_lock is only initialised
	 * INSIDE __mdiobus_register(), so preinit uses bus->read/write
	 * directly rather than the generic locked mdiobus_read/write wrappers
	 * -- safe here because the bus is not yet visible to anything else,
	 * so there is nothing to race with; (2) the MDIO controller's own
	 * clock/divider setup normally runs as part of registration too, so
	 * preinit calls bus->reset() itself first. bus->read/write/reset and
	 * bus->parent are all already assigned above.
	 */
	if (of_property_read_bool(pdev->dev.of_node, "qcom,qca8084-preinit")) {
		dev_info(&pdev->dev, "qca8084: MARKER preinit call starting\n");
		qca8084_preinit(bus);
		dev_info(&pdev->dev, "qca8084: MARKER preinit call finished\n");
	}

	dev_info(&pdev->dev, "qca8084: MARKER about to call of_mdiobus_register\n");
	ret = of_mdiobus_register(bus, pdev->dev.of_node);
	if (ret) {
		dev_err(&pdev->dev, "Cannot register MDIO bus!\n");
		return ret;
	}

	platform_set_drvdata(pdev, bus);

	return 0;
}

static void ipq4019_mdio_remove(struct platform_device *pdev)
{
	struct mii_bus *bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus);
}

static const struct of_device_id ipq4019_mdio_dt_ids[] = {
	{ .compatible = "qcom,ipq4019-mdio" },
	{ .compatible = "qcom,ipq5018-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipq4019_mdio_dt_ids);

static struct platform_driver ipq4019_mdio_driver = {
	.probe = ipq4019_mdio_probe,
	.remove_new = ipq4019_mdio_remove,
	.driver = {
		.name = "ipq4019-mdio",
		.of_match_table = ipq4019_mdio_dt_ids,
	},
};

module_platform_driver(ipq4019_mdio_driver);

MODULE_DESCRIPTION("ipq4019 MDIO interface driver");
MODULE_AUTHOR("Qualcomm Atheros");
MODULE_LICENSE("Dual BSD/GPL");
