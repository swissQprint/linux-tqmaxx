// SPDX-License-Identifier: GPL-2.0-only
/*
 * PRU-ICSS platform driver for various TI SoCs
 *
 * Copyright (C) 2014-2020 Texas Instruments Incorporated - http://www.ti.com/
 *	Suman Anna <s-anna@ti.com>
 *	Andrew F. Davis <afd@ti.com>
 *	Tero Kristo <t-kristo@ti.com>
 */

#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/pruss_driver.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>

#define PRUSS_CFG_IEPCLK	0x30
#define ICSSG_CFG_CORE_SYNC	0x3c

#define SYSCFG_STANDBY_INIT	BIT(4)
#define SYSCFG_SUB_MWAIT_READY	BIT(5)

/**
 * struct pruss_private_data - PRUSS driver private data
 * @has_no_sharedram: flag to indicate the absence of PRUSS Shared Data RAM
 * @has_ocp_syscfg: flag to indicate if OCP SYSCFG is present
 */
struct pruss_private_data {
	bool has_no_sharedram;
	bool has_ocp_syscfg;
};

/**
 * struct pruss_match_private_data - private data to handle multiple instances
 * @device_name: device name of the PRUSS instance
 * @priv_data: PRUSS driver private data for this PRUSS instance
 */
struct pruss_match_private_data {
	const char *device_name;
	const struct pruss_private_data *priv_data;
};

/**
 * pruss_get() - get the pruss for a given PRU remoteproc
 * @rproc: remoteproc handle of a PRU instance
 *
 * Finds the parent pruss device for a PRU given the @rproc handle of the
 * PRU remote processor. This function increments the pruss device's refcount,
 * so always use pruss_put() to decrement it back once pruss isn't needed
 * anymore.
 *
 * Returns the pruss handle on success, and an ERR_PTR on failure using one
 * of the following error values
 *    -EINVAL if invalid parameter
 *    -ENODEV if PRU device or PRUSS device is not found
 */
struct pruss *pruss_get(struct rproc *rproc)
{
	struct pruss *pruss;
	struct device *dev;
	struct platform_device *ppdev;

	if (IS_ERR_OR_NULL(rproc))
		return ERR_PTR(-EINVAL);

	dev = &rproc->dev;
	if (!dev->parent)
		return ERR_PTR(-ENODEV);

	/* rudimentary check to make sure rproc handle is for a PRU or RTU */
	if (!strstr(dev_name(dev->parent), "pru") &&
	    !strstr(dev_name(dev->parent), "rtu"))
		return ERR_PTR(-ENODEV);

	ppdev = to_platform_device(dev->parent->parent);
	pruss = platform_get_drvdata(ppdev);
	if (!pruss)
		return ERR_PTR(-ENODEV);

	get_device(pruss->dev);

	return pruss;
}
EXPORT_SYMBOL_GPL(pruss_get);

/**
 * pruss_put() - decrement pruss device's usecount
 * @pruss: pruss handle
 *
 * Complimentary function for pruss_get(). Needs to be called
 * after the PRUSS is used, and only if the pruss_get() succeeds.
 */
void pruss_put(struct pruss *pruss)
{
	if (IS_ERR_OR_NULL(pruss))
		return;

	put_device(pruss->dev);
}
EXPORT_SYMBOL_GPL(pruss_put);

/**
 * pruss_request_mem_region() - request a memory resource
 * @pruss: the pruss instance
 * @mem_id: the memory resource id
 * @region: pointer to memory region structure to be filled in
 *
 * This function allows a client driver to request a memory resource,
 * and if successful, will let the client driver own the particular
 * memory region until released using the pruss_release_mem_region()
 * API.
 *
 * Returns the memory region if requested resource is available, an
 * error otherwise
 */
int pruss_request_mem_region(struct pruss *pruss, enum pruss_mem mem_id,
			     struct pruss_mem_region *region)
{
	if (!pruss || !region)
		return -EINVAL;

	if (mem_id >= PRUSS_MEM_MAX)
		return -EINVAL;

	mutex_lock(&pruss->lock);

	if (pruss->mem_in_use[mem_id]) {
		mutex_unlock(&pruss->lock);
		return -EBUSY;
	}

	*region = pruss->mem_regions[mem_id];
	pruss->mem_in_use[mem_id] = region;

	mutex_unlock(&pruss->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(pruss_request_mem_region);

/**
 * pruss_release_mem_region() - release a memory resource
 * @pruss: the pruss instance
 * @region: the memory region to release
 *
 * This function is the complimentary function to
 * pruss_request_mem_region(), and allows the client drivers to
 * release back a memory resource.
 *
 * Returns 0 on success, an error code otherwise
 */
int pruss_release_mem_region(struct pruss *pruss,
			     struct pruss_mem_region *region)
{
	int id;

	if (!pruss || !region)
		return -EINVAL;

	mutex_lock(&pruss->lock);

	/* find out the memory region being released */
	for (id = 0; id < PRUSS_MEM_MAX; id++) {
		if (pruss->mem_in_use[id] == region)
			break;
	}

	if (id == PRUSS_MEM_MAX) {
		mutex_unlock(&pruss->lock);
		return -EINVAL;
	}

	pruss->mem_in_use[id] = NULL;

	mutex_unlock(&pruss->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(pruss_release_mem_region);

/**
 * pruss_cfg_read() - read a PRUSS CFG sub-module register
 * @pruss: the pruss instance handle
 * @reg: register offset within the CFG sub-module
 * @val: pointer to return the value in
 *
 * Reads a given register within the PRUSS CFG sub-module and
 * returns it through the passed-in @val pointer
 *
 * Returns 0 on success, or an error code otherwise
 */
int pruss_cfg_read(struct pruss *pruss, unsigned int reg, unsigned int *val)
{
	if (IS_ERR_OR_NULL(pruss))
		return -EINVAL;

	return regmap_read(pruss->cfg, reg, val);
}
EXPORT_SYMBOL_GPL(pruss_cfg_read);

/**
 * pruss_cfg_update() - configure a PRUSS CFG sub-module register
 * @pruss: the pruss instance handle
 * @reg: register offset within the CFG sub-module
 * @mask: bit mask to use for programming the @val
 * @val: value to write
 *
 * Programs a given register within the PRUSS CFG sub-module
 *
 * Returns 0 on success, or an error code otherwise
 */
int pruss_cfg_update(struct pruss *pruss, unsigned int reg,
		     unsigned int mask, unsigned int val)
{
	if (IS_ERR_OR_NULL(pruss))
		return -EINVAL;

	return regmap_update_bits(pruss->cfg, reg, mask, val);
}
EXPORT_SYMBOL_GPL(pruss_cfg_update);

/**
 * pruss_cfg_ocp_master_ports() - configure PRUSS OCP master ports
 *
 * This function programs the PRUSS_SYSCFG.STANDBY_INIT bit either to enable or
 * disable the OCP master ports (applicable only on SoCs using OCP interconnect
 * like the OMAP family). Clearing the bit achieves dual functionalities - one
 * is to deassert the MStandby signal to the device PRCM, and the other is to
 * enable OCP master ports to allow accesses outside of the PRU-ICSS. The
 * function has to wait for the PRCM to acknowledge through the monitoring of
 * the PRUSS_SYSCFG.SUB_MWAIT bit when enabling master ports. Setting the bit
 * disables the master access, and also signals the PRCM that the PRUSS is ready
 * for Standby.
 *
 * Returns 0 on success, or an error code otherwise. ETIMEDOUT is returned
 * when the ready-state fails.
 */
int pruss_cfg_ocp_master_ports(struct pruss *pruss, bool enable)
{
	int ret;
	u32 syscfg_val, i;
	bool ready = false;

	if (IS_ERR_OR_NULL(pruss))
		return -EINVAL;

	/* nothing to do on non OMAP-SoCs */
	if (!pruss->has_ocp_syscfg)
		return 0;

	/* assert the MStandby signal during disable path */
	if (!enable) {
		return pruss_cfg_update(pruss, PRUSS_CFG_SYSCFG,
					SYSCFG_STANDBY_INIT,
					SYSCFG_STANDBY_INIT);
	}

	/* enable the OCP master ports and disable MStandby */
	ret = pruss_cfg_update(pruss, PRUSS_CFG_SYSCFG,
			       SYSCFG_STANDBY_INIT, 0);
	if (ret)
		return ret;

	/* wait till we are ready for transactions - delay is arbitrary */
	for (i = 0; i < 10; i++) {
		ret = pruss_cfg_read(pruss, PRUSS_CFG_SYSCFG, &syscfg_val);
		if (ret)
			goto disable;
		ready = !(syscfg_val & SYSCFG_SUB_MWAIT_READY);
		if (ready)
			return 0;
		udelay(5);
	}

	dev_err(pruss->dev, "timeout waiting for SUB_MWAIT_READY\n");
	ret = -ETIMEDOUT;

disable:
	pruss_cfg_update(pruss, PRUSS_CFG_SYSCFG, SYSCFG_STANDBY_INIT,
			 SYSCFG_STANDBY_INIT);
	return ret;
}
EXPORT_SYMBOL_GPL(pruss_cfg_ocp_master_ports);

static void pruss_of_free_clk_provider(void *data)
{
	struct device_node *clk_mux_np = data;

	of_clk_del_provider(clk_mux_np);
	of_node_put(clk_mux_np);
}

static int pruss_clk_mux_setup(struct pruss *pruss, struct clk *clk_mux,
			       char *mux_name, unsigned int reg_offset)
{
	unsigned int num_parents;
	const char **parent_names;
	void __iomem *reg;
	int ret;
	char *clk_mux_name = NULL;
	struct device_node *clk_mux_np;

	clk_mux_np = of_get_child_by_name(pruss->dev->of_node, mux_name);
	if (!clk_mux_np)
		return -EINVAL;

	num_parents = of_clk_get_parent_count(clk_mux_np);
	if (num_parents < 1) {
		dev_err(pruss->dev, "mux-clock %pOF must have parents\n",
			clk_mux_np);
		return -EINVAL;
	}

	parent_names = devm_kcalloc(pruss->dev, sizeof(char *), num_parents,
				    GFP_KERNEL);
	if (!parent_names)
		return -ENOMEM;

	of_clk_parent_fill(clk_mux_np, parent_names, num_parents);

	clk_mux_name = devm_kasprintf(pruss->dev, GFP_KERNEL, "%s.%pOFn",
				      dev_name(pruss->dev), clk_mux_np);
	if (!clk_mux_name)
		return -ENOMEM;

	reg = pruss->cfg_base + reg_offset;
	/*
	 * WARN: dev must be NULL to avoid recursive incrementing
	 * of module refcnt
	 */
	clk_mux = clk_register_mux(NULL, clk_mux_name, parent_names,
				   num_parents, 0, reg, 0, 1, 0, NULL);
	if (IS_ERR(clk_mux))
		return PTR_ERR(clk_mux);

	ret = devm_add_action_or_reset(pruss->dev,
				       (void(*)(void *))clk_unregister_mux,
				       clk_mux);
	if (ret) {
		dev_err(pruss->dev, "failed to add clkmux reset action %d",
			ret);
		return ret;
	}

	ret = of_clk_add_provider(clk_mux_np, of_clk_src_simple_get, clk_mux);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(pruss->dev, pruss_of_free_clk_provider,
				       clk_mux_np);
	if (ret)
		dev_err(pruss->dev, "failed to add clkmux reset action %d",
			ret);

	return ret;
}

static const
struct pruss_private_data *pruss_get_private_data(struct platform_device *pdev)
{
	const struct pruss_match_private_data *data;

	data = of_device_get_match_data(&pdev->dev);

	if (!of_device_is_compatible(pdev->dev.of_node, "ti,am4376-pruss"))
		return data ? data->priv_data : ERR_PTR(-ENODEV);

	for (; data && data->device_name; data++) {
		if (!strcmp(dev_name(&pdev->dev), data->device_name))
			return data->priv_data;
	}

	return ERR_PTR(-ENODEV);
}

static const struct regmap_config syscon_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int pruss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct device_node *np;
	struct pruss *pruss;
	struct resource res;
	int ret, i, index;
	const struct pruss_private_data *data;
	const char *mem_names[PRUSS_MEM_MAX] = { "dram0", "dram1", "shrdram2" };
	struct regmap_config syscon_config = syscon_regmap_config;

	if (!node) {
		dev_err(dev, "Non-DT platform device not supported\n");
		return -ENODEV;
	}

	data = pruss_get_private_data(pdev);
	if (IS_ERR(data)) {
		dev_err(dev, "missing private data\n");
		return -ENODEV;
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "dma_set_coherent_mask: %d\n", ret);
		return ret;
	}

	pruss = devm_kzalloc(dev, sizeof(*pruss), GFP_KERNEL);
	if (!pruss)
		return -ENOMEM;

	pruss->dev = dev;
	pruss->has_ocp_syscfg = data->has_ocp_syscfg;
	mutex_init(&pruss->lock);

	np = of_get_child_by_name(node, "memories");
	if (!np) {
		dev_err(dev, "%pOF is missing memories node\n", np);
		return -ENODEV;
	}

	for (i = 0; i < ARRAY_SIZE(mem_names); i++) {
		if (data && data->has_no_sharedram &&
		    !strcmp(mem_names[i], "shrdram2"))
			continue;

		index = of_property_match_string(np, "reg-names", mem_names[i]);
		if (index < 0) {
			of_node_put(np);
			return index;
		}

		if (of_address_to_resource(np, index, &res)) {
			of_node_put(np);
			return -EINVAL;
		}

		pruss->mem_regions[i].va = devm_ioremap(dev, res.start,
							resource_size(&res));
		if (!pruss->mem_regions[i].va) {
			dev_err(dev, "failed to parse and map memory resource %d %s\n",
				i, mem_names[i]);
			of_node_put(np);
			return -ENOMEM;
		}
		pruss->mem_regions[i].pa = res.start;
		pruss->mem_regions[i].size = resource_size(&res);

		dev_dbg(dev, "memory %8s: pa %pa size 0x%zx va %pK\n",
			mem_names[i], &pruss->mem_regions[i].pa,
			pruss->mem_regions[i].size, pruss->mem_regions[i].va);
	}
	of_node_put(np);

	platform_set_drvdata(pdev, pruss);

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "couldn't enable module\n");
		pm_runtime_put_noidle(dev);
		goto rpm_disable;
	}

	np = of_get_child_by_name(node, "cfg");
	if (!np) {
		dev_err(dev, "%pOF is missing cfg node\n", np);
		ret = -ENODEV;
		goto rpm_put;
	}

	if (of_address_to_resource(np, 0, &res)) {
		ret = -ENOMEM;
		of_node_put(np);
		goto rpm_put;
	}
	of_node_put(np);

	pruss->cfg_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!pruss->cfg_base) {
		ret = -ENOMEM;
		goto rpm_put;
	}

	if (!of_device_is_compatible(pdev->dev.of_node, "ti,am654-icssg") &&
	    !of_device_is_compatible(pdev->dev.of_node, "ti,j721e-icssg"))
		goto skip_mux;

	ret = pruss_clk_mux_setup(pruss, pruss->core_clk_mux, "coreclk-mux",
				  ICSSG_CFG_CORE_SYNC);
	if (ret) {
		dev_err(dev, "failed to setup coreclk-mux\n");
		goto rpm_put;
	}

	ret = pruss_clk_mux_setup(pruss, pruss->core_clk_mux, "iepclk-mux",
				  PRUSS_CFG_IEPCLK);
	if (ret) {
		dev_err(dev, "failed to setup iepclk-mux\n");
		goto rpm_put;
	}

skip_mux:
	syscon_config.name = kasprintf(GFP_KERNEL, "%pOFn@%llx", np,
				       (u64)res.start);
	syscon_config.max_register = resource_size(&res) - 4;

	pruss->cfg = regmap_init_mmio(NULL, pruss->cfg_base, &syscon_config);
	kfree(syscon_config.name);
	if (IS_ERR(pruss->cfg)) {
		dev_err(dev, "regmap_init_mmio failed for cfg, ret = %ld\n",
			PTR_ERR(pruss->cfg));
		ret = PTR_ERR(pruss->cfg);
		goto rpm_put;
	}

	dev_dbg(&pdev->dev, "creating PRU cores and other child platform devices\n");
	ret = of_platform_populate(node, NULL, NULL, &pdev->dev);
	if (ret) {
		dev_err(dev, "of_platform_populate failed\n");
		goto reg_exit;
	}

	return 0;

reg_exit:
	regmap_exit(pruss->cfg);
rpm_put:
	pm_runtime_put_sync(dev);
rpm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static int pruss_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pruss *pruss = platform_get_drvdata(pdev);

	regmap_exit(pruss->cfg);

	dev_dbg(dev, "remove PRU cores and other child platform devices\n");
	of_platform_depopulate(dev);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);

	return 0;
}

/* instance-specific driver private data */
static const struct pruss_private_data am437x_pruss1_priv_data = {
	.has_no_sharedram = false,
	.has_ocp_syscfg = true,
};

static const struct pruss_private_data am437x_pruss0_priv_data = {
	.has_no_sharedram = true,
	.has_ocp_syscfg = false,
};

static const struct pruss_private_data am33xx_am57xx_priv_data = {
	.has_no_sharedram = false,
	.has_ocp_syscfg = true,
};

static const struct pruss_private_data k2g_am65x_j7_priv_data = {
	.has_no_sharedram = false,
	.has_ocp_syscfg = false,
};

static const struct pruss_match_private_data am437x_match_data[] = {
	{
		.device_name	= "54400000.pruss",
		.priv_data	= &am437x_pruss1_priv_data,
	},
	{
		.device_name	= "54440000.pruss",
		.priv_data	= &am437x_pruss0_priv_data,
	},
	{
		/* sentinel */
	},
};

static const struct pruss_match_private_data am33xx_am57xx_match_data = {
	.priv_data = &am33xx_am57xx_priv_data,
};

static const struct pruss_match_private_data k2g_am65x_j7_match_data = {
	.priv_data = &k2g_am65x_j7_priv_data,
};

static const struct of_device_id pruss_of_match[] = {
	{ .compatible = "ti,am3356-pruss", .data = &am33xx_am57xx_match_data, },
	{ .compatible = "ti,am4376-pruss", .data = &am437x_match_data, },
	{ .compatible = "ti,am5728-pruss", .data = &am33xx_am57xx_match_data, },
	{ .compatible = "ti,k2g-pruss", .data = &k2g_am65x_j7_match_data, },
	{ .compatible = "ti,am654-icssg", .data = &k2g_am65x_j7_match_data, },
	{ .compatible = "ti,j721e-icssg", .data = &k2g_am65x_j7_match_data, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, pruss_of_match);

static struct platform_driver pruss_driver = {
	.driver = {
		.name = "pruss",
		.of_match_table = pruss_of_match,
	},
	.probe  = pruss_probe,
	.remove = pruss_remove,
};
module_platform_driver(pruss_driver);

MODULE_AUTHOR("Suman Anna <s-anna@ti.com>");
MODULE_DESCRIPTION("PRU-ICSS Subsystem Driver");
MODULE_LICENSE("GPL v2");
