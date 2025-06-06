// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>
#include <linux/msm-geni-se.h>
#include <linux/ipc_logging.h>
#include <linux/dmaengine.h>
#include <linux/msm_gpi.h>
#include <linux/ioctl.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>
#include <soc/qcom/boot_stats.h>

#ifdef OPLUS_FEATURE_CHG_BASIC
#include <soc/oplus/system/boot_mode.h>
#endif /* OPLUS_FEATURE_CHG_BASIC */

#define SE_GENI_CFG_REG68		(0x210)
#define SE_I2C_TX_TRANS_LEN		(0x26C)
#define SE_I2C_RX_TRANS_LEN		(0x270)
#define SE_I2C_SCL_COUNTERS		(0x278)
#define SE_GENI_IOS			(0x908)

#define SE_I2C_ERR  (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN |\
			M_GP_IRQ_1_EN | M_GP_IRQ_3_EN | M_GP_IRQ_4_EN)
#define SE_I2C_ABORT (1U << 1)
/* M_CMD OP codes for I2C */
#define I2C_WRITE		(0x1)
#define I2C_READ		(0x2)
#define I2C_WRITE_READ		(0x3)
#define I2C_ADDR_ONLY		(0x4)
#define I2C_BUS_CLEAR		(0x6)
#define I2C_STOP_ON_BUS		(0x7)
/* M_CMD params for I2C */
#define PRE_CMD_DELAY		(BIT(0))
#define TIMESTAMP_BEFORE	(BIT(1))
#define STOP_STRETCH		(BIT(2))
#define TIMESTAMP_AFTER		(BIT(3))
#define POST_COMMAND_DELAY	(BIT(4))
#define IGNORE_ADD_NACK		(BIT(6))
#define READ_FINISHED_WITH_ACK	(BIT(7))
#define BYPASS_ADDR_PHASE	(BIT(8))
#define SLV_ADDR_MSK		(GENMASK(15, 9))
#define SLV_ADDR_SHFT		(9)

#define I2C_PACK_EN		(BIT(0) | BIT(1))
#define GP_IRQ0			0
#define GP_IRQ1			1
#define GP_IRQ2			2
#define GP_IRQ3			3
#define GP_IRQ4			4
#define GP_IRQ5			5
#define GENI_OVERRUN		6
#define GENI_ILLEGAL_CMD	7
#define GENI_ABORT_DONE		8
#define GENI_TIMEOUT		9

#define I2C_NACK		GP_IRQ1
#define I2C_BUS_PROTO		GP_IRQ3
#define I2C_ARB_LOST		GP_IRQ4
#define DM_I2C_CB_ERR		((BIT(GP_IRQ1) | BIT(GP_IRQ3) | BIT(GP_IRQ4)) \
									<< 5)

#define I2C_AUTO_SUSPEND_DELAY	250

#define I2C_TIMEOUT_SAFETY_COEFFICIENT	10

#define I2C_TIMEOUT_MIN_USEC	3000000

#define MAX_SE	20

enum i2c_se_mode {
	UNINITIALIZED,
	FIFO_SE_DMA,
	GSI_ONLY,
};

struct dbg_buf_ctxt {
	void *virt_buf;
	void *map_buf;
};

struct geni_i2c_dev {
	struct device *dev;
	void __iomem *base;
	unsigned int tx_wm;
	int irq;
	int err;
	u32 xfer_timeout;
	struct i2c_adapter adap;
	struct completion xfer;
	struct i2c_msg *cur;
	struct se_geni_rsc i2c_rsc;
	int cur_wr;
	int cur_rd;
	struct device *wrapper_dev;
	void *ipcl;
	int clk_fld_idx;
	struct dma_chan *tx_c;
	struct dma_chan *rx_c;
	struct msm_gpi_tre lock_t;
	struct msm_gpi_tre unlock_t;
	struct msm_gpi_tre cfg0_t;
	struct msm_gpi_tre go_t;
	struct msm_gpi_tre tx_t;
	struct msm_gpi_tre rx_t;
	dma_addr_t tx_ph;
	dma_addr_t rx_ph;
	struct msm_gpi_ctrl tx_ev;
	struct msm_gpi_ctrl rx_ev;
	struct scatterlist tx_sg[5]; /* lock, cfg0, go, TX, unlock */
	struct scatterlist rx_sg;
	int cfg_sent;
	struct dma_async_tx_descriptor *tx_desc;
	struct dma_async_tx_descriptor *rx_desc;
	struct msm_gpi_dma_async_tx_cb_param tx_cb;
	struct msm_gpi_dma_async_tx_cb_param rx_cb;
	enum i2c_se_mode se_mode;
	bool cmd_done;
	bool is_shared;
	u32 dbg_num;
	struct dbg_buf_ctxt *dbg_buf_ptr;
	bool is_le_vm;
	bool req_chan;
	bool first_resume;
	bool gpi_reset;
	bool disable_dma_mode;
	bool prev_cancel_pending; //Halt cancel till IOS in good state
	bool is_i2c_rtl_based; /* doing pending cancel only for rtl based SE's */
	atomic_t is_xfer_in_progress; /* Used to maintain xfer inprogress status */
#ifdef OPLUS_FEATURE_CHG_BASIC
	struct delayed_work i2c_gpio_reset_work;
#endif
};

static struct geni_i2c_dev *gi2c_dev_dbg[MAX_SE];
static int arr_idx;
static int geni_i2c_runtime_suspend(struct device *dev);

struct geni_i2c_err_log {
	int err;
	const char *msg;
};

static struct geni_i2c_err_log gi2c_log[] = {
	[GP_IRQ0] = {-EINVAL, "Unknown I2C err GP_IRQ0"},
	[I2C_NACK] = {-ENOTCONN,
			"NACK: slv unresponsive, check its power/reset-ln"},
	[GP_IRQ2] = {-EINVAL, "Unknown I2C err GP IRQ2"},
	[I2C_BUS_PROTO] = {-EPROTO,
				"Bus proto err, noisy/unepxected start/stop"},
	[I2C_ARB_LOST] = {-EBUSY,
				"Bus arbitration lost, clock line undriveable"},
	[GP_IRQ5] = {-EINVAL, "Unknown I2C err GP IRQ5"},
	[GENI_OVERRUN] = {-EIO, "Cmd overrun, check GENI cmd-state machine"},
	[GENI_ILLEGAL_CMD] = {-EILSEQ,
				"Illegal cmd, check GENI cmd-state machine"},
	[GENI_ABORT_DONE] = {-ETIMEDOUT, "Abort after timeout successful"},
	[GENI_TIMEOUT] = {-ETIMEDOUT, "I2C TXN timed out"},
};

struct geni_i2c_clk_fld {
	u32	clk_freq_out;
	u8	clk_div;
	u8	t_high;
	u8	t_low;
	u8	t_cycle;
};

static struct geni_i2c_clk_fld geni_i2c_clk_map[] = {
	{KHz(100), 7, 10, 11, 26},
	{KHz(400), 2,  7, 10, 24},
	{KHz(1000), 1, 3,  9, 18},
};

static int geni_i2c_clk_map_idx(struct geni_i2c_dev *gi2c)
{
	int i;
	int ret = 0;
	bool clk_map_present = false;
	struct geni_i2c_clk_fld *itr = geni_i2c_clk_map;

	for (i = 0; i < ARRAY_SIZE(geni_i2c_clk_map); i++, itr++) {
		if (itr->clk_freq_out == gi2c->i2c_rsc.clk_freq_out) {
			clk_map_present = true;
			break;
		}
	}

	if (clk_map_present)
		gi2c->clk_fld_idx = i;
	else
		ret = -EINVAL;

	return ret;
}

static inline void qcom_geni_i2c_conf(struct geni_i2c_dev *gi2c, int dfs)
{
	struct geni_i2c_clk_fld *itr = geni_i2c_clk_map + gi2c->clk_fld_idx;

	geni_write_reg(dfs, gi2c->base, SE_GENI_CLK_SEL);

	geni_write_reg((itr->clk_div << 4) | 1, gi2c->base, GENI_SER_M_CLK_CFG);
	geni_write_reg(((itr->t_high << 20) | (itr->t_low << 10) |
			itr->t_cycle), gi2c->base, SE_I2C_SCL_COUNTERS);

	/*
	 * Ensure Clk config completes before return.
	 */
	mb();
}

static inline void qcom_geni_i2c_calc_timeout(struct geni_i2c_dev *gi2c)
{

	struct geni_i2c_clk_fld *clk_itr = geni_i2c_clk_map + gi2c->clk_fld_idx;
	size_t bit_cnt = gi2c->cur->len*9;
	size_t bit_usec = (bit_cnt*USEC_PER_SEC)/clk_itr->clk_freq_out;
	size_t xfer_max_usec = (bit_usec*I2C_TIMEOUT_SAFETY_COEFFICIENT) +
							I2C_TIMEOUT_MIN_USEC;

	gi2c->xfer_timeout = usecs_to_jiffies(xfer_max_usec);

}

static void geni_i2c_err(struct geni_i2c_dev *gi2c, int err)
{
	if (err == I2C_NACK || err == GENI_ABORT_DONE) {
		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev, "%s\n",
			     gi2c_log[err].msg);
		goto err_ret;
	} else {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev, "%s\n",
			     gi2c_log[err].msg);
	}
	geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base, gi2c->ipcl);
err_ret:
	gi2c->err = gi2c_log[err].err;
}

static void do_reg68_war_for_rtl_se(struct geni_i2c_dev *gi2c)
{
	u32 status;

	//Add REG68 WAR if stretch bit is set
	status = geni_read_reg_nolog(gi2c->base, SE_GENI_M_CMD0);
	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
		"%s: SE_GENI_M_CMD0:0x%x\n", __func__,  status);

	//BIT(2) - STOP/STRETCH set then configure REG68 register
	if ((status & 0x4) && gi2c->is_i2c_rtl_based) {
		status = geni_read_reg_nolog(gi2c->base, SE_GENI_CFG_REG68);
		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
			"%s: Before WAR REG68:0x%x\n", __func__, status);
		if (status & 0x20) {
			//Toggle Bit#4, Bit#5 of REG68 to disable/enable stretch
			geni_write_reg(0x00100110, gi2c->base,
							SE_GENI_CFG_REG68);
		} else {
			//Restore FW to suggested value i.e. 0x00100120
			geni_write_reg(0x00100120, gi2c->base,
							SE_GENI_CFG_REG68);
		}
		status = geni_read_reg_nolog(gi2c->base, SE_GENI_CFG_REG68);
		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"%s: After WAR REG68:0x%x\n", __func__, status);
	}
}

static int do_pending_cancel(struct geni_i2c_dev *gi2c)
{
	int timeout = 0;
	u32 geni_ios = 0;

	/* doing pending cancel only rtl based SE's */
	if (!gi2c->is_i2c_rtl_based)
		return 0;

	geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
	if ((geni_ios & 0x3) != 0x3) {
		/* Try to restore IOS with FORCE_DEFAULT */
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			"%s: IOS:0x%x, bad state\n", __func__, geni_ios);

		geni_write_reg(FORCE_DEFAULT,
			gi2c->base, GENI_FORCE_DEFAULT_REG);
		geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
		if ((geni_ios & 0x3) != 0x3) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s: IOS:0x%x, Fix from Slave side\n",
				__func__, geni_ios);
			return -EINVAL;
		}
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			"%s: IOS:0x%x restored properly\n", __func__, geni_ios);
	}

	if (gi2c->se_mode == GSI_ONLY) {
		dmaengine_terminate_all(gi2c->tx_c);
		gi2c->cfg_sent = 0;
	} else {
		reinit_completion(&gi2c->xfer);
		geni_cancel_m_cmd(gi2c->base);
		timeout = wait_for_completion_timeout(&gi2c->xfer, HZ);
		if (!timeout) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s:Pending Cancel failed\n", __func__);
			reinit_completion(&gi2c->xfer);
			geni_abort_m_cmd(gi2c->base);
			timeout = wait_for_completion_timeout(&gi2c->xfer, HZ);
			if (!timeout)
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s:Abort failed\n", __func__);
		}
	}
	gi2c->prev_cancel_pending = false;
	GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			"%s: Pending Cancel done\n", __func__);
	return timeout;
}

static int geni_i2c_prepare(struct geni_i2c_dev *gi2c)
{
	if (gi2c->se_mode == UNINITIALIZED) {
		int proto = get_se_proto(gi2c->base);
		u32 se_mode;
		int ret = 0;

		if (proto != I2C) {
			dev_err(gi2c->dev, "Invalid proto %d\n", proto);
			if (!gi2c->is_le_vm) {
				ret = se_geni_resources_off(&gi2c->i2c_rsc);
				if (ret) {
					dev_err(gi2c->dev, "%s: resource_off Error ret %d\n",
						__func__, ret);
					return ret;
				}
			}
			return -ENXIO;
		}

		se_mode = readl_relaxed(gi2c->base +
					GENI_IF_FIFO_DISABLE_RO);
		if (se_mode) {
			gi2c->se_mode = GSI_ONLY;
			geni_se_select_mode(gi2c->base, GSI_DMA);
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
					"i2c in GSI ONLY mode\n");
		} else {
			int gi2c_tx_depth = get_tx_fifo_depth(gi2c->base);

			gi2c->se_mode = FIFO_SE_DMA;

			gi2c->tx_wm = gi2c_tx_depth - 1;
			geni_se_init(gi2c->base, gi2c->tx_wm, gi2c_tx_depth);
			qcom_geni_i2c_conf(gi2c, 0);
			se_config_packing(gi2c->base, 8, 4, true);
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
					"i2c fifo/se-dma mode. fifo depth:%d\n",
					gi2c_tx_depth);
		}
	}
	return 0;
}

static irqreturn_t geni_i2c_irq(int irq, void *dev)
{
	struct geni_i2c_dev *gi2c = dev;
	int i, j;
	u32 m_stat = readl_relaxed(gi2c->base + SE_GENI_M_IRQ_STATUS);
	u32 rx_st = readl_relaxed(gi2c->base + SE_GENI_RX_FIFO_STATUS);
	u32 dm_tx_st = readl_relaxed(gi2c->base + SE_DMA_TX_IRQ_STAT);
	u32 dm_rx_st = readl_relaxed(gi2c->base + SE_DMA_RX_IRQ_STAT);
	u32 dma = readl_relaxed(gi2c->base + SE_GENI_DMA_MODE_EN);
	struct i2c_msg *cur = gi2c->cur;

	if (!cur) {
		geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base, gi2c->ipcl);
		GENI_SE_ERR(gi2c->ipcl, false, gi2c->dev, "Spurious irq\n");
		goto irqret;
	}
	GENI_SE_ERR(gi2c->ipcl, false, gi2c->dev,
		    "m_stat:0x%x rx_st:0x%x dm_tx_st:0x%x dm_rx_st:0x%x dma:0x%x\n",
		    m_stat, rx_st, dm_tx_st, dm_rx_st, dma);

	if ((m_stat & M_CMD_FAILURE_EN) ||
		    (dm_rx_st & (DM_I2C_CB_ERR)) ||
		    (m_stat & M_CMD_CANCEL_EN) ||
		    (m_stat & M_CMD_ABORT_EN) ||
		    (m_stat & M_GP_IRQ_1_EN)) {

		if (m_stat & M_GP_IRQ_1_EN)
			geni_i2c_err(gi2c, I2C_NACK);
		if (m_stat & M_GP_IRQ_3_EN)
			geni_i2c_err(gi2c, I2C_BUS_PROTO);
		if (m_stat & M_GP_IRQ_4_EN)
			geni_i2c_err(gi2c, I2C_ARB_LOST);
		if (m_stat & M_CMD_OVERRUN_EN)
			geni_i2c_err(gi2c, GENI_OVERRUN);
		if (m_stat & M_ILLEGAL_CMD_EN)
			geni_i2c_err(gi2c, GENI_ILLEGAL_CMD);
		if (m_stat & M_CMD_ABORT_EN)
			geni_i2c_err(gi2c, GENI_ABORT_DONE);
		if (m_stat & M_GP_IRQ_0_EN)
			geni_i2c_err(gi2c, GP_IRQ0);

		if (!dma)
			writel_relaxed(0, (gi2c->base +
					   SE_GENI_TX_WATERMARK_REG));
		gi2c->cmd_done = true;
		goto irqret;
	}

	if (((m_stat & M_RX_FIFO_WATERMARK_EN) ||
		(m_stat & M_RX_FIFO_LAST_EN)) && (cur->flags & I2C_M_RD)) {
		u32 rxcnt = rx_st & RX_FIFO_WC_MSK;

		for (j = 0; j < rxcnt; j++) {
			u32 temp;
			int p;

			temp = readl_relaxed(gi2c->base + SE_GENI_RX_FIFOn);
			for (i = gi2c->cur_rd, p = 0; (i < cur->len && p < 4);
				i++, p++)
				cur->buf[i] = (u8) ((temp >> (p * 8)) & 0xff);
			gi2c->cur_rd = i;
			if (gi2c->cur_rd == cur->len) {
				dev_dbg(gi2c->dev, "FIFO i:%d,read 0x%x\n",
					i, temp);
				break;
			}
		}
	} else if ((m_stat & M_TX_FIFO_WATERMARK_EN) &&
					!(cur->flags & I2C_M_RD)) {
		for (j = 0; j < gi2c->tx_wm; j++) {
			u32 temp = 0;
			int p;

			for (i = gi2c->cur_wr, p = 0; (i < cur->len && p < 4);
				i++, p++)
				temp |= (((u32)(cur->buf[i]) << (p * 8)));
			writel_relaxed(temp, gi2c->base + SE_GENI_TX_FIFOn);
			gi2c->cur_wr = i;
			dev_dbg(gi2c->dev, "FIFO i:%d,wrote 0x%x\n", i, temp);
			if (gi2c->cur_wr == cur->len) {
				dev_dbg(gi2c->dev, "FIFO i2c bytes done writing\n");
				writel_relaxed(0,
				(gi2c->base + SE_GENI_TX_WATERMARK_REG));
				break;
			}
		}
	}
irqret:
	if (m_stat)
		writel_relaxed(m_stat, gi2c->base + SE_GENI_M_IRQ_CLEAR);

	if (dma) {
		if (dm_tx_st)
			writel_relaxed(dm_tx_st, gi2c->base +
				       SE_DMA_TX_IRQ_CLR);

		if (dm_rx_st)
			writel_relaxed(dm_rx_st, gi2c->base +
				       SE_DMA_RX_IRQ_CLR);
		/* Ensure all writes are done before returning from ISR. */
		wmb();

		if ((dm_tx_st & TX_DMA_DONE) || (dm_rx_st & RX_DMA_DONE))
			gi2c->cmd_done = true;
	}

	else if (m_stat & M_CMD_DONE_EN)
		gi2c->cmd_done = true;

	if (gi2c->cmd_done) {
		gi2c->cmd_done = false;
		complete(&gi2c->xfer);
	}

	return IRQ_HANDLED;
}

static void gi2c_ev_cb(struct dma_chan *ch, struct msm_gpi_cb const *cb_str,
		       void *ptr)
{
	struct geni_i2c_dev *gi2c;
	u32 m_stat;

	if (!ptr || !cb_str) {
		pr_err("%s: Invalid ev_cb buffer\n", __func__);
		return;
	}

	gi2c = (struct geni_i2c_dev *)ptr;
	m_stat = cb_str->status;

	switch (cb_str->cb_event) {
	case MSM_GPI_QUP_ERROR:
	case MSM_GPI_QUP_SW_ERROR:
	case MSM_GPI_QUP_MAX_EVENT:
		/* fall through to stall impacted channel */
	case MSM_GPI_QUP_CH_ERROR:
	case MSM_GPI_QUP_FW_ERROR:
	case MSM_GPI_QUP_PENDING_EVENT:
	case MSM_GPI_QUP_EOT_DESC_MISMATCH:
		break;
	case MSM_GPI_QUP_NOTIFY:
		if (m_stat & M_GP_IRQ_1_EN)
			geni_i2c_err(gi2c, I2C_NACK);
		if (m_stat & M_GP_IRQ_3_EN)
			geni_i2c_err(gi2c, I2C_BUS_PROTO);
		if (m_stat & M_GP_IRQ_4_EN)
			geni_i2c_err(gi2c, I2C_ARB_LOST);
		break;
	default:
		break;
	}
	complete(&gi2c->xfer);
	if (cb_str->cb_event != MSM_GPI_QUP_NOTIFY)
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"GSI QN err:0x%x, status:0x%x, err:%d\n",
				cb_str->error_log.error_code,
				m_stat, cb_str->cb_event);
}

static void gi2c_gsi_cb_err(struct msm_gpi_dma_async_tx_cb_param *cb,
								char *xfer)
{
	struct geni_i2c_dev *gi2c = cb->userdata;

	if (cb->status & DM_I2C_CB_ERR) {
		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
			    "%s TCE Unexpected Err, stat:0x%x\n",
				xfer, cb->status);
		if (cb->status & (BIT(GP_IRQ1) << 5))
			geni_i2c_err(gi2c, I2C_NACK);
		if (cb->status & (BIT(GP_IRQ3) << 5))
			geni_i2c_err(gi2c, I2C_BUS_PROTO);
		if (cb->status & (BIT(GP_IRQ4) << 5))
			geni_i2c_err(gi2c, I2C_ARB_LOST);
	}
}

static void gi2c_gsi_tx_cb(void *ptr)
{
	struct msm_gpi_dma_async_tx_cb_param *tx_cb = ptr;
	struct geni_i2c_dev *gi2c = tx_cb->userdata;

	gi2c_gsi_cb_err(tx_cb, "TX");
	complete(&gi2c->xfer);
}

static void gi2c_gsi_rx_cb(void *ptr)
{
	struct msm_gpi_dma_async_tx_cb_param *rx_cb = ptr;
	struct geni_i2c_dev *gi2c = rx_cb->userdata;

	if (gi2c->cur == NULL) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			"%s: Error: unexpected callback\n", __func__);
		WARN_ON(1);
		return;
	}

	if (gi2c->cur->flags & I2C_M_RD) {
		gi2c_gsi_cb_err(rx_cb, "RX");
		complete(&gi2c->xfer);
	}
}

static int geni_i2c_gsi_request_channel(struct geni_i2c_dev *gi2c)
{
	int ret = 0;

	if (!gi2c->tx_c) {
		gi2c->tx_c = dma_request_slave_channel(gi2c->dev, "tx");
		if (!gi2c->tx_c) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    "tx dma req slv chan ret :%d\n", ret);
			return -EIO;
		}
	}

	if (!gi2c->rx_c) {
		gi2c->rx_c = dma_request_slave_channel(gi2c->dev, "rx");
		if (!gi2c->rx_c) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    "rx dma req slv chan ret :%d\n", ret);
			dma_release_channel(gi2c->tx_c);
			return -EIO;
		}
	}

	gi2c->tx_ev.init.callback = gi2c_ev_cb;
	gi2c->tx_ev.init.cb_param = gi2c;
	gi2c->tx_ev.cmd = MSM_GPI_INIT;
	gi2c->tx_c->private = &gi2c->tx_ev;
	ret = dmaengine_slave_config(gi2c->tx_c, NULL);
	if (ret) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"tx dma slave config ret :%d\n", ret);
		goto dmaengine_slave_config_fail;
	}

	gi2c->rx_ev.init.cb_param = gi2c;
	gi2c->rx_ev.init.callback = gi2c_ev_cb;
	gi2c->rx_ev.cmd = MSM_GPI_INIT;
	gi2c->rx_c->private = &gi2c->rx_ev;
	ret = dmaengine_slave_config(gi2c->rx_c, NULL);
	if (ret) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"rx dma slave config ret :%d\n", ret);
		goto dmaengine_slave_config_fail;
	}

	gi2c->tx_cb.userdata = gi2c;
	gi2c->rx_cb.userdata = gi2c;
	gi2c->req_chan = true;

	return ret;

dmaengine_slave_config_fail:
	dma_release_channel(gi2c->tx_c);
	dma_release_channel(gi2c->rx_c);
	gi2c->tx_c = NULL;
	gi2c->rx_c = NULL;
	return ret;
}

static struct msm_gpi_tre *setup_lock_tre(struct geni_i2c_dev *gi2c)
{
	struct msm_gpi_tre *lock_t = &gi2c->lock_t;

	/* lock: chain bit set */
	lock_t->dword[0] = MSM_GPI_LOCK_TRE_DWORD0;
	lock_t->dword[1] = MSM_GPI_LOCK_TRE_DWORD1;
	lock_t->dword[2] = MSM_GPI_LOCK_TRE_DWORD2;
	/* ieob for le-vm and chain for shared se */
	if (gi2c->is_shared)
		lock_t->dword[3] = MSM_GPI_LOCK_TRE_DWORD3(0, 0, 0, 0, 1);
	else if (gi2c->is_le_vm)
		lock_t->dword[3] = MSM_GPI_LOCK_TRE_DWORD3(0, 0, 0, 1, 0);

	return lock_t;
}

static struct msm_gpi_tre *setup_cfg0_tre(struct geni_i2c_dev *gi2c)
{
	struct geni_i2c_clk_fld *itr = geni_i2c_clk_map +
							gi2c->clk_fld_idx;
	struct msm_gpi_tre *cfg0_t = &gi2c->cfg0_t;

	/* config0 */
	cfg0_t->dword[0] = MSM_GPI_I2C_CONFIG0_TRE_DWORD0(I2C_PACK_EN,
				itr->t_cycle, itr->t_high, itr->t_low);
	cfg0_t->dword[1] = MSM_GPI_I2C_CONFIG0_TRE_DWORD1(0, 0);
	cfg0_t->dword[2] = MSM_GPI_I2C_CONFIG0_TRE_DWORD2(0,
							itr->clk_div);
	cfg0_t->dword[3] = MSM_GPI_I2C_CONFIG0_TRE_DWORD3(0, 0, 0, 0, 1);

	return cfg0_t;
}

static struct msm_gpi_tre *setup_go_tre(struct geni_i2c_dev *gi2c,
				struct i2c_msg msgs[], int i, int num)
{
	struct msm_gpi_tre *go_t = &gi2c->go_t;
	u8 op = (msgs[i].flags & I2C_M_RD) ? 2 : 1;
	int stretch = (i < (num - 1));

	go_t->dword[0] = MSM_GPI_I2C_GO_TRE_DWORD0((stretch << 2),
							   msgs[i].addr, op);
	go_t->dword[1] = MSM_GPI_I2C_GO_TRE_DWORD1;

	if (msgs[i].flags & I2C_M_RD) {
		go_t->dword[2] = MSM_GPI_I2C_GO_TRE_DWORD2(msgs[i].len);
		/*
		 * For Rx Go tre: Set ieob for non-shared se and for all
		 * but last transfer in shared se
		 */
		if (!gi2c->is_shared || (gi2c->is_shared && i != num-1))
			go_t->dword[3] = MSM_GPI_I2C_GO_TRE_DWORD3(1, 0,
							0, 1, 0);
		else
			go_t->dword[3] = MSM_GPI_I2C_GO_TRE_DWORD3(1, 0,
							0, 0, 0);
	} else {
		/* For Tx Go tre: ieob is not set, chain bit is set */
		go_t->dword[2] = MSM_GPI_I2C_GO_TRE_DWORD2(0);
		go_t->dword[3] = MSM_GPI_I2C_GO_TRE_DWORD3(0, 0, 0, 0,
								1);
	}

	return go_t;
}

static struct msm_gpi_tre *setup_rx_tre(struct geni_i2c_dev *gi2c,
				struct i2c_msg msgs[], int i, int num)
{
	struct msm_gpi_tre *rx_t = &gi2c->rx_t;

	rx_t->dword[0] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD0(gi2c->rx_ph);
	rx_t->dword[1] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD1(gi2c->rx_ph);
	rx_t->dword[2] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD2(msgs[i].len);
	/* Set ieot for all Rx/Tx DMA tres */
	rx_t->dword[3] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD3(0, 0, 1, 0, 0);

	return rx_t;
}

static struct msm_gpi_tre *setup_tx_tre(struct geni_i2c_dev *gi2c,
				struct i2c_msg msgs[], int i, int num)
{
	struct msm_gpi_tre *tx_t = &gi2c->tx_t;

	tx_t->dword[0] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD0(gi2c->tx_ph);
	tx_t->dword[1] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD1(gi2c->tx_ph);
	tx_t->dword[2] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD2(msgs[i].len);
	if (gi2c->is_shared && i == num-1)
		/*
		 * For Tx: unlock tre is send for last transfer
		 * so set chain bit for last transfer DMA tre.
		 */
		tx_t->dword[3] =
		MSM_GPI_DMA_W_BUFFER_TRE_DWORD3(0, 0, 1, 0, 1);
	else
		tx_t->dword[3] =
		MSM_GPI_DMA_W_BUFFER_TRE_DWORD3(0, 0, 1, 0, 0);

	return tx_t;
}

static struct msm_gpi_tre *setup_unlock_tre(struct geni_i2c_dev *gi2c)
{
	struct msm_gpi_tre *unlock_t = &gi2c->unlock_t;

	/* unlock tre: ieob set */
	unlock_t->dword[0] = MSM_GPI_UNLOCK_TRE_DWORD0;
	unlock_t->dword[1] = MSM_GPI_UNLOCK_TRE_DWORD1;
	unlock_t->dword[2] = MSM_GPI_UNLOCK_TRE_DWORD2;
	unlock_t->dword[3] = MSM_GPI_UNLOCK_TRE_DWORD3(0, 0, 0, 1, 0);

	return unlock_t;
}

static struct dma_async_tx_descriptor *geni_i2c_prep_desc
(struct geni_i2c_dev *gi2c, struct dma_chan *chan, int segs, bool tx_chan)
{
	struct dma_async_tx_descriptor *geni_desc = NULL;

	if (tx_chan) {
		geni_desc = dmaengine_prep_slave_sg(gi2c->tx_c, gi2c->tx_sg,
					segs, DMA_MEM_TO_DEV,
					(DMA_PREP_INTERRUPT | DMA_CTRL_ACK));
		if (!geni_desc) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					"prep_slave_sg for tx failed\n");
			gi2c->err = -ENOMEM;
			return NULL;
		}
		geni_desc->callback = gi2c_gsi_tx_cb;
		geni_desc->callback_param = &gi2c->tx_cb;
	} else {
		geni_desc = dmaengine_prep_slave_sg(gi2c->rx_c,
					&gi2c->rx_sg, 1, DMA_DEV_TO_MEM,
					(DMA_PREP_INTERRUPT | DMA_CTRL_ACK));
		if (!geni_desc) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					"prep_slave_sg for rx failed\n");
			gi2c->err = -ENOMEM;
			return NULL;
		}
		geni_desc->callback = gi2c_gsi_rx_cb;
		geni_desc->callback_param = &gi2c->rx_cb;
	}

	return geni_desc;
}

static int geni_i2c_lock_bus(struct geni_i2c_dev *gi2c)
{
	struct msm_gpi_tre *lock_t = NULL;
	int ret = 0, timeout = 0;
	dma_cookie_t tx_cookie;
	bool tx_chan = true;

	if (!gi2c->req_chan) {
		ret = geni_i2c_gsi_request_channel(gi2c);
		if (ret)
			return ret;
	}

	lock_t = setup_lock_tre(gi2c);
	sg_init_table(gi2c->tx_sg, 1);
	sg_set_buf(&gi2c->tx_sg[0], lock_t,
					sizeof(gi2c->lock_t));

	gi2c->tx_desc = geni_i2c_prep_desc(gi2c, gi2c->tx_c, 1, tx_chan);
	if (!gi2c->tx_desc) {
		gi2c->err = -ENOMEM;
		goto geni_i2c_err_lock_bus;
	}

	reinit_completion(&gi2c->xfer);
	/* Issue TX */
	tx_cookie = dmaengine_submit(gi2c->tx_desc);
	dma_async_issue_pending(gi2c->tx_c);

	timeout = wait_for_completion_timeout(&gi2c->xfer, HZ);
	if (!timeout) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s timedout\n", __func__);
		geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base,
					gi2c->ipcl);
		gi2c->err = -ETIMEDOUT;
		goto geni_i2c_err_lock_bus;
	}
	return 0;

geni_i2c_err_lock_bus:
	if (gi2c->err) {
		dmaengine_terminate_all(gi2c->tx_c);
		gi2c->cfg_sent = 0;
	}
	return gi2c->err;
}

static void geni_i2c_unlock_bus(struct geni_i2c_dev *gi2c)
{
	struct msm_gpi_tre *unlock_t = NULL;
	int timeout = 0;
	dma_cookie_t tx_cookie;
	bool tx_chan = true;

	if (gi2c->gpi_reset)
		goto geni_i2c_err_unlock_bus;

	unlock_t = setup_unlock_tre(gi2c);
	sg_init_table(gi2c->tx_sg, 1);
	sg_set_buf(&gi2c->tx_sg[0], unlock_t,
					sizeof(gi2c->unlock_t));

	gi2c->tx_desc = geni_i2c_prep_desc(gi2c, gi2c->tx_c, 1, tx_chan);
	if (!gi2c->tx_desc) {
		gi2c->err = -ENOMEM;
		goto geni_i2c_err_unlock_bus;
	}

	reinit_completion(&gi2c->xfer);
	/* Issue TX */
	tx_cookie = dmaengine_submit(gi2c->tx_desc);
	dma_async_issue_pending(gi2c->tx_c);

	timeout = wait_for_completion_timeout(&gi2c->xfer, HZ);
	if (!timeout) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s failed\n", __func__);
		geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base,
					gi2c->ipcl);
		gi2c->err = -ETIMEDOUT;
		goto geni_i2c_err_unlock_bus;
	}

geni_i2c_err_unlock_bus:
	if (gi2c->gpi_reset || gi2c->err) {
		dmaengine_terminate_all(gi2c->tx_c);
		gi2c->cfg_sent = 0;
		gi2c->err = 0;
		gi2c->gpi_reset = false;
	}
}

static int geni_i2c_gsi_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			     int num)
{
	struct geni_i2c_dev *gi2c = i2c_get_adapdata(adap);
	int i, ret = 0, timeout = 0;
	struct msm_gpi_tre *lock_t = NULL;
	struct msm_gpi_tre *unlock_t = NULL;
	struct msm_gpi_tre *cfg0_t = NULL;

	if (!gi2c->req_chan) {
		ret = geni_i2c_gsi_request_channel(gi2c);
		if (ret)
			return ret;
	}

	if (gi2c->is_shared) {
		lock_t = setup_lock_tre(gi2c);
		unlock_t = setup_unlock_tre(gi2c);
	}

	if (!gi2c->cfg_sent)
		cfg0_t = setup_cfg0_tre(gi2c);

	for (i = 0; i < num; i++) {
		u8 op = (msgs[i].flags & I2C_M_RD) ? 2 : 1;
		int segs = 3 - op;
		int index = 0;
		u8 *dma_buf = NULL;
		dma_cookie_t tx_cookie, rx_cookie;
		struct msm_gpi_tre *go_t = NULL;
		struct msm_gpi_tre *rx_t = NULL;
		struct msm_gpi_tre *tx_t = NULL;
		struct device *rx_dev = gi2c->wrapper_dev;
		struct device *tx_dev = gi2c->wrapper_dev;
		bool tx_chan = true;

		reinit_completion(&gi2c->xfer);
		gi2c->cur = &msgs[i];

		dma_buf = i2c_get_dma_safe_msg_buf(&msgs[i], 1);
		if (!dma_buf) {
			ret = -ENOMEM;
			goto geni_i2c_gsi_xfer_out;
		}

		qcom_geni_i2c_calc_timeout(gi2c);

		if (!gi2c->cfg_sent)
			segs++;
		if (gi2c->is_shared && (i == 0 || i == num-1)) {
			segs++;
			if (num == 1)
				segs++;
			sg_init_table(gi2c->tx_sg, segs);
			if (i == 0)
				/* Send lock tre for first transfer in a msg */
				sg_set_buf(&gi2c->tx_sg[index++], lock_t,
					sizeof(gi2c->lock_t));
		} else {
			sg_init_table(gi2c->tx_sg, segs);
		}

		/* Send cfg tre when cfg not sent already */
		if (!gi2c->cfg_sent) {
			sg_set_buf(&gi2c->tx_sg[index++], cfg0_t,
						sizeof(gi2c->cfg0_t));
			gi2c->cfg_sent = 1;
		}

		go_t = setup_go_tre(gi2c, msgs, i, num);
		sg_set_buf(&gi2c->tx_sg[index++], go_t,
						  sizeof(gi2c->go_t));

		if (msgs[i].flags & I2C_M_RD) {
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"msg[%d].len:%d R\n", i, gi2c->cur->len);
			sg_init_table(&gi2c->rx_sg, 1);
			ret = geni_se_iommu_map_buf(rx_dev, &gi2c->rx_ph,
						dma_buf, msgs[i].len,
						DMA_FROM_DEVICE);
			if (ret) {
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					    "geni_se_iommu_map_buf for rx failed :%d\n",
					    ret);
				i2c_put_dma_safe_msg_buf(dma_buf, &msgs[i],
								false);
				goto geni_i2c_gsi_xfer_out;

			} else if (gi2c->dbg_buf_ptr) {
				gi2c->dbg_buf_ptr[i].virt_buf =
							(void *)dma_buf;
				gi2c->dbg_buf_ptr[i].map_buf =
							(void *)&gi2c->rx_ph;
			}

			rx_t = setup_rx_tre(gi2c, msgs, i, num);
			sg_set_buf(&gi2c->rx_sg, rx_t,
						 sizeof(gi2c->rx_t));

			gi2c->rx_desc =
			geni_i2c_prep_desc(gi2c, gi2c->rx_c, segs, !tx_chan);
			if (!gi2c->rx_desc) {
				gi2c->err = -ENOMEM;
				goto geni_i2c_err_prep_sg;
			}

			/* Issue RX */
			rx_cookie = dmaengine_submit(gi2c->rx_desc);
			dma_async_issue_pending(gi2c->rx_c);
		} else {
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"msg[%d].len:%d W\n", i, gi2c->cur->len);
			ret = geni_se_iommu_map_buf(tx_dev, &gi2c->tx_ph,
						dma_buf, msgs[i].len,
						DMA_TO_DEVICE);
			if (ret) {
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					    "geni_se_iommu_map_buf for tx failed :%d\n",
					    ret);
				i2c_put_dma_safe_msg_buf(dma_buf, &msgs[i],
								false);
				goto geni_i2c_gsi_xfer_out;

			} else if (gi2c->dbg_buf_ptr) {
				gi2c->dbg_buf_ptr[i].virt_buf =
							(void *)dma_buf;
				gi2c->dbg_buf_ptr[i].map_buf =
							(void *)&gi2c->tx_ph;
			}

			tx_t = setup_tx_tre(gi2c, msgs, i, num);
			sg_set_buf(&gi2c->tx_sg[index++], tx_t,
							  sizeof(gi2c->tx_t));
		}

		if (gi2c->is_shared && i == num-1) {
			/* Send unlock tre at the end of last transfer */
			sg_set_buf(&gi2c->tx_sg[index++],
				unlock_t, sizeof(gi2c->unlock_t));
		}

		gi2c->tx_desc =
		geni_i2c_prep_desc(gi2c, gi2c->tx_c, segs, tx_chan);
		if (!gi2c->tx_desc) {
			gi2c->err = -ENOMEM;
			goto geni_i2c_err_prep_sg;
		}

		/* Issue TX */
		tx_cookie = dmaengine_submit(gi2c->tx_desc);
		dma_async_issue_pending(gi2c->tx_c);

		timeout = wait_for_completion_timeout(&gi2c->xfer,
						gi2c->xfer_timeout);
		if (!timeout) {
			u32 geni_ios = 0;

			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"I2C gsi xfer timeout:%u flags:%d addr:0x%x\n",
				gi2c->xfer_timeout, gi2c->cur->flags,
				gi2c->cur->addr);
			geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base,
						gi2c->ipcl);
			gi2c->err = -ETIMEDOUT;

			/* WAR: Set flag to mark cancel pending if IOS stuck */
			geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
			if ((geni_ios & 0x3) != 0x3) { //SCL:b'1, SDA:b'0
				GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
					"%s: IO lines not in good state\n", __func__);
					/* doing pending cancel only rtl based SE's */
					if (gi2c->is_i2c_rtl_based) {
						gi2c->prev_cancel_pending = true;
						goto geni_i2c_gsi_cancel_pending;
					}
			}
		}
geni_i2c_err_prep_sg:
		if (gi2c->err) {
			if (!gi2c->is_le_vm) {
				dmaengine_terminate_all(gi2c->tx_c);
				gi2c->cfg_sent = 0;
			} else {
				/* Stop channel in case of error in LE-VM */
				ret = dmaengine_pause(gi2c->tx_c);
				if (ret) {
					gi2c->gpi_reset = true;
					gi2c->err = ret;
					GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
						"Channel cancel failed\n");
					goto geni_i2c_gsi_xfer_out;
				}
			}
		}
		if (gi2c->is_shared)
			/* Resend cfg tre for every new message on shared se */
			gi2c->cfg_sent = 0;

geni_i2c_gsi_cancel_pending:
		if (msgs[i].flags & I2C_M_RD)
			geni_se_iommu_unmap_buf(rx_dev, &gi2c->rx_ph,
				msgs[i].len, DMA_FROM_DEVICE);
		else
			geni_se_iommu_unmap_buf(tx_dev, &gi2c->tx_ph,
				msgs[i].len, DMA_TO_DEVICE);
		i2c_put_dma_safe_msg_buf(dma_buf, &msgs[i], !gi2c->err);
		if (gi2c->err)
			goto geni_i2c_gsi_xfer_out;
	}

geni_i2c_gsi_xfer_out:
	if (!ret && gi2c->err)
		ret = gi2c->err;
	return ret;
}

#ifdef OPLUS_FEATURE_CHG_BASIC
#define MAX_RESET_COUNT			10
#define I2C_MAX_ERROR_COUNT 		2
#define FG_DEVICE_ADDR			0x55
#define BQ25890H_DEVICE_ADDR		0x6a
#define DEVICE_TYPE_ZY0602		3
static bool (*poplus_vooc_get_fastchg_started)(void);
static bool (*poplus_vooc_get_fastchg_ing)(void);
static bool i2c_err_occured = false;
static int fg_device_type = 0;
static unsigned int err_count = 0;
int oplus_get_fg_device_type(void)
{
	/* pr_err("oplus_get_fg_device_type fg_device_type[%d]\n", fg_device_type); */
	return fg_device_type;
}
EXPORT_SYMBOL(oplus_get_fg_device_type);

void oplus_set_fg_device_type(int device_type)
{
	pr_err("oplus_set_fg_device_type fg_device_type[%d]\n", fg_device_type);
	fg_device_type = device_type;
	return;
}
EXPORT_SYMBOL(oplus_set_fg_device_type);

bool oplus_get_fg_i2c_err_occured(void)
{
	return i2c_err_occured;
}
EXPORT_SYMBOL(oplus_get_fg_i2c_err_occured);

void oplus_set_fg_i2c_err_occured(bool i2c_err)
{
	i2c_err_occured = i2c_err;
}
EXPORT_SYMBOL(oplus_set_fg_i2c_err_occured);

void oplus_vooc_get_fastchg_started_pfunc(bool (*pfunc)(void))
{
	poplus_vooc_get_fastchg_started = pfunc;
}
EXPORT_SYMBOL(oplus_vooc_get_fastchg_started_pfunc);

void oplus_vooc_get_fastchg_ing_pfunc(bool (*pfunc)(void))
{
	poplus_vooc_get_fastchg_ing = pfunc;
}
EXPORT_SYMBOL(oplus_vooc_get_fastchg_ing_pfunc);

static bool oplus_vooc_get_fastchg_started(void)
{
	bool ret = false;

	if (poplus_vooc_get_fastchg_started == NULL) {
		ret = false;
	} else {
		ret = poplus_vooc_get_fastchg_started();
	}

	return ret;
}

static bool oplus_vooc_get_fastchg_ing(void)
{
	bool ret = false;

	if (poplus_vooc_get_fastchg_ing == NULL) {
		ret = false;
	} else {
		ret = poplus_vooc_get_fastchg_ing();
	}

	return ret;
}

static bool i2c_reset_processing = false;
static int reset_count = 0;
static atomic_t i2c_reset_status;

#define I2C_RST_DELAY_CNT	250
#define I2C_RST_MAX_COUNT       5
static void oplus_i2c_gpio_reset_work(struct work_struct *work)
{
	int ret = 0;
	int i = 0;
	int boot_mode = get_boot_mode();
	struct delayed_work *dwork = to_delayed_work(work);
	struct geni_i2c_dev *gi2c = container_of(dwork,
					struct geni_i2c_dev, i2c_gpio_reset_work);

	if (gi2c == NULL) {
		pr_err("gi2c dev is NULL");
		return;
	}

	if ((boot_mode != MSM_BOOT_MODE__NORMAL)
			&& (boot_mode != MSM_BOOT_MODE__RECOVERY)
			&& (boot_mode != MSM_BOOT_MODE__SILENCE)
			&& (boot_mode != MSM_BOOT_MODE__SAU)
			&& (boot_mode != MSM_BOOT_MODE__CHARGE)) {
		dev_err(gi2c->dev, "%s: get_boot_mode[%d], return\n", __func__, boot_mode);
		return;
	}

	if (i2c_reset_processing == true) {
		dev_err(gi2c->dev, "%s: i2c_reset is processing, return\n", __func__);
		return;
	}

	if (reset_count > I2C_RST_MAX_COUNT) {
		dev_err(gi2c->dev, "%s: i2c reset_count=%d >max_reset_count=%d, return\n",
				__func__, reset_count, I2C_RST_MAX_COUNT);
                return;
	}

	dev_err(gi2c->dev, "%s: start, reset_count = %d\n", __func__, reset_count);

	i2c_reset_processing = true;
	reset_count++;

	if (!IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_pulldown)) {
		dev_err(gi2c->dev, "%s: set geni_gpio_pulldown\n", __func__);
		ret = pinctrl_select_state(gi2c->i2c_rsc.geni_pinctrl, gi2c->i2c_rsc.geni_gpio_pulldown);
		if (ret) {
			dev_err(gi2c->dev, "%s: error pinctrl_select_state pulldown, ret:%d\n", __func__, ret);
			goto err;
		}
	} else {
		goto err;
	}

	for (i = 0; i < I2C_RST_DELAY_CNT; i++) {
		usleep_range(10000, 11000);
		if (oplus_vooc_get_fastchg_started() == true && oplus_vooc_get_fastchg_ing() == false) {
			dev_err(gi2c->dev, "%s: vooc ready to start, don't pull down i2c, i:%d\n", __func__, i);
			break;
		}
	}
	oplus_set_fg_i2c_err_occured(true);

	if (!IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_pullup)) {
		dev_err(gi2c->dev, "%s: set geni_gpio_pullup\n", __func__);
		ret = pinctrl_select_state(gi2c->i2c_rsc.geni_pinctrl, gi2c->i2c_rsc.geni_gpio_pullup);
		if (ret) {
			dev_err(gi2c->dev, "%s:error pinctrl_select_state pullup, ret:%d\n", __func__, ret);
		}
	}
	if (!IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_active)) {
		dev_err(gi2c->dev, "%s: set geni_gpio_active\n", __func__);
		ret = pinctrl_select_state(gi2c->i2c_rsc.geni_pinctrl, gi2c->i2c_rsc.geni_gpio_active);
		if (ret) {
			dev_err(gi2c->dev, "%s:error pinctrl_select_state active, ret:%d\n", __func__, ret);
			goto err;
		}
	} else {
		goto err;
	}

	i2c_reset_processing = false;
	err_count = 0; /*clear the err_count after the GPIO reset is completed.*/
	dev_err(gi2c->dev, "%s: gpio reset successful id:%d\n", __func__, gi2c->adap.nr);
	atomic_set(&i2c_reset_status, 0);
	return;

err:
	atomic_set(&i2c_reset_status, 0);
	i2c_reset_processing = false;
}

#define I2C_GPIO_RESET_DELAY_MS   (2000)
static bool fg_need_i2c_reset(struct geni_i2c_dev *gi2c, int mseconds)
{
	bool ret = false;

	if (!gi2c) {
		return false;
	}

	if (NULL == (gi2c->i2c_gpio_reset_work.work.func)) {
		return false;
	}

	dev_err(gi2c->dev, "%s, err_count = %d, device_type = %d i2c_reset_status=%d\n",
		__func__, err_count, oplus_get_fg_device_type(), atomic_read(&i2c_reset_status));

	if (atomic_read(&i2c_reset_status))
		return false;

	if (oplus_get_fg_device_type() == DEVICE_TYPE_ZY0602) {
		if (!i2c_reset_processing && (err_count >= I2C_MAX_ERROR_COUNT )) {
			atomic_set(&i2c_reset_status, 1);
			dev_err(gi2c->dev, "%s start i2c_gpio_reset_work after %d ms\n", __func__, mseconds);
			cancel_delayed_work(&gi2c->i2c_gpio_reset_work);
			schedule_delayed_work(&gi2c->i2c_gpio_reset_work, msecs_to_jiffies(mseconds));
			err_count = 0;
			ret = true;
		} else {
			dev_err(gi2c->dev, "%s err_count = %d.\n", __func__, err_count);
			err_count++;
		}
	} else {
		if (err_count >= 1 && err_count < MAX_RESET_COUNT) {
			dev_err(gi2c->dev, "err_count(%d) reset the gpio.\n", err_count);
			cancel_delayed_work(&gi2c->i2c_gpio_reset_work);
			schedule_delayed_work(&gi2c->i2c_gpio_reset_work, msecs_to_jiffies(mseconds));
			ret = true;
		} else {
			dev_err(gi2c->dev, "err_count(%d) >= %d or < 1, so not reset\n", err_count, MAX_RESET_COUNT);
		}
		err_count++;
	}
	return ret;
}
#endif /* OPLUS_FEATURE_CHG_BASIC */

static int geni_i2c_xfer(struct i2c_adapter *adap,
			 struct i2c_msg msgs[],
			 int num)
{
	struct geni_i2c_dev *gi2c = i2c_get_adapdata(adap);
	int i, ret = 0, timeout = 0;
	u32 geni_ios = 0;

	gi2c->err = 0;
	atomic_set(&gi2c->is_xfer_in_progress, 1);

	/* Client to respect system suspend */
	if (!pm_runtime_enabled(gi2c->dev)) {
		GENI_SE_ERR(gi2c->ipcl, false, gi2c->dev,
			"%s: System suspended\n", __func__);
		atomic_set(&gi2c->is_xfer_in_progress, 0);
		return -EACCES;
	}

	if (!gi2c->is_le_vm) {
		ret = pm_runtime_get_sync(gi2c->dev);
		if (ret < 0) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					"error turning SE resources:%d\n", ret);
			pm_runtime_put_noidle(gi2c->dev);
			/* Set device in suspended since resume failed */
			pm_runtime_set_suspended(gi2c->dev);
			atomic_set(&gi2c->is_xfer_in_progress, 0);
			return ret;
		}
	}

	// WAR : Complete previous pending cancel cmd
	if (gi2c->prev_cancel_pending) {
		ret = do_pending_cancel(gi2c);
		if (ret) {
			pm_runtime_mark_last_busy(gi2c->dev);
			pm_runtime_put_autosuspend(gi2c->dev);
			atomic_set(&gi2c->is_xfer_in_progress, 0);
			return ret; //Don't perform xfer is cancel failed
		}
	}

	geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
	if ((geni_ios & 0x3) != 0x3) { //SCL:b'1, SDA:b'0
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			"IO lines in bad state1, Power the slave, ios:0x%x\n",
			geni_ios);

		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev, "Dumping I2C HW registers\n", geni_ios);
		geni_se_dump_dbg_regs(&gi2c->i2c_rsc, gi2c->base, gi2c->ipcl);

#ifdef OPLUS_FEATURE_CHG_BASIC
		for (i = 0; i < num; i++) {
			if (msgs[i].addr == FG_DEVICE_ADDR || msgs[i].addr == BQ25890H_DEVICE_ADDR) {
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
						"%s:IO lines not in good state, need i2c reset.\n", __func__);
				fg_need_i2c_reset(gi2c, I2C_GPIO_RESET_DELAY_MS);
				break;
			}
		}
#endif
		pm_runtime_mark_last_busy(gi2c->dev);
		pm_runtime_put_autosuspend(gi2c->dev);
		return -ENXIO;
	}

	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
		    "n:%d addr:0x%x\n", num, msgs[0].addr);

	gi2c->dbg_num = num;
	kfree(gi2c->dbg_buf_ptr);
	gi2c->dbg_buf_ptr =
		kcalloc(num, sizeof(struct dbg_buf_ctxt), GFP_KERNEL);
	if (!gi2c->dbg_buf_ptr)
		GENI_SE_ERR(gi2c->ipcl, false, gi2c->dev,
			"Buf logging pointer not available\n");

	if (gi2c->se_mode == GSI_ONLY) {
		ret = geni_i2c_gsi_xfer(adap, msgs, num);
		goto geni_i2c_txn_ret;
	} else {
		/* Don't set shared flag in non-GSI mode */
		gi2c->is_shared = false;
	}

	for (i = 0; i < num; i++) {
		int stretch = (i < (num - 1));
		u32 m_param = 0;
		u32 m_cmd = 0;
		u8 *dma_buf = NULL;
		dma_addr_t tx_dma = 0;
		dma_addr_t rx_dma = 0;
		enum se_xfer_mode mode = FIFO_MODE;

		reinit_completion(&gi2c->xfer);

		m_param |= (stretch ? STOP_STRETCH : 0);
		m_param |= ((msgs[i].addr & 0x7F) << SLV_ADDR_SHFT);

		gi2c->cur = &msgs[i];
		qcom_geni_i2c_calc_timeout(gi2c);
		mode = msgs[i].len > 32 ? SE_DMA : FIFO_MODE;

		/* Complete the transfer in FIFO mode if DMA mode
		 * is not supported for some Automotive platform.
		 */
		if (gi2c->disable_dma_mode) {
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
					"Disable DMA mode\n");
			mode = FIFO_MODE;
		}
		ret = geni_se_select_mode(gi2c->base, mode);
		if (ret) {
			dev_err(gi2c->dev, "%s: Error mode init %d:%d:%d\n",
				__func__, mode, i, msgs[i].len);
			break;
		}

		if (mode == SE_DMA) {
			dma_buf = i2c_get_dma_safe_msg_buf(&msgs[i], 1);
			if (!dma_buf) {
				ret = -ENOMEM;
				goto geni_i2c_txn_ret;
			}
		}

		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"%s: stretch:%d, m_param:0x%x\n",
				__func__, stretch, m_param);

		if (msgs[i].flags & I2C_M_RD) {
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"msgs[%d].len:%d R\n", i, msgs[i].len);
			geni_write_reg(msgs[i].len,
				       gi2c->base, SE_I2C_RX_TRANS_LEN);
			m_cmd = I2C_READ;
			geni_setup_m_cmd(gi2c->base, m_cmd, m_param);
			if (mode == SE_DMA) {
				ret = geni_se_rx_dma_prep(gi2c->wrapper_dev,
							gi2c->base, dma_buf,
							msgs[i].len, &rx_dma);
				if (ret) {
					i2c_put_dma_safe_msg_buf(dma_buf,
							&msgs[i], false);
					mode = FIFO_MODE;
					ret = geni_se_select_mode(gi2c->base,
								  mode);
				} else if (gi2c->dbg_buf_ptr) {
					gi2c->dbg_buf_ptr[i].virt_buf =
								(void *)dma_buf;
					gi2c->dbg_buf_ptr[i].map_buf =
								(void *)&rx_dma;
				}
			}
		} else {
			GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
				"msgs[%d].len:%d W\n", i, msgs[i].len);
			geni_write_reg(msgs[i].len, gi2c->base,
						SE_I2C_TX_TRANS_LEN);
			m_cmd = I2C_WRITE;
			geni_setup_m_cmd(gi2c->base, m_cmd, m_param);
			if (mode == SE_DMA) {
				ret = geni_se_tx_dma_prep(gi2c->wrapper_dev,
							gi2c->base, dma_buf,
							msgs[i].len, &tx_dma);
				if (ret) {
					i2c_put_dma_safe_msg_buf(dma_buf,
							&msgs[i], false);
					mode = FIFO_MODE;
					ret = geni_se_select_mode(gi2c->base,
								  mode);
				} else if (gi2c->dbg_buf_ptr) {
					gi2c->dbg_buf_ptr[i].virt_buf =
								(void *)dma_buf;
					gi2c->dbg_buf_ptr[i].map_buf =
								(void *)&tx_dma;
				}
			}
			if (mode == FIFO_MODE) /* Get FIFO IRQ */
				geni_write_reg(1, gi2c->base,
						SE_GENI_TX_WATERMARK_REG);
		}
		/* Ensure FIFO write go through before waiting for Done evet */
		mb();
		timeout = wait_for_completion_timeout(&gi2c->xfer,
						gi2c->xfer_timeout);
		if (!timeout) {

			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"I2C xfer timeout: %d\n", gi2c->xfer_timeout);
			geni_i2c_err(gi2c, GENI_TIMEOUT);

			/* WAR: Set flag to mark cancel pending if IOS bad */
			geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
			if ((geni_ios & 0x3) != 0x3) { //SCL:b'1, SDA:b'0
				GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
					"%s: IO lines not in good state2\n",
					__func__);
#ifdef OPLUS_FEATURE_CHG_BASIC
				if (msgs[i].addr == FG_DEVICE_ADDR || msgs[i].addr == BQ25890H_DEVICE_ADDR) {
                                        GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
                                                "%s:IO lines not in good state, need i2c reset.\n", __func__);
                                        fg_need_i2c_reset(gi2c, I2C_GPIO_RESET_DELAY_MS);
                                }
#endif
				/* doing pending cancel only rtl based SE's */
				if (gi2c->is_i2c_rtl_based) {
					gi2c->prev_cancel_pending = true;
					goto geni_i2c_txn_ret;
				}
			}
		} else {
			if (msgs[i].flags & I2C_M_RD)
				GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
				"%s: Read operation completed for len:%d\n",
				__func__, msgs[i].len);
			else
				GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
				"%s:Write operation completed for len:%d\n",
				__func__,  msgs[i].len);
		}

		if (gi2c->err) {
			if (gi2c->is_i2c_rtl_based) {
				/* WAR: Set flag to mark cancel pending if IOS bad */
				geni_ios = geni_read_reg_nolog(gi2c->base, SE_GENI_IOS);
				if ((geni_ios & 0x3) != 0x3) { //SCL:b'1, SDA:b'0
					GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
						"%s: IO lines not in good state3\n",
						__func__);
					gi2c->prev_cancel_pending = true;
					goto geni_i2c_txn_ret;
				}

				/* EBUSY set by ARB_LOST error condition */
				if (gi2c->err == -EBUSY) {
					GENI_SE_DBG(gi2c->ipcl, true, gi2c->dev,
						"%s:run reg68 war\n", __func__);
					do_reg68_war_for_rtl_se(gi2c);
				}
			}
			reinit_completion(&gi2c->xfer);
			geni_cancel_m_cmd(gi2c->base);
			timeout = wait_for_completion_timeout(&gi2c->xfer, HZ);
			if (!timeout) {
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					"Cancel failed\n");
				reinit_completion(&gi2c->xfer);
				geni_abort_m_cmd(gi2c->base);
				timeout =
				wait_for_completion_timeout(&gi2c->xfer, HZ);
				if (!timeout)
					GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
						"Abort failed\n");
			}
		}
		gi2c->cur_wr = 0;
		gi2c->cur_rd = 0;
		if (mode == SE_DMA) {
			if (gi2c->err) {
				reinit_completion(&gi2c->xfer);
				if (msgs[i].flags != I2C_M_RD)
					writel_relaxed(1, gi2c->base +
							SE_DMA_TX_FSM_RST);
				else
					writel_relaxed(1, gi2c->base +
							SE_DMA_RX_FSM_RST);
				wait_for_completion_timeout(&gi2c->xfer, HZ);
			}
			geni_se_rx_dma_unprep(gi2c->wrapper_dev, rx_dma,
					      msgs[i].len);
			geni_se_tx_dma_unprep(gi2c->wrapper_dev, tx_dma,
					      msgs[i].len);
			i2c_put_dma_safe_msg_buf(dma_buf, &msgs[i], !gi2c->err);
		}
		ret = gi2c->err;
#ifdef OPLUS_FEATURE_CHG_BASIC
/* BSP.CHG.Basic, 2021/11/02, Add for gauge i2c reset */
		if (msgs[i].addr == FG_DEVICE_ADDR || msgs[i].addr == BQ25890H_DEVICE_ADDR) {
			if (gi2c->err) {
				GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
					"i2c error :%d need i2c reset.\n", gi2c->err);
				fg_need_i2c_reset(gi2c, I2C_GPIO_RESET_DELAY_MS);
			} else {
				err_count = 0;
				reset_count = 0;
				atomic_set(&i2c_reset_status, 0);
			}
		}
#endif /* OPLUS_FEATURE_CHG_BASIC */
		if (gi2c->err) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"i2c error :%d\n", gi2c->err);
			break;
		}
	}
geni_i2c_txn_ret:
	if (ret == 0)
		ret = num;

	if (!gi2c->is_le_vm) {
		pm_runtime_mark_last_busy(gi2c->dev);
		pm_runtime_put_autosuspend(gi2c->dev);
	}
	atomic_set(&gi2c->is_xfer_in_progress, 0);
	gi2c->cur = NULL;
	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
		"i2c txn ret:%d, num:%d, err:%d\n", ret, num, gi2c->err);

	if (gi2c->err)
		return gi2c->err;
	else
		return ret;
}

static u32 geni_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm geni_i2c_algo = {
	.master_xfer	= geni_i2c_xfer,
	.functionality	= geni_i2c_func,
};

static int geni_i2c_probe(struct platform_device *pdev)
{
	struct geni_i2c_dev *gi2c;
	struct resource *res;
	struct platform_device *wrapper_pdev;
	struct device_node *wrapper_ph_node;
	int ret;
	char boot_marker[40];
	u32 geni_se_hw_param_2;

	gi2c = devm_kzalloc(&pdev->dev, sizeof(*gi2c), GFP_KERNEL);
	if (!gi2c)
		return -ENOMEM;

	if (arr_idx < MAX_SE)
		/* Debug purpose */
		gi2c_dev_dbg[arr_idx++] = gi2c;

	gi2c->dev = &pdev->dev;

	snprintf(boot_marker, sizeof(boot_marker),
				"M - DRIVER GENI_I2C Init");
	place_marker(boot_marker);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	wrapper_ph_node = of_parse_phandle(pdev->dev.of_node,
				"qcom,wrapper-core", 0);
	if (IS_ERR_OR_NULL(wrapper_ph_node)) {
		ret = PTR_ERR(wrapper_ph_node);
		dev_err(&pdev->dev, "No wrapper core defined\n");
		return ret;
	}
	wrapper_pdev = of_find_device_by_node(wrapper_ph_node);
	of_node_put(wrapper_ph_node);
	if (IS_ERR_OR_NULL(wrapper_pdev)) {
		ret = PTR_ERR(wrapper_pdev);
		dev_err(&pdev->dev, "Cannot retrieve wrapper device\n");
		return ret;
	}
	gi2c->wrapper_dev = &wrapper_pdev->dev;

	gi2c->base = devm_ioremap_resource(gi2c->dev, res);
	if (IS_ERR(gi2c->base))
		return PTR_ERR(gi2c->base);

	if (of_property_read_bool(pdev->dev.of_node, "qcom,le-vm")) {
		gi2c->is_le_vm = true;
		gi2c->first_resume = true;
		dev_info(&pdev->dev, "LE-VM usecase\n");
	}

	if (of_property_read_bool(pdev->dev.of_node, "qcom,leica-used-i2c"))
		gi2c->i2c_rsc.skip_bw_vote = true;

	if (of_property_read_bool(pdev->dev.of_node, "qcom,rtl_se")) {
		gi2c->is_i2c_rtl_based  = true;
		dev_info(&pdev->dev, "%s: RTL based SE\n", __func__);
	}

	gi2c->i2c_rsc.wrapper_dev = &wrapper_pdev->dev;
	gi2c->i2c_rsc.ctrl_dev = gi2c->dev;

	/*
	 * For LE, clocks, gpio and icb voting will be provided by
	 * by LA. The I2C operates in GSI mode only for LE usecase,
	 * se irq not required. Below properties will not be present
	 * in I2C LE dt.
	 */
	if (!gi2c->is_le_vm) {
		gi2c->i2c_rsc.se_clk = devm_clk_get(&pdev->dev, "se-clk");
		if (IS_ERR(gi2c->i2c_rsc.se_clk)) {
			ret = PTR_ERR(gi2c->i2c_rsc.se_clk);
			dev_err(&pdev->dev, "Err getting SE Core clk %d\n",
				ret);
			return ret;
		}

		gi2c->i2c_rsc.m_ahb_clk = devm_clk_get(&pdev->dev, "m-ahb");
		if (IS_ERR(gi2c->i2c_rsc.m_ahb_clk)) {
			ret = PTR_ERR(gi2c->i2c_rsc.m_ahb_clk);
			dev_err(&pdev->dev, "Err getting M AHB clk %d\n", ret);
			return ret;
		}

		gi2c->i2c_rsc.s_ahb_clk = devm_clk_get(&pdev->dev, "s-ahb");
		if (IS_ERR(gi2c->i2c_rsc.s_ahb_clk)) {
			ret = PTR_ERR(gi2c->i2c_rsc.s_ahb_clk);
			dev_err(&pdev->dev, "Err getting S AHB clk %d\n", ret);
			return ret;
		}

		ret = geni_se_resources_init(&gi2c->i2c_rsc, I2C_CORE2X_VOTE,
				     (DEFAULT_SE_CLK * DEFAULT_BUS_WIDTH));
		if (ret) {
			dev_err(gi2c->dev, "geni_se_resources_init\n");
			return ret;
		}

		gi2c->i2c_rsc.geni_pinctrl = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_pinctrl)) {
			dev_err(&pdev->dev, "No pinctrl config specified\n");
			ret = PTR_ERR(gi2c->i2c_rsc.geni_pinctrl);
			return ret;
		}
		gi2c->i2c_rsc.geni_gpio_active =
			pinctrl_lookup_state(gi2c->i2c_rsc.geni_pinctrl,
							PINCTRL_DEFAULT);
		if (IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_active)) {
			dev_err(&pdev->dev, "No default config specified\n");
			ret = PTR_ERR(gi2c->i2c_rsc.geni_gpio_active);
			return ret;
		}
		gi2c->i2c_rsc.geni_gpio_sleep =
			pinctrl_lookup_state(gi2c->i2c_rsc.geni_pinctrl,
							PINCTRL_SLEEP);
		if (IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_sleep)) {
			dev_err(&pdev->dev, "No sleep config specified\n");
			ret = PTR_ERR(gi2c->i2c_rsc.geni_gpio_sleep);
			return ret;
		}

		gi2c->disable_dma_mode = of_property_read_bool(pdev->dev.of_node,
						"qcom,disable-dma");

		gi2c->irq = platform_get_irq(pdev, 0);
		if (gi2c->irq < 0)
			return gi2c->irq;

		irq_set_status_flags(gi2c->irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(gi2c->dev, gi2c->irq, geni_i2c_irq,
					0, "i2c_geni", gi2c);
		if (ret) {
			dev_err(gi2c->dev, "Request_irq failed:%d: err:%d\n",
					   gi2c->irq, ret);
			return ret;
		}

		/* Check if SE is RTL based SE */
		geni_se_hw_param_2 = readl_relaxed(gi2c->base + SE_HW_PARAM_2);
		if (geni_se_hw_param_2 & GEN_HW_FSM_I2C) {
			gi2c->is_i2c_rtl_based  = true;
			dev_info(&pdev->dev, "%s: RTL based SE\n", __func__);
		}
	}

	if (of_property_read_bool(pdev->dev.of_node, "qcom,shared")) {
		gi2c->is_shared = true;
		dev_info(&pdev->dev, "Multi-EE usecase\n");
	}
#ifdef OPLUS_FEATURE_CHG_BASIC
	gi2c->i2c_rsc.geni_gpio_pulldown =
		pinctrl_lookup_state(gi2c->i2c_rsc.geni_pinctrl,
					PINCTRL_PULLDOWN);
	if (IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_pulldown)) {
		dev_err(&pdev->dev, "No pulldown config specified\n");
	}
	gi2c->i2c_rsc.geni_gpio_pullup =
		pinctrl_lookup_state(gi2c->i2c_rsc.geni_pinctrl,
					PINCTRL_PULLUP);
	if (IS_ERR_OR_NULL(gi2c->i2c_rsc.geni_gpio_pullup)) {
		dev_err(&pdev->dev, "No pullup config specified\n");
	}
#endif /* OPLUS_FEATURE_CHG_BASIC */

	if (of_property_read_u32(pdev->dev.of_node, "qcom,clk-freq-out",
				&gi2c->i2c_rsc.clk_freq_out))
		gi2c->i2c_rsc.clk_freq_out = KHz(400);

	dev_info(&pdev->dev, "Bus frequency is set to %dHz.\n",
						gi2c->i2c_rsc.clk_freq_out);

	ret = geni_i2c_clk_map_idx(gi2c);
	if (ret) {
		dev_err(gi2c->dev, "Invalid clk frequency %d KHz: %d\n",
				gi2c->i2c_rsc.clk_freq_out, ret);
		return ret;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "could not set DMA mask\n");
			return ret;
		}
	}

	gi2c->adap.algo = &geni_i2c_algo;
	init_completion(&gi2c->xfer);
	platform_set_drvdata(pdev, gi2c);
	i2c_set_adapdata(&gi2c->adap, gi2c);
	gi2c->adap.dev.parent = gi2c->dev;
	gi2c->adap.dev.of_node = pdev->dev.of_node;

	strlcpy(gi2c->adap.name, "Geni-I2C", sizeof(gi2c->adap.name));

	pm_runtime_set_suspended(gi2c->dev);
	pm_runtime_set_autosuspend_delay(gi2c->dev, I2C_AUTO_SUSPEND_DELAY);
	pm_runtime_use_autosuspend(gi2c->dev);
	pm_runtime_enable(gi2c->dev);
	ret = i2c_add_adapter(&gi2c->adap);
	if (ret) {
		dev_err(gi2c->dev, "Add adapter failed, ret=%d\n", ret);
		return ret;
	}

	atomic_set(&gi2c->is_xfer_in_progress, 0);
	snprintf(boot_marker, sizeof(boot_marker),
		 "M - DRIVER GENI_I2C_%d Ready", gi2c->adap.nr);
	place_marker(boot_marker);

#ifdef OPLUS_FEATURE_CHG_BASIC
/*BSP.CHG.Basic, 2022/08/11, Add for gauge i2c reset */
	INIT_DELAYED_WORK(&gi2c->i2c_gpio_reset_work, oplus_i2c_gpio_reset_work);
#endif /* OPLUS_FEATURE_CHG_BASIC */
	dev_info(gi2c->dev, "I2C probed\n");
	return 0;
}

static int geni_i2c_remove(struct platform_device *pdev)
{
	struct geni_i2c_dev *gi2c = platform_get_drvdata(pdev);
	int i;

	if (atomic_read(&gi2c->is_xfer_in_progress)) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			    "%s: Xfer is in progress\n", __func__);
		return -EBUSY;
	}

	if (!pm_runtime_status_suspended(gi2c->dev)) {
		if (geni_i2c_runtime_suspend(gi2c->dev))
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    "%s: runtime suspend failed\n", __func__);
	}

	if (gi2c->se_mode == GSI_ONLY) {
		if (gi2c->tx_c) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    "%s: clearing tx dma resource\n", __func__);
			dma_release_channel(gi2c->tx_c);
		}
		if (gi2c->rx_c) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    "%s: clearing rx dma resource\n", __func__);
			dma_release_channel(gi2c->rx_c);
		}
	}

	pm_runtime_put_noidle(gi2c->dev);
	pm_runtime_set_suspended(gi2c->dev);
	pm_runtime_disable(gi2c->dev);
	i2c_del_adapter(&gi2c->adap);

	for (i = 0; i < arr_idx; i++)
		gi2c_dev_dbg[i] = NULL;
	arr_idx = 0;

	if (gi2c->ipcl)
		ipc_log_context_destroy(gi2c->ipcl);
	return 0;
}

static int geni_i2c_resume_early(struct device *device)
{
	struct geni_i2c_dev *gi2c = dev_get_drvdata(device);

	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev, "%s\n", __func__);
	return 0;
}

/*
 * get sync/put sync in LA-VM -> do resources on/off
 * get sync/put sync in LE-VM -> do lock/unlock gpii
 */
#if IS_ENABLED(CONFIG_PM)
static int geni_i2c_runtime_suspend(struct device *dev)
{
	int ret = 0;
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	if (gi2c->se_mode == FIFO_SE_DMA)
		disable_irq(gi2c->irq);

	if (gi2c->is_le_vm) {
		if (!gi2c->first_resume)
			geni_i2c_unlock_bus(gi2c);
		else
			gi2c->first_resume = false;
	} else if (gi2c->is_shared) {
		/* Do not unconfigure GPIOs if shared se */
		ret = se_geni_clks_off(&gi2c->i2c_rsc);
		if (ret)
			dev_err(dev, "%s: clks_off Error ret %d\n", __func__, ret);
	} else {
		ret = se_geni_resources_off(&gi2c->i2c_rsc);
		if (ret)
			dev_err(dev, "%s: resource_off Error ret %d\n", __func__, ret);
	}
	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev, "%s\n", __func__);
	return ret;
}

static int geni_i2c_runtime_resume(struct device *dev)
{
	int ret;
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	if (!gi2c->ipcl) {
		char ipc_name[I2C_NAME_SIZE];

		snprintf(ipc_name, I2C_NAME_SIZE, "%s", dev_name(gi2c->dev));
		gi2c->ipcl = ipc_log_context_create(20, ipc_name, 0);
	}

	if (!gi2c->is_le_vm) {
		/* Do not control clk/gpio/icb for LE-VM */
		ret = se_geni_resources_on(&gi2c->i2c_rsc);
		if (ret) {
			dev_err(dev, "%s: resource_off Error ret %d\n", __func__, ret);
			return ret;
		}

		ret = geni_i2c_prepare(gi2c);
		if (ret) {
			dev_err(gi2c->dev, "I2C prepare failed: %d\n", ret);
			return ret;
		}

		if (gi2c->se_mode == FIFO_SE_DMA)
			enable_irq(gi2c->irq);
	} else if (!gi2c->first_resume) {
		/*
		 * For first resume call in le, do nothing, and in
		 * corresponding first suspend, set the first_resume
		 * flag to false, to enable lock/unlock per resume/suspend
		 * session.
		 */
		ret = geni_i2c_prepare(gi2c);
		if (ret) {
			dev_err(gi2c->dev, "I2C prepare failed:%d\n", ret);
			return ret;
		}

		ret = geni_i2c_lock_bus(gi2c);
		if (ret) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"%s failed: %d\n", __func__, ret);
			return ret;
		}
	}

	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev, "%s\n", __func__);

	return 0;
}

static int geni_i2c_suspend_late(struct device *device)
{
	struct geni_i2c_dev *gi2c = dev_get_drvdata(device);
	int ret;

	GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev, "%s\n", __func__);

	if (atomic_read(&gi2c->is_xfer_in_progress)) {
		if (!pm_runtime_status_suspended(gi2c->dev)) {
			GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				    ":%s: runtime PM is active\n", __func__);
			return -EBUSY;
		}
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
			    "%s System suspend not allowed while xfer in progress\n",
			    __func__);
		return -EBUSY;
	}

	/* Make sure no transactions are pending */
	ret = i2c_trylock_bus(&gi2c->adap, I2C_LOCK_SEGMENT);
	if (!ret) {
		GENI_SE_ERR(gi2c->ipcl, true, gi2c->dev,
				"late I2C transaction request\n");
		return -EBUSY;
	}
	if (!pm_runtime_status_suspended(device)) {
		GENI_SE_DBG(gi2c->ipcl, false, gi2c->dev,
			"%s: Force suspend\n", __func__);
		geni_i2c_runtime_suspend(device);
		pm_runtime_disable(device);
		pm_runtime_set_suspended(device);
		pm_runtime_enable(device);
	}
	i2c_unlock_bus(&gi2c->adap, I2C_LOCK_SEGMENT);
	return 0;
}
#else
static int geni_i2c_runtime_suspend(struct device *dev)
{
	return 0;
}

static int geni_i2c_runtime_resume(struct device *dev)
{
	return 0;
}

static int geni_i2c_suspend_late(struct device *device)
{
	return 0;
}
#endif

static const struct dev_pm_ops geni_i2c_pm_ops = {
	.suspend_late		= geni_i2c_suspend_late,
	.resume_early		= geni_i2c_resume_early,
	.runtime_suspend	= geni_i2c_runtime_suspend,
	.runtime_resume		= geni_i2c_runtime_resume,
};

static const struct of_device_id geni_i2c_dt_match[] = {
	{ .compatible = "qcom,i2c-geni" },
	{}
};
MODULE_DEVICE_TABLE(of, geni_i2c_dt_match);

static struct platform_driver geni_i2c_driver = {
	.probe  = geni_i2c_probe,
	.remove = geni_i2c_remove,
	.driver = {
		.name = "i2c_geni",
		.pm = &geni_i2c_pm_ops,
		.of_match_table = geni_i2c_dt_match,
	},
};

static int __init i2c_dev_init(void)
{
	return platform_driver_register(&geni_i2c_driver);
}

static void __exit i2c_dev_exit(void)
{
	platform_driver_unregister(&geni_i2c_driver);
}

module_init(i2c_dev_init);
module_exit(i2c_dev_exit);
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c_geni");
