// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale Management Complex (MC) bus driver
 *
 * Copyright (C) 2014-2016 Freescale Semiconductor, Inc.
 * Author: German Rivera <German.Rivera@freescale.com>
 *
 */

#define pr_fmt(fmt) "fsl-mc: " fmt

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include <linux/msi.h>
#include <linux/dma-mapping.h>

#include "fsl-mc-private.h"

/**
 * Default DMA mask for devices on a fsl-mc bus
 */
#define FSL_MC_DEFAULT_DMA_MASK	(~0ULL)

/**
 * struct fsl_mc - Private data of a "fsl,qoriq-mc" platform device
 * @root_mc_bus_dev: fsl-mc device representing the root DPRC
 * @num_translation_ranges: number of entries in addr_translation_ranges
 * @translation_ranges: array of bus to system address translation ranges
 */
struct fsl_mc {
	struct fsl_mc_device *root_mc_bus_dev;
	u8 num_translation_ranges;
	struct fsl_mc_addr_translation_range *translation_ranges;
};

/**
 * struct fsl_mc_addr_translation_range - bus to system address translation
 * range
 * @mc_region_type: Type of MC region for the range being translated
 * @start_mc_offset: Start MC offset of the range being translated
 * @end_mc_offset: MC offset of the first byte after the range (last MC
 * offset of the range is end_mc_offset - 1)
 * @start_phys_addr: system physical address corresponding to start_mc_addr
 */
struct fsl_mc_addr_translation_range {
	enum dprc_region_type mc_region_type;
	u64 start_mc_offset;
	u64 end_mc_offset;
	phys_addr_t start_phys_addr;
};

/**
 * struct mc_version
 * @major: Major version number: incremented on API compatibility changes
 * @minor: Minor version number: incremented on API additions (that are
 *		backward compatible); reset when major version is incremented
 * @revision: Internal revision number: incremented on implementation changes
 *		and/or bug fixes that have no impact on API
 */
struct mc_version {
	u32 major;
	u32 minor;
	u32 revision;
};

/**
 * fsl_mc_bus_match - device to driver matching callback
 * @dev: the fsl-mc device to match against
 * @drv: the device driver to search for matching fsl-mc object type
 * structures
 *
 * Returns 1 on success, 0 otherwise.
 */
static int fsl_mc_bus_match(struct device *dev, struct device_driver *drv)
{
	const struct fsl_mc_device_id *id;
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(drv);
	bool found = false;

	if (!mc_drv->match_id_table)
		goto out;

	/*
	 * If the object is not 'plugged' don't match.
	 * Only exception is the root DPRC, which is a special case.
	 */
	if ((mc_dev->obj_desc.state & FSL_MC_OBJ_STATE_PLUGGED) == 0 &&
	    !fsl_mc_is_root_dprc(&mc_dev->dev))
		goto out;

	/*
	 * Traverse the match_id table of the given driver, trying to find
	 * a matching for the given device.
	 */
	for (id = mc_drv->match_id_table; id->vendor != 0x0; id++) {
		if (id->vendor == mc_dev->obj_desc.vendor &&
		    strcmp(id->obj_type, mc_dev->obj_desc.type) == 0) {
			found = true;

			break;
		}
	}

out:
	dev_dbg(dev, "%smatched\n", found ? "" : "not ");
	return found;
}

/**
 * fsl_mc_bus_uevent - callback invoked when a device is added
 */
static int fsl_mc_bus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	if (add_uevent_var(env, "MODALIAS=fsl-mc:v%08Xd%s",
			   mc_dev->obj_desc.vendor,
			   mc_dev->obj_desc.type))
		return -ENOMEM;

	return 0;
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	return sprintf(buf, "fsl-mc:v%08Xd%s\n", mc_dev->obj_desc.vendor,
		       mc_dev->obj_desc.type);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *fsl_mc_dev_attrs[] = {
	&dev_attr_modalias.attr,
	NULL,
};

ATTRIBUTE_GROUPS(fsl_mc_dev);

struct bus_type fsl_mc_bus_type = {
	.name = "fsl-mc",
	.match = fsl_mc_bus_match,
	.uevent = fsl_mc_bus_uevent,
	.dev_groups = fsl_mc_dev_groups,
};
EXPORT_SYMBOL_GPL(fsl_mc_bus_type);

struct device_type fsl_mc_bus_dprc_type = {
	.name = "fsl_mc_bus_dprc"
};

struct device_type fsl_mc_bus_dpni_type = {
	.name = "fsl_mc_bus_dpni"
};

struct device_type fsl_mc_bus_dpio_type = {
	.name = "fsl_mc_bus_dpio"
};

struct device_type fsl_mc_bus_dpsw_type = {
	.name = "fsl_mc_bus_dpsw"
};

struct device_type fsl_mc_bus_dpbp_type = {
	.name = "fsl_mc_bus_dpbp"
};

struct device_type fsl_mc_bus_dpcon_type = {
	.name = "fsl_mc_bus_dpcon"
};

struct device_type fsl_mc_bus_dpmcp_type = {
	.name = "fsl_mc_bus_dpmcp"
};

struct device_type fsl_mc_bus_dpmac_type = {
	.name = "fsl_mc_bus_dpmac"
};

struct device_type fsl_mc_bus_dprtc_type = {
	.name = "fsl_mc_bus_dprtc"
};

static struct device_type *fsl_mc_get_device_type(const char *type)
{
	static const struct {
		struct device_type *dev_type;
		const char *type;
	} dev_types[] = {
		{ &fsl_mc_bus_dprc_type, "dprc" },
		{ &fsl_mc_bus_dpni_type, "dpni" },
		{ &fsl_mc_bus_dpio_type, "dpio" },
		{ &fsl_mc_bus_dpsw_type, "dpsw" },
		{ &fsl_mc_bus_dpbp_type, "dpbp" },
		{ &fsl_mc_bus_dpcon_type, "dpcon" },
		{ &fsl_mc_bus_dpmcp_type, "dpmcp" },
		{ &fsl_mc_bus_dpmac_type, "dpmac" },
		{ &fsl_mc_bus_dprtc_type, "dprtc" },
		{ NULL, NULL }
	};
	int i;

	for (i = 0; dev_types[i].dev_type; i++)
		if (!strcmp(dev_types[i].type, type))
			return dev_types[i].dev_type;

	return NULL;
}

static int fsl_mc_driver_probe(struct device *dev)
{
	struct fsl_mc_driver *mc_drv;
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	int error;

	mc_drv = to_fsl_mc_driver(dev->driver);

	error = mc_drv->probe(mc_dev);
	if (error < 0) {
		if (error != -EPROBE_DEFER)
			dev_err(dev, "%s failed: %d\n", __func__, error);
		return error;
	}

	return 0;
}

static int fsl_mc_driver_remove(struct device *dev)
{
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(dev->driver);
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	int error;

	error = mc_drv->remove(mc_dev);
	if (error < 0) {
		dev_err(dev, "%s failed: %d\n", __func__, error);
		return error;
	}

	return 0;
}

static void fsl_mc_driver_shutdown(struct device *dev)
{
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(dev->driver);
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	mc_drv->shutdown(mc_dev);
}

/**
 * __fsl_mc_driver_register - registers a child device driver with the
 * MC bus
 *
 * This function is implicitly invoked from the registration function of
 * fsl_mc device drivers, which is generated by the
 * module_fsl_mc_driver() macro.
 */
int __fsl_mc_driver_register(struct fsl_mc_driver *mc_driver,
			     struct module *owner)
{
	int error;

	mc_driver->driver.owner = owner;
	mc_driver->driver.bus = &fsl_mc_bus_type;

	if (mc_driver->probe)
		mc_driver->driver.probe = fsl_mc_driver_probe;

	if (mc_driver->remove)
		mc_driver->driver.remove = fsl_mc_driver_remove;

	if (mc_driver->shutdown)
		mc_driver->driver.shutdown = fsl_mc_driver_shutdown;

	error = driver_register(&mc_driver->driver);
	if (error < 0) {
		pr_err("driver_register() failed for %s: %d\n",
		       mc_driver->driver.name, error);
		return error;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__fsl_mc_driver_register);

/**
 * fsl_mc_driver_unregister - unregisters a device driver from the
 * MC bus
 */
void fsl_mc_driver_unregister(struct fsl_mc_driver *mc_driver)
{
	driver_unregister(&mc_driver->driver);
}
EXPORT_SYMBOL_GPL(fsl_mc_driver_unregister);

/**
 * mc_get_version() - Retrieves the Management Complex firmware
 *			version information
 * @mc_io:		Pointer to opaque I/O object
 * @cmd_flags:		Command flags; one or more of 'MC_CMD_FLAG_'
 * @mc_ver_info:	Returned version information structure
 *
 * Return:	'0' on Success; Error code otherwise.
 */
static int mc_get_version(struct fsl_mc_io *mc_io,
			  u32 cmd_flags,
			  struct mc_version *mc_ver_info)
{
	struct fsl_mc_command cmd = { 0 };
	struct dpmng_rsp_get_version *rsp_params;
	int err;

	/* prepare command */
	cmd.header = mc_encode_cmd_header(DPMNG_CMDID_GET_VERSION,
					  cmd_flags,
					  0);

	/* send command to mc*/
	err = mc_send_command(mc_io, &cmd);
	if (err)
		return err;

	/* retrieve response parameters */
	rsp_params = (struct dpmng_rsp_get_version *)cmd.params;
	mc_ver_info->revision = le32_to_cpu(rsp_params->revision);
	mc_ver_info->major = le32_to_cpu(rsp_params->version_major);
	mc_ver_info->minor = le32_to_cpu(rsp_params->version_minor);

	return 0;
}

/**
 * fsl_mc_get_root_dprc - function to traverse to the root dprc
 */
static void fsl_mc_get_root_dprc(struct device *dev,
				 struct device **root_dprc_dev)
{
	if (!dev) {
		*root_dprc_dev = NULL;
	} else if (!dev_is_fsl_mc(dev)) {
		*root_dprc_dev = NULL;
	} else {
		*root_dprc_dev = dev;
		while (dev_is_fsl_mc((*root_dprc_dev)->parent))
			*root_dprc_dev = (*root_dprc_dev)->parent;
	}
}

static int get_dprc_attr(struct fsl_mc_io *mc_io,
			 int container_id, struct dprc_attributes *attr)
{
	u16 dprc_handle;
	int error;

	error = dprc_open(mc_io, 0, container_id, &dprc_handle);
	if (error < 0) {
		dev_err(mc_io->dev, "dprc_open() failed: %d\n", error);
		return error;
	}

	memset(attr, 0, sizeof(struct dprc_attributes));
	error = dprc_get_attributes(mc_io, 0, dprc_handle, attr);
	if (error < 0) {
		dev_err(mc_io->dev, "dprc_get_attributes() failed: %d\n",
			error);
		goto common_cleanup;
	}

	error = 0;

common_cleanup:
	(void)dprc_close(mc_io, 0, dprc_handle);
	return error;
}

static int get_dprc_icid(struct fsl_mc_io *mc_io,
			 int container_id, u16 *icid)
{
	struct dprc_attributes attr;
	int error;

	error = get_dprc_attr(mc_io, container_id, &attr);
	if (error == 0)
		*icid = attr.icid;

	return error;
}

static int translate_mc_addr(struct fsl_mc_device *mc_dev,
			     enum dprc_region_type mc_region_type,
			     u64 mc_offset, phys_addr_t *phys_addr)
{
	int i;
	struct device *root_dprc_dev;
	struct fsl_mc *mc;

	fsl_mc_get_root_dprc(&mc_dev->dev, &root_dprc_dev);
	mc = dev_get_drvdata(root_dprc_dev->parent);

	if (mc->num_translation_ranges == 0) {
		/*
		 * Do identity mapping:
		 */
		*phys_addr = mc_offset;
		return 0;
	}

	for (i = 0; i < mc->num_translation_ranges; i++) {
		struct fsl_mc_addr_translation_range *range =
			&mc->translation_ranges[i];

		if (mc_region_type == range->mc_region_type &&
		    mc_offset >= range->start_mc_offset &&
		    mc_offset < range->end_mc_offset) {
			*phys_addr = range->start_phys_addr +
				     (mc_offset - range->start_mc_offset);
			return 0;
		}
	}

	return -EFAULT;
}

static int fsl_mc_device_get_mmio_regions(struct fsl_mc_device *mc_dev,
					  struct fsl_mc_device *mc_bus_dev)
{
	int i;
	int error;
	struct resource *regions;
	struct fsl_mc_obj_desc *obj_desc = &mc_dev->obj_desc;
	struct device *parent_dev = mc_dev->dev.parent;
	enum dprc_region_type mc_region_type;

	if (is_fsl_mc_bus_dprc(mc_dev) ||
	    is_fsl_mc_bus_dpmcp(mc_dev)) {
		mc_region_type = DPRC_REGION_TYPE_MC_PORTAL;
	} else if (is_fsl_mc_bus_dpio(mc_dev)) {
		mc_region_type = DPRC_REGION_TYPE_QBMAN_PORTAL;
	} else {
		/*
		 * This function should not have been called for this MC object
		 * type, as this object type is not supposed to have MMIO
		 * regions
		 */
		return -EINVAL;
	}

	regions = kmalloc_array(obj_desc->region_count,
				sizeof(regions[0]), GFP_KERNEL);
	if (!regions)
		return -ENOMEM;

	for (i = 0; i < obj_desc->region_count; i++) {
		struct dprc_region_desc region_desc;

		error = dprc_get_obj_region(mc_bus_dev->mc_io,
					    0,
					    mc_bus_dev->mc_handle,
					    obj_desc->type,
					    obj_desc->id, i, &region_desc);
		if (error < 0) {
			dev_err(parent_dev,
				"dprc_get_obj_region() failed: %d\n", error);
			goto error_cleanup_regions;
		}

		error = translate_mc_addr(mc_dev, mc_region_type,
					  region_desc.base_offset,
					  &regions[i].start);
		if (error < 0) {
			dev_err(parent_dev,
				"Invalid MC offset: %#x (for %s.%d\'s region %d)\n",
				region_desc.base_offset,
				obj_desc->type, obj_desc->id, i);
			goto error_cleanup_regions;
		}

		regions[i].end = regions[i].start + region_desc.size - 1;
		regions[i].name = "fsl-mc object MMIO region";
		regions[i].flags = IORESOURCE_IO;
		if (region_desc.flags & DPRC_REGION_CACHEABLE)
			regions[i].flags |= IORESOURCE_CACHEABLE;
	}

	mc_dev->regions = regions;
	return 0;

error_cleanup_regions:
	kfree(regions);
	return error;
}

/**
 * fsl_mc_is_root_dprc - function to check if a given device is a root dprc
 */
bool fsl_mc_is_root_dprc(struct device *dev)
{
	struct device *root_dprc_dev;

	fsl_mc_get_root_dprc(dev, &root_dprc_dev);
	if (!root_dprc_dev)
		return false;
	return dev == root_dprc_dev;
}

static void fsl_mc_device_release(struct device *dev)
{
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	kfree(mc_dev->regions);

	if (is_fsl_mc_bus_dprc(mc_dev))
		kfree(to_fsl_mc_bus(mc_dev));
	else
		kfree(mc_dev);
}

/**
 * Add a newly discovered fsl-mc device to be visible in Linux
 */
int fsl_mc_device_add(struct fsl_mc_obj_desc *obj_desc,
		      struct fsl_mc_io *mc_io,
		      struct device *parent_dev,
		      struct fsl_mc_device **new_mc_dev)
{
	int error;
	struct fsl_mc_device *mc_dev = NULL;
	struct fsl_mc_bus *mc_bus = NULL;
	struct fsl_mc_device *parent_mc_dev;

	if (dev_is_fsl_mc(parent_dev))
		parent_mc_dev = to_fsl_mc_device(parent_dev);
	else
		parent_mc_dev = NULL;

	if (strcmp(obj_desc->type, "dprc") == 0) {
		/*
		 * Allocate an MC bus device object:
		 */
		mc_bus = kzalloc(sizeof(*mc_bus), GFP_KERNEL);
		if (!mc_bus)
			return -ENOMEM;

		mc_dev = &mc_bus->mc_dev;
	} else {
		/*
		 * Allocate a regular fsl_mc_device object:
		 */
		mc_dev = kzalloc(sizeof(*mc_dev), GFP_KERNEL);
		if (!mc_dev)
			return -ENOMEM;
	}

	mc_dev->obj_desc = *obj_desc;
	mc_dev->mc_io = mc_io;
	device_initialize(&mc_dev->dev);
	mc_dev->dev.parent = parent_dev;
	mc_dev->dev.bus = &fsl_mc_bus_type;
	mc_dev->dev.release = fsl_mc_device_release;
	mc_dev->dev.type = fsl_mc_get_device_type(obj_desc->type);
	if (!mc_dev->dev.type) {
		error = -ENODEV;
		dev_err(parent_dev, "unknown device type %s\n", obj_desc->type);
		goto error_cleanup_dev;
	}
	dev_set_name(&mc_dev->dev, "%s.%d", obj_desc->type, obj_desc->id);

	if (strcmp(obj_desc->type, "dprc") == 0) {
		struct fsl_mc_io *mc_io2;

		mc_dev->flags |= FSL_MC_IS_DPRC;

		/*
		 * To get the DPRC's ICID, we need to open the DPRC
		 * in get_dprc_icid(). For child DPRCs, we do so using the
		 * parent DPRC's MC portal instead of the child DPRC's MC
		 * portal, in case the child DPRC is already opened with
		 * its own portal (e.g., the DPRC used by AIOP).
		 *
		 * NOTE: There cannot be more than one active open for a
		 * given MC object, using the same MC portal.
		 */
		if (parent_mc_dev) {
			/*
			 * device being added is a child DPRC device
			 */
			mc_io2 = parent_mc_dev->mc_io;
		} else {
			/*
			 * device being added is the root DPRC device
			 */
			if (!mc_io) {
				error = -EINVAL;
				goto error_cleanup_dev;
			}

			mc_io2 = mc_io;
		}

		error = get_dprc_icid(mc_io2, obj_desc->id, &mc_dev->icid);
		if (error < 0)
			goto error_cleanup_dev;
	} else {
		/*
		 * A non-DPRC object has to be a child of a DPRC, use the
		 * parent's ICID and interrupt domain.
		 */
		mc_dev->icid = parent_mc_dev->icid;
		mc_dev->dma_mask = FSL_MC_DEFAULT_DMA_MASK;
		mc_dev->dev.dma_mask = &mc_dev->dma_mask;
		dev_set_msi_domain(&mc_dev->dev,
				   dev_get_msi_domain(&parent_mc_dev->dev));
	}

	/*
	 * Get MMIO regions for the device from the MC:
	 *
	 * NOTE: the root DPRC is a special case as its MMIO region is
	 * obtained from the device tree
	 */
	if (parent_mc_dev && obj_desc->region_count != 0) {
		error = fsl_mc_device_get_mmio_regions(mc_dev,
						       parent_mc_dev);
		if (error < 0)
			goto error_cleanup_dev;
	}

	/* Objects are coherent, unless 'no shareability' flag set. */
	if (!(obj_desc->flags & FSL_MC_OBJ_FLAG_NO_MEM_SHAREABILITY))
		arch_setup_dma_ops(&mc_dev->dev, 0, 0, NULL, true);

	/*
	 * The device-specific probe callback will get invoked by device_add()
	 */
	error = device_add(&mc_dev->dev);
	if (error < 0) {
		dev_err(parent_dev,
			"device_add() failed for device %s: %d\n",
			dev_name(&mc_dev->dev), error);
		goto error_cleanup_dev;
	}

	dev_dbg(parent_dev, "added %s\n", dev_name(&mc_dev->dev));

	*new_mc_dev = mc_dev;
	return 0;

error_cleanup_dev:
	kfree(mc_dev->regions);
	if (mc_bus)
		kfree(mc_bus);
	else
		kfree(mc_dev);

	return error;
}
EXPORT_SYMBOL_GPL(fsl_mc_device_add);

/**
 * fsl_mc_device_remove - Remove an fsl-mc device from being visible to
 * Linux
 *
 * @mc_dev: Pointer to an fsl-mc device
 */
void fsl_mc_device_remove(struct fsl_mc_device *mc_dev)
{
	/*
	 * The device-specific remove callback will get invoked by device_del()
	 */
	device_del(&mc_dev->dev);
	put_device(&mc_dev->dev);
}
EXPORT_SYMBOL_GPL(fsl_mc_device_remove);

static int parse_mc_ranges(struct device *dev,
			   int *paddr_cells,
			   int *mc_addr_cells,
			   int *mc_size_cells,
			   const __be32 **ranges_start)
{
	const __be32 *prop;
	int range_tuple_cell_count;
	int ranges_len;
	int tuple_len;
	struct device_node *mc_node = dev->of_node;

	*ranges_start = of_get_property(mc_node, "ranges", &ranges_len);
	if (!(*ranges_start) || !ranges_len) {
		dev_warn(dev,
			 "missing or empty ranges property for device tree node '%s'\n",
			 mc_node->name);
		return 0;
	}

	*paddr_cells = of_n_addr_cells(mc_node);

	prop = of_get_property(mc_node, "#address-cells", NULL);
	if (prop)
		*mc_addr_cells = be32_to_cpup(prop);
	else
		*mc_addr_cells = *paddr_cells;

	prop = of_get_property(mc_node, "#size-cells", NULL);
	if (prop)
		*mc_size_cells = be32_to_cpup(prop);
	else
		*mc_size_cells = of_n_size_cells(mc_node);

	range_tuple_cell_count = *paddr_cells + *mc_addr_cells +
				 *mc_size_cells;

	tuple_len = range_tuple_cell_count * sizeof(__be32);
	if (ranges_len % tuple_len != 0) {
		dev_err(dev, "malformed ranges property '%s'\n", mc_node->name);
		return -EINVAL;
	}

	return ranges_len / tuple_len;
}

static int get_mc_addr_translation_ranges(struct device *dev,
					  struct fsl_mc_addr_translation_range
						**ranges,
					  u8 *num_ranges)
{
	int ret;
	int paddr_cells;
	int mc_addr_cells;
	int mc_size_cells;
	int i;
	const __be32 *ranges_start;
	const __be32 *cell;

	ret = parse_mc_ranges(dev,
			      &paddr_cells,
			      &mc_addr_cells,
			      &mc_size_cells,
			      &ranges_start);
	if (ret < 0)
		return ret;

	*num_ranges = ret;
	if (!ret) {
		/*
		 * Missing or empty ranges property ("ranges;") for the
		 * 'fsl,qoriq-mc' node. In this case, identity mapping
		 * will be used.
		 */
		*ranges = NULL;
		return 0;
	}

	*ranges = devm_kcalloc(dev, *num_ranges,
			       sizeof(struct fsl_mc_addr_translation_range),
			       GFP_KERNEL);
	if (!(*ranges))
		return -ENOMEM;

	cell = ranges_start;
	for (i = 0; i < *num_ranges; ++i) {
		struct fsl_mc_addr_translation_range *range = &(*ranges)[i];

		range->mc_region_type = of_read_number(cell, 1);
		range->start_mc_offset = of_read_number(cell + 1,
							mc_addr_cells - 1);
		cell += mc_addr_cells;
		range->start_phys_addr = of_read_number(cell, paddr_cells);
		cell += paddr_cells;
		range->end_mc_offset = range->start_mc_offset +
				     of_read_number(cell, mc_size_cells);

		cell += mc_size_cells;
	}

	return 0;
}

/**
 * fsl_mc_bus_probe - callback invoked when the root MC bus is being
 * added
 */
static int fsl_mc_bus_probe(struct platform_device *pdev)
{
	struct fsl_mc_obj_desc obj_desc;
	int error;
	struct fsl_mc *mc;
	struct fsl_mc_device *mc_bus_dev = NULL;
	struct fsl_mc_io *mc_io = NULL;
	int container_id;
	phys_addr_t mc_portal_phys_addr;
	u32 mc_portal_size;
	struct mc_version mc_version;
	struct resource res;

	mc = devm_kzalloc(&pdev->dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	platform_set_drvdata(pdev, mc);

	/*
	 * Get physical address of MC portal for the root DPRC:
	 */
	error = of_address_to_resource(pdev->dev.of_node, 0, &res);
	if (error < 0) {
		dev_err(&pdev->dev,
			"of_address_to_resource() failed for %pOF\n",
			pdev->dev.of_node);
		return error;
	}

	mc_portal_phys_addr = res.start;
	mc_portal_size = resource_size(&res);
	error = fsl_create_mc_io(&pdev->dev, mc_portal_phys_addr,
				 mc_portal_size, NULL,
				 FSL_MC_IO_ATOMIC_CONTEXT_PORTAL, &mc_io);
	if (error < 0)
		return error;

	error = mc_get_version(mc_io, 0, &mc_version);
	if (error != 0) {
		dev_err(&pdev->dev,
			"mc_get_version() failed with error %d\n", error);
		goto error_cleanup_mc_io;
	}

	dev_info(&pdev->dev, "MC firmware version: %u.%u.%u\n",
		 mc_version.major, mc_version.minor, mc_version.revision);

	error = get_mc_addr_translation_ranges(&pdev->dev,
					       &mc->translation_ranges,
					       &mc->num_translation_ranges);
	if (error < 0)
		goto error_cleanup_mc_io;

	error = dprc_get_container_id(mc_io, 0, &container_id);
	if (error < 0) {
		dev_err(&pdev->dev,
			"dprc_get_container_id() failed: %d\n", error);
		goto error_cleanup_mc_io;
	}

	memset(&obj_desc, 0, sizeof(struct fsl_mc_obj_desc));
	error = dprc_get_api_version(mc_io, 0,
				     &obj_desc.ver_major,
				     &obj_desc.ver_minor);
	if (error < 0)
		goto error_cleanup_mc_io;

	obj_desc.vendor = FSL_MC_VENDOR_FREESCALE;
	strcpy(obj_desc.type, "dprc");
	obj_desc.id = container_id;
	obj_desc.irq_count = 1;
	obj_desc.region_count = 0;

	error = fsl_mc_device_add(&obj_desc, mc_io, &pdev->dev, &mc_bus_dev);
	if (error < 0)
		goto error_cleanup_mc_io;

	mc->root_mc_bus_dev = mc_bus_dev;
	return 0;

error_cleanup_mc_io:
	fsl_destroy_mc_io(mc_io);
	return error;
}

/**
 * fsl_mc_bus_remove - callback invoked when the root MC bus is being
 * removed
 */
static int fsl_mc_bus_remove(struct platform_device *pdev)
{
	struct fsl_mc *mc = platform_get_drvdata(pdev);

	if (!fsl_mc_is_root_dprc(&mc->root_mc_bus_dev->dev))
		return -EINVAL;

	fsl_mc_device_remove(mc->root_mc_bus_dev);

	fsl_destroy_mc_io(mc->root_mc_bus_dev->mc_io);
	mc->root_mc_bus_dev->mc_io = NULL;

	return 0;
}

static const struct of_device_id fsl_mc_bus_match_table[] = {
	{.compatible = "fsl,qoriq-mc",},
	{},
};

MODULE_DEVICE_TABLE(of, fsl_mc_bus_match_table);

static struct platform_driver fsl_mc_bus_driver = {
	.driver = {
		   .name = "fsl_mc_bus",
		   .pm = NULL,
		   .of_match_table = fsl_mc_bus_match_table,
		   },
	.probe = fsl_mc_bus_probe,
	.remove = fsl_mc_bus_remove,
};

static int __init fsl_mc_bus_driver_init(void)
{
	int error;

	error = bus_register(&fsl_mc_bus_type);
	if (error < 0) {
		pr_err("bus type registration failed: %d\n", error);
		goto error_cleanup_cache;
	}

	error = platform_driver_register(&fsl_mc_bus_driver);
	if (error < 0) {
		pr_err("platform_driver_register() failed: %d\n", error);
		goto error_cleanup_bus;
	}

	error = dprc_driver_init();
	if (error < 0)
		goto error_cleanup_driver;

	error = fsl_mc_allocator_driver_init();
	if (error < 0)
		goto error_cleanup_dprc_driver;

	return 0;

error_cleanup_dprc_driver:
	dprc_driver_exit();

error_cleanup_driver:
	platform_driver_unregister(&fsl_mc_bus_driver);

error_cleanup_bus:
	bus_unregister(&fsl_mc_bus_type);

error_cleanup_cache:
	return error;
}
postcore_initcall(fsl_mc_bus_driver_init);
