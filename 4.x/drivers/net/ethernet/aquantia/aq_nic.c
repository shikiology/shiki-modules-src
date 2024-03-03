// SPDX-License-Identifier: GPL-2.0-only
/* Atlantic Network Driver
 *
 * Copyright (C) 2014-2019 aQuantia Corporation
 * Copyright (C) 2019-2020 Marvell International Ltd.
 */

/* File aq_nic.c: Definition of common code for NIC. */

#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pm_runtime.h>
#include <linux/firmware.h>
#include <linux/timer.h>
#include <linux/cpu.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/ipv6.h>
#include <net/pkt_cls.h>

#include "aq_nic.h"
#include "aq_ring.h"
#include "aq_vec.h"
#include "aq_hw.h"
#include "aq_pci_func.h"
#include "aq_macsec.h"
#include "aq_main.h"
#include "aq_ptp.h"
#include "aq_phy.h"
#include "aq_filters.h"
#include "aq_trace.h"
#include "aq_hw_utils.h"

static unsigned int aq_itr = AQ_CFG_INTERRUPT_MODERATION_AUTO;
module_param_named(aq_itr, aq_itr, uint, 0644);
MODULE_PARM_DESC(aq_itr, "Interrupt throttling mode");

static unsigned int aq_itr_tx;
module_param_named(aq_itr_tx, aq_itr_tx, uint, 0644);
MODULE_PARM_DESC(aq_itr_tx, "TX interrupt throttle rate");

static unsigned int aq_itr_rx;
module_param_named(aq_itr_rx, aq_itr_rx, uint, 0644);
MODULE_PARM_DESC(aq_itr_rx, "RX interrupt throttle rate");

static unsigned int aq_rxpageorder;
module_param_named(aq_rxpageorder, aq_rxpageorder, uint, 0644);
MODULE_PARM_DESC(aq_rxpageorder, "RX page order override");

unsigned int aq_rx_refill_thres = 32;
module_param_named(aq_rx_refill_thres, aq_rx_refill_thres, uint, 0644);
MODULE_PARM_DESC(aq_rx_refill_thres, "RX refill threshold");

unsigned int debug = AQ_MSG_DRV | AQ_MSG_LINK;
module_param_named(debug, debug, uint, 0644);
MODULE_PARM_DESC(debug, "Default debug msglevel");

#define AQ_MODULE_PARAM_ARR(X, type, desc) \
	static type X[AQ_NIC_MAX] = { 0 }; \
	static unsigned int X##_count; \
	module_param_array_named(X, X, type, &X##_count, 0644); \
	MODULE_PARM_DESC(X, desc)

AQ_MODULE_PARAM_ARR(aq_fw_did, uint, "Use FW image for this DID");
AQ_MODULE_PARAM_ARR(aq_fw_sid, uint, "Use provisioning data for this SID");
AQ_MODULE_PARAM_ARR(aq_force_host_boot, uint, "Force host boot");

int aq_enable_wa;
module_param_named(aq_enable_wa, aq_enable_wa, int, 0644);
MODULE_PARM_DESC(aq_enable_wa, "Quirk bits to enable HW workarounds");

static bool aq_enable_ptp = AQ_CFG_PTP_DEF;
module_param(aq_enable_ptp, bool, 0644);
MODULE_PARM_DESC(aq_enable_ptp, "Enable PTP");

static void aq_nic_update_ndev_stats(struct aq_nic_s *self);

static void aq_nic_rss_init(struct aq_nic_s *self, unsigned int num_rss_queues)
{
	static u8 rss_key[AQ_CFG_RSS_HASHKEY_SIZE] = {
		0x1e, 0xad, 0x71, 0x87, 0x65, 0xfc, 0x26, 0x7d,
		0x0d, 0x45, 0x67, 0x74, 0xcd, 0x06, 0x1a, 0x18,
		0xb6, 0xc1, 0xf0, 0xc7, 0xbb, 0x18, 0xbe, 0xf8,
		0x19, 0x13, 0x4b, 0xa9, 0xd0, 0x3e, 0xfe, 0x70,
		0x25, 0x03, 0xab, 0x50, 0x6a, 0x8b, 0x82, 0x0c
	};
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	struct aq_rss_parameters *rss_params;
	int i = 0;

	rss_params = &cfg->aq_rss;

	rss_params->hash_secret_key_size = sizeof(rss_key);
	memcpy(rss_params->hash_secret_key, rss_key, sizeof(rss_key));
	rss_params->indirection_table_size = AQ_CFG_RSS_INDIRECTION_TABLE_MAX;

	for (i = rss_params->indirection_table_size; i--;)
		rss_params->indirection_table[i] = i & (num_rss_queues - 1);
}

/* Recalculate the number of vectors */
static void aq_nic_cfg_update_num_vecs(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	cfg->vecs = min(cfg->aq_hw_caps->vecs, AQ_CFG_VECS_DEF);
	cfg->vecs = min(cfg->vecs, num_online_cpus());
	if (self->irqvecs > AQ_HW_SERVICE_IRQS + AQ_HW_PTP_IRQS)
		cfg->vecs = min(cfg->vecs,
				self->irqvecs - AQ_HW_SERVICE_IRQS - AQ_HW_PTP_IRQS);
	else if (self->irqvecs > AQ_HW_PTP_IRQS)
		cfg->vecs = min(cfg->vecs,
				self->irqvecs - AQ_HW_PTP_IRQS);
	else
		cfg->vecs = 1U;

	/* cfg->vecs should be power of 2 for RSS */
	cfg->vecs = rounddown_pow_of_two(cfg->vecs);

	if (ATL_HW_IS_CHIP_FEATURE(self->aq_hw, ANTIGUA)) {
		if (cfg->tcs > 2)
			cfg->vecs = min(cfg->vecs, 4U);
	}

	aq_pr_verbose(self, AQ_MSG_DEBUG, "cfg->vecs = %d\n", cfg->vecs);
	if (cfg->vecs <= 4)
		cfg->tc_mode = AQ_TC_MODE_8TCS;
	else
		cfg->tc_mode = AQ_TC_MODE_4TCS;

	/*rss rings */
	cfg->num_rss_queues = min(cfg->vecs, AQ_CFG_NUM_RSS_QUEUES_DEF);
	aq_nic_rss_init(self, cfg->num_rss_queues);
}

/* Checks hw_caps and 'corrects' aq_nic_cfg in runtime */
void aq_nic_cfg_start(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	int i;

	cfg->tcs = AQ_CFG_TCS_DEF;

	cfg->is_polling = AQ_CFG_IS_POLLING_DEF;

	cfg->itr = aq_itr;
	cfg->tx_itr = aq_itr_tx;
	cfg->rx_itr = aq_itr_rx;

	cfg->rxpageorder = aq_rxpageorder;
	cfg->is_rss = AQ_CFG_IS_RSS_DEF;
	cfg->aq_rss.base_cpu_number = AQ_CFG_RSS_BASE_CPU_NUM_DEF;
	cfg->fc.req = AQ_CFG_FC_MODE;
	cfg->wol = AQ_CFG_WOL_MODES;

	cfg->mtu = AQ_CFG_MTU_DEF;
	cfg->link_speed_msk = AQ_CFG_SPEED_MSK;
	cfg->is_autoneg = AQ_CFG_IS_AUTONEG_DEF;

	cfg->is_lro = AQ_CFG_IS_LRO_DEF;
	cfg->is_ptp = aq_enable_ptp;

	/*descriptors */
	cfg->rxds = min(cfg->aq_hw_caps->rxds_max, AQ_CFG_RXDS_DEF);
	cfg->txds = min(cfg->aq_hw_caps->txds_max, AQ_CFG_TXDS_DEF);

	aq_nic_cfg_update_num_vecs(self);

	cfg->num_irq_vecs = self->irqvecs;
	cfg->irq_type = aq_pci_func_get_irq_type(self);

	if ((cfg->irq_type == AQ_HW_IRQ_LEGACY) ||
	    (cfg->aq_hw_caps->vecs == 1U) ||
	    (cfg->vecs == 1U)) {
		cfg->is_rss = 0U;
		cfg->vecs = 1U;
	}

	/* Check if we have enough vectors allocated for
	 * link status IRQ. If no - we'll know link state from
	 * slower service task.
	 */
	if (AQ_HW_SERVICE_IRQS > 0 &&
	    self->irqvecs > AQ_HW_PTP_IRQS + AQ_HW_SERVICE_IRQS)
		cfg->link_irq_vec = cfg->vecs;
	else
		cfg->link_irq_vec = 0;

	cfg->link_speed_msk &= cfg->aq_hw_caps->link_speed_msk;
	cfg->features = cfg->aq_hw_caps->hw_features;
	cfg->is_vlan_rx_strip = !!(cfg->features & NETIF_F_HW_VLAN_CTAG_RX);
	cfg->is_vlan_tx_insert = !!(cfg->features & NETIF_F_HW_VLAN_CTAG_TX);
	cfg->is_vlan_force_promisc = true;
	/* enable downshift feature by default */
	cfg->priv_flags = AQ_HW_DOWNSHIFT_MASK;

	for (i = 0; i < sizeof(cfg->prio_tc_map); i++)
		cfg->prio_tc_map[i] = cfg->tcs * i / 8;
}

static int aq_nic_update_link_status(struct aq_nic_s *self)
{
	struct aq_hw_link_status_s *new_link_status = &self->aq_hw->aq_link_status;
	int err = self->aq_fw_ops->update_link_status(self->aq_hw);
	u32 fc = 0;

	if (err)
		return err;

	if (self->aq_fw_ops->get_flow_control)
		self->aq_fw_ops->get_flow_control(self->aq_hw, &fc);
	self->aq_nic_cfg.fc.cur = fc;

	if (self->link_status.mbps != new_link_status->mbps) {
		aq_pr_verbose(self, NETIF_MSG_LINK, "%s: link change old %d new %d\n",
			      AQ_CFG_DRV_NAME, self->link_status.mbps,
			      new_link_status->mbps);
		aq_nic_update_interrupt_moderation_settings(self);

		if (self->aq_ptp) {
			/* PTP does not work in some modes even if physical link is up */
			bool ptp_link_good = (new_link_status->mbps >= 100 &&
					      new_link_status->full_duplex);

			aq_ptp_clock_init(self, ptp_link_good ? AQ_PTP_LINK_UP : AQ_PTP_NO_LINK);
			aq_ptp_tm_offset_set(self, new_link_status->mbps);
		}

		/* Driver has to update flow control settings on RX block
		 * on any link event.
		 * We should query FW whether it negotiated FC.
		 */
		if (self->aq_hw_ops->hw_set_fc)
			self->aq_hw_ops->hw_set_fc(self->aq_hw, fc, 0);
	}

	self->link_status = self->aq_hw->aq_link_status;
	if (!netif_carrier_ok(self->ndev) && self->link_status.mbps) {
		aq_utils_obj_set(&self->flags,
				 AQ_NIC_FLAG_STARTED);
		aq_utils_obj_clear(&self->flags,
				   AQ_NIC_LINK_DOWN);

		pm_runtime_get_sync(&self->pdev->dev);
		netif_carrier_on(self->ndev);
#if IS_ENABLED(CONFIG_MACSEC)
		aq_macsec_enable(self);
#endif
		if (self->aq_hw_ops->hw_tc_rate_limit_set)
			self->aq_hw_ops->hw_tc_rate_limit_set(self->aq_hw);

		netif_tx_wake_all_queues(self->ndev);
	}
	if (netif_carrier_ok(self->ndev) && !self->link_status.mbps) {
		netif_carrier_off(self->ndev);
		netif_tx_disable(self->ndev);
		aq_utils_obj_set(&self->flags, AQ_NIC_LINK_DOWN);
		pm_runtime_put(&self->pdev->dev);
	}

	return 0;
}

static irqreturn_t aq_linkstate_threaded_isr(int irq, void *private)
{
	struct aq_nic_s *self = private;

	if (!self)
		return IRQ_NONE;

	aq_nic_update_link_status(self);

	self->aq_hw_ops->hw_irq_enable(self->aq_hw,
				       BIT(self->aq_nic_cfg.link_irq_vec));

	return IRQ_HANDLED;
}

static void aq_nic_service_task(struct work_struct *work)
{
	struct aq_nic_s *self = container_of(work, struct aq_nic_s,
					     service_task);
	int err;

	aq_ptp_service_task(self);

	if (aq_utils_obj_test(&self->flags, AQ_NIC_FLAGS_IS_NOT_READY))
		return;

	err = aq_nic_update_link_status(self);
	if (err)
		return;

#if IS_ENABLED(CONFIG_MACSEC)
	aq_macsec_work(self);
#endif

	mutex_lock(&self->fwreq_mutex);
	if (self->aq_fw_ops->update_stats)
		self->aq_fw_ops->update_stats(self->aq_hw);
	mutex_unlock(&self->fwreq_mutex);

	aq_nic_update_ndev_stats(self);

	/* DASH event support on FW 4.x */
	aq_dash_process_events(self);
}

static void aq_nic_service_timer_cb(struct timer_list *t)
{
	struct aq_nic_s *self = from_timer(self, t, service_timer);

	mod_timer(&self->service_timer,
		  jiffies + AQ_CFG_SERVICE_TIMER_INTERVAL);

	aq_ndev_schedule_work(&self->service_task);
}

static void aq_nic_polling_timer_cb(struct timer_list *t)
{
	struct aq_nic_s *self = from_timer(self, t, polling_timer);
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_isr(i, (void *)aq_vec);

	mod_timer(&self->polling_timer, jiffies +
		  AQ_CFG_POLLING_TIMER_INTERVAL);
}

static int aq_nic_hw_prepare(struct aq_nic_s *self)
{
	int err = 0;

	err = self->aq_hw_ops->hw_soft_reset(self->aq_hw);

	self->aq_hw->clk_select = -1;

	if (err) {
		if (self->aq_hw->image_required) {
			aq_nic_request_firmware(self);
			err = self->aq_hw_ops->hw_soft_reset(self->aq_hw);
			if (err)
				goto exit;
		} else {
			goto exit;
		}
	}

	err = self->aq_hw_ops->hw_prepare(self->aq_hw, &self->aq_fw_ops);

exit:
	return err;
}

static bool aq_nic_is_valid_ether_addr(const u8 *addr)
{
	/* Some engineering samples of Aquantia NICs are provisioned with a
	 * partially populated MAC, which is still invalid.
	 */
	return !(addr[0] == 0 && addr[1] == 0 && addr[2] == 0);
}

int aq_nic_ndev_register(struct aq_nic_s *self)
{
	u8 addr[ETH_ALEN];
	int err = 0;

	if (!self->ndev) {
		err = -EINVAL;
		goto err_exit;
	}

#ifdef AQ_CFG_FAST_START
	self->aq_hw->fast_start_enabled = true;
#endif
	err = aq_nic_hw_prepare(self);
	if (err) {
		aq_pr_err("HW prepare failed, err = %d\n", err);
		goto err_exit;
	}

#if IS_ENABLED(CONFIG_MACSEC)
	aq_macsec_init(self);
#endif

	if (platform_get_ethdev_address(&self->pdev->dev, self->ndev) != 0) {
		// If DT has none or an invalid one, ask device for MAC address
		mutex_lock(&self->fwreq_mutex);
		err = self->aq_fw_ops->get_mac_permanent(self->aq_hw, addr);
		mutex_unlock(&self->fwreq_mutex);

		if (err)
			goto err_exit;

		if (is_valid_ether_addr(addr) &&
		    aq_nic_is_valid_ether_addr(addr)) {
			eth_hw_addr_set(self->ndev, addr);
		} else {
			netdev_warn(self->ndev, "MAC is invalid, will use random.");
			eth_hw_addr_random(self->ndev);
		}
	}

#if defined(AQ_CFG_MAC_ADDR_PERMANENT)
	{
		static u8 mac_addr_permanent[] = AQ_CFG_MAC_ADDR_PERMANENT;

		ether_addr_copy(self->ndev->dev_addr, mac_addr_permanent);
	}
#endif

	for (self->aq_vecs = 0; self->aq_vecs < aq_nic_get_cfg(self)->vecs;
	     self->aq_vecs++) {
		self->aq_vec[self->aq_vecs] =
		    aq_vec_alloc(self, self->aq_vecs, aq_nic_get_cfg(self));
		if (!self->aq_vec[self->aq_vecs]) {
			err = -ENOMEM;
			goto err_exit;
		}
	}

	netif_carrier_off(self->ndev);

	netif_tx_disable(self->ndev);

	err = register_netdev(self->ndev);
	if (err) {
		aq_pr_err("Netedev register failed, err = %d\n", err);
		goto err_exit;
	}

err_exit:
#if IS_ENABLED(CONFIG_MACSEC)
	if (err)
		aq_macsec_free(self);
#endif
	return err;
}

void aq_nic_ndev_init(struct aq_nic_s *self)
{
	const struct aq_hw_caps_s *aq_hw_caps = self->aq_nic_cfg.aq_hw_caps;
	struct aq_nic_cfg_s *aq_nic_cfg = &self->aq_nic_cfg;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
	self->ndev->hw_features |= aq_hw_caps->hw_features;
#endif
	self->ndev->features = aq_hw_caps->hw_features;
	self->ndev->vlan_features |= NETIF_F_HW_CSUM | NETIF_F_RXCSUM |
				     NETIF_F_RXHASH | NETIF_F_SG |
				     NETIF_F_LRO | NETIF_F_TSO | NETIF_F_TSO6;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	self->ndev->gso_partial_features = NETIF_F_GSO_UDP_L4;
	self->ndev->gso_max_size = 256*1024;
#endif
	self->ndev->priv_flags |= aq_hw_caps->hw_priv_flags;
	self->ndev->priv_flags |= IFF_LIVE_ADDR_CHANGE;

	self->msg_enable = debug;
	self->ndev->mtu = aq_nic_cfg->mtu - ETH_HLEN;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	self->ndev->max_mtu = aq_hw_caps->mtu - ETH_FCS_LEN - ETH_HLEN;
#elif (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7, 5))
	self->ndev->extended->min_mtu = ETH_MIN_MTU;
	self->ndev->extended->max_mtu = aq_hw_caps->mtu -
					ETH_FCS_LEN - ETH_HLEN;
#endif
}

void aq_nic_set_tx_ring(struct aq_nic_s *self, unsigned int idx,
			struct aq_ring_s *ring)
{
	self->aq_ring_tx[idx] = ring;
}

struct net_device *aq_nic_get_ndev(struct aq_nic_s *self)
{
	return self->ndev;
}

int aq_nic_init(struct aq_nic_s *self)
{
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;
	int err = 0;

	self->power_state = AQ_HW_POWER_STATE_D0;
	mutex_lock(&self->fwreq_mutex);
	err = self->aq_hw_ops->hw_reset(self->aq_hw);
	mutex_unlock(&self->fwreq_mutex);
	if (err < 0) {
		aq_pr_err("HW reset failed, err = %d\n", err);
		goto err_exit;
	}

	/* Restore default settings */
	aq_nic_set_downshift(self, self->aq_nic_cfg.downshift_counter);
	aq_nic_set_media_detect(self, self->aq_nic_cfg.is_media_detect ?
				AQ_HW_MEDIA_DETECT_CNT : 0);

	err = self->aq_hw_ops->hw_init(self->aq_hw,
				       aq_nic_get_ndev(self)->dev_addr);
	if (err < 0) {
		aq_pr_err("HW init failed, err = %d\n", err);
		goto err_exit;
	}

	if (ATL_HW_IS_CHIP_FEATURE(self->aq_hw, ATLANTIC) &&
	    self->aq_nic_cfg.aq_hw_caps->media_type == AQ_HW_MEDIA_TYPE_TP) {
		self->aq_hw->phy_id = HW_ATL_PHY_ID_MAX;
		err = aq_phy_init(self->aq_hw);

		/* [ATLDRV-742] Workaround for Bermuda:
		 * Disable PTP block because it can cause data path problems.
		 * This should be done by PHY provisioning but a lot of units
		 * with enabled PTP block has been shipped already.
		 * So, we workaround this issue in the driver.
		 */
		if (self->aq_nic_cfg.aq_hw_caps->quirks & AQ_NIC_QUIRK_BAD_PTP)
			if (self->aq_hw->phy_id != HW_ATL_PHY_ID_MAX)
				aq_phy_disable_ptp(self->aq_hw);
	}

	for (i = 0U; i < self->aq_vecs; i++) {
		aq_vec = self->aq_vec[i];
		err = aq_vec_ring_alloc(aq_vec, self, i,
					aq_nic_get_cfg(self));
		if (err) {
			aq_pr_err("Vector ring allocation failed, err = %d\n", err);
			goto err_exit;
		}

		aq_vec_init(aq_vec, self->aq_hw_ops, self->aq_hw);
	}

	if (aq_nic_get_cfg(self)->is_ptp) {
		u32 ptp_isr_vec, ptp_ext_vec;

		if (self->irqvecs > AQ_HW_PTP_IRQS) {
			ptp_isr_vec = self->irqvecs - AQ_HW_PTP_IRQS;
			ptp_ext_vec = self->irqvecs - AQ_HW_PTP_IRQS + 1U;
		} else {
			ptp_isr_vec = ptp_ext_vec = 0;
		}

		err = aq_ptp_init(self, ptp_isr_vec, ptp_ext_vec);
		if (err < 0)
			goto err_exit;

		err = aq_ptp_ring_alloc(self);
		if (err < 0)
			goto err_exit;

		err = aq_ptp_ring_init(self);
		if (err < 0)
			goto err_exit;
	}

	netif_carrier_off(self->ndev);

err_exit:
	return err;
}

int aq_nic_start(struct aq_nic_s *self)
{
	const struct aq_hw_ops *hw_ops;
	struct aq_vec_s *aq_vec = NULL;
	struct aq_nic_cfg_s *cfg;
	unsigned int i = 0U;
	int err = 0;

	hw_ops = self->aq_hw_ops;
	cfg = aq_nic_get_cfg(self);

	err = hw_ops->hw_multicast_list_set(self->aq_hw, self->mc_list.ar,
					    self->mc_list.count);
	if (err < 0)
		goto err_exit;

	err = hw_ops->hw_packet_filter_set(self->aq_hw, self->packet_filter);
	if (err < 0)
		goto err_exit;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i]) {
		err = aq_vec_start(aq_vec);
		if (err < 0)
			goto err_exit;
	}

	if (AQ_CFG_UDP_RSS_DISABLE) {
		/* HW bug workaround:
		 * Disable RSS for UDP using rx flow filter.
		 * HW does not track RSS stream for fragmented UDP,
		 * 0x5040 control reg does not work.
		 */
		self->udp_filter.location =
				aq_nic_reserve_filter(self, aq_rx_filter_l3l4);
		self->udp_filter.cmd = HW_ATL_RX_ENABLE_FLTR_L3L4 |
				HW_ATL_RX_ENABLE_CMP_PROT_L4 |
				HW_ATL_RX_ENABLE_QUEUE_L3L4 |
				HW_ATL_RX_HOST << HW_ATL_RX_ACTION_FL3F4_SHIFT |
				HW_ATL_RX_UDP;
		if (hw_ops->hw_filter_l3l4_set) {
			err = hw_ops->hw_filter_l3l4_set(self->aq_hw,
							 &self->udp_filter);
		}
	}

	err = aq_apply_all_rule(self);
	if (err < 0)
		goto err_exit;

	err = aq_filters_vlans_update(self);
	if (err < 0)
		goto err_exit;

	err = aq_ptp_ring_start(self);
	if (err < 0)
		goto err_exit;

	aq_nic_set_loopback(self);

	err = hw_ops->hw_start(self->aq_hw);
	if (err < 0)
		goto err_exit;

	err = aq_nic_update_interrupt_moderation_settings(self);
	if (err)
		goto err_exit;

	INIT_WORK(&self->service_task, aq_nic_service_task);

	aq_nic_set_downshift(self, self->aq_nic_cfg.downshift_counter);

	timer_setup(&self->service_timer, aq_nic_service_timer_cb, 0);
	aq_nic_service_timer_cb(&self->service_timer);

	if (cfg->is_polling) {
		timer_setup(&self->polling_timer, aq_nic_polling_timer_cb, 0);
		mod_timer(&self->polling_timer, jiffies +
			  AQ_CFG_POLLING_TIMER_INTERVAL);
	} else {
		for (i = 0U, aq_vec = self->aq_vec[0];
			self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i]) {
			err = aq_pci_func_alloc_irq(self, i, self->ndev->name,
						    aq_vec_isr, aq_vec,
						    aq_vec_get_affinity_mask(aq_vec));
			if (err < 0)
				goto err_exit;
		}

		err = aq_ptp_irq_alloc(self);
		if (err < 0)
			goto err_exit;

		if (cfg->link_irq_vec) {
			int irqvec = pci_irq_vector(self->pdev,
						    cfg->link_irq_vec);
			err = request_threaded_irq(irqvec, NULL,
						   aq_linkstate_threaded_isr,
						   IRQF_SHARED | IRQF_ONESHOT,
						   self->ndev->name, self);
			if (err < 0)
				goto err_exit;
			self->msix_entry_mask |= (1 << cfg->link_irq_vec);
		}

		err = hw_ops->hw_irq_enable(self->aq_hw, AQ_CFG_IRQ_MASK);
		if (err < 0)
			goto err_exit;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
	err = netif_set_real_num_tx_queues(self->ndev,
					   self->aq_vecs * cfg->tcs);
	if (err < 0)
		goto err_exit;

	err = netif_set_real_num_rx_queues(self->ndev,
					   self->aq_vecs * cfg->tcs);
	if (err < 0)
		goto err_exit;
#endif

	for (i = 0; i < cfg->tcs; i++) {
		u16 offset = self->aq_vecs * i;

		netdev_set_tc_queue(self->ndev, i, self->aq_vecs, offset);
	}
	netif_tx_start_all_queues(self->ndev);

err_exit:
	return err;
}

unsigned int aq_nic_map_skb(struct aq_nic_s *self, struct sk_buff *skb,
			    struct aq_ring_s *ring)
{
	unsigned int nr_frags = skb_shinfo(skb)->nr_frags;
	struct aq_nic_cfg_s *cfg = aq_nic_get_cfg(self);
	struct device *dev = aq_nic_get_dev(self);
	struct aq_ring_buff_s *first = NULL;
	u8 ipver = ip_hdr(skb)->version;
	struct aq_ring_buff_s *dx_buff;
	bool need_context_tag = false;
	unsigned int frag_count = 0U;
	unsigned int ret = 0U;
	unsigned int dx;
	u8 l4proto = 0;

	trace_aq_dump_skb(skb);

	if (ipver == 4)
		l4proto = ip_hdr(skb)->protocol;
	else if (ipver == 6)
		l4proto = ipv6_hdr(skb)->nexthdr;

	dx = ring->sw_tail;
	dx_buff = &ring->buff_ring[dx];
	dx_buff->flags = 0U;

	if (unlikely(skb_is_gso(skb))) {
		dx_buff->mss = skb_shinfo(skb)->gso_size;
		if (l4proto == IPPROTO_TCP) {
			dx_buff->is_gso_tcp = 1U;
			dx_buff->len_l4 = tcp_hdrlen(skb);
		} else if (l4proto == IPPROTO_UDP) {
			dx_buff->is_gso_udp = 1U;
			dx_buff->len_l4 = sizeof(struct udphdr);
			/* UDP GSO Hardware does not replace packet length. */
			udp_hdr(skb)->len = htons(dx_buff->mss +
						  dx_buff->len_l4);
		} else {
			WARN_ONCE(true, "Bad GSO mode");
			goto exit;
		}
		dx_buff->len_pkt = skb->len;
		dx_buff->len_l2 = ETH_HLEN;
		dx_buff->len_l3 = skb_network_header_len(skb);
		dx_buff->eop_index = 0xffffU;
		dx_buff->is_ipv6 = (ipver == 6);
		need_context_tag = true;
	}

	if (cfg->is_vlan_tx_insert && skb_vlan_tag_present(skb)) {
		dx_buff->vlan_tx_tag = skb_vlan_tag_get(skb);
		dx_buff->len_pkt = skb->len;
		dx_buff->is_vlan = 1U;
		need_context_tag = true;
	}

	if (need_context_tag) {
		dx = aq_ring_next_dx(ring, dx);
		dx_buff = &ring->buff_ring[dx];
		dx_buff->flags = 0U;
		++ret;
	}

	dx_buff->len = skb_headlen(skb);
	dx_buff->pa = dma_map_single(dev,
				     skb->data,
				     dx_buff->len,
				     DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(dev, dx_buff->pa))) {
		ret = 0;
		goto exit;
	}

	first = dx_buff;
	dx_buff->len_pkt = skb->len;
	dx_buff->is_sop = 1U;
	dx_buff->is_mapped = 1U;
	++ret;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		dx_buff->is_ip_cso = (htons(ETH_P_IP) == skb->protocol);
		dx_buff->is_tcp_cso = (l4proto == IPPROTO_TCP);
		dx_buff->is_udp_cso = (l4proto == IPPROTO_UDP);
	}

	for (; nr_frags--; ++frag_count) {
		unsigned int frag_len = 0U;
		unsigned int buff_offset = 0U;
		unsigned int buff_size = 0U;
		dma_addr_t frag_pa;
		skb_frag_t *frag = &skb_shinfo(skb)->frags[frag_count];

		frag_len = skb_frag_size(frag);

		while (frag_len) {
			if (frag_len > AQ_CFG_TX_FRAME_MAX)
				buff_size = AQ_CFG_TX_FRAME_MAX;
			else
				buff_size = frag_len;

			frag_pa = skb_frag_dma_map(dev,
						   frag,
						   buff_offset,
						   buff_size,
						   DMA_TO_DEVICE);

			if (unlikely(dma_mapping_error(dev,
						       frag_pa)))
				goto mapping_error;

			dx = aq_ring_next_dx(ring, dx);
			dx_buff = &ring->buff_ring[dx];

			dx_buff->flags = 0U;
			dx_buff->len = buff_size;
			dx_buff->pa = frag_pa;
			dx_buff->is_mapped = 1U;
			dx_buff->eop_index = 0xffffU;

			frag_len -= buff_size;
			buff_offset += buff_size;

			++ret;
		}
	}

	first->eop_index = dx;
	dx_buff->is_eop = 1U;
	if (skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS &&
	    self->aq_hw_ops->enable_ptp) {
		dx_buff->request_ts = 1U;
		dx_buff->clk_sel = self->aq_hw_ops->hw_get_clk_sel(self->aq_hw);
	}
	dx_buff->skb = skb;
	goto exit;

mapping_error:
	for (dx = ring->sw_tail;
	     ret > 0;
	     --ret, dx = aq_ring_next_dx(ring, dx)) {
		dx_buff = &ring->buff_ring[dx];

		if (!(dx_buff->is_gso_tcp || dx_buff->is_gso_udp) &&
		    !dx_buff->is_vlan && dx_buff->pa) {
			if (unlikely(dx_buff->is_sop)) {
				dma_unmap_single(dev,
						 dx_buff->pa,
						 dx_buff->len,
						 DMA_TO_DEVICE);
			} else {
				dma_unmap_page(dev,
					       dx_buff->pa,
					       dx_buff->len,
					       DMA_TO_DEVICE);
			}
		}
	}

exit:
	return ret;
}

int aq_nic_xmit(struct aq_nic_s *self, struct sk_buff *skb)
{
	struct aq_nic_cfg_s *cfg = aq_nic_get_cfg(self);
	unsigned int vec = skb->queue_mapping % cfg->vecs;
	unsigned int tc = skb->queue_mapping / cfg->vecs;
	struct aq_ring_s *ring = NULL;
	unsigned int frags = 0U;
	int err = NETDEV_TX_OK;

	frags = skb_shinfo(skb)->nr_frags + 1;

	ring = self->aq_ring_tx[AQ_NIC_CFG_TCVEC2RING(cfg, tc, vec)];

	if (frags > AQ_CFG_SKB_FRAGS_MAX) {
		dev_kfree_skb_any(skb);
		goto err_exit;
	}

	aq_ring_update_queue_state(ring);

	if (cfg->priv_flags & BIT(AQ_HW_LOOPBACK_DMA_NET)) {
		err = NETDEV_TX_BUSY;
		goto err_exit;
	}

	/* Above status update may stop the queue. Check this. */
	if (__netif_subqueue_stopped(self->ndev,
				     AQ_NIC_RING2QMAP(self, ring->idx))) {
		err = NETDEV_TX_BUSY;
		goto err_exit;
	}

	frags = aq_nic_map_skb(self, skb, ring);

	if (likely(frags)) {
		err = self->aq_hw_ops->hw_ring_tx_xmit(self->aq_hw,
						       ring, frags);
	} else {
		err = NETDEV_TX_BUSY;
	}

err_exit:
	return err;
}

int aq_nic_update_interrupt_moderation_settings(struct aq_nic_s *self)
{
	return self->aq_hw_ops->hw_interrupt_moderation_set(self->aq_hw);
}

int aq_nic_set_packet_filter(struct aq_nic_s *self, unsigned int flags)
{
	int err = 0;

	if (pm_runtime_active(&self->pdev->dev)) {
		err = self->aq_hw_ops->hw_packet_filter_set(self->aq_hw, flags);
		if (err < 0)
			goto err_exit;
	}

	self->packet_filter = flags;

err_exit:
	return err;
}

int aq_nic_set_multicast_list(struct aq_nic_s *self, struct net_device *ndev)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#define netdev_hw_addr dev_addr_list
#define addr da_addr
#endif
	const struct aq_hw_ops *hw_ops = self->aq_hw_ops;
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	unsigned int packet_filter = ndev->flags;
	struct netdev_hw_addr *ha = NULL;
	unsigned int i = 0U;
	int err = 0;

	self->mc_list.count = 0;
	if (netdev_uc_count(ndev) > self->aq_hw->mac_filter_max) {
		packet_filter |= IFF_PROMISC;
	} else {
		netdev_for_each_uc_addr(ha, ndev) {
			ether_addr_copy(self->mc_list.ar[i++], ha->addr);
		}
	}

	cfg->is_mc_list_enabled = !!(packet_filter & IFF_MULTICAST);
	if (cfg->is_mc_list_enabled) {
		if (i + netdev_mc_count(ndev) > self->aq_hw->mac_filter_max) {
			packet_filter |= IFF_ALLMULTI;
		} else {
			netdev_for_each_mc_addr(ha, ndev) {
				ether_addr_copy(self->mc_list.ar[i++],
						ha->addr);
			}
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#undef netdev_hw_addr
#undef addr
#endif
	if (i > 0 && i <= self->aq_hw->mac_filter_max) {
		self->mc_list.count = i;
		if (pm_runtime_active(&self->pdev->dev)) {
			err = hw_ops->hw_multicast_list_set(self->aq_hw,
							self->mc_list.ar,
							self->mc_list.count);
			if (err < 0)
				return err;
		}
	}

	return aq_nic_set_packet_filter(self, packet_filter);
}

int aq_nic_set_mtu(struct aq_nic_s *self, int new_mtu)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	if (new_mtu < ETH_MIN_MTU+ETH_HLEN ||
	    new_mtu > self->aq_nic_cfg.aq_hw_caps->mtu - ETH_FCS_LEN)
		return -EINVAL;
#endif
	self->aq_nic_cfg.mtu = new_mtu;

	return 0;
}

int aq_nic_set_mac(struct aq_nic_s *self, struct net_device *ndev)
{
	return self->aq_hw_ops->hw_set_mac_address(self->aq_hw, ndev->dev_addr);
}

unsigned int aq_nic_get_link_speed(struct aq_nic_s *self)
{
	return self->link_status.mbps;
}

int aq_nic_get_regs(struct aq_nic_s *self, struct ethtool_regs *regs, void *p)
{
	u32 *regs_buff = p;
	int err = 0;

	if (unlikely(!self->aq_hw_ops->hw_get_regs))
		return -EOPNOTSUPP;

	regs->version = 1;

	err = self->aq_hw_ops->hw_get_regs(self->aq_hw,
					   self->aq_nic_cfg.aq_hw_caps,
					   regs_buff);
	if (err < 0)
		goto err_exit;

err_exit:
	return err;
}

int aq_nic_get_regs_count(struct aq_nic_s *self)
{
	if (unlikely(!self->aq_hw_ops->hw_get_regs))
		return 0;

	return self->aq_nic_cfg.aq_hw_caps->mac_regs_count;
}

u64 *aq_nic_get_stats(struct aq_nic_s *self, u64 *data)
{
	struct aq_vec_s *aq_vec = NULL;
	struct aq_stats_s *stats;
	unsigned int count = 0U;
	unsigned int i = 0U;
	unsigned int tc;

	if (self->aq_fw_ops->update_stats) {
		mutex_lock(&self->fwreq_mutex);
		self->aq_fw_ops->update_stats(self->aq_hw);
		mutex_unlock(&self->fwreq_mutex);
	}
	stats = self->aq_hw_ops->hw_get_hw_stats(self->aq_hw);

	if (!stats)
		goto err_exit;

	data[i] = stats->uprc + stats->mprc + stats->bprc;
	data[++i] = stats->uprc;
	data[++i] = stats->mprc;
	data[++i] = stats->bprc;
	data[++i] = stats->erpr;
	data[++i] = stats->uptc + stats->mptc + stats->bptc;
	data[++i] = stats->uptc;
	data[++i] = stats->mptc;
	data[++i] = stats->bptc;
	data[++i] = stats->ubrc;
	data[++i] = stats->ubtc;
	data[++i] = stats->mbrc;
	data[++i] = stats->mbtc;
	data[++i] = stats->bbrc;
	data[++i] = stats->bbtc;
	if (stats->brc)
		data[++i] = stats->brc;
	else
		data[++i] = stats->ubrc + stats->mbrc + stats->bbrc;
	if (stats->btc)
		data[++i] = stats->btc;
	else
		data[++i] = stats->ubtc + stats->mbtc + stats->bbtc;
	data[++i] = stats->dma_pkt_rc;
	data[++i] = stats->dma_pkt_tc;
	data[++i] = stats->dma_oct_rc;
	data[++i] = stats->dma_oct_tc;
	data[++i] = stats->dpc;

	i++;

	data += i;

	for (tc = 0U; tc < self->aq_nic_cfg.tcs; tc++) {
		for (i = 0U, aq_vec = self->aq_vec[0];
		     aq_vec && self->aq_vecs > i;
		     ++i, aq_vec = self->aq_vec[i]) {
			data += count;
			count = aq_vec_get_sw_stats(aq_vec, tc, data);
		}
	}

	data += count;

err_exit:
	return data;
}

static void aq_nic_update_ndev_stats(struct aq_nic_s *self)
{
	struct net_device *ndev = self->ndev;
	struct aq_stats_s *stats;

	stats = self->aq_hw_ops->hw_get_hw_stats(self->aq_hw);

	ndev->stats.rx_packets = stats->dma_pkt_rc;
	ndev->stats.rx_bytes = stats->dma_oct_rc;
	ndev->stats.rx_errors = stats->erpr;
	ndev->stats.rx_dropped = stats->dpc;
	ndev->stats.tx_packets = stats->dma_pkt_tc;
	ndev->stats.tx_bytes = stats->dma_oct_tc;
	ndev->stats.tx_errors = stats->erpt;
	ndev->stats.multicast = stats->mprc;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
void aq_nic_get_link_ksettings(struct aq_nic_s *self,
			       struct ethtool_link_ksettings *cmd)
{
	u32 lp_link_speed_msk;

	if (self->aq_nic_cfg.aq_hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		cmd->base.port = PORT_FIBRE;
	else
		cmd->base.port = PORT_TP;

	cmd->base.duplex = DUPLEX_UNKNOWN;
	if (self->link_status.mbps)
		cmd->base.duplex = self->link_status.full_duplex ?
				   DUPLEX_FULL : DUPLEX_HALF;
	cmd->base.autoneg = self->aq_nic_cfg.is_autoneg;

	ethtool_link_ksettings_zero_link_mode(cmd, supported);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_10G)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     10000baseT_Full);

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0)
	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_5G)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     5000baseT_Full);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_2G5)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     2500baseT_Full);
#endif
	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_1G)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     1000baseT_Full);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_1G_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     1000baseT_Half);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_100M)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     100baseT_Full);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_100M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     100baseT_Half);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_10M)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     10baseT_Full);

	if (self->aq_nic_cfg.aq_hw_caps->link_speed_msk & AQ_NIC_RATE_10M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     10baseT_Half);

	if (self->aq_nic_cfg.aq_hw_caps->flow_control) {
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     Pause);
		ethtool_link_ksettings_add_link_mode(cmd, supported,
						     Asym_Pause);
	}

	ethtool_link_ksettings_add_link_mode(cmd, supported, Autoneg);

	if (self->aq_nic_cfg.aq_hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		ethtool_link_ksettings_add_link_mode(cmd, supported, FIBRE);
	else
		ethtool_link_ksettings_add_link_mode(cmd, supported, TP);

	ethtool_link_ksettings_zero_link_mode(cmd, advertising);

	if (self->aq_nic_cfg.is_autoneg)
		ethtool_link_ksettings_add_link_mode(cmd, advertising, Autoneg);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10G)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     10000baseT_Full);
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0)
	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_5G)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     5000baseT_Full);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_2G5)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     2500baseT_Full);
#endif
	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_1G)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     1000baseT_Full);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_1G_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     1000baseT_Half);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_100M)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     100baseT_Full);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_100M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     100baseT_Half);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10M)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     10baseT_Full);

	if (self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     10baseT_Half);

	if (self->aq_nic_cfg.fc.cur & AQ_NIC_FC_RX)
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     Pause);

	/* Asym is when either RX or TX, but not both */
	if (!!(self->aq_nic_cfg.fc.cur & AQ_NIC_FC_TX) ^
	    !!(self->aq_nic_cfg.fc.cur & AQ_NIC_FC_RX))
		ethtool_link_ksettings_add_link_mode(cmd, advertising,
						     Asym_Pause);

	if (self->aq_nic_cfg.aq_hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		ethtool_link_ksettings_add_link_mode(cmd, advertising, FIBRE);
	else
		ethtool_link_ksettings_add_link_mode(cmd, advertising, TP);

	ethtool_link_ksettings_zero_link_mode(cmd, lp_advertising);
	lp_link_speed_msk = self->aq_hw->aq_link_status.lp_link_speed_msk;

	if (lp_link_speed_msk & AQ_NIC_RATE_10G)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     10000baseT_Full);
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0)
	if (lp_link_speed_msk & AQ_NIC_RATE_5G)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     5000baseT_Full);

	if (lp_link_speed_msk & AQ_NIC_RATE_2G5)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     2500baseT_Full);
#endif
	if (lp_link_speed_msk & AQ_NIC_RATE_1G)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     1000baseT_Full);

	if (lp_link_speed_msk & AQ_NIC_RATE_1G_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     1000baseT_Half);

	if (lp_link_speed_msk & AQ_NIC_RATE_100M)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     100baseT_Full);

	if (lp_link_speed_msk & AQ_NIC_RATE_100M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     100baseT_Half);

	if (lp_link_speed_msk & AQ_NIC_RATE_10M)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     10baseT_Full);

	if (lp_link_speed_msk & AQ_NIC_RATE_10M_HALF)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     10baseT_Half);

	if (self->aq_hw->aq_link_status.lp_flow_control & AQ_NIC_FC_RX)
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     Pause);
	if (!!(self->aq_hw->aq_link_status.lp_flow_control & AQ_NIC_FC_TX) ^
	    !!(self->aq_hw->aq_link_status.lp_flow_control & AQ_NIC_FC_RX))
		ethtool_link_ksettings_add_link_mode(cmd, lp_advertising,
						     Asym_Pause);
}

int aq_nic_set_link_ksettings(struct aq_nic_s *self,
			      const struct ethtool_link_ksettings *cmd)
{
	int fduplex = (cmd->base.duplex == DUPLEX_FULL);
	u32 speed = cmd->base.speed;
	u32 rate = 0U;
	int err = 0;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "fduplex = %d autoneg = %d speed = %d\n",
			fduplex, speed, cmd->base.autoneg);
	if (!fduplex && speed > SPEED_1000) {
		err = -EINVAL;
		goto err_exit;
	}

	if (cmd->base.autoneg == AUTONEG_ENABLE) {
		rate = self->aq_nic_cfg.aq_hw_caps->link_speed_msk;
		self->aq_nic_cfg.is_autoneg = true;
	} else {
		switch (speed) {
		case SPEED_10:
			rate = fduplex ? AQ_NIC_RATE_10M : AQ_NIC_RATE_10M_HALF;
			break;

		case SPEED_100:
			rate = fduplex ? AQ_NIC_RATE_100M
				       : AQ_NIC_RATE_100M_HALF;
			break;

		case SPEED_1000:
			rate = fduplex ? AQ_NIC_RATE_1G : AQ_NIC_RATE_1G_HALF;
			break;

		case SPEED_2500:
			rate = AQ_NIC_RATE_2G5;
			break;

		case SPEED_5000:
			rate = AQ_NIC_RATE_5G;
			break;

		case SPEED_10000:
			rate = AQ_NIC_RATE_10G;
			break;

		default:
			err = -1;
			goto err_exit;
		break;
		}
		if (!(self->aq_nic_cfg.aq_hw_caps->link_speed_msk & rate)) {
			err = -1;
			goto err_exit;
		}

		self->aq_nic_cfg.is_autoneg = false;
	}

	mutex_lock(&self->fwreq_mutex);
	err = self->aq_fw_ops->set_link_speed(self->aq_hw, rate);
	mutex_unlock(&self->fwreq_mutex);
	if (err < 0)
		goto err_exit;

	self->aq_nic_cfg.link_speed_msk = rate;

err_exit:
	return err;
}
#else
void aq_nic_get_link_settings(struct aq_nic_s *self, struct ethtool_cmd *cmd)
{
	const struct aq_hw_caps_s *hw_caps = self->aq_nic_cfg.aq_hw_caps;

	if (hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		cmd->port = PORT_FIBRE;
	else
		cmd->port = PORT_TP;
	cmd->transceiver = XCVR_EXTERNAL;

	cmd->duplex = DUPLEX_UNKNOWN;
	if (self->link_status.mbps)
		cmd->duplex = self->link_status.full_duplex ?
			      DUPLEX_FULL : DUPLEX_HALF;

	cmd->autoneg = self->aq_nic_cfg.is_autoneg;

	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_10G) ?
				ADVERTISED_10000baseT_Full : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_1G) ?
				ADVERTISED_1000baseT_Full : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_100M) ?
				ADVERTISED_100baseT_Full : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_10M) ?
				ADVERTISED_10baseT_Full : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_1G_HALF) ?
				ADVERTISED_1000baseT_Half : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_100M_HALF) ?
				ADVERTISED_100baseT_Half : 0U;
	cmd->supported |= (hw_caps->link_speed_msk & AQ_NIC_RATE_10M_HALF) ?
				ADVERTISED_10baseT_Half : 0U;
	cmd->supported |= hw_caps->flow_control ? SUPPORTED_Pause : 0;
	cmd->supported |= SUPPORTED_Autoneg;

	if (hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		cmd->supported |= SUPPORTED_FIBRE;
	else
		cmd->supported |= SUPPORTED_TP;

	cmd->advertising = (self->aq_nic_cfg.is_autoneg) ?
							ADVERTISED_Autoneg : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10G) ?
			ADVERTISED_10000baseT_Full : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_1G) ?
			ADVERTISED_1000baseT_Full : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_100M) ?
			ADVERTISED_100baseT_Full : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10M) ?
			ADVERTISED_10baseT_Full : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_1G_HALF) ?
			ADVERTISED_1000baseT_Half : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_100M_HALF) ?
			ADVERTISED_100baseT_Half : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10M_HALF) ?
			ADVERTISED_10baseT_Half : 0U;
	cmd->advertising |= (self->aq_nic_cfg.fc.cur) ?
				ADVERTISED_Pause : 0U;

	if (hw_caps->media_type == AQ_HW_MEDIA_TYPE_FIBRE)
		cmd->advertising |= ADVERTISED_FIBRE;
	else
		cmd->advertising |= ADVERTISED_TP;
}

int aq_nic_set_link_settings(struct aq_nic_s *self, struct ethtool_cmd *cmd)
{
	u32 speed = 0U;
	u32 rate = 0U;
	int err = 0;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "autoneg = %d speed = %d\n",
			cmd->autoneg, ethtool_cmd_speed(cmd));
	if (cmd->autoneg == AUTONEG_ENABLE) {
		rate = self->aq_nic_cfg.aq_hw_caps->link_speed_msk;
		self->aq_nic_cfg.is_autoneg = true;
	} else {
		speed = ethtool_cmd_speed(cmd);

		switch (speed) {
		case SPEED_10:
			rate = AQ_NIC_RATE_10M;
			break;

		case SPEED_100:
			rate = AQ_NIC_RATE_100M;
			break;

		case SPEED_1000:
			rate = AQ_NIC_RATE_1G;
			break;

		case SPEED_2500:
			rate = AQ_NIC_RATE_2G5;
			break;

		case SPEED_5000:
			rate = AQ_NIC_RATE_5G;
			break;

		case SPEED_10000:
			rate = AQ_NIC_RATE_10G;
			break;

		default:
			err = -1;
			goto err_exit;
		break;
		}

		if (!(self->aq_nic_cfg.aq_hw_caps->link_speed_msk & rate)) {
			err = -1;
			goto err_exit;
		}

		self->aq_nic_cfg.is_autoneg = false;
	}

	mutex_lock(&self->fwreq_mutex);
	err = self->aq_fw_ops->set_link_speed(self->aq_hw, rate);
	mutex_unlock(&self->fwreq_mutex);
	if (err < 0)
		goto err_exit;

	self->aq_nic_cfg.link_speed_msk = rate;

err_exit:
	return err;
}

#endif

struct aq_nic_cfg_s *aq_nic_get_cfg(struct aq_nic_s *self)
{
	return &self->aq_nic_cfg;
}

u32 aq_nic_get_fw_version(struct aq_nic_s *self)
{
	return self->aq_hw_ops->hw_get_fw_version(self->aq_hw);
}

int aq_nic_set_loopback(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	if (!self->aq_hw_ops->hw_set_loopback ||
	    !self->aq_fw_ops->set_phyloopback)
		return -EOPNOTSUPP;

	self->aq_hw_ops->hw_set_loopback(self->aq_hw,
					 AQ_HW_LOOPBACK_DMA_SYS,
					 !!(cfg->priv_flags &
					    BIT(AQ_HW_LOOPBACK_DMA_SYS)));

	self->aq_hw_ops->hw_set_loopback(self->aq_hw,
					 AQ_HW_LOOPBACK_PKT_SYS,
					 !!(cfg->priv_flags &
					    BIT(AQ_HW_LOOPBACK_PKT_SYS)));

	self->aq_hw_ops->hw_set_loopback(self->aq_hw,
					 AQ_HW_LOOPBACK_DMA_NET,
					 !!(cfg->priv_flags &
					    BIT(AQ_HW_LOOPBACK_DMA_NET)));

	mutex_lock(&self->fwreq_mutex);
	self->aq_fw_ops->set_phyloopback(self->aq_hw,
					 AQ_HW_LOOPBACK_PHYINT_SYS,
					 !!(cfg->priv_flags &
					    BIT(AQ_HW_LOOPBACK_PHYINT_SYS)));

	self->aq_fw_ops->set_phyloopback(self->aq_hw,
					 AQ_HW_LOOPBACK_PHYEXT_SYS,
					 !!(cfg->priv_flags &
					    BIT(AQ_HW_LOOPBACK_PHYEXT_SYS)));
	mutex_unlock(&self->fwreq_mutex);

	return 0;
}

int aq_nic_stop(struct aq_nic_s *self)
{
	bool was_up = netif_carrier_ok(self->ndev);
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;
	int res;

	netif_tx_disable(self->ndev);
	netif_carrier_off(self->ndev);

	del_timer_sync(&self->service_timer);
	cancel_work_sync(&self->service_task);

	self->aq_hw_ops->hw_irq_disable(self->aq_hw, AQ_CFG_IRQ_MASK);

	if (self->aq_nic_cfg.is_polling)
		del_timer_sync(&self->polling_timer);
	else
		aq_pci_func_free_irqs(self);

	aq_ptp_irq_free(self);

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_stop(aq_vec);

	aq_ptp_ring_stop(self);

	if (AQ_CFG_UDP_RSS_DISABLE)
		aq_nic_release_filter(self, aq_rx_filter_l3l4,
				      self->udp_filter.location);

	res = self->aq_hw_ops->hw_stop(self->aq_hw);

	if (was_up)
		pm_runtime_put(&self->pdev->dev);

	return res;
}

void aq_nic_set_power(struct aq_nic_s *self, u32 wol)
{
	if (self->power_state != AQ_HW_POWER_STATE_D0 || wol)
		if (likely(self->aq_fw_ops->set_power)) {
			mutex_lock(&self->fwreq_mutex);
			self->aq_fw_ops->set_power(self->aq_hw,
						   self->power_state,
						   self->ndev->dev_addr,
						   wol);
			mutex_unlock(&self->fwreq_mutex);
		}
}

void aq_nic_deinit(struct aq_nic_s *self, bool link_down)
{
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;

	if (!self)
		goto err_exit;

	for (i = 0U; i < self->aq_vecs; i++) {
		aq_vec = self->aq_vec[i];
		aq_vec_deinit(aq_vec);
		aq_vec_ring_free(aq_vec);
	}

	aq_ptp_unregister(self);
	aq_ptp_ring_deinit(self);
	aq_ptp_ring_free(self);
	aq_ptp_free(self);

	if (likely(self->aq_fw_ops->deinit) && link_down) {
		mutex_lock(&self->fwreq_mutex);
		self->aq_fw_ops->deinit(self->aq_hw);
		mutex_unlock(&self->fwreq_mutex);
	}

err_exit:;
}

void aq_nic_free_vectors(struct aq_nic_s *self)
{
	unsigned int i = 0U;

	if (!self)
		goto err_exit;

	for (i = ARRAY_SIZE(self->aq_vec); i--;) {
		if (self->aq_vec[i]) {
			aq_vec_free(self->aq_vec[i]);
			self->aq_vec[i] = NULL;
		}
	}

err_exit:;
}

int aq_nic_realloc_vectors(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = aq_nic_get_cfg(self);

	aq_nic_free_vectors(self);

	for (self->aq_vecs = 0; self->aq_vecs < cfg->vecs; self->aq_vecs++) {
		self->aq_vec[self->aq_vecs] = aq_vec_alloc(self, self->aq_vecs,
							   cfg);
		if (unlikely(!self->aq_vec[self->aq_vecs]))
			return -ENOMEM;
	}

	return 0;
}

void aq_nic_shutdown(struct aq_nic_s *self)
{
	int err = 0;

	if (!self->ndev)
		return;

	rtnl_lock();

	netif_device_detach(self->ndev);

	if (netif_running(self->ndev)) {
		err = aq_nic_stop(self);
		if (err < 0)
			goto err_exit;
	}
	aq_nic_deinit(self, !self->aq_hw->aq_nic_cfg->wol);
	aq_nic_set_power(self, self->aq_hw->aq_nic_cfg->wol);

err_exit:
	rtnl_unlock();
}

void aq_nic_parse_parameters(struct aq_nic_s *self, unsigned int nic_id)
{
	struct aq_nic_cfg_s *cfg = aq_nic_get_cfg(self);

	if (nic_id >= AQ_NIC_MAX)
		goto err_exit;

	cfg->fw_did = aq_fw_did[nic_id];
	cfg->fw_sid = aq_fw_sid[nic_id];
	cfg->force_host_boot = !!aq_force_host_boot[nic_id];

err_exit:
	return;
}

static void aq_nic_detect_fw_image_for_legacy(struct aq_nic_s *self,
					      const char **fw_image_name)
{
	struct aq_hw_chip_info chip_info = {};

	self->aq_hw_ops->hw_get_chip_info(self->aq_hw, &chip_info);
	if (chip_info.chip_id == AQ_CHIP_AQC107X ||
	    chip_info.chip_id == AQ_CHIP_AQC108X ||
	    chip_info.chip_id == AQ_CHIP_AQC109X) {
		*fw_image_name = AQ_FW_AQC10XX;
	} else if (chip_info.chip_id == AQ_CHIP_AQCC111X ||
		   chip_info.chip_id == AQ_CHIP_AQCC112X ||
		   chip_info.chip_id == AQ_CHIP_AQC111EX ||
		   chip_info.chip_id == AQ_CHIP_AQC112EX) {
		*fw_image_name = AQ_FW_AQC11XX;
	} else if (chip_info.chip_id == AQ_CHIP_AQC100X) {
		*fw_image_name = AQ_FW_AQC100X;
	} else {
		/*Host boot is not supported for unknown chip*/
		*fw_image_name = NULL;
	}
}

void aq_nic_request_firmware(struct aq_nic_s *self)
{
	const struct aq_hw_caps_s *hw_caps = self->aq_nic_cfg.aq_hw_caps;
	const char *fw_image_name = hw_caps->fw_image_name;
	struct aq_nic_cfg_s *cfg = aq_nic_get_cfg(self);

	if (cfg->fw_did) {
		switch (cfg->fw_did) {
		case AQ_DEVICE_ID_AQC100S:
			fw_image_name = AQ_FW_AQC100X;
			break;
		case AQ_DEVICE_ID_AQC107S:
			fw_image_name = AQ_FW_AQC10XX;
			break;
		case AQ_DEVICE_ID_AQC111S:
			fw_image_name = AQ_FW_AQC11XX;
			break;
		case AQ_DEVICE_ID_AQC113:
		case AQ_DEVICE_ID_AQC113DEV:
		case AQ_DEVICE_ID_AQC113C:
		case AQ_DEVICE_ID_AQC113CA:
		case AQ_DEVICE_ID_AQC115C:
		case AQ_DEVICE_ID_AQC116C:
		case AQ_DEVICE_ID_AQC113CS:
		case AQ_DEVICE_ID_AQC114CS:
			fw_image_name = AQ_FW_AQC113X;
			break;
		default:
			fw_image_name = NULL;
		}
	} else if (self->pdev->device == AQ_DEVICE_ID_0001) {
		aq_nic_detect_fw_image_for_legacy(self, &fw_image_name);
	}

	if (fw_image_name)
		request_firmware(&aq_nic_get_cfg(self)->fw_image, fw_image_name,
				 &self->pdev->dev);

	self->aq_hw->ssid = ((uint32_t)self->pdev->subsystem_device << 16) |
			    self->pdev->subsystem_vendor;
}

u8 aq_nic_reserve_filter(struct aq_nic_s *self, enum aq_rx_filter_type type)
{
	u8 location = 0xFF;

	switch (type) {
	case aq_rx_filter_ethertype:
		location = self->aq_hw->etype_filter_max - 1 -
			   self->aq_hw_rx_fltrs.fet_reserved_count;
		self->aq_hw_rx_fltrs.fet_reserved_count++;
		break;
	case aq_rx_filter_l3l4:
		location = self->aq_hw->l3l4_filter_max - 1 -
			   self->aq_hw_rx_fltrs.fl3l4.reserved_count;

		self->aq_hw_rx_fltrs.fl3l4.reserved_count++;
		break;
	default:
		break;
	}

	return location;
}

void aq_nic_release_filter(struct aq_nic_s *self, enum aq_rx_filter_type type,
			   u32 location)
{
	switch (type) {
	case aq_rx_filter_ethertype:
		self->aq_hw_rx_fltrs.fet_reserved_count--;
		break;
	case aq_rx_filter_l3l4:
		self->aq_hw_rx_fltrs.fl3l4.reserved_count--;
		break;
	default:
		break;
	}
}

int aq_nic_set_downshift(struct aq_nic_s *self, int val)
{
	int err = 0;
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "Downshift val = %d\n", val);
	if (!self->aq_fw_ops->set_downshift)
		return -EOPNOTSUPP;

	if (val > 15) {
		netdev_err(self->ndev, "downshift counter should be <= 15\n");
		return -EINVAL;
	}
	cfg->downshift_counter = val;

	mutex_lock(&self->fwreq_mutex);
	err = self->aq_fw_ops->set_downshift(self->aq_hw, cfg->downshift_counter);
	mutex_unlock(&self->fwreq_mutex);

	return err;
}

int aq_nic_set_media_detect(struct aq_nic_s *self, int val)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	int err = 0;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "Media detect val = %d\n", val);
	if (!self->aq_fw_ops->set_media_detect)
		return -EOPNOTSUPP;

	if (val > 0 && val != AQ_HW_MEDIA_DETECT_CNT) {
		netdev_err(self->ndev, "EDPD on this device could have only fixed value of %d\n",
			   AQ_HW_MEDIA_DETECT_CNT);
		return -EINVAL;
	}

	mutex_lock(&self->fwreq_mutex);
	err = self->aq_fw_ops->set_media_detect(self->aq_hw, !!val);
	mutex_unlock(&self->fwreq_mutex);

	/* msecs plays no role - configuration is always fixed in PHY */
	if (!err)
		cfg->is_media_detect = !!val;

	return err;
}

int aq_nic_setup_tc_mqprio(struct aq_nic_s *self, u32 tcs, u8 *prio_tc_map)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	const unsigned int prev_vecs = cfg->vecs;
	bool ndev_running;
	int err = 0;
	int i;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "tcs = %d\n", tcs);
	/* if already the same configuration or
	 * disable request (tcs is 0) and we already is disabled
	 */
	if (tcs == cfg->tcs || (tcs == 0 && !cfg->is_qos))
		return 0;

	pm_runtime_get_sync(&self->pdev->dev);

	ndev_running = netif_running(self->ndev);
	if (ndev_running)
		dev_close(self->ndev);

	cfg->tcs = tcs;
	if (cfg->tcs == 0)
		cfg->tcs = 1;
	if (prio_tc_map)
		memcpy(cfg->prio_tc_map, prio_tc_map, sizeof(cfg->prio_tc_map));
	else
		for (i = 0; i < sizeof(cfg->prio_tc_map); i++)
			cfg->prio_tc_map[i] = cfg->tcs * i / 8;

	cfg->is_qos = !!tcs;
	cfg->is_ptp = aq_enable_ptp && (cfg->tcs > AQ_HW_PTP_TC);

	netdev_set_num_tc(self->ndev, cfg->tcs);

	/* Changing the number of TCs might change the number of vectors */
	aq_nic_cfg_update_num_vecs(self);
	if (prev_vecs != cfg->vecs) {
		err = aq_nic_realloc_vectors(self);
		if (err)
			goto err_exit;
	}

	if (ndev_running)
		err = dev_open(self->ndev, NULL);

err_exit:
	pm_runtime_put(&self->pdev->dev);

	return err;
}

int aq_nic_setup_tc_max_rate(struct aq_nic_s *self, const unsigned int tc,
			     const u32 max_rate)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "tc = %d max_rate = %d\n", tc, max_rate);
	if (tc >= AQ_CFG_TCS_MAX)
		return -EINVAL;

	if (max_rate && max_rate < 10) {
		netdev_warn(self->ndev,
			"Setting %s to the minimum usable value of %dMbps.\n",
			"max rate", 10);
		cfg->tc_max_rate[tc] = 10;
	} else {
		cfg->tc_max_rate[tc] = max_rate;
	}

	return 0;
}

int aq_nic_setup_tc_min_rate(struct aq_nic_s *self, const unsigned int tc,
			     const u32 min_rate)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	aq_pr_verbose(self, AQ_MSG_DEBUG, "tc = %d min_rate = %d\n", tc, min_rate);
	if (tc >= AQ_CFG_TCS_MAX)
		return -EINVAL;

	if (min_rate)
		set_bit(tc, &cfg->tc_min_rate_msk);
	else
		clear_bit(tc, &cfg->tc_min_rate_msk);

	if (min_rate && min_rate < 20) {
		netdev_warn(self->ndev,
			"Setting %s to the minimum usable value of %dMbps.\n",
			"min rate", 20);
		cfg->tc_min_rate[tc] = 20;
	} else {
		cfg->tc_min_rate[tc] = min_rate;
	}

	return 0;
}
