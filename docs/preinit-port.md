# Porting the QCA8084 bring-up: scope, and a safety warning

## The blocker, stated exactly

The SSDK reaches the QCA8386's 32-bit register space through function pointers it
expects the **MDIO bus driver** to publish in `bus->priv`:

```c
sw_error_t qca_mii_raw_read(struct mii_bus *bus, a_uint32_t reg, a_uint32_t *val)
{
	struct qca_mdio_data *mdio_priv = bus->priv;

	if (mdio_priv && mdio_priv->sw_read) {
		*val = mdio_priv->sw_read(bus, reg);
		return SW_OK;
	}
	return SW_FAIL;
}
```

`qca_mht_mii_read` / `qca_mht_mii_write` are `#define`s onto that path
(`ssdk_plat.h:517-518`), and everything MHT does — reading `EPHY_CFG`, writing
`WORK_MODE`, the whole clock tree — goes through it.

What the SSDK expects in `bus->priv`:

```c
struct qca_mdio_data {
	void __iomem *membase[2];
	void __iomem *eth_ldo_rdy[ETH_LDO_RDY_CNT];
	int clk_div;
	bool force_c22;
	struct gpio_descs *reset_gpios;
	void (*preinit)(struct mii_bus *bus);
	u32  (*sw_read)(struct mii_bus *bus, u32 reg);
	void (*sw_write)(struct mii_bus *bus, u32 reg, u32 val);
	struct clk *clk[];
};
```

What OpenWrt's `mdio-ipq4019.c` actually puts there:

```c
struct ipq4019_mdio_data {
	void __iomem *membase;
	void __iomem *eth_ldo_rdy;
	struct clk *mdio_clk;
	unsigned int mdc_rate;
};
```

**Completely different layout, and much smaller.**

## ⚠ This is a memory-safety bug, not just a missing feature

`devm_mdiobus_alloc_size(&pdev->dev, sizeof(*priv))` allocates only
`sizeof(struct ipq4019_mdio_data)` — four words. `struct qca_mdio_data` puts
`sw_read` roughly 64 bytes in. So any call into `qca_mii_raw_read` on OpenWrt
reads **past the end of the allocation** and, if the garbage there is non-NULL,
**calls it as a function pointer**.

So: **`MHT_ENABLE=enable` on OpenWrt without also fixing the MDIO driver is
unsafe, not merely non-functional.** Our current image has MHT enabled and has
not crashed only because `chip_ver_get` fails first, at
`qca_detect_phyid`, which uses the normal `mdiobus_read()` API rather than this
path. Nothing reaches `qca_mii_raw_read`. That is luck, not design.

Anyone enabling MHT on this target should know this before booting it.

## What the port actually has to do

Patch `drivers/net/mdio/mdio-ipq4019.c`, gated on the DT properties so nothing
changes for boards that do not ask for it:

1. **Make `bus->priv` layout-compatible** with `struct qca_mdio_data`, and
   allocate the right size. This is the part that must be exactly right; getting
   it wrong is the wild-jump above
2. **Implement `sw_read` / `sw_write`** — 32-bit access to the QCA8386's register
   space over MDIO. This is a multi-cycle page-select protocol, not a plain C22
   read
3. **Implement `preinit`**, called at probe:
   - program the four EPHY addresses into `EPHY_CFG` (`0xC90F018`, four 5-bit
     fields at offsets 0/5/10/15) from `phyaddr_fixup`
   - program the SerDes/PCS addresses into `SERDES_CFG` (`0xC90F014`, fields
     `S0_ADDR` at 0, `S1_ADDR` at 5, `S1_XPCS_ADDR` at 10) from
     `uniphyaddr_fixup`
   - when `mdio_clk_fixup` is present, run the clock init: SRDS0/1 enable and
     reset, EPHY0-3 SYS clocks, deassert EPHY DSP in `GCC_GEPHY_MISC`, efuse
     load, analog fixups, settle
4. **Read the `*_fixup` properties** and the per-child `fixup` flags

Then extend `&mdio1` in the DTS to carry those properties, matching the vendor.

## Why the SSDK cannot do it itself

Ordering rules it out even if the accessor worked. `regi_init` runs
`ssdk_dt_parse` → `ssdk_plat_init` → `chip_ver_get` → `ssdk_init` →
`case CHIP_MHT: qca_mht_hw_init()`. All the MHT clock code in `ssdk_mht_clk.c`
runs **after** detection succeeds, and detection is what needs the chip strapped.
`ssdk_plat_init` for `HSL_REG_MDIO` is one line (`cfg->reg_mode = HSL_MDIO;`).

And the SSDK never writes the strap in any case: `grep` over the whole tree finds
exactly one reference to `EPHY_CFG_OFFSET`, and it is the read in
`qca_mht_ephy_addr_get`.

## Test for success

The 32-address scan already in the DTS. Success is `0x004dd180` appearing at
MDIO addresses 1, 2, 3 and 4 on `mdio@90000`, replacing today's map of
`0xffff` / `0x00000000` / `0xb00eb00e`.

## Risk

Every iteration is a kernel driver change, so a bad one is a boot failure rather
than a probe failure. The self-recovering image (bootcount deliberately without
a `linksys,spnmx57` case) makes that cost three power cycles. **Serial console
should be reconnected before starting** — a driver that hangs at probe produces
no dmesg to read over SSH.
