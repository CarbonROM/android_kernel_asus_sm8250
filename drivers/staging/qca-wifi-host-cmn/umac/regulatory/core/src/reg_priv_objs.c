/*
 * Copyright (c) 2014-2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: reg_priv_objs.c
 * This file defines the APIs to create regulatory private PSOC and PDEV
 * objects.
 */

#include <wlan_cmn.h>
#include <reg_services_public_struct.h>
#include <wlan_objmgr_psoc_obj.h>
#include <wlan_objmgr_pdev_obj.h>
#include <qdf_lock.h>
#include "reg_priv_objs.h"
#include "reg_utils.h"
#include "reg_services_common.h"
#include "reg_build_chan_list.h"
#include "reg_host_11d.h"

struct wlan_regulatory_psoc_priv_obj *reg_get_psoc_obj(
		struct wlan_objmgr_psoc *psoc)
{
	struct wlan_regulatory_psoc_priv_obj *psoc_priv_obj;

	if (!psoc) {
		reg_alert("psoc is NULL");
		return NULL;
	}
	psoc_priv_obj = wlan_objmgr_psoc_get_comp_private_obj(
			psoc, WLAN_UMAC_COMP_REGULATORY);

	return psoc_priv_obj;
}

struct wlan_regulatory_pdev_priv_obj *reg_get_pdev_obj(
		struct wlan_objmgr_pdev *pdev)
{
	struct wlan_regulatory_pdev_priv_obj *pdev_reg;

	if (!pdev) {
		reg_alert("pdev is NULL");
		return NULL;
	}

	pdev_reg = wlan_objmgr_pdev_get_comp_private_obj(
			pdev, WLAN_UMAC_COMP_REGULATORY);

	return pdev_reg;
}

QDF_STATUS wlan_regulatory_psoc_obj_created_notification(
		struct wlan_objmgr_psoc *psoc, void *arg_list)
{
	struct wlan_regulatory_psoc_priv_obj *soc_reg_obj;
	struct regulatory_channel *mas_chan_list;
	enum channel_enum chan_enum;
	QDF_STATUS status;
	uint8_t i;
	uint8_t pdev_cnt;

	soc_reg_obj = qdf_mem_malloc(sizeof(*soc_reg_obj));
	if (!soc_reg_obj)
		return QDF_STATUS_E_NOMEM;

	soc_reg_obj->offload_enabled = false;
	soc_reg_obj->psoc_ptr = psoc;
	soc_reg_obj->dfs_enabled = true;
	soc_reg_obj->band_capability = BAND_ALL;
	soc_reg_obj->enable_11d_supp = false;
	soc_reg_obj->indoor_chan_enabled = true;
	soc_reg_obj->force_ssc_disable_indoor_channel = false;
	soc_reg_obj->master_vdev_cnt = 0;
	soc_reg_obj->vdev_cnt_11d = 0;
	soc_reg_obj->vdev_id_for_11d_scan = INVALID_VDEV_ID;
	soc_reg_obj->restart_beaconing = CH_AVOID_RULE_RESTART;
	soc_reg_obj->enable_srd_chan_in_master_mode = false;
	soc_reg_obj->enable_11d_in_world_mode = false;
	soc_reg_obj->def_pdev_id = -1;

	for (i = 0; i < MAX_STA_VDEV_CNT; i++)
		soc_reg_obj->vdev_ids_11d[i] = INVALID_VDEV_ID;

	qdf_spinlock_create(&soc_reg_obj->cbk_list_lock);

	for (pdev_cnt = 0; pdev_cnt < PSOC_MAX_PHY_REG_CAP; pdev_cnt++) {
		mas_chan_list =
			soc_reg_obj->mas_chan_params[pdev_cnt].mas_chan_list;

		for (chan_enum = 0; chan_enum < NUM_CHANNELS; chan_enum++) {
			mas_chan_list[chan_enum].chan_flags |=
				REGULATORY_CHAN_DISABLED;
			mas_chan_list[chan_enum].state = CHANNEL_STATE_DISABLE;
			mas_chan_list[chan_enum].nol_chan = false;
		}
	}

	status = wlan_objmgr_psoc_component_obj_attach(
			psoc, WLAN_UMAC_COMP_REGULATORY, soc_reg_obj,
			QDF_STATUS_SUCCESS);
	if (QDF_IS_STATUS_ERROR(status)) {
		qdf_spinlock_destroy(&soc_reg_obj->cbk_list_lock);
		qdf_mem_free(soc_reg_obj);
		reg_err("Obj attach failed");
		return status;
	}

	reg_debug("reg psoc obj created with status %d", status);

	return status;
}

QDF_STATUS wlan_regulatory_psoc_obj_destroyed_notification(
	struct wlan_objmgr_psoc *psoc, void *arg_list)
{
	QDF_STATUS status;
	struct wlan_regulatory_psoc_priv_obj *psoc_priv_obj;

	psoc_priv_obj = reg_get_psoc_obj(psoc);
	if (!psoc_priv_obj) {
		reg_err_rl("NULL reg psoc priv obj");
		return QDF_STATUS_E_FAULT;
	}

	psoc_priv_obj->psoc_ptr = NULL;
	qdf_spinlock_destroy(&psoc_priv_obj->cbk_list_lock);

	status = wlan_objmgr_psoc_component_obj_detach(
			psoc, WLAN_UMAC_COMP_REGULATORY, psoc_priv_obj);

	if (status != QDF_STATUS_SUCCESS)
		reg_err_rl("psoc_priv_obj private obj detach failed");

	reg_debug("reg psoc obj detached");

	qdf_mem_free(psoc_priv_obj);

	return status;
}

QDF_STATUS wlan_regulatory_pdev_obj_created_notification(
	struct wlan_objmgr_pdev *pdev, void *arg_list)
{
	struct wlan_regulatory_pdev_priv_obj *pdev_priv_obj;
	struct wlan_regulatory_psoc_priv_obj *psoc_priv_obj;
	struct wlan_psoc_host_hal_reg_capabilities_ext *reg_cap_ptr;
	struct wlan_objmgr_psoc *parent_psoc;
	uint32_t pdev_id;
	uint32_t cnt;
	uint32_t range_2g_low, range_2g_high;
	uint32_t range_5g_low, range_5g_high;
	QDF_STATUS status;
	struct reg_rule_info *psoc_reg_rules;

	pdev_priv_obj = qdf_mem_malloc(sizeof(*pdev_priv_obj));
	if (!pdev_priv_obj)
		return QDF_STATUS_E_NOMEM;

	parent_psoc = wlan_pdev_get_psoc(pdev);
	pdev_id = wlan_objmgr_pdev_get_pdev_id(pdev);

	psoc_priv_obj = reg_get_psoc_obj(parent_psoc);
	if (!psoc_priv_obj) {
		reg_err("reg psoc private obj is NULL");
		qdf_mem_free(pdev_priv_obj);
		return QDF_STATUS_E_FAULT;
	}

	if (psoc_priv_obj->def_pdev_id == -1)
		psoc_priv_obj->def_pdev_id = pdev_id;
	else
		reg_err("reg cannot handle more than one pdev");

	pdev_priv_obj->pdev_ptr = pdev;
	pdev_priv_obj->dfs_enabled = psoc_priv_obj->dfs_enabled;
	pdev_priv_obj->set_fcc_channel = false;
	pdev_priv_obj->band_capability =  psoc_priv_obj->band_capability;
	pdev_priv_obj->indoor_chan_enabled =
		psoc_priv_obj->indoor_chan_enabled;
	pdev_priv_obj->en_chan_144 = true;

	qdf_spinlock_create(&pdev_priv_obj->reg_rules_lock);

	reg_cap_ptr = psoc_priv_obj->reg_cap;
	pdev_priv_obj->force_ssc_disable_indoor_channel =
		psoc_priv_obj->force_ssc_disable_indoor_channel;

	for (cnt = 0; cnt < PSOC_MAX_PHY_REG_CAP; cnt++) {
		if (!reg_cap_ptr) {
			qdf_mem_free(pdev_priv_obj);
			reg_err("reg cap ptr is NULL");
			return QDF_STATUS_E_FAULT;
		}

		if (reg_cap_ptr->phy_id == pdev_id)
			break;
		reg_cap_ptr++;
	}

	if (cnt == PSOC_MAX_PHY_REG_CAP) {
		qdf_mem_free(pdev_priv_obj);
		reg_err("extended capabilities not found for pdev");
		return QDF_STATUS_E_FAULT;
	}

	range_2g_low = reg_cap_ptr->low_2ghz_chan;
	range_2g_high = reg_cap_ptr->high_2ghz_chan;
	range_5g_low = reg_cap_ptr->low_5ghz_chan;
	range_5g_high = reg_cap_ptr->high_5ghz_chan;

	pdev_priv_obj->range_2g_low = range_2g_low;
	pdev_priv_obj->range_2g_high = range_2g_high;
	pdev_priv_obj->range_5g_low = range_5g_low;
	pdev_priv_obj->range_5g_high = range_5g_high;
	pdev_priv_obj->wireless_modes = reg_cap_ptr->wireless_modes;

	reg_init_pdev_mas_chan_list(pdev_priv_obj,
				    &psoc_priv_obj->mas_chan_params[pdev_id]);

	reg_compute_pdev_current_chan_list(pdev_priv_obj);

	psoc_reg_rules = &psoc_priv_obj->mas_chan_params[pdev_id].reg_rules;
	reg_save_reg_rules_to_pdev(psoc_reg_rules, pdev_priv_obj);

	status = wlan_objmgr_pdev_component_obj_attach(
			pdev, WLAN_UMAC_COMP_REGULATORY, pdev_priv_obj,
			QDF_STATUS_SUCCESS);
	if (QDF_IS_STATUS_ERROR(status)) {
		reg_err("Obj attach failed");
		qdf_mem_free(pdev_priv_obj);
		return status;
	}
	if (!psoc_priv_obj->is_11d_offloaded)
		reg_11d_host_scan_init(parent_psoc);

	reg_debug("reg pdev obj created with status %d", status);

	return status;
}

QDF_STATUS wlan_regulatory_pdev_obj_destroyed_notification(
		struct wlan_objmgr_pdev *pdev, void *arg_list)
{
	QDF_STATUS status;
	struct wlan_regulatory_pdev_priv_obj *pdev_priv_obj;
	struct wlan_regulatory_psoc_priv_obj *psoc_priv_obj;

	pdev_priv_obj = reg_get_pdev_obj(pdev);

	if (!IS_VALID_PDEV_REG_OBJ(pdev_priv_obj)) {
		reg_err("reg pdev private obj is NULL");
		return QDF_STATUS_E_FAILURE;
	}

	psoc_priv_obj = reg_get_psoc_obj(wlan_pdev_get_psoc(pdev));
	if (!IS_VALID_PSOC_REG_OBJ(psoc_priv_obj)) {
		reg_err("reg psoc private obj is NULL");
		return QDF_STATUS_E_FAILURE;
	}

	if (!psoc_priv_obj->is_11d_offloaded)
		reg_11d_host_scan_deinit(wlan_pdev_get_psoc(pdev));

	pdev_priv_obj->pdev_ptr = NULL;

	status = wlan_objmgr_pdev_component_obj_detach(
			pdev, WLAN_UMAC_COMP_REGULATORY, pdev_priv_obj);

	if (status != QDF_STATUS_SUCCESS)
		reg_err("reg pdev private obj detach failed");

	reg_debug("reg pdev obj deleted");

	qdf_spin_lock_bh(&pdev_priv_obj->reg_rules_lock);
	reg_reset_reg_rules(&pdev_priv_obj->reg_rules);
	qdf_spin_unlock_bh(&pdev_priv_obj->reg_rules_lock);

	qdf_spinlock_destroy(&pdev_priv_obj->reg_rules_lock);

	qdf_mem_free(pdev_priv_obj);

	return status;
}
