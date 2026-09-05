// SPDX-License-Identifier: GPL-2.0
/*
 * qca8386.c - DSA driver for the Qualcomm QCA8386 ("Manhattan"/MHT) switch
 *
 * The QCA8386 integrates a qca8k-family L2 switch core, two SerDes/UNIPHY
 * instances, four QCA8084 2.5G EPHYs and an NSS clock controller in one
 * package, all reached over the SoC MDIO bus. On the Linksys SPNMX57
 * (IPQ5018) the SoC talks to the switch CPU port (port 0) over a forced
 * 2.5G SGMII+ link and the switch bridges internally to the four EPHYs
 * (MDIO 1..4).
 *
 * This is a PHASE-2b SKELETON: the register-access layer (32-bit indirect
 * MDIO with the page written to pseudo-PHY 0x18 reg 0x0c) and chip
 * identification (device id 0x17) are the only reverse-engineering-confirmed
 * facts. It is forked from the mainline v6.18 qca8k driver
 * (drivers/net/dsa/qca/qca8k-8xxx.c + qca8k-common.c), dropping the internal
 * MDIO-master / eth-mgmt paths (the EPHYs are external QCA8084s on the SoC
 * mdio1 bus). The switch-core register offsets are assumed QCA8337-compatible
 * per the SSDK isisc register map but MUST be confirmed on hardware before the
 * full FDB/VLAN/PCS body is built out - see docs/phase2b-research/.
 *
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 * Copyright (c) 2026 SPNMX57 OpenWrt port
 */

#include <linux/module.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <net/dsa.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/mdio.h>
#include <linux/phylink.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>

/* ---- QCA8386 identity + indirect-MDIO access ---------------------------- */

#define QCA8386_ID				0x17
/* The defining register-access delta from qca8k: page -> phy 0x18 reg 0x0c
 * (qca8k writes the page to reg 0). Phase-1 RE fact from the live bus.
 */
#define QCA8386_MDIO_PAGE_REG			0x0c

/* CPU port 0 + four user ports (1..4). qca8k has 7 (two CPU). */
#define QCA8386_NUM_PORTS			5
#define QCA8386_CPU_PORT			0

/* ---- Switch-core registers (QCA8337/isisc-family; verify on QCA8386) ---- */

#define QCA8386_REG_MASK_CTRL			0x000
#define   QCA8386_MASK_CTRL_REV_ID_MASK		GENMASK(7, 0)
#define   QCA8386_MASK_CTRL_REV_ID(x)		FIELD_GET(QCA8386_MASK_CTRL_REV_ID_MASK, x)
#define   QCA8386_MASK_CTRL_DEVICE_ID_MASK	GENMASK(15, 8)
#define   QCA8386_MASK_CTRL_DEVICE_ID(x)	FIELD_GET(QCA8386_MASK_CTRL_DEVICE_ID_MASK, x)

#define QCA8386_REG_PORT_STATUS(_i)		(0x07c + (_i) * 4)
#define   QCA8386_PORT_STATUS_SPEED		GENMASK(1, 0)
#define   QCA8386_PORT_STATUS_SPEED_10		0
#define   QCA8386_PORT_STATUS_SPEED_100		0x1
#define   QCA8386_PORT_STATUS_SPEED_1000	0x2
/* 2.5G is NOT a distinct code: the switch-core MAC reuses the 1000 code (2)
 * and the real rate comes from the SerDes. (SSDK: MHT_PORT_SPEED_2500M ==
 * MHT_PORT_SPEED_1000M == 2.)
 */
#define   QCA8386_PORT_STATUS_SPEED_2500	QCA8386_PORT_STATUS_SPEED_1000
#define   QCA8386_PORT_STATUS_TXMAC		BIT(2)
#define   QCA8386_PORT_STATUS_RXMAC		BIT(3)
#define   QCA8386_PORT_STATUS_TXFLOW		BIT(4)
#define   QCA8386_PORT_STATUS_RXFLOW		BIT(5)
#define   QCA8386_PORT_STATUS_DUPLEX		BIT(6)
#define   QCA8386_PORT_STATUS_LINK_AUTO		BIT(9)

#define QCA8386_REG_PORT_HDR_CTRL(_i)		(0x9c + (_i) * 4)
#define   QCA8386_PORT_HDR_CTRL_RX_MASK		GENMASK(3, 2)
#define   QCA8386_PORT_HDR_CTRL_TX_MASK		GENMASK(1, 0)
#define   QCA8386_PORT_HDR_CTRL_ALL		2

#define QCA8386_REG_GLOBAL_FW_CTRL0		0x620
#define   QCA8386_GLOBAL_FW_CTRL0_CPU_PORT_EN	BIT(10)

/* Bus-less regmap bound; TODO: confirm against the QCA8386 memory map. */
#define QCA8386_MAX_REGISTER			0x16ac

struct qca8386_info {
	u8 id;
	const char *name;
};

struct qca8386_priv {
	struct device *dev;
	struct mii_bus *bus;
	struct regmap *regmap;
	struct dsa_switch *ds;
	struct gpio_desc *reset_gpio;
	const struct qca8386_info *info;

	struct mutex reg_mutex;
	u16 cached_page;
	u8 switch_id;
	u8 switch_revision;
};

/* ---- 32-bit indirect MDIO (forked verbatim from qca8k, page reg 0x0c) --- */

static void
qca8386_split_addr(u32 regaddr, u16 *r1, u16 *r2, u16 *page)
{
	regaddr >>= 1;
	*r1 = regaddr & 0x1e;

	regaddr >>= 5;
	*r2 = regaddr & 0x7;

	regaddr >>= 3;
	*page = regaddr & 0x3ff;
}

static int
qca8386_mii_write_lo(struct mii_bus *bus, int phy_id, u32 regnum, u32 val)
{
	int ret;
	u16 lo;

	lo = val & 0xffff;
	ret = bus->write(bus, phy_id, regnum, lo);
	if (ret < 0)
		dev_err_ratelimited(&bus->dev,
				    "failed to write qca8386 32bit lo register\n");

	return ret;
}

static int
qca8386_mii_write_hi(struct mii_bus *bus, int phy_id, u32 regnum, u32 val)
{
	int ret;
	u16 hi;

	hi = (u16)(val >> 16);
	ret = bus->write(bus, phy_id, regnum, hi);
	if (ret < 0)
		dev_err_ratelimited(&bus->dev,
				    "failed to write qca8386 32bit hi register\n");

	return ret;
}

static int
qca8386_mii_read_lo(struct mii_bus *bus, int phy_id, u32 regnum, u32 *val)
{
	int ret;

	ret = bus->read(bus, phy_id, regnum);
	if (ret < 0)
		goto err;

	*val = ret & 0xffff;
	return 0;

err:
	dev_err_ratelimited(&bus->dev,
			    "failed to read qca8386 32bit lo register\n");
	*val = 0;

	return ret;
}

static int
qca8386_mii_read_hi(struct mii_bus *bus, int phy_id, u32 regnum, u32 *val)
{
	int ret;

	ret = bus->read(bus, phy_id, regnum);
	if (ret < 0)
		goto err;

	*val = ret << 16;
	return 0;

err:
	dev_err_ratelimited(&bus->dev,
			    "failed to read qca8386 32bit hi register\n");
	*val = 0;

	return ret;
}

static int
qca8386_mii_read32(struct mii_bus *bus, int phy_id, u32 regnum, u32 *val)
{
	u32 hi, lo;
	int ret;

	*val = 0;

	ret = qca8386_mii_read_lo(bus, phy_id, regnum, &lo);
	if (ret < 0)
		goto err;

	ret = qca8386_mii_read_hi(bus, phy_id, regnum + 1, &hi);
	if (ret < 0)
		goto err;

	*val = lo | hi;

err:
	return ret;
}

static void
qca8386_mii_write32(struct mii_bus *bus, int phy_id, u32 regnum, u32 val)
{
	if (qca8386_mii_write_lo(bus, phy_id, regnum, val) < 0)
		return;

	qca8386_mii_write_hi(bus, phy_id, regnum + 1, val);
}

static int
qca8386_set_page(struct qca8386_priv *priv, u16 page)
{
	u16 *cached_page = &priv->cached_page;
	struct mii_bus *bus = priv->bus;
	int ret;

	if (page == *cached_page)
		return 0;

	/* THE defining QCA8386 delta: page register is 0x0c, not 0. */
	ret = bus->write(bus, 0x18, QCA8386_MDIO_PAGE_REG, page);
	if (ret < 0) {
		dev_err_ratelimited(&bus->dev,
				    "failed to set qca8386 page\n");
		return ret;
	}

	*cached_page = page;
	usleep_range(1000, 2000);
	return 0;
}

static int
qca8386_read_mii(struct qca8386_priv *priv, u32 reg, u32 *val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	int ret;

	qca8386_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8386_set_page(priv, page);
	if (ret < 0)
		goto exit;

	ret = qca8386_mii_read32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int
qca8386_write_mii(struct qca8386_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	int ret;

	qca8386_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8386_set_page(priv, page);
	if (ret < 0)
		goto exit;

	qca8386_mii_write32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int
qca8386_regmap_update_bits_mii(struct qca8386_priv *priv, u32 reg,
			       u32 mask, u32 write_val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	u32 val;
	int ret;

	qca8386_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8386_set_page(priv, page);
	if (ret < 0)
		goto exit;

	ret = qca8386_mii_read32(bus, 0x10 | r2, r1, &val);
	if (ret < 0)
		goto exit;

	val &= ~mask;
	val |= write_val;
	qca8386_mii_write32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int
qca8386_bulk_read(void *ctx, const void *reg_buf, size_t reg_len,
		  void *val_buf, size_t val_len)
{
	int i, count = val_len / sizeof(u32), ret;
	struct qca8386_priv *priv = ctx;
	u32 reg = *(u16 *)reg_buf;

	for (i = 0; i < count; i++, reg += sizeof(u32)) {
		ret = qca8386_read_mii(priv, reg, val_buf + i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int
qca8386_bulk_gather_write(void *ctx, const void *reg_buf, size_t reg_len,
			  const void *val_buf, size_t val_len)
{
	int i, count = val_len / sizeof(u32), ret;
	struct qca8386_priv *priv = ctx;
	u32 reg = *(u16 *)reg_buf;
	u32 *val = (u32 *)val_buf;

	for (i = 0; i < count; i++, reg += sizeof(u32), val++) {
		ret = qca8386_write_mii(priv, reg, *val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int
qca8386_bulk_write(void *ctx, const void *data, size_t bytes)
{
	return qca8386_bulk_gather_write(ctx, data, sizeof(u16),
					data + sizeof(u16), bytes - sizeof(u16));
}

static int
qca8386_regmap_update_bits(void *ctx, u32 reg, u32 mask, u32 write_val)
{
	struct qca8386_priv *priv = ctx;

	return qca8386_regmap_update_bits_mii(priv, reg, mask, write_val);
}

static const struct regmap_config qca8386_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = QCA8386_MAX_REGISTER,
	.read = qca8386_bulk_read,
	.write = qca8386_bulk_write,
	.reg_update_bits = qca8386_regmap_update_bits,
	.disable_locking = true, /* locking handled by the read/write helpers */
	.cache_type = REGCACHE_NONE,
	.max_raw_read = 32,
	/* ATU regs suffer from a bug where some data is not correctly written;
	 * disable bulk write to correctly write ATU entries.
	 */
	.use_single_write = true,
};

/* ---- register helpers -------------------------------------------------- */

static int qca8386_read(struct qca8386_priv *priv, u32 reg, u32 *val)
{
	return regmap_read(priv->regmap, reg, val);
}

static int qca8386_write(struct qca8386_priv *priv, u32 reg, u32 val)
{
	return regmap_write(priv->regmap, reg, val);
}

static int qca8386_rmw(struct qca8386_priv *priv, u32 reg, u32 mask, u32 write_val)
{
	return regmap_update_bits(priv->regmap, reg, mask, write_val);
}

static int qca8386_read_switch_id(struct qca8386_priv *priv)
{
	u32 val;
	u8 id;
	int ret;

	if (!priv->info)
		return -ENODEV;

	ret = qca8386_read(priv, QCA8386_REG_MASK_CTRL, &val);
	if (ret < 0)
		return -ENODEV;

	id = QCA8386_MASK_CTRL_DEVICE_ID(val);
	if (id != priv->info->id) {
		dev_err(priv->dev,
			"Switch id detected %x but expected %x\n",
			id, priv->info->id);
		return -ENODEV;
	}

	priv->switch_id = id;
	priv->switch_revision = QCA8386_MASK_CTRL_REV_ID(val);

	dev_info(priv->dev, "%s: detected QCA8386 (id 0x%02x rev 0x%02x)\n",
		 priv->info->name, priv->switch_id, priv->switch_revision);

	return 0;
}

static void qca8386_port_set_status(struct qca8386_priv *priv, int port, int enable)
{
	u32 mask = QCA8386_PORT_STATUS_TXMAC | QCA8386_PORT_STATUS_RXMAC;

	if (enable)
		qca8386_rmw(priv, QCA8386_REG_PORT_STATUS(port), 0, mask);
	else
		qca8386_rmw(priv, QCA8386_REG_PORT_STATUS(port), mask, 0);
}

/* ---- DSA / phylink ops ------------------------------------------------- */

static enum dsa_tag_protocol
qca8386_get_tag_protocol(struct dsa_switch *ds, int port,
			 enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_QCA;
}

static int qca8386_setup(struct dsa_switch *ds)
{
	struct qca8386_priv *priv = ds->priv;
	int ret;

	/* Enable the CPU port. */
	ret = qca8386_rmw(priv, QCA8386_REG_GLOBAL_FW_CTRL0,
			  QCA8386_GLOBAL_FW_CTRL0_CPU_PORT_EN,
			  QCA8386_GLOBAL_FW_CTRL0_CPU_PORT_EN);
	if (ret)
		return ret;

	/* QCA header mode (tx+rx) on the CPU port so DSA tagging works. */
	ret = qca8386_rmw(priv, QCA8386_REG_PORT_HDR_CTRL(QCA8386_CPU_PORT),
			  QCA8386_PORT_HDR_CTRL_RX_MASK | QCA8386_PORT_HDR_CTRL_TX_MASK,
			  FIELD_PREP(QCA8386_PORT_HDR_CTRL_RX_MASK, QCA8386_PORT_HDR_CTRL_ALL) |
			  FIELD_PREP(QCA8386_PORT_HDR_CTRL_TX_MASK, QCA8386_PORT_HDR_CTRL_ALL));
	if (ret)
		return ret;

	/* TODO(phase2c): MIB init, per-port lookup/forwarding, unknown-frame
	 * flood-to-CPU, FDB flush, ageing, MAX_FRAME_SIZE, VLAN. These reuse
	 * qca8k-common.c logic once the QCA8386 register map is confirmed on
	 * hardware - see docs/phase2b-research/UNRESOLVED-CHECKLIST.md.
	 */
	dev_info(priv->dev, "qca8386 skeleton setup complete (CPU port enabled)\n");

	return 0;
}

static void qca8386_phylink_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	switch (port) {
	case QCA8386_CPU_PORT: /* SoC uplink: forced 2.5G SGMII+ */
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
		break;
	case 1:
	case 2:
	case 3:
	case 4: /* user ports face external QCA8084 EPHYs */
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
		break;
	default:
		dev_warn(ds->dev, "invalid port %d\n", port);
		return;
	}

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD | MAC_2500FD;
}

static void
qca8386_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{
	/* TODO(phase2c): program the QCA8386 internal UNIPHY/SerDes for
	 * SGMII+/2500 on the CPU port (port from SSDK mht_interface_ctrl.c).
	 * See docs/phase2b-research/phase2b-2-cpu-uplink-mmd-seq.md.
	 */
}

static void
qca8386_phylink_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca8386_priv *priv = dp->ds->priv;

	qca8386_port_set_status(priv, dp->index, 0);
}

static void
qca8386_phylink_mac_link_up(struct phylink_config *config,
			    struct phy_device *phydev, unsigned int mode,
			    phy_interface_t interface, int speed, int duplex,
			    bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca8386_priv *priv = dp->ds->priv;
	int port = dp->index;
	u32 reg;

	if (phylink_autoneg_inband(mode)) {
		reg = QCA8386_PORT_STATUS_LINK_AUTO;
	} else {
		switch (speed) {
		case SPEED_10:
			reg = QCA8386_PORT_STATUS_SPEED_10;
			break;
		case SPEED_100:
			reg = QCA8386_PORT_STATUS_SPEED_100;
			break;
		case SPEED_1000:
			reg = QCA8386_PORT_STATUS_SPEED_1000;
			break;
		case SPEED_2500:
			/* switch-core MAC reuses the 1000 code; rate is set by
			 * the SerDes.
			 */
			reg = QCA8386_PORT_STATUS_SPEED_2500;
			break;
		default:
			reg = QCA8386_PORT_STATUS_LINK_AUTO;
			break;
		}

		if (duplex == DUPLEX_FULL)
			reg |= QCA8386_PORT_STATUS_DUPLEX;

		if (rx_pause || dsa_port_is_cpu(dp))
			reg |= QCA8386_PORT_STATUS_RXFLOW;

		if (tx_pause || dsa_port_is_cpu(dp))
			reg |= QCA8386_PORT_STATUS_TXFLOW;
	}

	reg |= QCA8386_PORT_STATUS_TXMAC | QCA8386_PORT_STATUS_RXMAC;

	qca8386_write(priv, QCA8386_REG_PORT_STATUS(port), reg);
}

static const struct phylink_mac_ops qca8386_phylink_mac_ops = {
	.mac_config	= qca8386_phylink_mac_config,
	.mac_link_down	= qca8386_phylink_mac_link_down,
	.mac_link_up	= qca8386_phylink_mac_link_up,
};

static const struct dsa_switch_ops qca8386_switch_ops = {
	.get_tag_protocol	= qca8386_get_tag_protocol,
	.setup			= qca8386_setup,
	.phylink_get_caps	= qca8386_phylink_get_caps,
	/* TODO(phase2c/2d): FDB/VLAN/bridge/STP/mirror/LAG/MTU/ethtool ops,
	 * reused from qca8k-common.c once the register map is confirmed.
	 */
};

/* ---- probe / remove ---------------------------------------------------- */

static int qca8386_sw_probe(struct mdio_device *mdiodev)
{
	struct qca8386_priv *priv;
	int ret;

	priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &mdiodev->dev;
	priv->bus = mdiodev->bus;
	priv->info = of_device_get_match_data(priv->dev);
	if (!priv->info)
		return -EINVAL;

	/* QCA8386 package reset (tlmm gpio24, active-low on SPNMX57).
	 * TODO(phase2d): when the nsscc node is added it must be the SOLE
	 * owner of this GPIO - drop this block then (see UNRESOLVED-CHECKLIST).
	 */
	priv->reset_gpio = devm_gpiod_get_optional(priv->dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return PTR_ERR(priv->reset_gpio);

	if (priv->reset_gpio) {
		msleep(20);
		gpiod_set_value_cansleep(priv->reset_gpio, 0);
		msleep(50);
	}

	priv->regmap = devm_regmap_init(priv->dev, NULL, priv,
					&qca8386_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(priv->dev, "regmap initialization failed\n");
		return PTR_ERR(priv->regmap);
	}

	mutex_init(&priv->reg_mutex);
	priv->cached_page = 0xffff; /* invalidate the page cache */

	ret = qca8386_read_switch_id(priv);
	if (ret)
		return ret;

	priv->ds = devm_kzalloc(priv->dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = priv->dev;
	priv->ds->num_ports = QCA8386_NUM_PORTS;
	priv->ds->priv = priv;
	priv->ds->ops = &qca8386_switch_ops;
	priv->ds->phylink_mac_ops = &qca8386_phylink_mac_ops;

	dev_set_drvdata(priv->dev, priv);

	return dsa_register_switch(priv->ds);
}

static void qca8386_sw_remove(struct mdio_device *mdiodev)
{
	struct qca8386_priv *priv = dev_get_drvdata(&mdiodev->dev);
	int i;

	if (!priv)
		return;

	for (i = 0; i < QCA8386_NUM_PORTS; i++)
		qca8386_port_set_status(priv, i, 0);

	dsa_unregister_switch(priv->ds);
}

static void qca8386_sw_shutdown(struct mdio_device *mdiodev)
{
	struct qca8386_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (!priv)
		return;

	dsa_switch_shutdown(priv->ds);

	dev_set_drvdata(&mdiodev->dev, NULL);
}

static const struct qca8386_info qca8386 = {
	.id = QCA8386_ID,
	.name = "qca8386",
};

static const struct of_device_id qca8386_of_match[] = {
	{ .compatible = "qca,qca8386", .data = &qca8386 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, qca8386_of_match);

static struct mdio_driver qca8386_driver = {
	.probe = qca8386_sw_probe,
	.remove = qca8386_sw_remove,
	.shutdown = qca8386_sw_shutdown,
	.mdiodrv.driver = {
		.name = "qca8386",
		.of_match_table = qca8386_of_match,
	},
};

mdio_module_driver(qca8386_driver);

MODULE_AUTHOR("SPNMX57 OpenWrt port");
MODULE_DESCRIPTION("Driver for QCA8386 ethernet switch");
MODULE_LICENSE("GPL");
