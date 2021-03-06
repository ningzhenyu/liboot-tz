/*
 * drivers/net/imx_ptp.c
 *
 * Copyright (C) 2010-2013 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Description: IEEE 1588 driver supporting imx5 Fast Ethernet Controller.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/proc_fs.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/time.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <asm/irq.h>
#include <asm/system.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

#include "fec.h"
#include "fec_1588.h"
#include "imx_ptp.h"

#ifdef PTP_DEBUG
#define VDBG(fmt, args...)	printk(KERN_DEBUG "[%s]  " fmt "\n", \
				__func__, ## args)
#else
#define VDBG(fmt, args...)	do {} while (0)
#endif

static u8 *fec_ptp_parse_packet(struct sk_buff *skb, u16 *eth_type);
static struct fec_ptp_private *ptp_private;
static void ptp_rtc_get_current_time(struct ptp *p_ptp,
					struct ptp_time *p_time);
static struct ptp *ptp_dev;

/* Alloc the ring resource */
static int fec_ptp_init_circ(struct fec_ptp_circular *buf, int size)
{
	buf->data_buf = (struct fec_ptp_ts_data *)
		vmalloc(size * sizeof(struct fec_ptp_ts_data));

	if (!buf->data_buf)
		return 1;
	buf->front = 0;
	buf->end = 0;
	buf->size = size;
	return 0;
}

static inline int fec_ptp_calc_index(int size, int curr_index, int offset)
{
	return (curr_index + offset) % size;
}

static int fec_ptp_is_empty(struct fec_ptp_circular *buf)
{
	return (buf->front == buf->end);
}

static int fec_ptp_nelems(struct fec_ptp_circular *buf)
{
	const int front = buf->front;
	const int end = buf->end;
	const int size = buf->size;
	int n_items;

	if (end > front)
		n_items = end - front;
	else if (end < front)
		n_items = size - (front - end);
	else
		n_items = 0;

	return n_items;
}

static int fec_ptp_is_full(struct fec_ptp_circular *buf)
{
	if (fec_ptp_nelems(buf) == (buf->size - 1))
		return 1;
	else
		return 0;
}

static int fec_ptp_insert(struct fec_ptp_circular *ptp_buf,
			  struct fec_ptp_ts_data *data)
{
	struct fec_ptp_ts_data *tmp;

	if (fec_ptp_is_full(ptp_buf))
		ptp_buf->end = fec_ptp_calc_index(ptp_buf->size,
						ptp_buf->end, 1);

	tmp = (ptp_buf->data_buf + ptp_buf->end);
	memcpy(tmp, data, sizeof(struct fec_ptp_ts_data));
	ptp_buf->end = fec_ptp_calc_index(ptp_buf->size, ptp_buf->end, 1);

	return 0;
}

static int fec_ptp_find_and_remove(struct fec_ptp_circular *ptp_buf,
			struct fec_ptp_ident *ident, struct ptp_time *ts)
{
	int i;
	int size = ptp_buf->size, end = ptp_buf->end;
	struct fec_ptp_ident *tmp_ident;

	if (fec_ptp_is_empty(ptp_buf))
		return 1;

	i = ptp_buf->front;
	while (i != end) {
		tmp_ident = &(ptp_buf->data_buf + i)->ident;
		if (tmp_ident->seq_id == ident->seq_id &&
				!memcmp(tmp_ident->spid, ident->spid, 10))
			break;
		i = fec_ptp_calc_index(size, i, 1);
	}

	if (i == end) {
		/* buffer full ? */
		if (fec_ptp_is_full(ptp_buf))
			/* drop one in front */
			ptp_buf->front =
			fec_ptp_calc_index(size, ptp_buf->front, 1);
		return 1;
	}

	*ts = (ptp_buf->data_buf + i)->ts;

	return 0;
}

/* ptp and rtc param configuration */
static void rtc_default_param(struct ptp_rtc *rtc)
{
	struct ptp_rtc_driver_param *drv_param = rtc->driver_param;
	int i;

	rtc->bypass_compensation = DEFAULT_BYPASS_COMPENSATION;
	rtc->output_clock_divisor = DEFAULT_OUTPUT_CLOCK_DIVISOR;

	drv_param->src_clock = DEFAULT_SRC_CLOCK;
	drv_param->src_clock_freq_hz = clk_get_rate(rtc->clk);

	drv_param->invert_input_clk_phase = DEFAULT_INVERT_INPUT_CLK_PHASE;
	drv_param->invert_output_clk_phase = DEFAULT_INVERT_OUTPUT_CLK_PHASE;
	drv_param->pulse_start_mode = DEFAULT_PULSE_START_MODE;
	drv_param->events_mask = DEFAULT_EVENTS_RTC_MASK;

	for (i = 0; i < PTP_RTC_NUM_OF_ALARMS; i++)
		drv_param->alarm_polarity[i] = DEFAULT_ALARM_POLARITY ;

	for (i = 0; i < PTP_RTC_NUM_OF_TRIGGERS; i++)
		drv_param->trigger_polarity[i] = DEFAULT_TRIGGER_POLARITY;
}

static int ptp_rtc_config(struct ptp_rtc *rtc)
{
	/*allocate memory for RTC driver parameter*/
	rtc->driver_param = kzalloc(sizeof(struct ptp_rtc_driver_param),
					GFP_KERNEL);
	if (!rtc->driver_param) {
		printk(KERN_ERR "allocate memory failed\n");
		return -ENOMEM;
	}

	/* expected RTC input clk frequency */
	rtc->driver_param->rtc_freq_hz = PTP_RTC_FREQ * MHZ;

	/*set default RTC configuration parameters*/
	rtc_default_param(rtc);

	return 0;
}

static void ptp_param_config(struct ptp *p_ptp)
{
	struct ptp_driver_param *drv_param;

	p_ptp->driver_param = kzalloc(sizeof(struct ptp_driver_param),
					GFP_KERNEL);
	if (!p_ptp->driver_param) {
		printk(KERN_ERR	"allocate memory failed for "
				"PTP driver parameters\n");
		return;
	}

	drv_param = p_ptp->driver_param;
	/*set the default configuration parameters*/
	drv_param->eth_type_value = ETH_TYPE_VALUE;
	drv_param->vlan_type_value = VLAN_TYPE_VALUE;
	drv_param->udp_general_port = UDP_GENERAL_PORT;
	drv_param->udp_event_port = UDP_EVENT_PORT;
	drv_param->ip_type_value = IP_TYPE_VALUE;
	drv_param->eth_type_offset = ETH_TYPE_OFFSET;
	drv_param->ip_type_offset = IP_TYPE_OFFSET;
	drv_param->udp_dest_port_offset = UDP_DEST_PORT_OFFSET;
	drv_param->ptp_type_offset = PTP_TYPE_OFFSET;

	drv_param->ptp_msg_codes[e_PTP_MSG_SYNC] = DEFAULT_MSG_SYNC;
	drv_param->ptp_msg_codes[e_PTP_MSG_DELAY_REQ] = DEFAULT_MSG_DELAY_REQ;
	drv_param->ptp_msg_codes[e_PTP_MSG_FOLLOW_UP] = DEFAULT_MSG_FOLLOW_UP;
	drv_param->ptp_msg_codes[e_PTP_MSG_DELAY_RESP] = DEFAULT_MSG_DELAY_RESP;
	drv_param->ptp_msg_codes[e_PTP_MSG_MANAGEMENT] = DEFAULT_MSG_MANAGEMENT;
}

/* 64 bits operation */
static u32 div64_oper(u64 dividend, u32 divisor, u32 *quotient)
{
	u32 time_h, time_l;
	u32 result;
	u64 tmp_dividend;
	int i;

	time_h = (u32)(dividend >> 32);
	time_l = (u32)dividend;
	time_h = time_h % divisor;
	for (i = 1; i <= 32; i++) {
		tmp_dividend = (((u64)time_h << 32) | (u64)time_l);
		tmp_dividend = (tmp_dividend << 1);
		time_h = (u32)(tmp_dividend >> 32);
		time_l = (u32)tmp_dividend;
		result = time_h / divisor;
		time_h = time_h % divisor;
		*quotient += (result << (32 - i));
	}

	return time_h;
}

/*64 bites add and return the result*/
static u64 add64_oper(u64 addend, u64 augend)
{
	u64 result = 0;
	u32 addendh, addendl, augendl, augendh;

	addendh = (u32)(addend >> 32);
	addendl = (u32)addend;

	augendh = (u32)(augend>>32);
	augendl = (u32)augend;

	__asm__(
	"adds %0,%2,%3\n"
	"adc %1,%4,%5"
	: "=r" (addendl), "=r" (addendh)
	: "r" (addendl), "r" (augendl), "r" (addendh), "r" (augendh)
	);

	udelay(1);
	result = (((u64)addendh << 32) | (u64)addendl);

	return result;
}

/*64 bits multiplication and return the result*/
static u64 multi64_oper(u32 multiplier, u32 multiplicand)
{
	u64 result = 0;
	u64 tmp_ret = 0;
	u32 tmp_multi = multiplicand;
	int i;

	for (i = 0; i < 32; i++) {
		if (tmp_multi & 0x1) {
			tmp_ret = ((u64)multiplier << i);
			result = add64_oper(result, tmp_ret);
		}
		tmp_multi = (tmp_multi >> 1);
	}

	VDBG("multi 64 low result is 0x%x\n", result);
	VDBG("multi 64 high result is 0x%x\n", (u32)(result>>32));

	return result;
}

/*convert the 64 bites time stamp to second and nanosecond*/
static void convert_rtc_time(u64 *rtc_time, struct ptp_time *p_time)
{
	u32 time_h;
	u32 time_sec = 0;

	time_h = div64_oper(*rtc_time, NANOSEC_IN_SEC, &time_sec);

	p_time->sec = time_sec;
	p_time->nsec = time_h;
}

/* convert rtc time to 64 bites timestamp */
static u64 convert_unsigned_time(struct ptp_time *ptime)
{
	return add64_oper(multi64_oper(ptime->sec, NANOSEC_IN_SEC),
			(u64)ptime->nsec);
}

/*RTC interrupt handler*/
static irqreturn_t ptp_rtc_interrupt(int irq, void *_ptp)
{
	struct ptp *p_ptp = (struct ptp *)_ptp;
	struct ptp_rtc *rtc = p_ptp->rtc;
	struct ptp_time time;
	register u32 events;

	/*get valid events*/
	events = readl(rtc->mem_map + PTP_TMR_TEVENT);

	/*clear event bits*/
	writel(events, rtc->mem_map + PTP_TMR_TEVENT);

	/*get the current time as quickly as possible*/
	ptp_rtc_get_current_time(p_ptp, &time);

	if (events & RTC_TEVENT_ALARM_1) {
		p_ptp->alarm_counters[0]++;
		VDBG("PTP Alarm 1 event, time = %2d:%09d[sec:nsec]\n",
			time.sec, time.nsec);
	}
	if (events & RTC_TEVENT_ALARM_2) {
		p_ptp->alarm_counters[1]++;
		VDBG("PTP Alarm 2 event, time = %2d:%09d[sec:nsec]\n",
			time.sec, time.nsec);
	}
	if (events & RTC_TEVENT_PERIODIC_PULSE_1) {
		p_ptp->pulse_counters[0]++;
		VDBG("PTP Pulse 1 event, time = %2d:%09d[sec:nsec]\n",
			time.sec, time.nsec);
	}
	if (events & RTC_TEVENT_PERIODIC_PULSE_2) {
		p_ptp->pulse_counters[1]++;
		VDBG("PTP Pulse 2 event, time = %2d:%09d[sec:nsec]\n",
			time.sec, time.nsec);
	}
	if (events & RTC_TEVENT_PERIODIC_PULSE_3) {
		p_ptp->pulse_counters[2]++;
		VDBG("PTP Pulse 3 event, time = %2d:%09d[sec:nsec]\n",
			time.sec, time.nsec);
	}

	return IRQ_HANDLED;
}

static int ptp_rtc_init(struct ptp *p_ptp)
{
	struct ptp_rtc *rtc = p_ptp->rtc;
	struct ptp_rtc_driver_param *rtc_drv_param = rtc->driver_param;
	void __iomem *rtc_mem = rtc->mem_map;
	u32 freq_compensation = 0;
	u32 tmr_ctrl = 0;
	int ret = 0;
	int i;

	rtc = p_ptp->rtc;
	rtc_drv_param = rtc->driver_param;
	rtc_mem = rtc->mem_map;

	if (!rtc->bypass_compensation)
		rtc->clock_period_nansec = NANOSEC_PER_ONE_HZ_TICK /
				rtc_drv_param->rtc_freq_hz;
	else {
		/*In bypass mode,the RTC clock equals the source clock*/
		rtc->clock_period_nansec = NANOSEC_PER_ONE_HZ_TICK /
				rtc_drv_param->src_clock_freq_hz;
		tmr_ctrl |= RTC_TMR_CTRL_BYP;
	}

	tmr_ctrl |= ((rtc->clock_period_nansec <<
			RTC_TMR_CTRL_TCLK_PERIOD_SHIFT) &
			RTC_TMR_CTRL_TCLK_PERIOD_MSK);

	if (rtc_drv_param->invert_input_clk_phase)
		tmr_ctrl |= RTC_TMR_CTRL_CIPH;
	if (rtc_drv_param->invert_output_clk_phase)
		tmr_ctrl |= RTC_TMR_CTRL_COPH;
	if (rtc_drv_param->pulse_start_mode == e_PTP_RTC_PULSE_START_ON_ALARM) {
		tmr_ctrl |= RTC_TMR_CTRL_FS;
		rtc->start_pulse_on_alarm = TRUE;
	}

	for (i = 0; i < PTP_RTC_NUM_OF_ALARMS; i++) {
		if (rtc_drv_param->alarm_polarity[i] ==
			e_PTP_RTC_ALARM_POLARITY_ACTIVE_LOW)
			tmr_ctrl |= (RTC_TMR_CTRL_ALMP1 >> i);

	}

	/*clear TMR_ALARM registers*/
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_ALARM1_L);
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_ALARM1_H);
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_ALARM2_L);
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_ALARM2_H);

	for (i = 0; i < PTP_RTC_NUM_OF_TRIGGERS; i++) {
		if (rtc_drv_param->trigger_polarity[i] ==
			e_PTP_RTC_TRIGGER_ON_FALLING_EDGE)
			tmr_ctrl |= (RTC_TMR_CTRL_ETEP1 << i);
	}

	/*clear TMR_FIPER registers*/
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_FIPER1);
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_FIPER2);
	writel(0xFFFFFFFF, rtc_mem + PTP_TMR_FIPER3);

	/*set the source clock*/
	/*use a clock from the QE bank of clocks*/
	tmr_ctrl |= RTC_TMR_CTRL_CKSEL_EXT_CLK;

	/*write register and perform software reset*/
	writel((tmr_ctrl | RTC_TMR_CTRL_TMSR), rtc_mem + PTP_TMR_CTRL);
	writel(tmr_ctrl, rtc_mem + PTP_TMR_CTRL);

	/*clear TMR_TEVEMT*/
	writel(RTC_EVENT_ALL, rtc_mem + PTP_TMR_TEVENT);

	/*initialize TMR_TEMASK*/
	writel(rtc_drv_param->events_mask, rtc_mem + PTP_TMR_TEMASK);

	/*initialize TMR_ADD with the initial frequency compensation value:
	 freq_compensation = (2^32 / frequency ratio)*/
	div64_oper(((u64)rtc_drv_param->rtc_freq_hz << 32),
			rtc_drv_param->src_clock_freq_hz, &freq_compensation);
	p_ptp->orig_freq_comp = freq_compensation;
	writel(freq_compensation, rtc_mem + PTP_TMR_ADD);

	/*initialize TMR_PRSC*/
	writel(rtc->output_clock_divisor, rtc_mem + PTP_TMR_PRSC);

	/*initialize TMR_OFF*/
	writel(0, rtc_mem + PTP_TMR_OFF_L);
	writel(0, rtc_mem + PTP_TMR_OFF_H);

	return ret;
}

static void init_ptp_parser(struct ptp *p_ptp)
{
	void __iomem *mem_map = p_ptp->mem_map;
	struct ptp_driver_param *drv_param = p_ptp->driver_param;
	u32 reg32;

	/*initialzie PTP TSPDR1*/
	reg32 = ((drv_param->eth_type_value << PTP_TSPDR1_ETT_SHIFT) &
			PTP_TSPDR1_ETT_MASK);
	reg32 |= ((drv_param->ip_type_value << PTP_TSPDR1_IPT_SHIFT) &
			PTP_TSPDR1_IPT_MASK);
	writel(reg32, mem_map + PTP_TSPDR1);

	/*initialize PTP TSPDR2*/
	reg32 = ((drv_param->udp_general_port << PTP_TSPDR2_DPNGE_SHIFT) &
			PTP_TSPDR2_DPNGE_MASK);
	reg32 |= (drv_param->udp_event_port & PTP_TSPDR2_DPNEV_MASK);
	writel(reg32, mem_map + PTP_TSPDR2);

	/*initialize PTP TSPDR3*/
	reg32 = ((drv_param->ptp_msg_codes[e_PTP_MSG_SYNC] <<
			PTP_TSPDR3_SYCTL_SHIFT) & PTP_TSPDR3_SYCTL_MASK);
	reg32 |= ((drv_param->ptp_msg_codes[e_PTP_MSG_DELAY_REQ] <<
			PTP_TSPDR3_DRCTL_SHIFT) & PTP_TSPDR3_DRCTL_MASK);
	reg32 |= ((drv_param->ptp_msg_codes[e_PTP_MSG_DELAY_RESP] <<
			PTP_TSPDR3_DRPCTL_SHIFT) & PTP_TSPDR3_DRPCTL_MASK);
	reg32 |= (drv_param->ptp_msg_codes[e_PTP_MSG_FOLLOW_UP] &
			PTP_TSPDR3_FUCTL_MASK);
	writel(reg32, mem_map + PTP_TSPDR3);

	/*initialzie PTP TSPDR4*/
	reg32 = ((drv_param->ptp_msg_codes[e_PTP_MSG_MANAGEMENT] <<
			PTP_TSPDR4_MACTL_SHIFT) & PTP_TSPDR4_MACTL_MASK);
	reg32 |= (drv_param->vlan_type_value & PTP_TSPDR4_VLAN_MASK);
	writel(reg32, mem_map + PTP_TSPDR4);

	/*initialize PTP TSPOV*/
	reg32 = ((drv_param->eth_type_offset << PTP_TSPOV_ETTOF_SHIFT) &
			PTP_TSPOV_ETTOF_MASK);
	reg32 |= ((drv_param->ip_type_offset << PTP_TSPOV_IPTOF_SHIFT) &
			PTP_TSPOV_IPTOF_MASK);
	reg32 |= ((drv_param->udp_dest_port_offset << PTP_TSPOV_UDOF_SHIFT) &
			PTP_TSPOV_UDOF_MASK);
	reg32 |= (drv_param->ptp_type_offset & PTP_TSPOV_PTOF_MASK);
	writel(reg32, mem_map + PTP_TSPOV);
}

/* compatible with MXS 1588 */
void fec_ptp_store_txstamp(struct fec_ptp_private *priv,
			   struct sk_buff *skb,
			   struct bufdesc *bdp)
{
	int msg_type, seq_id, control;
	struct fec_ptp_ts_data tmp_tx_time;
	struct ptp *p_ptp = ptp_dev;
	unsigned char *sp_id;
	u64 timestamp;

	/* Check for PTP Event */
	if ((bdp->cbd_sc & BD_ENET_TX_PTP) == 0)
		return;

	/*read timestamp from register*/
	timestamp = ((u64)readl(p_ptp->mem_map + PTP_TMR_TXTS_H)
			<< 32) |
			(readl(p_ptp->mem_map + PTP_TMR_TXTS_L));
	convert_rtc_time(&timestamp, &(tmp_tx_time.ts));

	seq_id = *((u16 *)(skb->data + FEC_PTP_SEQ_ID_OFFS));
	control = *((u8 *)(skb->data + FEC_PTP_CTRL_OFFS));
	sp_id = skb->data + FEC_PTP_SPORT_ID_OFFS;

	tmp_tx_time.ident.seq_id = ntohs(seq_id);
	memcpy(tmp_tx_time.ident.spid, sp_id, 10);

	switch (control) {

	case PTP_MSG_SYNC:
		fec_ptp_insert(&(priv->tx_time_sync), &tmp_tx_time);
		break;

	case PTP_MSG_DEL_REQ:
		fec_ptp_insert(&(priv->tx_time_del_req), &tmp_tx_time);
		break;

	/* clear transportSpecific field*/
	case PTP_MSG_ALL_OTHER:
		msg_type = (*((u8 *)(skb->data +
				FEC_PTP_MSG_TYPE_OFFS))) & 0x0F;
		switch (msg_type) {
		case PTP_MSG_P_DEL_REQ:
			fec_ptp_insert(&(priv->tx_time_pdel_req),
					&tmp_tx_time);
			break;
		case PTP_MSG_P_DEL_RESP:
			fec_ptp_insert(&(priv->tx_time_pdel_resp),
					&tmp_tx_time);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

/* in-band rx ts store */
void fec_ptp_store_rxstamp(struct fec_ptp_private *priv,
			   struct sk_buff *skb,
			   struct bufdesc *bdp)
{
	int msg_type, seq_id, control;
	struct fec_ptp_ts_data tmp_rx_time;
	struct ptp *p_ptp = ptp_dev;
	u64 timestamp;
	unsigned char *sp_id;

	/* Check for PTP Event */
	if ((bdp->cbd_sc & BD_ENET_RX_PTP) == 0)
		return;

	/* read ts from register */
	timestamp = ((u64)readl(p_ptp->mem_map + PTP_TMR_RXTS_H)
		<< 32) | (readl(p_ptp->mem_map + PTP_TMR_RXTS_L));
	convert_rtc_time(&timestamp, &(tmp_rx_time.ts));

	seq_id = *((u16 *)(skb->data + FEC_PTP_SEQ_ID_OFFS));
	control = *((u8 *)(skb->data + FEC_PTP_CTRL_OFFS));
	sp_id = skb->data + FEC_PTP_SPORT_ID_OFFS;

	tmp_rx_time.ident.seq_id = ntohs(seq_id);
	memcpy(tmp_rx_time.ident.spid, sp_id, 10);

	switch (control) {

	case PTP_MSG_SYNC:
		fec_ptp_insert(&(priv->rx_time_sync), &tmp_rx_time);
		break;

	case PTP_MSG_DEL_REQ:
		fec_ptp_insert(&(priv->rx_time_del_req), &tmp_rx_time);
		break;

	/* clear transportSpecific field*/
	case PTP_MSG_ALL_OTHER:
		msg_type = (*((u8 *)(skb->data +
				FEC_PTP_MSG_TYPE_OFFS))) & 0x0F;
		switch (msg_type) {
		case PTP_MSG_P_DEL_REQ:
			fec_ptp_insert(&(priv->rx_time_pdel_req),
						&tmp_rx_time);
			break;
		case PTP_MSG_P_DEL_RESP:
			fec_ptp_insert(&(priv->rx_time_pdel_resp),
						&tmp_rx_time);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

}

static void init_ptp_tsu(struct ptp *p_ptp)
{
	struct ptp_driver_param *drv_param = p_ptp->driver_param;
	void __iomem *mem_map;
	u32 tsmr, pemask, events_mask;

	mem_map = p_ptp->mem_map;

	/*Tx timestamp events are required in all modes*/
	events_mask = PTP_TS_TX_FRAME1 | PTP_TS_TX_OVR1;

	/*read current values of TSU registers*/
	tsmr = readl(mem_map + PTP_TSMR);
	pemask = readl(mem_map + PTP_TMR_PEMASK);

	if (drv_param->delivery_mode ==	e_PTP_TSU_DELIVERY_IN_BAND) {
		tsmr |= PTP_TSMR_OPMODE1_IN_BAND;
		events_mask &= ~(PTP_TS_TX_OVR1);
	} else
		/*rx timestamp events are required for out of band mode*/
		events_mask |= (PTP_TS_RX_SYNC1 | PTP_TS_RX_DELAY_REQ1 |
				PTP_TS_RX_OVR1);

	pemask |= events_mask;

	/*update TSU register*/
	writel(tsmr, mem_map + PTP_TSMR);
	writel(pemask, mem_map + PTP_TMR_PEMASK);
}

/* ptp module init */
static void ptp_tsu_init(struct ptp *p_ptp)
{
	void __iomem *mem_map = p_ptp->mem_map;

	/*initialization of registered PTP modules*/
	init_ptp_parser(p_ptp);

	/*reset timestamp*/
	writel(0, mem_map + PTP_TSMR);
	writel(0, mem_map + PTP_TMR_PEMASK);
	writel(PTP_TMR_PEVENT_ALL, mem_map + PTP_TMR_PEVENT);

}

/* TSU configure function */
static u32 ptp_tsu_enable(struct ptp *p_ptp)
{
	void __iomem *mem_map;
	u32 tsmr;

	/*enable the TSU*/
	mem_map = p_ptp->mem_map;

	/*set the TSU enable bit*/
	tsmr = readl(mem_map + PTP_TSMR);
	tsmr |= PTP_TSMR_EN1;

	writel(tsmr, mem_map + PTP_TSMR);

	return 0;
}

static u32 ptp_tsu_disable(struct ptp *p_ptp)
{
	void __iomem *mem_map;
	u32 tsmr;

	mem_map = p_ptp->mem_map;

	tsmr = readl(mem_map + PTP_TSMR);
	tsmr &= ~(PTP_TSMR_EN1);
	writel(tsmr, mem_map + PTP_TSMR);

	return 0;
}

static int ptp_tsu_config_events_mask(struct ptp *p_ptp,
					u32 events_mask)
{

	p_ptp->events_mask = events_mask;

	return 0;
}

/* rtc configure function */
static u32 rtc_enable(struct ptp_rtc *rtc, bool reset_clock)
{
	u32 tmr_ctrl;

	tmr_ctrl = readl(rtc->mem_map + PTP_TMR_CTRL);
	if (reset_clock) {
		writel((tmr_ctrl | RTC_TMR_CTRL_TMSR),
				rtc->mem_map + PTP_TMR_CTRL);

		/*clear TMR_OFF*/
		writel(0, rtc->mem_map + PTP_TMR_OFF_L);
		writel(0, rtc->mem_map + PTP_TMR_OFF_H);
	}

	writel((tmr_ctrl | RTC_TMR_CTRL_TE),
			rtc->mem_map + PTP_TMR_CTRL);

	return 0;
}

static u32 rtc_disable(struct ptp_rtc *rtc)
{
	u32 tmr_ctrl;

	tmr_ctrl = readl(rtc->mem_map + PTP_TMR_CTRL);
	writel((tmr_ctrl & ~RTC_TMR_CTRL_TE),
			rtc->mem_map + PTP_TMR_CTRL);

	return 0;
}

static u32 rtc_set_periodic_pulse(
	struct ptp_rtc *rtc,
	enum e_ptp_rtc_pulse_id pulse_ID,
	u32 pulse_periodic)
{
	u32 factor;

	if (rtc->start_pulse_on_alarm) {
		/*from the spec:the ratio between the prescale register value
		 *and the fiper value should be decisable by the clock period
		 *FIPER_VALUE = (prescale_value * tclk_per * N) - tclk_per*/
		 factor = (u32)((pulse_periodic + rtc->clock_period_nansec) /
			(rtc->clock_period_nansec * rtc->output_clock_divisor));

		if ((factor * rtc->clock_period_nansec *
			rtc->output_clock_divisor) <
			(pulse_periodic + rtc->clock_period_nansec))
			pulse_periodic = ((factor * rtc->clock_period_nansec *
				rtc->output_clock_divisor) -
				rtc->clock_period_nansec);
	}

	/* Decrease it to fix PPS question (frequecy error)*/
	pulse_periodic -= rtc->clock_period_nansec;

	writel((u32)pulse_periodic, rtc->mem_map + PTP_TMR_FIPER1 +
			(pulse_ID * 4));
	return 0;
}

static u32 ptp_rtc_set_periodic_pulse(
	struct ptp *p_ptp,
	enum e_ptp_rtc_pulse_id pulse_ID,
	struct ptp_time *ptime)
{
	u32 ret;
	u64 pulse_periodic;

	if (pulse_ID >= PTP_RTC_NUM_OF_PULSES)
		return -1;
	if (ptime->nsec < 0)
		return -1;

	pulse_periodic = convert_unsigned_time(ptime);
	if (pulse_periodic > 0xFFFFFFFF)
		return -1;

	ret = rtc_set_periodic_pulse(p_ptp->rtc, pulse_ID, (u32)pulse_periodic);

	return ret;
}

static u32 rtc_set_alarm(
	struct ptp_rtc *rtc,
	enum e_ptp_rtc_alarm_id alarm_ID,
	u64 alarm_time)
{
	u32 fiper;
	int i;

	if ((alarm_ID == e_PTP_RTC_ALARM_1) && rtc->start_pulse_on_alarm)
		alarm_time -= (3 * rtc->clock_period_nansec);

	/*TMR_ALARM_L must be written first*/
	writel((u32)alarm_time, rtc->mem_map + PTP_TMR_ALARM1_L +
			(alarm_ID * 4));
	writel((u32)(alarm_time >> 32),
			rtc->mem_map + PTP_TMR_ALARM1_H + (alarm_ID * 4));

	if ((alarm_ID == e_PTP_RTC_ALARM_1) && rtc->start_pulse_on_alarm) {
		/*we must write the TMR_FIPER register again(hardware
		 *constraint),From the spec:in order to keep tracking
		 *the prescale output clock each tiem before enabling
		 *the fiper,the user must reset the fiper by writing
		 *a new value to the reigster*/
		for (i = 0; i < PTP_RTC_NUM_OF_PULSES; i++) {
			fiper = readl(rtc->mem_map + PTP_TMR_FIPER1 +
					(i * 4));
			writel(fiper, rtc->mem_map + PTP_TMR_FIPER1 +
					(i * 4));
		}
	}

	return 0;
}

static u32 ptp_rtc_set_alarm(
	struct ptp *p_ptp,
	enum e_ptp_rtc_alarm_id alarm_ID,
	struct ptp_time *ptime)
{
	u32 ret;
	u64 alarm_time;

	if (alarm_ID >= PTP_RTC_NUM_OF_ALARMS)
		return -1;
	if (ptime->nsec < 0)
		return -1;

	alarm_time = convert_unsigned_time(ptime);

	ret = rtc_set_alarm(p_ptp->rtc, alarm_ID, alarm_time);

	return ret;
}

/* rtc ioctl function */
/*get the current time from RTC time counter register*/
static void ptp_rtc_get_current_time(struct ptp *p_ptp,
					struct ptp_time *p_time)
{
	u64 times;
	struct ptp_rtc *rtc = p_ptp->rtc;

	/*TMR_CNT_L must be read first to get an accurate value*/
	times = (u64)readl(rtc->mem_map + PTP_TMR_CNT_L);
	times |= ((u64)readl(rtc->mem_map + PTP_TMR_CNT_H)) << 32;

	/*convert RTC time*/
	convert_rtc_time(&times, p_time);
}

static void ptp_rtc_reset_counter(struct ptp *p_ptp, struct ptp_time *p_time)
{
	u64 times;
	struct ptp_rtc *rtc = p_ptp->rtc;

	times = convert_unsigned_time(p_time);
	writel((u32)times, rtc->mem_map + PTP_TMR_CNT_L);
	writel((u32)(times >> 32), rtc->mem_map + PTP_TMR_CNT_H);

}

static void rtc_modify_frequency_compensation(
	struct ptp_rtc *rtc,
	u32 freq_compensation)
{
	writel(freq_compensation, rtc->mem_map + PTP_TMR_ADD);
}

/**
 * Parse packets if they are PTP.
 * The PTP header can be found in an IPv4, IPv6 or in an IEEE802.3
 * ethernet frame. The function returns the position of the PTP packet
 * or NULL, if no PTP found
 */
static u8 *fec_ptp_parse_packet(struct sk_buff *skb, u16 *eth_type)
{
	u8 *position = skb->data + ETH_ALEN + ETH_ALEN;
	u8 *ptp_loc = NULL;

	*eth_type = *((u16 *)position);
	/* Check if outer vlan tag is here */
	if (ntohs(*eth_type) == ETH_P_8021Q) {
		position += FEC_VLAN_TAG_LEN;
		*eth_type = *((u16 *)position);
	}

	/* set position after ethertype */
	position += FEC_ETHTYPE_LEN;
	if (ETH_P_1588 == ntohs(*eth_type)) {
		ptp_loc = position;
		/* IEEE1588 event message which needs timestamping */
		if ((ptp_loc[0] & 0xF) <= 3) {
			if (skb->len >=
			((ptp_loc - skb->data) + PTP_HEADER_SZE))
				return ptp_loc;
		}
	} else if (ETH_P_IP == ntohs(*eth_type)) {
		u8 *ip_header, *prot, *udp_header;
		u8 ip_version, ip_hlen;
		ip_header = position;
		ip_version = ip_header[0] >> 4; /* correct IP version? */
		if (0x04 == ip_version) { /* IPv4 */
			prot = ip_header + 9; /* protocol */
			if (FEC_PACKET_TYPE_UDP == *prot) {
				u16 udp_dstPort;
				/* retrieve the size of the ip-header
				 * with the first byte of the ip-header:
				 * version ( 4 bits) + Internet header
				 * length (4 bits)
				 */
				ip_hlen   = (*ip_header & 0xf) * 4;
				udp_header = ip_header + ip_hlen;
				udp_dstPort = *((u16 *)(udp_header + 2));
				/* check the destination port address
				 * ( 319 (0x013F) = PTP event port )
				 */
				if (ntohs(udp_dstPort) == PTP_EVENT_PORT) {
					ptp_loc = udp_header + 8;
					/* long enough ? */
					if (skb->len >= ((ptp_loc - skb->data)
							+ PTP_HEADER_SZE))
						return ptp_loc;
				}
			}
		}
	} else if (ETH_P_IPV6 == ntohs(*eth_type)) {
		u8 *ip_header, *udp_header, *prot;
		u8 ip_version;
		ip_header = position;
		ip_version = ip_header[0] >> 4;
		if (0x06 == ip_version) {
			prot = ip_header + 6;
			if (FEC_PACKET_TYPE_UDP == *prot) {
				u16 udp_dstPort;
				udp_header = ip_header + 40;
				udp_dstPort = *((u16 *)(udp_header + 2));
				/* check the destination port address
				 * ( 319 (0x013F) = PTP event port )
				 */
				if (ntohs(udp_dstPort) == PTP_EVENT_PORT) {
					ptp_loc = udp_header + 8;
					/* long enough ? */
					if (skb->len >= ((ptp_loc - skb->data)
							+ PTP_HEADER_SZE))
						return ptp_loc;
				}
			}
		}
	}

	return NULL; /* no PTP frame */
}

/* Set the BD to ptp */
int fec_ptp_do_txstamp(struct sk_buff *skb)
{
	u8 *ptp_loc;
	u16 eth_type;

	ptp_loc = fec_ptp_parse_packet(skb, &eth_type);
	if (ptp_loc != NULL)
		return 1;

	return 0;
}

static int fec_get_tx_timestamp(struct fec_ptp_private *priv,
				 struct fec_ptp_ts_data *pts,
				 struct ptp_time *tx_time)
{
	int flag;
	u8 mode;

	mode = pts->ident.message_type;
	switch (mode) {
	case PTP_MSG_SYNC:
		flag =
		fec_ptp_find_and_remove(&(priv->tx_time_sync),
				&pts->ident, tx_time);
		break;
	case PTP_MSG_DEL_REQ:
		flag =
		fec_ptp_find_and_remove(&(priv->tx_time_del_req),
				&pts->ident, tx_time);
		break;

	case PTP_MSG_P_DEL_REQ:
		flag =
		fec_ptp_find_and_remove(&(priv->tx_time_pdel_req),
				&pts->ident, tx_time);
		break;
	case PTP_MSG_P_DEL_RESP:
		flag =
		fec_ptp_find_and_remove(&(priv->tx_time_pdel_resp),
				&pts->ident, tx_time);
		break;

	default:
		flag = 1;
		printk(KERN_ERR "ERROR\n");
		break;
	}

	if (flag) {
		switch (mode) {
		case PTP_MSG_SYNC:
			flag =
			fec_ptp_find_and_remove(&(priv->tx_time_sync),
						&pts->ident, tx_time);
			break;
		case PTP_MSG_DEL_REQ:
			flag =
			fec_ptp_find_and_remove(&(priv->tx_time_del_req),
						&pts->ident, tx_time);
			break;
		case PTP_MSG_P_DEL_REQ:
			flag =
			fec_ptp_find_and_remove(&(priv->tx_time_pdel_req),
						&pts->ident, tx_time);
			break;
		case PTP_MSG_P_DEL_RESP:
			flag =
			fec_ptp_find_and_remove(&(priv->tx_time_pdel_resp),
						&pts->ident, tx_time);
			break;
		}
	}

	return flag ? -1 : 0;
}

static uint8_t fec_get_rx_timestamp(struct fec_ptp_private *priv,
				    struct fec_ptp_ts_data *pts,
				    struct ptp_time *rx_time)
{
	int flag;
	u8 mode;

	mode = pts->ident.message_type;
	switch (mode) {
	case PTP_MSG_SYNC:
		flag =
		fec_ptp_find_and_remove(&(priv->rx_time_sync),
			&pts->ident, rx_time);
		break;
	case PTP_MSG_DEL_REQ:
		flag =
		fec_ptp_find_and_remove(&(priv->rx_time_del_req),
			&pts->ident, rx_time);
		break;

	case PTP_MSG_P_DEL_REQ:
		flag =
		fec_ptp_find_and_remove(&(priv->rx_time_pdel_req),
			&pts->ident, rx_time);
		break;
	case PTP_MSG_P_DEL_RESP:
		flag =
		fec_ptp_find_and_remove(&(priv->rx_time_pdel_resp),
				&pts->ident, rx_time);
		break;

	default:
		flag = 1;
		printk(KERN_ERR "ERROR\n");
		break;
	}

	if (flag) {
		switch (mode) {
		case PTP_MSG_SYNC:
			flag =
			fec_ptp_find_and_remove(&(priv->rx_time_sync),
				&pts->ident, rx_time);
			break;
		case PTP_MSG_DEL_REQ:
			flag =
			fec_ptp_find_and_remove(&(priv->rx_time_del_req),
					&pts->ident, rx_time);
			break;
		case PTP_MSG_P_DEL_REQ:
			flag =
			fec_ptp_find_and_remove(&(priv->rx_time_pdel_req),
					&pts->ident, rx_time);
			break;
		case PTP_MSG_P_DEL_RESP:
			flag =
			fec_ptp_find_and_remove(&(priv->rx_time_pdel_resp),
					&pts->ident, rx_time);
			break;
		}
	}

	return flag ? -1 : 0;
}

/* 1588 Module start */
int fec_ptp_start(struct fec_ptp_private *priv)
{
	struct ptp *p_ptp = ptp_dev;

	/* Enable TSU clk */
	clk_enable(p_ptp->clk);

	/*initialize the TSU using the register function*/
	init_ptp_tsu(p_ptp);

	/* start counter */
	p_ptp->fpp = ptp_private;
	ptp_tsu_enable(p_ptp);

	return 0;
}

/* Cleanup routine for 1588 module.
 * When PTP is disabled this routing is called */
void fec_ptp_stop(struct fec_ptp_private *priv)
{
	struct ptp *p_ptp = ptp_dev;

	/* stop counter */
	ptp_tsu_disable(p_ptp);
	clk_disable(p_ptp->clk);

	return;
}

/* ptp device ioctl function */
int fec_ptp_ioctl(struct fec_ptp_private *priv, struct ifreq *ifr, int cmd)
{
	struct ptp_rtc_time curr_time;
	struct ptp_time rx_time, tx_time;
	struct fec_ptp_ts_data p_ts;
	struct fec_ptp_ts_data *p_ts_user;
	struct ptp_set_comp p_comp;
	u32 freq_compensation;
	int retval = 0;

	switch (cmd) {
	case PTP_ENBL_TXTS_IOCTL:
	case PTP_DSBL_TXTS_IOCTL:
	case PTP_ENBL_RXTS_IOCTL:
	case PTP_DSBL_RXTS_IOCTL:
		break;
	case PTP_GET_RX_TIMESTAMP:
		p_ts_user = (struct fec_ptp_ts_data *)ifr->ifr_data;
		if (0 != copy_from_user(&p_ts.ident,
			&p_ts_user->ident, sizeof(p_ts.ident)))
			return -EINVAL;
		if (fec_get_rx_timestamp(priv, &p_ts, &rx_time) != 0)
			return -EAGAIN;
		if (copy_to_user((void __user *)(&p_ts_user->ts), &rx_time,
					sizeof(rx_time)))
			return -EFAULT;
		break;
	case PTP_GET_TX_TIMESTAMP:
		p_ts_user = (struct fec_ptp_ts_data *)ifr->ifr_data;
		if (0 != copy_from_user(&p_ts.ident,
			&p_ts_user->ident, sizeof(p_ts.ident)))
			return -EINVAL;
		retval = fec_get_tx_timestamp(priv, &p_ts, &tx_time);
		if (retval == 0 &&
			copy_to_user((void __user *)(&p_ts_user->ts),
				&tx_time, sizeof(tx_time)))
			retval = -EFAULT;
		break;
	case PTP_GET_CURRENT_TIME:
		ptp_rtc_get_current_time(ptp_dev, &(curr_time.rtc_time));
		if (0 != copy_to_user(ifr->ifr_data,
			&(curr_time.rtc_time), sizeof(struct ptp_time)))
				return -EFAULT;
		break;
	case PTP_SET_RTC_TIME:
		if (0 != copy_from_user(&(curr_time.rtc_time),
			 ifr->ifr_data,	sizeof(struct ptp_time)))
			return -EINVAL;
		ptp_rtc_reset_counter(ptp_dev, &(curr_time.rtc_time));
		break;
	case PTP_FLUSH_TIMESTAMP:
		/* reset sync buffer */
		priv->rx_time_sync.front = 0;
		priv->rx_time_sync.end = 0;
		priv->rx_time_sync.size = (DEFAULT_PTP_RX_BUF_SZ + 1);
		/* reset delay_req buffer */
		priv->rx_time_del_req.front = 0;
		priv->rx_time_del_req.end = 0;
		priv->rx_time_del_req.size = (DEFAULT_PTP_RX_BUF_SZ + 1);
		/* reset pdelay_req buffer */
		priv->rx_time_pdel_req.front = 0;
		priv->rx_time_pdel_req.end = 0;
		priv->rx_time_pdel_req.size = (DEFAULT_PTP_RX_BUF_SZ + 1);
		/* reset pdelay_resp buffer */
		priv->rx_time_pdel_resp.front = 0;
		priv->rx_time_pdel_resp.end = 0;
		priv->rx_time_pdel_resp.size = (DEFAULT_PTP_RX_BUF_SZ + 1);
		/* reset sync buffer */
		priv->tx_time_sync.front = 0;
		priv->tx_time_sync.end = 0;
		priv->tx_time_sync.size = (DEFAULT_PTP_TX_BUF_SZ + 1);
		/* reset delay_req buffer */
		priv->tx_time_del_req.front = 0;
		priv->tx_time_del_req.end = 0;
		priv->tx_time_del_req.size = (DEFAULT_PTP_TX_BUF_SZ + 1);
		/* reset pdelay_req buffer */
		priv->tx_time_pdel_req.front = 0;
		priv->tx_time_pdel_req.end = 0;
		priv->tx_time_pdel_req.size = (DEFAULT_PTP_TX_BUF_SZ + 1);
		/* reset pdelay_resp buffer */
		priv->tx_time_pdel_resp.front = 0;
		priv->tx_time_pdel_resp.end = 0;
		priv->tx_time_pdel_resp.size = (DEFAULT_PTP_TX_BUF_SZ + 1);
		break;
	case PTP_SET_COMPENSATION:
		if (0 != copy_from_user(&p_comp, ifr->ifr_data,
			sizeof(struct ptp_set_comp)))
			return -EINVAL;
		rtc_modify_frequency_compensation(ptp_dev->rtc,
				p_comp.freq_compensation);
		break;
	case PTP_GET_ORIG_COMP:
		freq_compensation = ptp_dev->orig_freq_comp;
		if (copy_to_user(ifr->ifr_data, &freq_compensation,
					sizeof(freq_compensation)) > 0)
			return -EFAULT;
		break;
	default:
		return -EINVAL;
	}
	return retval;
}

static int init_ptp_driver(struct ptp *p_ptp)
{
	struct ptp_time ptime;
	int ret;

	/* configure RTC param */
	ret = ptp_rtc_config(p_ptp->rtc);
	if (ret)
		return -1;

	/* initialize RTC register */
	ptp_rtc_init(p_ptp);

	/* initialize PTP TSU param */
	ptp_param_config(p_ptp);

	/* set TSU configuration parameters */
	p_ptp->driver_param->delivery_mode = e_PTP_TSU_DELIVERY_OUT_OF_BAND;

	if (ptp_tsu_config_events_mask(p_ptp, DEFAULT_events_PTP_Mask))
			goto end;

	/* initialize PTP TSU register */
	ptp_tsu_init(p_ptp);

	/* set periodic pulses */
	ptime.sec = USE_CASE_PULSE_1_PERIOD / NANOSEC_IN_SEC;
	ptime.nsec = USE_CASE_PULSE_1_PERIOD % NANOSEC_IN_SEC;
	ret = ptp_rtc_set_periodic_pulse(p_ptp, e_PTP_RTC_PULSE_1, &ptime);
	if (ret)
		goto end;

	ptime.sec = USE_CASE_PULSE_2_PERIOD / NANOSEC_IN_SEC;
	ptime.nsec = USE_CASE_PULSE_2_PERIOD % NANOSEC_IN_SEC;
	ret = ptp_rtc_set_periodic_pulse(p_ptp, e_PTP_RTC_PULSE_2, &ptime);
	if (ret)
		goto end;

	ptime.sec = USE_CASE_PULSE_3_PERIOD / NANOSEC_IN_SEC;
	ptime.nsec = USE_CASE_PULSE_3_PERIOD % NANOSEC_IN_SEC;
	ret = ptp_rtc_set_periodic_pulse(p_ptp, e_PTP_RTC_PULSE_3, &ptime);
	if (ret)
		goto end;

	/* set alarm */
	ptime.sec = (USE_CASE_ALARM_1_TIME / NANOSEC_IN_SEC);
	ptime.nsec = (USE_CASE_ALARM_1_TIME % NANOSEC_IN_SEC);
	ret = ptp_rtc_set_alarm(p_ptp, e_PTP_RTC_ALARM_1, &ptime);
	if (ret)
		goto end;

	ptime.sec = (USE_CASE_ALARM_2_TIME / NANOSEC_IN_SEC);
	ptime.nsec = (USE_CASE_ALARM_2_TIME % NANOSEC_IN_SEC);
	ret = ptp_rtc_set_alarm(p_ptp, e_PTP_RTC_ALARM_2, &ptime);
	if (ret)
		goto end;

	/* enable the RTC */
	ret = rtc_enable(p_ptp->rtc, FALSE);
	if (ret)
		goto end;

	udelay(10);
	ptp_rtc_get_current_time(p_ptp, &ptime);
	if (ptime.nsec == 0) {
		printk(KERN_ERR "PTP RTC is not running\n");
		goto end;
	}

end:
	return ret;
}

static void ptp_free(void)
{
	rtc_disable(ptp_dev->rtc);
}

/*
 * Resource required for accessing 1588 Timer Registers.
 */
int fec_ptp_init(struct fec_ptp_private *priv, int id)
{
	fec_ptp_init_circ(&(priv->rx_time_sync), DEFAULT_PTP_RX_BUF_SZ);
	fec_ptp_init_circ(&(priv->rx_time_del_req), DEFAULT_PTP_RX_BUF_SZ);
	fec_ptp_init_circ(&(priv->rx_time_pdel_req), DEFAULT_PTP_RX_BUF_SZ);
	fec_ptp_init_circ(&(priv->rx_time_pdel_resp), DEFAULT_PTP_RX_BUF_SZ);
	fec_ptp_init_circ(&(priv->tx_time_sync), DEFAULT_PTP_TX_BUF_SZ);
	fec_ptp_init_circ(&(priv->tx_time_del_req), DEFAULT_PTP_TX_BUF_SZ);
	fec_ptp_init_circ(&(priv->tx_time_pdel_req), DEFAULT_PTP_TX_BUF_SZ);
	fec_ptp_init_circ(&(priv->tx_time_pdel_resp), DEFAULT_PTP_TX_BUF_SZ);


	spin_lock_init(&priv->ptp_lock);
	ptp_private = priv;

	return 0;
}
EXPORT_SYMBOL(fec_ptp_init);

void fec_ptp_cleanup(struct fec_ptp_private *priv)
{
	if (priv->rx_time_sync.data_buf)
		vfree(priv->rx_time_sync.data_buf);
	if (priv->rx_time_del_req.data_buf)
		vfree(priv->rx_time_del_req.data_buf);
	if (priv->rx_time_pdel_req.data_buf)
		vfree(priv->rx_time_pdel_req.data_buf);
	if (priv->rx_time_pdel_resp.data_buf)
		vfree(priv->rx_time_pdel_resp.data_buf);
	if (priv->tx_time_sync.data_buf)
		vfree(priv->tx_time_sync.data_buf);
	if (priv->tx_time_del_req.data_buf)
		vfree(priv->tx_time_del_req.data_buf);
	if (priv->tx_time_pdel_req.data_buf)
		vfree(priv->tx_time_pdel_req.data_buf);
	if (priv->tx_time_pdel_resp.data_buf)
		vfree(priv->tx_time_pdel_resp.data_buf);

	ptp_free();
}
EXPORT_SYMBOL(fec_ptp_cleanup);

/* probe just register memory and irq */
static int __devinit
ptp_probe(struct platform_device *pdev)
{
	int i, irq, ret = 0;
	struct resource *r;

	/* setup board info structure */
	ptp_dev = kzalloc(sizeof(struct ptp), GFP_KERNEL);
	if (!ptp_dev) {
		ret = -ENOMEM;
		goto err1;
	}
	ptp_dev->rtc = kzalloc(sizeof(struct ptp_rtc),
				GFP_KERNEL);
	if (!ptp_dev->rtc) {
		ret = -ENOMEM;
		goto err2;
	}

	/* PTP register memory */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		ret = -ENXIO;
		goto err3;
	}

	r = request_mem_region(r->start, resource_size(r), pdev->name);
	if (!r) {
		ret = -EBUSY;
		goto err3;
	}

	ptp_dev->mem_map = ioremap(r->start, resource_size(r));
	if (!ptp_dev->mem_map) {
		ret = -ENOMEM;
		goto failed_ioremap;
	}

	/* RTC register memory */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!r) {
		ret = -ENXIO;
		goto err4;
	}

	r = request_mem_region(r->start, resource_size(r), "PTP_RTC");
	if (!r) {
		ret = -EBUSY;
		goto err4;
	}

	ptp_dev->rtc->mem_map = ioremap(r->start, resource_size(r));

	if (!ptp_dev->rtc->mem_map) {
		ret = -ENOMEM;
		goto failed_ioremap1;
	}

	/* This device has up to two irqs on some platforms */
	for (i = 0; i < 2; i++) {
		irq = platform_get_irq(pdev, i);
		if (i && irq < 0)
			break;
		if (i != 0)
			ret = request_irq(irq, ptp_rtc_interrupt,
					IRQF_DISABLED, "ptp_rtc", ptp_dev);
		if (ret) {
			while (i >= 0) {
				irq = platform_get_irq(pdev, i);
				free_irq(irq, ptp_dev);
				i--;
			}
			goto failed_irq;
		}
	}

	ptp_dev->rtc->clk = clk_get(NULL, "ieee_rtc_clk");
	if (IS_ERR(ptp_dev->rtc->clk)) {
		ret = PTR_ERR(ptp_dev->rtc->clk);
		goto failed_clk1;
	}

	ptp_dev->clk = clk_get(&pdev->dev, "ieee_1588_clk");
	if (IS_ERR(ptp_dev->clk)) {
		ret = PTR_ERR(ptp_dev->clk);
		goto failed_clk2;
	}

	clk_enable(ptp_dev->clk);

	init_ptp_driver(ptp_dev);
	clk_disable(ptp_dev->clk);

	return 0;

failed_clk2:
	clk_put(ptp_dev->rtc->clk);
failed_clk1:
	for (i = 0; i < 2; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq > 0)
			free_irq(irq, ptp_dev);
	}
failed_irq:
	iounmap((void __iomem *)ptp_dev->rtc->mem_map);
failed_ioremap1:
err4:
	iounmap((void __iomem *)ptp_dev->mem_map);
failed_ioremap:
err3:
	kfree(ptp_dev->rtc);
err2:
	kfree(ptp_dev);
err1:
	return ret;
}

static int __devexit
ptp_drv_remove(struct platform_device *pdev)
{
	clk_disable(ptp_dev->clk);
	clk_put(ptp_dev->clk);
	clk_put(ptp_dev->rtc->clk);
	iounmap((void __iomem *)ptp_dev->rtc->mem_map);
	iounmap((void __iomem *)ptp_dev->mem_map);
	kfree(ptp_dev->rtc->driver_param);
	kfree(ptp_dev->rtc);
	kfree(ptp_dev->driver_param);
	kfree(ptp_dev);
	return 0;
}

static struct platform_driver ptp_driver = {
	.driver	= {
		.name    = "ptp",
		.owner	 = THIS_MODULE,
	},
	.probe   = ptp_probe,
	.remove  = __devexit_p(ptp_drv_remove),
};

static int __init
ptp_module_init(void)
{
	printk(KERN_INFO "iMX PTP Driver\n");

	return platform_driver_register(&ptp_driver);
}

static void __exit
ptp_cleanup(void)
{
	platform_driver_unregister(&ptp_driver);
}

module_exit(ptp_cleanup);
module_init(ptp_module_init);

MODULE_LICENSE("GPL");
