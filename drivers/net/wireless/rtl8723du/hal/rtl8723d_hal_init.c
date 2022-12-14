// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2007 - 2017 Realtek Corporation */

#define _HAL_INIT_C_

#include <rtl8723d_hal.h>
#include "hal_com_h2c.h"
#include <hal_com.h>
#include "hal8723d_fw.h"

#ifndef CONFIG_DLFW_TXPKT
#define DL_FW_MAX 15
#else
#define FW_DOWNLOAD_SIZE_8723D 8192
#endif

static void
_FWDownloadEnable(
	struct adapter *		adapt,
	bool			enable
)
{
	u8	tmp, count = 0;

	if (enable) {
		/* 8051 enable */
		tmp = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
		rtw_write8(adapt, REG_SYS_FUNC_EN + 1, tmp | 0x04);

		tmp = rtw_read8(adapt, REG_MCUFWDL);
		rtw_write8(adapt, REG_MCUFWDL, tmp | 0x01);

		do {
			tmp = rtw_read8(adapt, REG_MCUFWDL);
			if (tmp & 0x01)
				break;
			rtw_write8(adapt, REG_MCUFWDL, tmp | 0x01);
			rtw_msleep_os(1);
		} while (count++ < 100);
		if (count > 0)
			RTW_INFO("%s: !!!!!!!!Write 0x80 Fail!: count = %d\n", __func__, count);

		/* 8051 reset */
		tmp = rtw_read8(adapt, REG_MCUFWDL + 2);
		rtw_write8(adapt, REG_MCUFWDL + 2, tmp & 0xf7);
	} else {
		/* MCU firmware download disable. */
		tmp = rtw_read8(adapt, REG_MCUFWDL);
		rtw_write8(adapt, REG_MCUFWDL, tmp & 0xfe);
	}
}

static int
_BlockWrite(
	struct adapter *		adapt,
	void *		buffer,
	u32			buffSize
)
{
	int ret = _SUCCESS;

	u32			blockSize_p1 = 4;	/* (Default) Phase #1 : PCI muse use 4-byte write to download FW */
	u32			blockSize_p2 = 8;	/* Phase #2 : Use 8-byte, if Phase#1 use big size to write FW. */
	u32			blockSize_p3 = 1;	/* Phase #3 : Use 1-byte, the remnant of FW image. */
	u32			blockCount_p1 = 0, blockCount_p2 = 0, blockCount_p3 = 0;
	u32			remainSize_p1 = 0, remainSize_p2 = 0;
	u8			*bufferPtr	= (u8 *)buffer;
	u32			i = 0, offset = 0;

	blockSize_p1 = 254;

	/* 3 Phase #1 */
	blockCount_p1 = buffSize / blockSize_p1;
	remainSize_p1 = buffSize % blockSize_p1;



	for (i = 0; i < blockCount_p1; i++) {
		ret = rtw_writeN(adapt, (FW_8723D_START_ADDRESS + i * blockSize_p1), blockSize_p1, (bufferPtr + i * blockSize_p1));
		if (ret == _FAIL) {
			printk(KERN_ERR "====>%s %d i:%d\n", __func__, __LINE__, i);
			goto exit;
		}
	}

	/* 3 Phase #2 */
	if (remainSize_p1) {
		offset = blockCount_p1 * blockSize_p1;

		blockCount_p2 = remainSize_p1 / blockSize_p2;
		remainSize_p2 = remainSize_p1 % blockSize_p2;

		for (i = 0; i < blockCount_p2; i++) {
			ret = rtw_writeN(adapt, (FW_8723D_START_ADDRESS + offset + i * blockSize_p2), blockSize_p2, (bufferPtr + offset + i * blockSize_p2));

			if (ret == _FAIL)
				goto exit;
		}
	}

	/* 3 Phase #3 */
	if (remainSize_p2) {
		offset = (blockCount_p1 * blockSize_p1) + (blockCount_p2 * blockSize_p2);

		blockCount_p3 = remainSize_p2 / blockSize_p3;


		for (i = 0 ; i < blockCount_p3 ; i++) {
			ret = rtw_write8(adapt, (FW_8723D_START_ADDRESS + offset + i), *(bufferPtr + offset + i));

			if (ret == _FAIL) {
				printk(KERN_ERR "====>%s %d i:%d\n", __func__, __LINE__, i);
				goto exit;
			}
		}
	}
exit:
	return ret;
}

static int
_PageWrite(
		struct adapter *	adapt,
		u32			page,
		void *		buffer,
		u32			size
)
{
	u8 value8;
	u8 u8Page = (u8)(page & 0x07);

	value8 = (rtw_read8(adapt, REG_MCUFWDL + 2) & 0xF8) | u8Page;
	rtw_write8(adapt, REG_MCUFWDL + 2, value8);

	return _BlockWrite(adapt, buffer, size);
}

static int
_WriteFW(
	struct adapter * adapt,
	void * buffer,
	u32 size
)
{
	/* Since we need dynamic decide method of dwonload fw, so we call this function to get chip version. */
	int ret = _SUCCESS;
	u32 pageNums, remainSize;
	u32 page, offset;
	u8 *bufferPtr = (u8 *)buffer;

	pageNums = size / MAX_DLFW_PAGE_SIZE;
	/* RT_ASSERT((pageNums <= 4), ("Page numbers should not greater then 4\n")); */
	remainSize = size % MAX_DLFW_PAGE_SIZE;

	for (page = 0; page < pageNums; page++) {
		offset = page * MAX_DLFW_PAGE_SIZE;
		ret = _PageWrite(adapt, page, bufferPtr + offset, MAX_DLFW_PAGE_SIZE);

		if (ret == _FAIL) {
			printk(KERN_ERR "====>%s %d\n", __func__, __LINE__);
			goto exit;
		}
	}
	if (remainSize) {
		offset = pageNums * MAX_DLFW_PAGE_SIZE;
		page = pageNums;
		ret = _PageWrite(adapt, page, bufferPtr + offset, remainSize);

		if (ret == _FAIL) {
			printk(KERN_ERR "====>%s %d\n", __func__, __LINE__);
			goto exit;
		}
	}

exit:
	return ret;
}

void _8051Reset8723(struct adapter * adapt)
{
	u8 cpu_rst;
	u8 io_rst;

	/* Reset 8051(WLMCU) IO wrapper */
	/* 0x1c[8] = 0 */
	/* Suggested by Isaac@SD1 and Gimmy@SD1, coding by Lucas@20130624 */
	io_rst = rtw_read8(adapt, REG_RSV_CTRL + 1);
	io_rst &= ~BIT(0);
	rtw_write8(adapt, REG_RSV_CTRL + 1, io_rst);

	cpu_rst = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
	cpu_rst &= ~BIT(2);
	rtw_write8(adapt, REG_SYS_FUNC_EN + 1, cpu_rst);

	/* Enable 8051 IO wrapper	 */
	/* 0x1c[8] = 1 */
	io_rst = rtw_read8(adapt, REG_RSV_CTRL + 1);
	io_rst |= BIT(0);
	rtw_write8(adapt, REG_RSV_CTRL + 1, io_rst);

	cpu_rst = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
	cpu_rst |= BIT(2);
	rtw_write8(adapt, REG_SYS_FUNC_EN + 1, cpu_rst);

	RTW_INFO("%s: Finish\n", __func__);
}

static int polling_fwdl_chksum(struct adapter *adapter, u32 min_cnt, u32 timeout_ms)
{
	int ret = _FAIL;
	u32 value32;
	unsigned long start = rtw_get_current_time();
	u32 cnt = 0;

	/* polling CheckSum report */
	do {
		cnt++;
		value32 = rtw_read32(adapter, REG_MCUFWDL);
		if (value32 & FWDL_ChkSum_rpt || RTW_CANNOT_IO(adapter))
			break;
		rtw_yield_os();
	} while (rtw_get_passing_time_ms(start) < timeout_ms || cnt < min_cnt);

	if (!(value32 & FWDL_ChkSum_rpt))
		goto exit;

	if (rtw_fwdl_test_trigger_chksum_fail())
		goto exit;

	ret = _SUCCESS;

exit:
	RTW_INFO("%s: Checksum report %s! (%u, %dms), REG_MCUFWDL:0x%08x\n", __func__
		, (ret == _SUCCESS) ? "OK" : "Fail", cnt, rtw_get_passing_time_ms(start), value32);

	return ret;
}

static int _FWFreeToGo(struct adapter *adapter, u32 min_cnt, u32 timeout_ms)
{
	int ret = _FAIL;
	u32	value32;
	unsigned long start = rtw_get_current_time();
	u32 cnt = 0;
	u32 value_to_check = 0;
	u32 value_expected = (MCUFWDL_RDY | FWDL_ChkSum_rpt | WINTINI_RDY | RAM_DL_SEL);

	value32 = rtw_read32(adapter, REG_MCUFWDL);
	value32 |= MCUFWDL_RDY;
	value32 &= ~WINTINI_RDY;
	rtw_write32(adapter, REG_MCUFWDL, value32);

	_8051Reset8723(adapter);

	/*  polling for FW ready */
	do {
		cnt++;
		value32 = rtw_read32(adapter, REG_MCUFWDL);
		value_to_check = value32 & value_expected;
		if ((value_to_check == value_expected) || RTW_CANNOT_IO(adapter))
			break;
		rtw_yield_os();
	} while (rtw_get_passing_time_ms(start) < timeout_ms || cnt < min_cnt);

	if (value_to_check != value_expected)
		goto exit;

	if (rtw_fwdl_test_trigger_wintint_rdy_fail())
		goto exit;

	ret = _SUCCESS;

exit:
	RTW_INFO("%s: Polling FW ready %s! (%u, %dms), REG_MCUFWDL:0x%08x\n", __func__
		, (ret == _SUCCESS) ? "OK" : "Fail", cnt, rtw_get_passing_time_ms(start), value32);

	return ret;
}

#define IS_FW_81xxC(adapt)	(((GET_HAL_DATA(adapt))->FirmwareSignature & 0xFFF0) == 0x88C0)

void rtl8723d_FirmwareSelfReset(struct adapter * adapt)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);
	u8	u1bTmp;
	u8	Delay = 100;

	if (!(IS_FW_81xxC(adapt) &&
	      ((pHalData->firmware_version < 0x21) ||
	       (pHalData->firmware_version == 0x21 &&
		pHalData->firmware_sub_version < 0x01)))) { /* after 88C Fw v33.1 */
		/* 0x1cf=0x20. Inform 8051 to reset. 2009.12.25. tynli_test */
		rtw_write8(adapt, REG_HMETFR + 3, 0x20);

		u1bTmp = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
		while (u1bTmp & BIT(2)) {
			Delay--;
			if (Delay == 0)
				break;
			rtw_udelay_os(50);
			u1bTmp = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
		}

		if (Delay == 0) {
			/* force firmware reset */
			u1bTmp = rtw_read8(adapt, REG_SYS_FUNC_EN + 1);
			rtw_write8(adapt, REG_SYS_FUNC_EN + 1, u1bTmp & (~BIT(2)));
		}
	}
}

/*
 *	Description:
 *		Download 8192C firmware code.
 *
 *   */
int rtl8723d_FirmwareDownload(struct adapter * adapt, bool  bUsedWoWLANFw)
{
	int	rtStatus = _SUCCESS;
	u8 write_fw = 0;
	unsigned long fwdl_start_time;
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct rt_firmware	*pFirmware = NULL;
	struct rt_8723d_firmware_hdr *pFwHdr = NULL;
	u8			*pFirmwareBuf;
	u32			FirmwareLen;
#ifdef CONFIG_FILE_FWIMG
	u8 *fwfilepath;
#endif /* CONFIG_FILE_FWIMG */
	u8			value8;
	struct dvobj_priv *psdpriv = adapt->dvobj;
	struct debug_priv *pdbgpriv = &psdpriv->drv_dbg;

	pFirmware = rtw_zmalloc(sizeof(struct rt_firmware));

	if (!pFirmware) {
		rtStatus = _FAIL;
		goto exit;
	}

	{
		u8 tmp_ps = 0;

		tmp_ps = rtw_read8(adapt, 0xa3);
		tmp_ps &= 0xf8;
		tmp_ps |= 0x02;
		/* 1. write 0xA3[:2:0] = 3b'010 */
		rtw_write8(adapt, 0xa3, tmp_ps);
		/* 2. read power_state = 0xA0[1:0] */
		tmp_ps = rtw_read8(adapt, 0xa0);
		tmp_ps &= 0x03;
		if (tmp_ps != 0x01) {
			RTW_INFO(FUNC_ADPT_FMT" tmp_ps=%x\n",
				 FUNC_ADPT_ARG(adapt), tmp_ps);
			pdbgpriv->dbg_downloadfw_pwr_state_cnt++;
		}
	}

	rtw_btcoex_PreLoadFirmware(adapt);

#ifdef CONFIG_FILE_FWIMG
	fwfilepath = rtw_fw_file_path;
#endif /* CONFIG_FILE_FWIMG */

#ifdef CONFIG_FILE_FWIMG
	if (rtw_is_file_readable(fwfilepath)) {
		RTW_INFO("%s acquire FW from file:%s\n", __func__, fwfilepath);
		pFirmware->eFWSource = FW_SOURCE_IMG_FILE;
	} else
#endif /* CONFIG_FILE_FWIMG */
	{
#ifdef CONFIG_EMBEDDED_FWIMG
		pFirmware->eFWSource = FW_SOURCE_HEADER_FILE;
#else /* !CONFIG_EMBEDDED_FWIMG */
		pFirmware->eFWSource = FW_SOURCE_IMG_FILE; /* We should decided by Reg. */
#endif /* !CONFIG_EMBEDDED_FWIMG */
	}

	switch (pFirmware->eFWSource) {
	case FW_SOURCE_IMG_FILE:
#ifdef CONFIG_FILE_FWIMG
		rtStatus = rtw_retrieve_from_file(fwfilepath, FwBuffer, FW_8723D_SIZE);
		pFirmware->ulFwLength = rtStatus >= 0 ? rtStatus : 0;
		pFirmware->szFwBuffer = FwBuffer;
#endif /* CONFIG_FILE_FWIMG */
		break;

	case FW_SOURCE_HEADER_FILE:
		if (bUsedWoWLANFw) {
		} else {
			pFirmware->szFwBuffer = array_mp_8723d_fw_nic;
			pFirmware->ulFwLength = array_length_mp_8723d_fw_nic;
			RTW_INFO("%s fw: %s, size: %d\n", __func__, "FW_NIC", pFirmware->ulFwLength);
		}
		break;
	}

	if ((pFirmware->ulFwLength - 32) > FW_8723D_SIZE) {
		rtStatus = _FAIL;
		RTW_ERR("Firmware size:%u exceed %u\n",
			pFirmware->ulFwLength, FW_8723D_SIZE);
		goto exit;
	}

	pFirmwareBuf = pFirmware->szFwBuffer;
	FirmwareLen = pFirmware->ulFwLength;

	/* To Check Fw header. Added by tynli. 2009.12.04. */
	pFwHdr = (struct rt_8723d_firmware_hdr *)pFirmwareBuf;

	pHalData->firmware_version =  le16_to_cpu(pFwHdr->Version);
	pHalData->firmware_sub_version = le16_to_cpu(pFwHdr->Subversion);
	pHalData->FirmwareSignature = le16_to_cpu(pFwHdr->Signature);

	RTW_INFO("%s: fw_ver=%x fw_subver=%04x sig=0x%x, Month=%02x, Date=%02x, Hour=%02x, Minute=%02x\n",
		 __func__, pHalData->firmware_version,
		 pHalData->firmware_sub_version, pHalData->FirmwareSignature
		 , pFwHdr->Month, pFwHdr->Date, pFwHdr->Hour, pFwHdr->Minute);

	if (IS_FW_HEADER_EXIST_8723D(pFwHdr)) {
		RTW_INFO("%s(): Shift for fw header!\n", __func__);
		/* Shift 32 bytes for FW header */
		pFirmwareBuf = pFirmwareBuf + 32;
		FirmwareLen = FirmwareLen - 32;
	}

	fwdl_start_time = rtw_get_current_time();

	/* To check if FW already exists before download FW */
	if (rtw_read8(adapt, REG_MCUFWDL) & RAM_DL_SEL) {
		RTW_INFO("%s: FW exists before download FW\n", __func__);
		rtw_write8(adapt, REG_MCUFWDL, 0x00);
		_8051Reset8723(adapt);
	}

#ifndef CONFIG_DLFW_TXPKT
	RTW_INFO("%s by IO write!\n", __func__);

	_FWDownloadEnable(adapt, true);

	while (!RTW_CANNOT_IO(adapt) &&
	       (write_fw++ < DL_FW_MAX ||
		rtw_get_passing_time_ms(fwdl_start_time) < 500)) {
		/* reset FWDL chksum */
		rtw_write8(adapt, REG_MCUFWDL,
			   rtw_read8(adapt, REG_MCUFWDL) | FWDL_ChkSum_rpt);

		rtStatus = _WriteFW(adapt, pFirmwareBuf, FirmwareLen);
		if (rtStatus != _SUCCESS)
			continue;

		rtStatus = polling_fwdl_chksum(adapt, 2, 10);
		if (rtStatus == _SUCCESS) {
			RTW_INFO("%s: download FW count:%d\n", __func__,
				 write_fw);
			break;
		} else {
			rtw_mdelay_os(10);
		}
	}
#else
	RTW_INFO("%s by Tx pkt write!\n", __func__);

	if ((rtw_read8(adapt, REG_MCUFWDL) & MCUFWDL_RDY) == 0) {
		/*
		 * SDIO DMA condition:
		 * all queue must be 256 (0x100 = 0x20 + 0xE0)
		*/

		value32 = 0x802000E0;
		rtw_write32(adapt, REG_RQPN, value32);

		/* Set beacon boundary to TXFIFO header */
		rtw_write8(adapt, REG_BCNQ_BDNY, 0);
		rtw_write16(adapt, REG_DWBCN0_CTRL_8723D + 1, BIT(8));

		/* SDIO need read this register before send packet */
		rtw_read32(adapt, 0x10250020);

		_FWDownloadEnable(adapt, true);

		/* Get original check sum */
		new_chk_sum = *(pFirmwareBuf + FirmwareLen - 2) |
			      ((u16)*(pFirmwareBuf + FirmwareLen - 1) << 8);

		/* Send ram code flow */
		dma_iram_sel = 0;
		mem_offset = 0;
		pkt_size_tmp = FirmwareLen;
		while (0 != pkt_size_tmp) {
			if (pkt_size_tmp >= FW_DOWNLOAD_SIZE_8723D) {
				send_pkt_size = FW_DOWNLOAD_SIZE_8723D;
				/* Modify check sum value */
				new_chk_sum = (u16)(new_chk_sum ^ (((send_pkt_size - 1) << 2) - TXDESC_SIZE));
			} else {
				send_pkt_size = pkt_size_tmp;
				new_chk_sum = (u16)(new_chk_sum ^ ((send_pkt_size << 2) - TXDESC_SIZE));

			}

			if (send_pkt_size == pkt_size_tmp) {
				/* last partition packet, write new check sum to ram code file */
				*(pFirmwareBuf + FirmwareLen - 2) = new_chk_sum & 0xFF;
				*(pFirmwareBuf + FirmwareLen - 1) = (new_chk_sum & 0xFF00) >> 8;
			}

			/* IRAM select */
			rtw_write8(adapt, REG_MCUFWDL + 1,
				(rtw_read8(adapt, REG_MCUFWDL + 1) & 0x3F) | (dma_iram_sel << 6));
			/* Enable DMA */
			rtw_write8(adapt, REG_MCUFWDL + 1,
				rtw_read8(adapt, REG_MCUFWDL + 1) | BIT(5));

			if (false == send_fw_packet(adapt, pFirmwareBuf + mem_offset, send_pkt_size)) {
				RTW_INFO("%s: Send FW fail !\n", __func__);
				rtStatus = _FAIL;
				goto DLFW_FAIL;
			}

			dma_iram_sel++;
			mem_offset += send_pkt_size;
			pkt_size_tmp -= send_pkt_size;
		}
	} else {
		RTW_INFO("%s: Download FW fail since MCUFWDL_RDY is not set!\n", __func__);
		rtStatus = _FAIL;
		goto DLFW_FAIL;
	}
#endif

	_FWDownloadEnable(adapt, false);

	if (rtStatus == _SUCCESS)
		rtStatus = _FWFreeToGo(adapt, 10, 200);
	else
		goto DLFW_FAIL;

	if (_SUCCESS != rtStatus)
		goto DLFW_FAIL;

DLFW_FAIL:
	if (rtStatus == _FAIL) {
		/* Disable FWDL_EN */
		value8 = rtw_read8(adapt, REG_MCUFWDL);
		value8 = (value8 & ~(BIT(0)) & ~(BIT(1)));
		rtw_write8(adapt, REG_MCUFWDL, value8);
	}

	RTW_INFO("%s %s. write_fw:%u, %dms\n"
		 , __func__, (rtStatus == _SUCCESS) ? "success" : "fail"
		 , write_fw
		 , rtw_get_passing_time_ms(fwdl_start_time)
		);

exit:
	if (pFirmware)
		rtw_mfree((u8 *)pFirmware, sizeof(struct rt_firmware));

	rtl8723d_InitializeFirmwareVars(adapt);

	RTW_INFO(" <=== %s()\n", __func__);

	return rtStatus;
}

void rtl8723d_InitializeFirmwareVars(struct adapter * adapt)
{
	struct hal_com_data * pHalData = GET_HAL_DATA(adapt);

	/* Init Fw LPS related. */
	adapter_to_pwrctl(adapt)->bFwCurrentInPSMode = false;

	/* Init H2C cmd. */
	rtw_write8(adapt, REG_HMETFR, 0x0f);

	/* Init H2C counter. by tynli. 2009.12.09. */
	pHalData->LastHMEBoxNum = 0;
	/*	pHalData->H2CQueueHead = 0;
	 *	pHalData->H2CQueueTail = 0;
	 *	pHalData->H2CStopInsertQueue = false; */
}

/* ***********************************************************
 *				Efuse related code
 * *********************************************************** */
static u8
hal_EfuseSwitchToBank(
	struct adapter *	adapt,
	u8			bank,
	u8			bPseudoTest)
{
	u8 bRet = false;
	u32 value32 = 0;
#ifdef HAL_EFUSE_MEMORY
	struct hal_com_data * pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal * pEfuseHal = &pHalData->EfuseHal;
#endif


	RTW_INFO("%s: Efuse switch bank to %d\n", __func__, bank);
	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		pEfuseHal->fakeEfuseBank = bank;
#else
		fakeEfuseBank = bank;
#endif
		bRet = true;
	} else {
		value32 = rtw_read32(adapt, EFUSE_TEST);
		bRet = true;
		switch (bank) {
		case 0:
			value32 = (value32 & ~EFUSE_SEL_MASK) | EFUSE_SEL(EFUSE_WIFI_SEL_0);
			break;
		case 1:
			value32 = (value32 & ~EFUSE_SEL_MASK) | EFUSE_SEL(EFUSE_BT_SEL_0);
			break;
		case 2:
			value32 = (value32 & ~EFUSE_SEL_MASK) | EFUSE_SEL(EFUSE_BT_SEL_1);
			break;
		case 3:
			value32 = (value32 & ~EFUSE_SEL_MASK) | EFUSE_SEL(EFUSE_BT_SEL_2);
			break;
		default:
			value32 = (value32 & ~EFUSE_SEL_MASK) | EFUSE_SEL(EFUSE_WIFI_SEL_0);
			bRet = false;
			break;
		}
		rtw_write32(adapt, EFUSE_TEST, value32);
	}

	return bRet;
}

static void Hal_GetEfuseDefinition(struct adapter * adapt, u8 efuseType, u8 type,
				   void *pOut, bool bPseudoTest)
{
	switch (type) {
	case TYPE_EFUSE_MAX_SECTION: {
		u8 *pMax_section;

		pMax_section = (u8 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pMax_section = EFUSE_MAX_SECTION_8723D;
		else
			*pMax_section = EFUSE_BT_MAX_SECTION;
	}
	break;

	case TYPE_EFUSE_REAL_CONTENT_LEN: {
		u16 *pu2Tmp;

		pu2Tmp = (u16 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu2Tmp = EFUSE_REAL_CONTENT_LEN_8723D;
		else
			*pu2Tmp = EFUSE_BT_REAL_CONTENT_LEN;
	}
	break;

	case TYPE_AVAILABLE_EFUSE_BYTES_BANK: {
		u16	*pu2Tmp;

		pu2Tmp = (u16 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu2Tmp = (EFUSE_REAL_CONTENT_LEN_8723D - EFUSE_OOB_PROTECT_BYTES);
		else
			*pu2Tmp = (EFUSE_BT_REAL_BANK_CONTENT_LEN - EFUSE_PROTECT_BYTES_BANK);
	}
	break;

	case TYPE_AVAILABLE_EFUSE_BYTES_TOTAL: {
		u16 *pu2Tmp;

		pu2Tmp = (u16 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu2Tmp = (EFUSE_REAL_CONTENT_LEN_8723D - EFUSE_OOB_PROTECT_BYTES);
		else
			*pu2Tmp = (EFUSE_BT_REAL_CONTENT_LEN - (EFUSE_PROTECT_BYTES_BANK * 3));
	}
	break;

	case TYPE_EFUSE_MAP_LEN: {
		u16 *pu2Tmp;

		pu2Tmp = (u16 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu2Tmp = EFUSE_MAP_LEN_8723D;
		else
			*pu2Tmp = EFUSE_BT_MAP_LEN;
	}
	break;

	case TYPE_EFUSE_PROTECT_BYTES_BANK: {
		u8 *pu1Tmp;

		pu1Tmp = (u8 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu1Tmp = EFUSE_OOB_PROTECT_BYTES;
		else
			*pu1Tmp = EFUSE_PROTECT_BYTES_BANK;
	}
	break;

	case TYPE_EFUSE_CONTENT_LEN_BANK: {
		u16 *pu2Tmp;

		pu2Tmp = (u16 *)pOut;

		if (efuseType == EFUSE_WIFI)
			*pu2Tmp = EFUSE_REAL_CONTENT_LEN_8723D;
		else
			*pu2Tmp = EFUSE_BT_REAL_BANK_CONTENT_LEN;
	}
	break;

	default: {
		u8 *pu1Tmp;

		pu1Tmp = (u8 *)pOut;
		*pu1Tmp = 0;
	}
	break;
	}
}

#define VOLTAGE_V25		0x03
#define LDOE25_SHIFT	28

/* *****************************************************************
 *	The following is for compile ok
 *	That should be merged with the original in the future
 * ***************************************************************** */
#define EFUSE_ACCESS_ON_8723			0x69	/* For RTL8723 only. */
#define EFUSE_ACCESS_OFF_8723			0x00	/* For RTL8723 only. */
#define REG_EFUSE_ACCESS_8723			0x00CF	/* Efuse access protection for RTL8723 */

/* ***************************************************************** */
static void Hal_BT_EfusePowerSwitch(
	struct adapter *	adapt,
	u8			bWrite,
	u8			PwrState)
{
	u8 tempval;

	if (PwrState) {
		/* enable BT power cut */
		/* 0x6A[14] = 1 */
		tempval = rtw_read8(adapt, 0x6B);
		tempval |= BIT(6);
		rtw_write8(adapt, 0x6B, tempval);

		/* Attention!! Between 0x6A[14] and 0x6A[15] setting need 100us delay */
		/* So don't write 0x6A[14]=1 and 0x6A[15]=0 together! */
		rtw_usleep_os(100);
		/* disable BT output isolation */
		/* 0x6A[15] = 0 */
		tempval = rtw_read8(adapt, 0x6B);
		tempval &= ~BIT(7);
		rtw_write8(adapt, 0x6B, tempval);
	} else {
		/* enable BT output isolation */
		/* 0x6A[15] = 1 */
		tempval = rtw_read8(adapt, 0x6B);
		tempval |= BIT(7);
		rtw_write8(adapt, 0x6B, tempval);

		/* Attention!! Between 0x6A[14] and 0x6A[15] setting need 100us delay */
		/* So don't write 0x6A[14]=1 and 0x6A[15]=0 together! */

		/* disable BT power cut */
		/* 0x6A[14] = 1 */
		tempval = rtw_read8(adapt, 0x6B);
		tempval &= ~BIT(6);
		rtw_write8(adapt, 0x6B, tempval);
	}

}
static void
Hal_EfusePowerSwitch(
	struct adapter *	adapt,
	u8			bWrite,
	u8			PwrState)
{
	u8	tempval;
	u16	tmpV16;


	if (PwrState) {
		rtw_write8(adapt, REG_EFUSE_ACCESS_8723, EFUSE_ACCESS_ON_8723);

		/* Reset: 0x0000h[28], default valid */
		tmpV16 =  rtw_read16(adapt, REG_SYS_FUNC_EN);
		if (!(tmpV16 & FEN_ELDR)) {
			tmpV16 |= FEN_ELDR;
			rtw_write16(adapt, REG_SYS_FUNC_EN, tmpV16);
		}

		/* Clock: Gated(0x0008h[5]) 8M(0x0008h[1]) clock from ANA, default valid */
		tmpV16 = rtw_read16(adapt, REG_SYS_CLKR);
		if ((!(tmpV16 & LOADER_CLK_EN))  || (!(tmpV16 & ANA8M))) {
			tmpV16 |= (LOADER_CLK_EN | ANA8M);
			rtw_write16(adapt, REG_SYS_CLKR, tmpV16);
		}

		if (bWrite) {
			/* Enable LDO 2.5V before read/write action */
			tempval = rtw_read8(adapt, EFUSE_TEST + 3);
			tempval &= 0x0F;
			tempval |= (VOLTAGE_V25 << 4);
			rtw_write8(adapt, EFUSE_TEST + 3, (tempval | 0x80));

			/* rtw_write8(adapt, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON); */
		}
	} else {
		rtw_write8(adapt, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);

		if (bWrite) {
			/* Disable LDO 2.5V after read/write action */
			tempval = rtw_read8(adapt, EFUSE_TEST + 3);
			rtw_write8(adapt, EFUSE_TEST + 3, (tempval & 0x7F));
		}

	}
}

static void hal_ReadEFuse_WiFi(struct adapter * adapt, u16 _offset, u16 _size_byte,
			       u8 *pbuf, bool bPseudoTest)
{
#ifdef HAL_EFUSE_MEMORY
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal *		pEfuseHal = &pHalData->EfuseHal;
#endif
	u8	*efuseTbl = NULL;
	u16	eFuse_Addr = 0;
	u8	offset, wden;
	u8	efuseHeader, efuseExtHdr, efuseData;
	u16	i, total, used;
	u8	efuse_usage = 0;

	/* RTW_INFO("YJ: ====>%s():_offset=%d _size_byte=%d bPseudoTest=%d\n", __func__, _offset, _size_byte, bPseudoTest); */
	/* */
	/* Do NOT excess total size of EFuse table. Added by Roger, 2008.11.10. */
	/* */
	if ((_offset + _size_byte) > EFUSE_MAX_MAP_LEN) {
		RTW_INFO("%s: Invalid offset(%#x) with read bytes(%#x)!!\n", __func__, _offset, _size_byte);
		return;
	}

	efuseTbl = (u8 *)rtw_malloc(EFUSE_MAX_MAP_LEN);
	if (!efuseTbl) {
		RTW_INFO("%s: alloc efuseTbl fail!\n", __func__);
		return;
	}
	/* 0xff will be efuse default value instead of 0x00. */
	memset(efuseTbl, 0xFF, EFUSE_MAX_MAP_LEN);

	/* switch bank back to bank 0 for later BT and wifi use. */
	hal_EfuseSwitchToBank(adapt, 0, bPseudoTest);

	while (AVAILABLE_EFUSE_ADDR(eFuse_Addr)) {
		/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseHeader, bPseudoTest); */
		efuse_OneByteRead(adapt, eFuse_Addr++, &efuseHeader, bPseudoTest);
		if (efuseHeader == 0xFF) {
			RTW_INFO("%s: data end at address=%#x\n", __func__, eFuse_Addr - 1);
			break;
		}
		/* RTW_INFO("%s: efuse[0x%X]=0x%02X\n", __func__, eFuse_Addr-1, efuseHeader); */

		/* Check PG header for section num. */
		if (EXT_HEADER(efuseHeader)) {	/* extended header */
			offset = GET_HDR_OFFSET_2_0(efuseHeader);
			/* RTW_INFO("%s: extended header offset=0x%X\n", __func__, offset); */

			/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseExtHdr, bPseudoTest); */
			efuse_OneByteRead(adapt, eFuse_Addr++, &efuseExtHdr, bPseudoTest);
			/* RTW_INFO("%s: efuse[0x%X]=0x%02X\n", __func__, eFuse_Addr-1, efuseExtHdr); */
			if (ALL_WORDS_DISABLED(efuseExtHdr))
				continue;

			offset |= ((efuseExtHdr & 0xF0) >> 1);
			wden = (efuseExtHdr & 0x0F);
		} else {
			offset = ((efuseHeader >> 4) & 0x0f);
			wden = (efuseHeader & 0x0f);
		}
		/* RTW_INFO("%s: Offset=%d Worden=0x%X\n", __func__, offset, wden); */

		if (offset < EFUSE_MAX_SECTION_8723D) {
			u16 addr;
			/* Get word enable value from PG header
			*			RTW_INFO("%s: Offset=%d Worden=0x%X\n", __func__, offset, wden); */

			addr = offset * PGPKT_DATA_SIZE;
			for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
				/* Check word enable condition in the section */
				if (!(wden & (0x01 << i))) {
					efuseData = 0;
					/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseData, bPseudoTest); */
					efuse_OneByteRead(adapt, eFuse_Addr++, &efuseData, bPseudoTest);
					/*					RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, eFuse_Addr-1, efuseData); */
					efuseTbl[addr] = efuseData;

					efuseData = 0;
					/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseData, bPseudoTest); */
					efuse_OneByteRead(adapt, eFuse_Addr++, &efuseData, bPseudoTest);
					/*					RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, eFuse_Addr-1, efuseData); */
					efuseTbl[addr + 1] = efuseData;
				}
				addr += 2;
			}
		} else {
			RTW_INFO(KERN_ERR "%s: offset(%d) is illegal!!\n", __func__, offset);
			eFuse_Addr += Efuse_CalculateWordCnts(wden) * 2;
		}
	}

	/* Copy from Efuse map to output pointer memory!!! */
	for (i = 0; i < _size_byte; i++)
		pbuf[i] = efuseTbl[_offset + i];

	/* Calculate Efuse utilization */
	total = 0;
	EFUSE_GetEfuseDefinition(adapt, EFUSE_WIFI, TYPE_AVAILABLE_EFUSE_BYTES_TOTAL, &total, bPseudoTest);
	used = eFuse_Addr - 1;
	if (total)
		efuse_usage = (u8)((used * 100) / total);
	else
		efuse_usage = 100;
	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		pEfuseHal->fakeEfuseUsedBytes = used;
#else
		fakeEfuseUsedBytes = used;
#endif
	} else {
		rtw_hal_set_hwreg(adapt, HW_VAR_EFUSE_BYTES, (u8 *)&used);
		rtw_hal_set_hwreg(adapt, HW_VAR_EFUSE_USAGE, (u8 *)&efuse_usage);
	}

	if (efuseTbl)
		rtw_mfree(efuseTbl, EFUSE_MAX_MAP_LEN);
}

static void hal_ReadEFuse_BT(struct adapter * adapt, u16 _offset, u16 _size_byte,
			     u8 *pbuf, bool bPseudoTest)
{
#ifdef HAL_EFUSE_MEMORY
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal *		pEfuseHal = &pHalData->EfuseHal;
#endif
	u8	*efuseTbl;
	u8	bank;
	u16	eFuse_Addr;
	u8	efuseHeader, efuseExtHdr, efuseData;
	u8	offset, wden;
	u16	i, total, used;
	u8	efuse_usage;


	/* */
	/* Do NOT excess total size of EFuse table. Added by Roger, 2008.11.10. */
	/* */
	if ((_offset + _size_byte) > EFUSE_BT_MAP_LEN) {
		RTW_INFO("%s: Invalid offset(%#x) with read bytes(%#x)!!\n", __func__, _offset, _size_byte);
		return;
	}

	efuseTbl = rtw_malloc(EFUSE_BT_MAP_LEN);
	if (!efuseTbl) {
		RTW_INFO("%s: efuseTbl malloc fail!\n", __func__);
		return;
	}
	/* 0xff will be efuse default value instead of 0x00. */
	memset(efuseTbl, 0xFF, EFUSE_BT_MAP_LEN);

	total = 0;
	EFUSE_GetEfuseDefinition(adapt, EFUSE_BT, TYPE_AVAILABLE_EFUSE_BYTES_BANK, &total, bPseudoTest);

	for (bank = 1; bank < 3; bank++) { /* 8723d Max bake 0~2 */
		if (!hal_EfuseSwitchToBank(adapt, bank, bPseudoTest)) {
			RTW_INFO("%s: hal_EfuseSwitchToBank Fail!!\n", __func__);
			goto exit;
		}

		eFuse_Addr = 0;

		while (AVAILABLE_EFUSE_ADDR(eFuse_Addr)) {
			/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseHeader, bPseudoTest); */
			efuse_OneByteRead(adapt, eFuse_Addr++, &efuseHeader, bPseudoTest);
			if (efuseHeader == 0xFF)
				break;
			RTW_INFO("%s: efuse[%#X]=0x%02x (header)\n", __func__, (((bank - 1) * EFUSE_REAL_CONTENT_LEN_8723D) + eFuse_Addr - 1), efuseHeader);

			/* Check PG header for section num. */
			if (EXT_HEADER(efuseHeader)) {	/* extended header */
				offset = GET_HDR_OFFSET_2_0(efuseHeader);
				RTW_INFO("%s: extended header offset_2_0=0x%X\n", __func__, offset);

				/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseExtHdr, bPseudoTest); */
				efuse_OneByteRead(adapt, eFuse_Addr++, &efuseExtHdr, bPseudoTest);
				RTW_INFO("%s: efuse[%#X]=0x%02x (ext header)\n", __func__, (((bank - 1) * EFUSE_REAL_CONTENT_LEN_8723D) + eFuse_Addr - 1), efuseExtHdr);
				if (ALL_WORDS_DISABLED(efuseExtHdr))
					continue;

				offset |= ((efuseExtHdr & 0xF0) >> 1);
				wden = (efuseExtHdr & 0x0F);
			} else {
				offset = ((efuseHeader >> 4) & 0x0f);
				wden = (efuseHeader & 0x0f);
			}

			if (offset < EFUSE_BT_MAX_SECTION) {
				u16 addr;

				/* Get word enable value from PG header */
				RTW_INFO("%s: Offset=%d Worden=%#X\n", __func__, offset, wden);

				addr = offset * PGPKT_DATA_SIZE;
				for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
					/* Check word enable condition in the section */
					if (!(wden & (0x01 << i))) {
						efuseData = 0;
						/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseData, bPseudoTest); */
						efuse_OneByteRead(adapt, eFuse_Addr++, &efuseData, bPseudoTest);
						RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, eFuse_Addr - 1, efuseData);
						efuseTbl[addr] = efuseData;

						efuseData = 0;
						/* ReadEFuseByte(adapt, eFuse_Addr++, &efuseData, bPseudoTest); */
						efuse_OneByteRead(adapt, eFuse_Addr++, &efuseData, bPseudoTest);
						RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, eFuse_Addr - 1, efuseData);
						efuseTbl[addr + 1] = efuseData;
					}
					addr += 2;
				}
			} else {
				RTW_INFO("%s: offset(%d) is illegal!!\n", __func__, offset);
				eFuse_Addr += Efuse_CalculateWordCnts(wden) * 2;
			}
		}

		if ((eFuse_Addr - 1) < total) {
			RTW_INFO("%s: bank(%d) data end at %#x\n", __func__, bank, eFuse_Addr - 1);
			break;
		}
	}

	/* switch bank back to bank 0 for later BT and wifi use. */
	hal_EfuseSwitchToBank(adapt, 0, bPseudoTest);

	/* Copy from Efuse map to output pointer memory!!! */
	for (i = 0; i < _size_byte; i++)
		pbuf[i] = efuseTbl[_offset + i];

	/* */
	/* Calculate Efuse utilization. */
	/* */
	EFUSE_GetEfuseDefinition(adapt, EFUSE_BT, TYPE_AVAILABLE_EFUSE_BYTES_TOTAL, &total, bPseudoTest);
	used = (EFUSE_BT_REAL_BANK_CONTENT_LEN * (bank - 1)) + eFuse_Addr - 1;
	RTW_INFO("%s: bank(%d) data end at %#x ,used =%d\n", __func__, bank, eFuse_Addr - 1, used);
	efuse_usage = (u8)((used * 100) / total);
	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		pEfuseHal->fakeBTEfuseUsedBytes = used;
#else
		fakeBTEfuseUsedBytes = used;
#endif
	} else {
		rtw_hal_set_hwreg(adapt, HW_VAR_EFUSE_BT_BYTES, (u8 *)&used);
		rtw_hal_set_hwreg(adapt, HW_VAR_EFUSE_BT_USAGE, (u8 *)&efuse_usage);
	}

exit:
	if (efuseTbl)
		rtw_mfree(efuseTbl, EFUSE_BT_MAP_LEN);
}

static void Hal_ReadEFuse(struct adapter * adapt, u8 efuseType, u16 _offset,
			  u16 _size_byte, u8 *pbuf, bool bPseudoTest)
{
	if (efuseType == EFUSE_WIFI)
		hal_ReadEFuse_WiFi(adapt, _offset, _size_byte, pbuf, bPseudoTest);
	else
		hal_ReadEFuse_BT(adapt, _offset, _size_byte, pbuf, bPseudoTest);
}

static u16 hal_EfuseGetCurrentSize_WiFi(struct adapter * adapt, bool bPseudoTest)
{
#ifdef HAL_EFUSE_MEMORY
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal *		pEfuseHal = &pHalData->EfuseHal;
#endif
	u16	efuse_addr = 0;
	u16 start_addr = 0; /* for debug */
	u8	hoffset = 0, hworden = 0;
	u8	efuse_data, word_cnts = 0;
	u32 count = 0; /* for debug */


	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		efuse_addr = (u16)pEfuseHal->fakeEfuseUsedBytes;
#else
		efuse_addr = (u16)fakeEfuseUsedBytes;
#endif
	} else
		rtw_hal_get_hwreg(adapt, HW_VAR_EFUSE_BYTES, (u8 *)&efuse_addr);
	start_addr = efuse_addr;
	RTW_INFO("%s: start_efuse_addr=0x%X\n", __func__, efuse_addr);

	/* switch bank back to bank 0 for later BT and wifi use. */
	hal_EfuseSwitchToBank(adapt, 0, bPseudoTest);

	count = 0;
	while (AVAILABLE_EFUSE_ADDR(efuse_addr)) {
		if (!efuse_OneByteRead(adapt, efuse_addr, &efuse_data, bPseudoTest)) {
			RTW_INFO(KERN_ERR "%s: efuse_OneByteRead Fail! addr=0x%X !!\n", __func__, efuse_addr);
			goto error;
		}
		if (efuse_data == 0xFF)
			break;

		if ((start_addr != 0) && (efuse_addr == start_addr)) {
			count++;
			RTW_INFO(FUNC_ADPT_FMT ": [WARNING] efuse raw 0x%X=0x%02X not 0xFF!!(%d times)\n",
				FUNC_ADPT_ARG(adapt), efuse_addr, efuse_data, count);

			efuse_data = 0xFF;
			if (count < 4) {
				/* try again! */

				if (count > 2) {
					/* try again form address 0 */
					efuse_addr = 0;
					start_addr = 0;
				}

				continue;
			}

			goto error;
		}

		if (EXT_HEADER(efuse_data)) {
			hoffset = GET_HDR_OFFSET_2_0(efuse_data);
			efuse_addr++;
			efuse_OneByteRead(adapt, efuse_addr, &efuse_data, bPseudoTest);
			if (ALL_WORDS_DISABLED(efuse_data))
				continue;

			hoffset |= ((efuse_data & 0xF0) >> 1);
			hworden = efuse_data & 0x0F;
		} else {
			hoffset = (efuse_data >> 4) & 0x0F;
			hworden = efuse_data & 0x0F;
		}

		word_cnts = Efuse_CalculateWordCnts(hworden);
		efuse_addr += (word_cnts * 2) + 1;
	}

	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		pEfuseHal->fakeEfuseUsedBytes = efuse_addr;
#else
		fakeEfuseUsedBytes = efuse_addr;
#endif
	} else
		rtw_hal_set_hwreg(adapt, HW_VAR_EFUSE_BYTES, (u8 *)&efuse_addr);

	goto exit;

error:
	/* report max size to prevent write efuse */
	EFUSE_GetEfuseDefinition(adapt, EFUSE_WIFI, TYPE_AVAILABLE_EFUSE_BYTES_TOTAL, &efuse_addr, bPseudoTest);

exit:
	RTW_INFO("%s: CurrentSize=%d\n", __func__, efuse_addr);

	return efuse_addr;
}

static u16
hal_EfuseGetCurrentSize_BT(
	struct adapter *	adapt,
	u8			bPseudoTest)
{
#ifdef HAL_EFUSE_MEMORY
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal *		pEfuseHal = &pHalData->EfuseHal;
#endif
	u16 btusedbytes;
	u16	efuse_addr;
	u8	bank, startBank;
	u8	hoffset = 0, hworden = 0;
	u8	efuse_data, word_cnts = 0;
	u16	retU2 = 0;

	if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
		btusedbytes = pEfuseHal->fakeBTEfuseUsedBytes;
#else
		btusedbytes = fakeBTEfuseUsedBytes;
#endif
	} else {
		btusedbytes = 0;
		rtw_hal_get_hwreg(adapt, HW_VAR_EFUSE_BT_BYTES, (u8 *)&btusedbytes);
	}
	efuse_addr = (u16)((btusedbytes % EFUSE_BT_REAL_BANK_CONTENT_LEN));
	startBank = (u8)(1 + (btusedbytes / EFUSE_BT_REAL_BANK_CONTENT_LEN));

	RTW_INFO("%s: start from bank=%d addr=0x%X\n", __func__, startBank, efuse_addr);

	EFUSE_GetEfuseDefinition(adapt, EFUSE_BT, TYPE_AVAILABLE_EFUSE_BYTES_BANK, &retU2, bPseudoTest);

	for (bank = startBank; bank < 3; bank++) {
		if (!hal_EfuseSwitchToBank(adapt, bank, bPseudoTest)) {
			RTW_INFO(KERN_ERR "%s: switch bank(%d) Fail!!\n", __func__, bank);
			/* bank = EFUSE_MAX_BANK; */
			break;
		}

		/* only when bank is switched we have to reset the efuse_addr. */
		if (bank != startBank)
			efuse_addr = 0;

		while (AVAILABLE_EFUSE_ADDR(efuse_addr)) {
			if (!efuse_OneByteRead(adapt, efuse_addr, &efuse_data, bPseudoTest)) {
				RTW_INFO(KERN_ERR "%s: efuse_OneByteRead Fail! addr=0x%X !!\n", __func__, efuse_addr);
				/* bank = EFUSE_MAX_BANK; */
				break;
			}
			RTW_INFO("%s: efuse_OneByteRead ! addr=0x%X !efuse_data=0x%X! bank =%d\n", __func__, efuse_addr, efuse_data, bank);

			if (efuse_data == 0xFF)
				break;

			if (EXT_HEADER(efuse_data)) {
				hoffset = GET_HDR_OFFSET_2_0(efuse_data);
				efuse_addr++;
				efuse_OneByteRead(adapt, efuse_addr, &efuse_data, bPseudoTest);
				RTW_INFO("%s: efuse_OneByteRead EXT_HEADER ! addr=0x%X !efuse_data=0x%X! bank =%d\n", __func__, efuse_addr, efuse_data, bank);

				if (ALL_WORDS_DISABLED(efuse_data)) {
					efuse_addr++;
					continue;
				}

				/*				hoffset = ((hoffset & 0xE0) >> 5) | ((efuse_data & 0xF0) >> 1); */
				hoffset |= ((efuse_data & 0xF0) >> 1);
				hworden = efuse_data & 0x0F;
			} else {
				hoffset = (efuse_data >> 4) & 0x0F;
				hworden =  efuse_data & 0x0F;
			}

			RTW_INFO(FUNC_ADPT_FMT": Offset=%d Worden=%#X\n",
				 FUNC_ADPT_ARG(adapt), hoffset, hworden);

			word_cnts = Efuse_CalculateWordCnts(hworden);
			/* read next header */
			efuse_addr += (word_cnts * 2) + 1;
		}
		/* Check if we need to check next bank efuse */
		if (efuse_addr < retU2)
			break;/* don't need to check next bank. */
	}
	retU2 = ((bank - 1) * EFUSE_BT_REAL_BANK_CONTENT_LEN) + efuse_addr;
	if (bPseudoTest) {
		pEfuseHal->fakeBTEfuseUsedBytes = retU2;
	} else {
		pEfuseHal->BTEfuseUsedBytes = retU2;
	}

	RTW_INFO("%s: CurrentSize=%d\n", __func__, retU2);
	return retU2;
}

static u16 Hal_EfuseGetCurrentSize(struct adapter * pAdapter, u8 efuseType,
				   bool bPseudoTest)
{
	u16	ret = 0;

	if (efuseType == EFUSE_WIFI)
		ret = hal_EfuseGetCurrentSize_WiFi(pAdapter, bPseudoTest);
	else
		ret = hal_EfuseGetCurrentSize_BT(pAdapter, bPseudoTest);

	return ret;
}

static u8
Hal_EfuseWordEnableDataWrite(
	struct adapter *	adapt,
	u16			efuse_addr,
	u8			word_en,
	u8			*data,
	bool			bPseudoTest)
{
	u16	tmpaddr = 0;
	u16	start_addr = efuse_addr;
	u8	badworden = 0x0F;
	u8	tmpdata[PGPKT_DATA_SIZE];


	/*	RTW_INFO("%s: efuse_addr=%#x word_en=%#x\n", __func__, efuse_addr, word_en); */
	memset(tmpdata, 0xFF, PGPKT_DATA_SIZE);

	if (!(word_en & BIT(0))) {
		tmpaddr = start_addr;
		efuse_OneByteWrite(adapt, start_addr++, data[0], bPseudoTest);
		efuse_OneByteWrite(adapt, start_addr++, data[1], bPseudoTest);

		efuse_OneByteRead(adapt, tmpaddr, &tmpdata[0], bPseudoTest);
		efuse_OneByteRead(adapt, tmpaddr + 1, &tmpdata[1], bPseudoTest);
		if ((data[0] != tmpdata[0]) || (data[1] != tmpdata[1]))
			badworden &= (~BIT(0));
	}
	if (!(word_en & BIT(1))) {
		tmpaddr = start_addr;
		efuse_OneByteWrite(adapt, start_addr++, data[2], bPseudoTest);
		efuse_OneByteWrite(adapt, start_addr++, data[3], bPseudoTest);

		efuse_OneByteRead(adapt, tmpaddr, &tmpdata[2], bPseudoTest);
		efuse_OneByteRead(adapt, tmpaddr + 1, &tmpdata[3], bPseudoTest);
		if ((data[2] != tmpdata[2]) || (data[3] != tmpdata[3]))
			badworden &= (~BIT(1));
	}
	if (!(word_en & BIT(2))) {
		tmpaddr = start_addr;
		efuse_OneByteWrite(adapt, start_addr++, data[4], bPseudoTest);
		efuse_OneByteWrite(adapt, start_addr++, data[5], bPseudoTest);

		efuse_OneByteRead(adapt, tmpaddr, &tmpdata[4], bPseudoTest);
		efuse_OneByteRead(adapt, tmpaddr + 1, &tmpdata[5], bPseudoTest);
		if ((data[4] != tmpdata[4]) || (data[5] != tmpdata[5]))
			badworden &= (~BIT(2));
	}
	if (!(word_en & BIT(3))) {
		tmpaddr = start_addr;
		efuse_OneByteWrite(adapt, start_addr++, data[6], bPseudoTest);
		efuse_OneByteWrite(adapt, start_addr++, data[7], bPseudoTest);

		efuse_OneByteRead(adapt, tmpaddr, &tmpdata[6], bPseudoTest);
		efuse_OneByteRead(adapt, tmpaddr + 1, &tmpdata[7], bPseudoTest);
		if ((data[6] != tmpdata[6]) || (data[7] != tmpdata[7]))
			badworden &= (~BIT(3));
	}

	return badworden;
}

static bool Hal_EfusePgPacketRead(struct adapter *	adapt,
	u8			offset,
	u8			*data,
	bool			bPseudoTest)
{
	u8	efuse_data, word_cnts = 0;
	u16	efuse_addr = 0;
	u8	hoffset = 0, hworden = 0;
	u8	i;
	u8	max_section = 0;
	bool	ret;


	if (!data)
		return false;

	EFUSE_GetEfuseDefinition(adapt, EFUSE_WIFI, TYPE_EFUSE_MAX_SECTION, &max_section, bPseudoTest);
	if (offset > max_section) {
		RTW_INFO("%s: Packet offset(%d) is illegal(>%d)!\n", __func__, offset, max_section);
		return false;
	}

	memset(data, 0xFF, PGPKT_DATA_SIZE);
	ret = true;

	/* */
	/* <Roger_TODO> Efuse has been pre-programmed dummy 5Bytes at the end of Efuse by CP. */
	/* Skip dummy parts to prevent unexpected data read from Efuse. */
	/* By pass right now. 2009.02.19. */
	/* */
	while (AVAILABLE_EFUSE_ADDR(efuse_addr)) {
		if (!efuse_OneByteRead(adapt, efuse_addr++, &efuse_data, bPseudoTest)) {
			ret = false;
			break;
		}

		if (efuse_data == 0xFF)
			break;

		if (EXT_HEADER(efuse_data)) {
			hoffset = GET_HDR_OFFSET_2_0(efuse_data);
			efuse_OneByteRead(adapt, efuse_addr++, &efuse_data, bPseudoTest);
			if (ALL_WORDS_DISABLED(efuse_data)) {
				RTW_INFO("%s: Error!! All words disabled!\n", __func__);
				continue;
			}

			hoffset |= ((efuse_data & 0xF0) >> 1);
			hworden = efuse_data & 0x0F;
		} else {
			hoffset = (efuse_data >> 4) & 0x0F;
			hworden =  efuse_data & 0x0F;
		}

		if (hoffset == offset) {
			for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
				/* Check word enable condition in the section */
				if (!(hworden & (0x01 << i))) {
					/* ReadEFuseByte(adapt, efuse_addr++, &efuse_data, bPseudoTest); */
					efuse_OneByteRead(adapt, efuse_addr++, &efuse_data, bPseudoTest);
					/*					RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, efuse_addr+tmpidx, efuse_data); */
					data[i * 2] = efuse_data;

					/* ReadEFuseByte(adapt, efuse_addr++, &efuse_data, bPseudoTest); */
					efuse_OneByteRead(adapt, efuse_addr++, &efuse_data, bPseudoTest);
					/*					RTW_INFO("%s: efuse[%#X]=0x%02X\n", __func__, efuse_addr+tmpidx, efuse_data); */
					data[(i * 2) + 1] = efuse_data;
				}
			}
		} else {
			word_cnts = Efuse_CalculateWordCnts(hworden);
			efuse_addr += word_cnts * 2;
		}
	}

	return ret;
}

static bool
hal_EfusePgCheckAvailableAddr(
	struct adapter *	pAdapter,
	u8			efuseType,
	u8		bPseudoTest)
{
	u16	max_available = 0;
	u16 current_size;


	EFUSE_GetEfuseDefinition(pAdapter, efuseType, TYPE_AVAILABLE_EFUSE_BYTES_TOTAL, &max_available, bPseudoTest);
	/*	RTW_INFO("%s: max_available=%d\n", __func__, max_available); */

	current_size = Efuse_GetCurrentSize(pAdapter, efuseType, bPseudoTest);
	if (current_size >= max_available) {
		RTW_INFO("%s: Error!! current_size(%d)>max_available(%d)\n", __func__, current_size, max_available);
		return false;
	}
	return true;
}

static void
hal_EfuseConstructPGPkt(
	u8				offset,
	u8				word_en,
	u8				*pData,
	struct pg_pkt_struct *	pTargetPkt)
{
	memset(pTargetPkt->data, 0xFF, PGPKT_DATA_SIZE);
	pTargetPkt->offset = offset;
	pTargetPkt->word_en = word_en;
	efuse_WordEnableDataRead(word_en, pData, pTargetPkt->data);
	pTargetPkt->word_cnts = Efuse_CalculateWordCnts(pTargetPkt->word_en);
}

static u8
hal_EfusePartialWriteCheck(
	struct adapter *		adapt,
	u8				efuseType,
	u16				*pAddr,
	struct pg_pkt_struct *	pTargetPkt,
	u8				bPseudoTest)
{
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	struct efuse_hal *		pEfuseHal = &pHalData->EfuseHal;
	u8	bRet = false;
	u16	startAddr = 0, efuse_max_available_len = 0, efuse_max = 0;
	u8	efuse_data = 0;

	EFUSE_GetEfuseDefinition(adapt, efuseType, TYPE_AVAILABLE_EFUSE_BYTES_TOTAL, &efuse_max_available_len, bPseudoTest);
	EFUSE_GetEfuseDefinition(adapt, efuseType, TYPE_EFUSE_CONTENT_LEN_BANK, &efuse_max, bPseudoTest);

	if (efuseType == EFUSE_WIFI) {
		if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
			startAddr = (u16)pEfuseHal->fakeEfuseUsedBytes;
#else
			startAddr = (u16)fakeEfuseUsedBytes;
#endif
		} else
			rtw_hal_get_hwreg(adapt, HW_VAR_EFUSE_BYTES, (u8 *)&startAddr);
	} else {
		if (bPseudoTest) {
#ifdef HAL_EFUSE_MEMORY
			startAddr = (u16)pEfuseHal->fakeBTEfuseUsedBytes;
#else
			startAddr = (u16)fakeBTEfuseUsedBytes;
#endif
		} else
			rtw_hal_get_hwreg(adapt, HW_VAR_EFUSE_BT_BYTES, (u8 *)&startAddr);
	}
	startAddr %= efuse_max;
	RTW_INFO("%s: startAddr=%#X\n", __func__, startAddr);

	while (1) {
		if (startAddr >= efuse_max_available_len) {
			bRet = false;
			RTW_INFO("%s: startAddr(%d) >= efuse_max_available_len(%d)\n",
				__func__, startAddr, efuse_max_available_len);
			break;
		}

		if (efuse_OneByteRead(adapt, startAddr, &efuse_data, bPseudoTest) && (efuse_data != 0xFF)) {
			bRet = false;
			RTW_INFO("%s: Something Wrong! last bytes(%#X=0x%02X) is not 0xFF\n",
				 __func__, startAddr, efuse_data);
			break;
		} else {
			/* not used header, 0xff */
			*pAddr = startAddr;
			/*			RTW_INFO("%s: Started from unused header offset=%d\n", __func__, startAddr)); */
			bRet = true;
			break;
		}
	}

	return bRet;
}

static bool
hal_EfusePgPacketWrite1ByteHeader(
	struct adapter *		pAdapter,
	u8				efuseType,
	u16				*pAddr,
	struct pg_pkt_struct *	pTargetPkt,
	u8				bPseudoTest)
{
	u8	pg_header = 0, tmp_header = 0;
	u16	efuse_addr = *pAddr;
	u8	repeatcnt = 0;


	/*	RTW_INFO("%s\n", __func__); */
	pg_header = ((pTargetPkt->offset << 4) & 0xf0) | pTargetPkt->word_en;

	do {
		efuse_OneByteWrite(pAdapter, efuse_addr, pg_header, bPseudoTest);
		efuse_OneByteRead(pAdapter, efuse_addr, &tmp_header, bPseudoTest);
		if (tmp_header != 0xFF)
			break;
		if (repeatcnt++ > EFUSE_REPEAT_THRESHOLD_) {
			RTW_INFO("%s: Repeat over limit for pg_header!!\n", __func__);
			return false;
		}
	} while (1);

	if (tmp_header != pg_header) {
		RTW_INFO(KERN_ERR "%s: PG Header Fail!!(pg=0x%02X read=0x%02X)\n", __func__, pg_header, tmp_header);
		return false;
	}

	*pAddr = efuse_addr;

	return true;
}

static bool
hal_EfusePgPacketWrite2ByteHeader(
	struct adapter *		adapt,
	u8				efuseType,
	u16				*pAddr,
	struct pg_pkt_struct *	pTargetPkt,
	u8				bPseudoTest)
{
	u16	efuse_addr, efuse_max_available_len = 0;
	u8	pg_header = 0, tmp_header = 0;
	u8	repeatcnt = 0;


	/*	RTW_INFO("%s\n", __func__); */
	EFUSE_GetEfuseDefinition(adapt, efuseType, TYPE_AVAILABLE_EFUSE_BYTES_BANK, &efuse_max_available_len, bPseudoTest);

	efuse_addr = *pAddr;
	if (efuse_addr >= efuse_max_available_len) {
		RTW_INFO("%s: addr(%d) over available(%d)!!\n", __func__, efuse_addr, efuse_max_available_len);
		return false;
	}

	pg_header = ((pTargetPkt->offset & 0x07) << 5) | 0x0F;
	/*	RTW_INFO("%s: pg_header=0x%x\n", __func__, pg_header); */

	do {
		efuse_OneByteWrite(adapt, efuse_addr, pg_header, bPseudoTest);
		efuse_OneByteRead(adapt, efuse_addr, &tmp_header, bPseudoTest);
		if (tmp_header != 0xFF)
			break;
		if (repeatcnt++ > EFUSE_REPEAT_THRESHOLD_) {
			RTW_INFO("%s: Repeat over limit for pg_header!!\n", __func__);
			return false;
		}
	} while (1);

	if (tmp_header != pg_header) {
		RTW_INFO(KERN_ERR "%s: PG Header Fail!!(pg=0x%02X read=0x%02X)\n", __func__, pg_header, tmp_header);
		return false;
	}

	/* to write ext_header */
	efuse_addr++;
	pg_header = ((pTargetPkt->offset & 0x78) << 1) | pTargetPkt->word_en;

	do {
		efuse_OneByteWrite(adapt, efuse_addr, pg_header, bPseudoTest);
		efuse_OneByteRead(adapt, efuse_addr, &tmp_header, bPseudoTest);
		if (tmp_header != 0xFF)
			break;
		if (repeatcnt++ > EFUSE_REPEAT_THRESHOLD_) {
			RTW_INFO("%s: Repeat over limit for ext_header!!\n", __func__);
			return false;
		}
	} while (1);

	if (tmp_header != pg_header) {	/* offset PG fail */
		RTW_INFO(KERN_ERR "%s: PG EXT Header Fail!!(pg=0x%02X read=0x%02X)\n", __func__, pg_header, tmp_header);
		return false;
	}

	*pAddr = efuse_addr;

	return true;
}

static u8
hal_EfusePgPacketWriteHeader(
	struct adapter *		adapt,
	u8				efuseType,
	u16				*pAddr,
	struct pg_pkt_struct *	pTargetPkt,
	u8				bPseudoTest)
{
	u8 bRet = false;

	if (pTargetPkt->offset >= EFUSE_MAX_SECTION_BASE)
		bRet = hal_EfusePgPacketWrite2ByteHeader(adapt, efuseType, pAddr, pTargetPkt, bPseudoTest);
	else
		bRet = hal_EfusePgPacketWrite1ByteHeader(adapt, efuseType, pAddr, pTargetPkt, bPseudoTest);

	return bRet;
}

static bool
hal_EfusePgPacketWriteData(
	struct adapter *		pAdapter,
	u8				efuseType,
	u16				*pAddr,
	struct pg_pkt_struct *	pTargetPkt,
	u8				bPseudoTest)
{
	u16	efuse_addr;
	u8	badworden;


	efuse_addr = *pAddr;
	badworden = Efuse_WordEnableDataWrite(pAdapter, efuse_addr + 1, pTargetPkt->word_en, pTargetPkt->data, bPseudoTest);
	if (badworden != 0x0F) {
		RTW_INFO("%s: Fail!!\n", __func__);
		return false;
	}

	/*	RTW_INFO("%s: ok\n", __func__); */
	return true;
}

static bool Hal_EfusePgPacketWrite(struct adapter * adapt,
	u8			offset,
	u8			word_en,
	u8			*pData,
	bool			bPseudoTest)
{
	struct pg_pkt_struct targetPkt;
	u16 startAddr = 0;
	u8 efuseType = EFUSE_WIFI;

	if (!hal_EfusePgCheckAvailableAddr(adapt, efuseType, bPseudoTest))
		return false;

	hal_EfuseConstructPGPkt(offset, word_en, pData, &targetPkt);

	if (!hal_EfusePartialWriteCheck(adapt, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	if (!hal_EfusePgPacketWriteHeader(adapt, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	if (!hal_EfusePgPacketWriteData(adapt, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	return true;
}

static bool
Hal_EfusePgPacketWrite_BT(
	struct adapter *	pAdapter,
	u8			offset,
	u8			word_en,
	u8			*pData,
	bool			bPseudoTest)
{
	struct pg_pkt_struct targetPkt;
	u16 startAddr = 0;
	u8 efuseType = EFUSE_BT;

	if (!hal_EfusePgCheckAvailableAddr(pAdapter, efuseType, bPseudoTest))
		return false;

	hal_EfuseConstructPGPkt(offset, word_en, pData, &targetPkt);

	if (!hal_EfusePartialWriteCheck(pAdapter, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	if (!hal_EfusePgPacketWriteHeader(pAdapter, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	if (!hal_EfusePgPacketWriteData(pAdapter, efuseType, &startAddr, &targetPkt, bPseudoTest))
		return false;

	return true;
}


static void read_chip_version_8723d(struct adapter * adapt)
{
	u32				value32;
	struct hal_com_data	*pHalData;

	pHalData = GET_HAL_DATA(adapt);

	value32 = rtw_read32(adapt, REG_SYS_CFG);

	pHalData->version_id.ICType = CHIP_8723D;
	pHalData->version_id.ChipType = ((value32 & RTL_ID) ? TEST_CHIP : NORMAL_CHIP);
	pHalData->version_id.RFType = RF_TYPE_1T1R;
	pHalData->version_id.VendorType = ((value32 & VENDOR_ID) ? CHIP_VENDOR_UMC : CHIP_VENDOR_TSMC);
	pHalData->version_id.CUTVersion = (value32 & CHIP_VER_RTL_MASK) >> CHIP_VER_RTL_SHIFT; /* IC version (CUT) */

	/* For regulator mode. by tynli. 2011.01.14 */
	pHalData->RegulatorMode = ((value32 & SPS_SEL) ? RT_LDO_REGULATOR : RT_SWITCHING_REGULATOR);

	value32 = rtw_read32(adapt, REG_GPIO_OUTSTS);
	pHalData->version_id.ROMVer = ((value32 & RF_RL_ID) >> 20);	/* ROM code version. */

	/* For multi-function consideration. Added by Roger, 2010.10.06. */
	pHalData->MultiFunc = RT_MULTI_FUNC_NONE;
	value32 = rtw_read32(adapt, REG_MULTI_FUNC_CTRL);
	pHalData->MultiFunc |= ((value32 & WL_FUNC_EN) ? RT_MULTI_FUNC_WIFI : 0);
	pHalData->MultiFunc |= ((value32 & BT_FUNC_EN) ? RT_MULTI_FUNC_BT : 0);
	pHalData->MultiFunc |= ((value32 & GPS_FUNC_EN) ? RT_MULTI_FUNC_GPS : 0);
	pHalData->PolarityCtl = ((value32 & WL_HWPDN_SL) ? RT_POLARITY_HIGH_ACT : RT_POLARITY_LOW_ACT);

	rtw_hal_config_rftype(adapt);

	/*
		if( IS_B_CUT(pHalData->version_id) || IS_C_CUT(pHalData->version_id))
		{
			RTW_INFO(" IS_B/C_CUT SWR up 1 level !!!!!!!!!!!!!!!!!\n");
			phy_set_mac_reg(adapt, 0x14, BIT(23)|BIT(22)|BIT(21)|BIT(20), 0x5);
		}else if ( IS_D_CUT(pHalData->version_id))
		{
			RTW_INFO(" IS_D_CUT SKIP SWR !!!!!!!!!!!!!!!!!\n");
		}
	*/

	dump_chip_info(pHalData->version_id);
}

void rtl8723d_InitBeaconParameters(struct adapter * adapt)
{
	u16 val16;
	u8 val8;


	val8 = DIS_TSF_UDT;
	val16 = val8 | (val8 << 8); /* port0 and port1 */
	/* Enable prot0 beacon function for PSTDMA */
	val16 |= EN_BCN_FUNCTION;

#ifdef CONFIG_CONCURRENT_MODE
	val16 |= (EN_BCN_FUNCTION << 8);
#endif
	rtw_write16(adapt, REG_BCN_CTRL, val16);

	/* TBTT setup time */
	rtw_write8(adapt, REG_TBTT_PROHIBIT, TBTT_PROHIBIT_SETUP_TIME);

	/* TBTT hold time: 0x540[19:8] */
	rtw_write8(adapt, REG_TBTT_PROHIBIT + 1, TBTT_PROHIBIT_HOLD_TIME_STOP_BCN & 0xFF);
	rtw_write8(adapt, REG_TBTT_PROHIBIT + 2,
		(rtw_read8(adapt, REG_TBTT_PROHIBIT + 2) & 0xF0) | (TBTT_PROHIBIT_HOLD_TIME_STOP_BCN >> 8));

	/* Firmware will control REG_DRVERLYINT when power saving is enable, */
	/* so don't set this register on STA mode. */
	if (!check_fwstate(&adapt->mlmepriv, WIFI_STATION_STATE))
		rtw_write8(adapt, REG_DRVERLYINT, DRIVER_EARLY_INT_TIME_8723D); /* 5ms */
	rtw_write8(adapt, REG_BCNDMATIM, BCN_DMA_ATIME_INT_TIME_8723D); /* 2ms */

	/* Suggested by designer timchen. Change beacon AIFS to the largest number */
	/* beacause test chip does not contension before sending beacon. by tynli. 2009.11.03 */
	rtw_write16(adapt, REG_BCNTCFG, 0x660F);

}

void rtl8723d_InitBeaconMaxError(struct adapter * adapt, u8 InfraMode)
{
#ifdef CONFIG_ADHOC_WORKAROUND_SETTING
	rtw_write8(adapt, REG_BCN_MAX_ERR, 0xFF);
#else
	/* rtw_write8(Adapter, REG_BCN_MAX_ERR, (InfraMode ? 0xFF : 0x10)); */
#endif
}

void _InitMacAPLLSetting_8723D(struct adapter * Adapter)
{
	struct hal_com_data *	pHalData = GET_HAL_DATA(Adapter);
	u16 RegValue;
	u8	afe;

	RegValue = rtw_read16(Adapter, REG_AFE_CTRL_4_8723D);
	RegValue |= BIT(4);
	RegValue |= BIT(15);
	rtw_write16(Adapter, REG_AFE_CTRL_4_8723D, RegValue);

/*
 *	8723D with 24MHz xtal has VCO noise issue
 *  This will cause some TRx test fail
 *	Therefore, set MAC GM parameter for 24MHz xtal
 *	AFE[0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 ] =
 *	[ 40M 25M 13M 19.2M 20M 26M 38.4M 17.664M 16M 14.318M 12M 52M 48M 27M 24M ]
 */
	afe = (pHalData->efuse_eeprom_data[4] >>4);
	if( afe == 14) {
		rtw_write32(Adapter, 0x2c, (rtw_read32(Adapter, 0x2c) | BIT28));
		rtw_write32(Adapter, 0x24, (rtw_read32(Adapter, 0x24) & 0xFFFFFF0F));
		rtw_write32(Adapter, 0x7c, ((rtw_read32(Adapter, 0x7c) | BIT29) & (~BIT28)));
	}
}

static void _BeaconFunctionEnable(struct adapter * adapt, u8 Enable, u8 Linked)
{
	rtw_write8(adapt, REG_BCN_CTRL, DIS_TSF_UDT | EN_BCN_FUNCTION | DIS_BCNQ_SUB);
	rtw_write8(adapt, REG_RD_CTRL + 1, 0x6F);
}

static void rtl8723d_SetBeaconRelatedRegisters(struct adapter * adapt)
{
	u8 val8;
	u32 value32;
	struct mlme_ext_priv *pmlmeext = &adapt->mlmeextpriv;
	struct mlme_ext_info *pmlmeinfo = &pmlmeext->mlmext_info;
	u32 bcn_ctrl_reg;

	/* reset TSF, enable update TSF, correcting TSF On Beacon */

	/* REG_BCN_INTERVAL */
	/* REG_BCNDMATIM */
	/* REG_ATIMWND */
	/* REG_TBTT_PROHIBIT */
	/* REG_DRVERLYINT */
	/* REG_BCN_MAX_ERR */
	/* REG_BCNTCFG //(0x510) */
	/* REG_DUAL_TSF_RST */
	/* REG_BCN_CTRL //(0x550) */


	bcn_ctrl_reg = REG_BCN_CTRL;
#ifdef CONFIG_CONCURRENT_MODE
	if (adapt->hw_port == HW_PORT1)
		bcn_ctrl_reg = REG_BCN_CTRL_1;
#endif

	/* */
	/* ATIM window */
	/* */
	rtw_write16(adapt, REG_ATIMWND, 2);

	/* */
	/* Beacon interval (in unit of TU). */
	/* */
	rtw_write16(adapt, REG_BCN_INTERVAL, pmlmeinfo->bcn_interval);

	rtl8723d_InitBeaconParameters(adapt);

	rtw_write8(adapt, REG_SLOT, 0x09);

	/* */
	/* Reset TSF Timer to zero, added by Roger. 2008.06.24 */
	/* */
	value32 = rtw_read32(adapt, REG_TCR);
	value32 &= ~TSFRST;
	rtw_write32(adapt, REG_TCR, value32);

	value32 |= TSFRST;
	rtw_write32(adapt, REG_TCR, value32);

	/* NOTE: Fix test chip's bug (about contention windows's randomness) */
	if (check_fwstate(&adapt->mlmepriv, WIFI_ADHOC_STATE | WIFI_ADHOC_MASTER_STATE | WIFI_AP_STATE | WIFI_MESH_STATE)) {
		rtw_write8(adapt, REG_RXTSF_OFFSET_CCK, 0x50);
		rtw_write8(adapt, REG_RXTSF_OFFSET_OFDM, 0x50);
	}

	_BeaconFunctionEnable(adapt, true, true);

	ResumeTxBeacon(adapt);
	val8 = rtw_read8(adapt, bcn_ctrl_reg);
	val8 |= DIS_BCNQ_SUB;
	rtw_write8(adapt, bcn_ctrl_reg, val8);
}

static void hal_notch_filter_8723d(struct adapter *adapter, bool enable)
{
	if (enable) {
		RTW_INFO("Enable notch filter\n");
		rtw_write8(adapter, rOFDM0_RxDSP + 1, rtw_read8(adapter, rOFDM0_RxDSP + 1) | BIT(1));
	} else {
		RTW_INFO("Disable notch filter\n");
		rtw_write8(adapter, rOFDM0_RxDSP + 1, rtw_read8(adapter, rOFDM0_RxDSP + 1) & ~BIT(1));
	}
}

/*
 * Description: In normal chip, we should send some packet to Hw which will be used by Fw
 *			in FW LPS mode. The function is to fill the Tx descriptor of this packets, then
 *			Fw can tell Hw to send these packet derectly.
 * Added by tynli. 2009.10.15.
 *
 * type1:pspoll, type2:null */
void rtl8723d_fill_fake_txdesc(
	struct adapter *	adapt,
	u8			*pDesc,
	u32			BufferLen,
	u8			IsPsPoll,
	u8			IsBTQosNull,
	u8			bDataFrame)
{
	/* Clear all status */
	memset(pDesc, 0, TXDESC_SIZE);

	SET_TX_DESC_OFFSET_8723D(pDesc, 0x28); /* Offset = 32 */

	SET_TX_DESC_PKT_SIZE_8723D(pDesc, BufferLen); /* Buffer size + command header */
	SET_TX_DESC_QUEUE_SEL_8723D(pDesc, QSLT_MGNT); /* Fixed queue of Mgnt queue */

	/* Set NAVUSEHDR to prevent Ps-poll AId filed to be changed to error vlaue by Hw. */
	if (IsPsPoll) {
		SET_TX_DESC_NAV_USE_HDR_8723D(pDesc, 1);
	} else {
		SET_TX_DESC_HWSEQ_EN_8723D(pDesc, 1); /* Hw set sequence number */
		SET_TX_DESC_HWSEQ_SEL_8723D(pDesc, 0);
	}

	if (IsBTQosNull)
		SET_TX_DESC_BT_INT_8723D(pDesc, 1);

	SET_TX_DESC_USE_RATE_8723D(pDesc, 1); /* use data rate which is set by Sw */

	SET_TX_DESC_TX_RATE_8723D(pDesc, DESC8723D_RATE1M);

	/* */
	/* Encrypt the data frame if under security mode excepct null data. Suggested by CCW. */
	/* */
	if (bDataFrame) {
		u32 EncAlg;

		EncAlg = adapt->securitypriv.dot11PrivacyAlgrthm;
		switch (EncAlg) {
		case _NO_PRIVACY_:
			SET_TX_DESC_SEC_TYPE_8723D(pDesc, 0x0);
			break;
		case _WEP40_:
		case _WEP104_:
		case _TKIP_:
			SET_TX_DESC_SEC_TYPE_8723D(pDesc, 0x1);
			break;
		case _SMS4_:
			SET_TX_DESC_SEC_TYPE_8723D(pDesc, 0x2);
			break;
		case _AES_:
			SET_TX_DESC_SEC_TYPE_8723D(pDesc, 0x3);
			break;
		default:
			SET_TX_DESC_SEC_TYPE_8723D(pDesc, 0x0);
			break;
		}
	}

	/*
	 * USB interface drop packet if the checksum of descriptor isn't correct.
	 * Using this checksum can let hardware recovery from packet bulk out error (e.g. Cancel URC, Bulk out error.).
	 */
	rtl8723d_cal_txdesc_chksum((struct tx_desc *)pDesc);
}

void rtl8723d_InitAntenna_Selection(struct adapter * adapt)
{
	rtw_write8(adapt, REG_LEDCFG2, 0x82);
}

void rtl8723d_CheckAntenna_Selection(struct adapter * adapt)
{
}

void rtl8723d_DeinitAntenna_Selection(struct adapter * adapt)
{
}

void init_hal_spec_8723d(struct adapter *adapter)
{
	struct hal_spec_t *hal_spec = GET_HAL_SPEC(adapter);

	hal_spec->ic_name = "rtl8723d";
	hal_spec->macid_num = 16;
	hal_spec->sec_cam_ent_num = 32;
	hal_spec->sec_cap = SEC_CAP_CHK_BMC;
	hal_spec->rfpath_num_2g = 2;
	hal_spec->rfpath_num_5g = 0;
	hal_spec->max_tx_cnt = 1;
	hal_spec->tx_nss_num = 1;
	hal_spec->rx_nss_num = 1;
	hal_spec->band_cap = BAND_CAP_2G;
	hal_spec->bw_cap = BW_CAP_20M | BW_CAP_40M;
	hal_spec->port_num = 3;
	hal_spec->proto_cap = PROTO_CAP_11B | PROTO_CAP_11G | PROTO_CAP_11N;

	hal_spec->wl_func = 0
			    | WL_FUNC_P2P
			    | WL_FUNC_MIRACAST
			    | WL_FUNC_TDLS
			    ;

	rtw_macid_ctl_init_sleep_reg(adapter_to_macidctl(adapter)
		, REG_MACID_SLEEP, 0, 0, 0);
}

void rtl8723d_init_default_value(struct adapter * adapt)
{
	struct hal_com_data * pHalData;
	u8 i;

	pHalData = GET_HAL_DATA(adapt);

	adapt->registrypriv.wireless_mode = WIRELESS_11BG_24N;

	/* init default value */
	pHalData->fw_ractrl = false;
	if (!adapter_to_pwrctl(adapt)->bkeepfwalive)
		pHalData->LastHMEBoxNum = 0;

	/* init phydm default value */
	pHalData->bIQKInitialized = false;
	pHalData->odmpriv.rf_calibrate_info.tm_trigger = 0;/* for IQK */
	pHalData->odmpriv.rf_calibrate_info.thermal_value_hp_index = 0;
	for (i = 0; i < HP_THERMAL_NUM; i++)
		pHalData->odmpriv.rf_calibrate_info.thermal_value_hp[i] = 0;

	/* init Efuse variables */
	pHalData->EfuseUsedBytes = 0;
	pHalData->EfuseUsedPercentage = 0;
#ifdef HAL_EFUSE_MEMORY
	pHalData->EfuseHal.fakeEfuseBank = 0;
	pHalData->EfuseHal.fakeEfuseUsedBytes = 0;
	memset(pHalData->EfuseHal.fakeEfuseContent, 0xFF, EFUSE_MAX_HW_SIZE);
	memset(pHalData->EfuseHal.fakeEfuseInitMap, 0xFF, EFUSE_MAX_MAP_LEN);
	memset(pHalData->EfuseHal.fakeEfuseModifiedMap, 0xFF, EFUSE_MAX_MAP_LEN);
	pHalData->EfuseHal.BTEfuseUsedBytes = 0;
	pHalData->EfuseHal.BTEfuseUsedPercentage = 0;
	memset(pHalData->EfuseHal.BTEfuseContent, 0xFF, EFUSE_MAX_BT_BANK * EFUSE_MAX_HW_SIZE);
	memset(pHalData->EfuseHal.BTEfuseInitMap, 0xFF, EFUSE_BT_MAX_MAP_LEN);
	memset(pHalData->EfuseHal.BTEfuseModifiedMap, 0xFF, EFUSE_BT_MAX_MAP_LEN);
	pHalData->EfuseHal.fakeBTEfuseUsedBytes = 0;
	memset(pHalData->EfuseHal.fakeBTEfuseContent, 0xFF, EFUSE_MAX_BT_BANK * EFUSE_MAX_HW_SIZE);
	memset(pHalData->EfuseHal.fakeBTEfuseInitMap, 0xFF, EFUSE_BT_MAX_MAP_LEN);
	memset(pHalData->EfuseHal.fakeBTEfuseModifiedMap, 0xFF, EFUSE_BT_MAX_MAP_LEN);
#endif
	pHalData->need_restore = false;
}

u8 GetEEPROMSize8723D(struct adapter * adapt)
{
	u8 size = 0;
	u32	cr;

	cr = rtw_read16(adapt, REG_9346CR);
	/* 6: EEPROM used is 93C46, 4: boot from E-Fuse. */
	size = (cr & BOOT_FROM_EEPROM) ? 6 : 4;

	RTW_INFO("EEPROM type is %s\n", size == 4 ? "E-FUSE" : "93C46");

	return size;
}

/* -------------------------------------------------------------------------
 *
 * LLT R/W/Init function
 *
 * ------------------------------------------------------------------------- */
int rtl8723d_InitLLTTable(struct adapter * adapt)
{
	unsigned long start;
	u32 passing_time;
	u32 val32;
	int ret;


	ret = _FAIL;

	val32 = rtw_read32(adapt, REG_AUTO_LLT);
	val32 |= BIT_AUTO_INIT_LLT;
	rtw_write32(adapt, REG_AUTO_LLT, val32);

	start = rtw_get_current_time();

	do {
		val32 = rtw_read32(adapt, REG_AUTO_LLT);
		if (!(val32 & BIT_AUTO_INIT_LLT)) {
			ret = _SUCCESS;
			break;
		}

		passing_time = rtw_get_passing_time_ms(start);
		if (passing_time > 1000) {
			RTW_INFO("%s: FAIL!! REG_AUTO_LLT(0x%X)=%08x\n",
				 __func__, REG_AUTO_LLT, val32);
			break;
		}

		rtw_usleep_os(2);
	} while (1);

	return ret;
}

static void _DisableGPIO(struct adapter *	adapt)
{
	/***************************************
	j. GPIO_PIN_CTRL 0x44[31:0]=0x000
	k.Value = GPIO_PIN_CTRL[7:0]
	l. GPIO_PIN_CTRL 0x44[31:0] = 0x00FF0000 | (value <<8);
	m. GPIO_MUXCFG 0x42 [15:0] = 0x0780
	n. LEDCFG 0x4C[15:0] = 0x8080
	***************************************/
	u8	value8;
	u16	value16;
	u32	value32;
	u32	u4bTmp;


	/* 1. Disable GPIO[7:0] */
	rtw_write16(adapt, REG_GPIO_PIN_CTRL + 2, 0x0000);
	value32 = rtw_read32(adapt, REG_GPIO_PIN_CTRL) & 0xFFFF00FF;
	u4bTmp = value32 & 0x000000FF;
	value32 |= ((u4bTmp << 8) | 0x00FF0000);
	rtw_write32(adapt, REG_GPIO_PIN_CTRL, value32);


	/* 2. Disable GPIO[10:8] */
	rtw_write8(adapt, REG_MAC_PINMUX_CFG, 0x00);
	value16 = rtw_read16(adapt, REG_GPIO_IO_SEL) & 0xFF0F;
	value8 = (u8)(value16 & 0x000F);
	value16 |= ((value8 << 4) | 0x0780);
	rtw_write16(adapt, REG_GPIO_IO_SEL, value16);


	/* 3. Disable LED0 & 1 */
	rtw_write16(adapt, REG_LEDCFG0, 0x8080);

} /* end of _DisableGPIO() */

static void _DisableRFAFEAndResetBB8723D(struct adapter * adapt)
{
	/**************************************
	a.	TXPAUSE 0x522[7:0] = 0xFF
	b.	RF path 0 offset 0x00 = 0x00
	c.	APSD_CTRL 0x600[7:0] = 0x40
	d.	SYS_FUNC_EN 0x02[7:0] = 0x16
	e.	SYS_FUNC_EN 0x02[7:0] = 0x14
	***************************************/
	enum rf_path eRFPath = RF_PATH_A, value8 = 0;

	rtw_write8(adapt, REG_TXPAUSE, 0xFF);

	phy_set_rf_reg(adapt, eRFPath, 0x0, bMaskByte0, 0x0);

	value8 |= APSDOFF;
	rtw_write8(adapt, REG_APSD_CTRL, value8);/* 0x40 */

	/* Set BB reset at first */
	value8 = 0;
	value8 |= (FEN_USBD | FEN_USBA | FEN_BB_GLB_RSTn);
	rtw_write8(adapt, REG_SYS_FUNC_EN, value8); /* 0x16 */

	/* Set global reset. */
	value8 &= ~FEN_BB_GLB_RSTn;
	rtw_write8(adapt, REG_SYS_FUNC_EN, value8); /* 0x14 */

	/* 2010/08/12 MH We need to set BB/GLBAL reset to save power for SS mode. */

}

static void _DisableRFAFEAndResetBB(struct adapter * adapt)
{
	_DisableRFAFEAndResetBB8723D(adapt);
}

static void _ResetDigitalProcedure1_8723D(struct adapter * adapt, bool bWithoutHWSM)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);

	if (IS_FW_81xxC(adapt) && (pHalData->firmware_version <= 0x20)) {
		/*****************************
		f.	MCUFWDL 0x80[7:0]=0
		g.	SYS_FUNC_EN 0x02[10]= 0
		h.	SYS_FUNC_EN 0x02[15-12]= 5
		i.     SYS_FUNC_EN 0x02[10]= 1
		******************************/
		u16 valu16 = 0;

		rtw_write8(adapt, REG_MCUFWDL, 0);

		valu16 = rtw_read16(adapt, REG_SYS_FUNC_EN);
		rtw_write16(adapt, REG_SYS_FUNC_EN, (valu16 & (~FEN_CPUEN)));/* reset MCU ,8051 */

		valu16 = rtw_read16(adapt, REG_SYS_FUNC_EN) & 0x0FFF;
		rtw_write16(adapt, REG_SYS_FUNC_EN, (valu16 | (FEN_HWPDN | FEN_ELDR))); /* reset MAC */

		valu16 = rtw_read16(adapt, REG_SYS_FUNC_EN);
		rtw_write16(adapt, REG_SYS_FUNC_EN, (valu16 | FEN_CPUEN));/* enable MCU ,8051 */
	} else {
		u8 retry_cnts = 0;

		/* 2010/08/12 MH For USB SS, we can not stop 8051 when we are trying to */
		/* enter IPS/HW&SW radio off. For S3/S4/S5/Disable, we can stop 8051 because */
		/* we will init FW when power on again. */
		/* if(!pDevice->RegUsbSS) */
		{	/* If we want to SS mode, we can not reset 8051. */
			if (rtw_read8(adapt, REG_MCUFWDL) & BIT(1)) {
				/* IF fw in RAM code, do reset */


				if (pHalData->bFWReady) {
					/* 2010/08/25 MH According to RD alfred's suggestion, we need to disable other */
					/* HRCV INT to influence 8051 reset. */
					rtw_write8(adapt, REG_FWIMR, 0x20);
					/* 2011/02/15 MH According to Alex's suggestion, close mask to prevent incorrect FW write operation. */
					rtw_write8(adapt, REG_FTIMR, 0x00);
					rtw_write8(adapt, REG_FSIMR, 0x00);

					rtw_write8(adapt, REG_HMETFR + 3, 0x20); /* 8051 reset by self */

					while ((retry_cnts++ < 100) && (FEN_CPUEN & rtw_read16(adapt, REG_SYS_FUNC_EN))) {
						rtw_udelay_os(50);/* us */
						/* 2010/08/25 For test only We keep on reset 5051 to prevent fail. */
						/* rtw_write8(adapt, REG_HMETFR+3, 0x20);//8051 reset by self */
					}
					/*					RT_ASSERT((retry_cnts < 100), ("8051 reset failed!\n")); */

					if (retry_cnts >= 100) {
						/* if 8051 reset fail we trigger GPIO 0 for LA */
						/* rtw_write32(	adapt, */
						/*						REG_GPIO_PIN_CTRL, */
						/*						0x00010100); */
						/* 2010/08/31 MH According to Filen's info, if 8051 reset fail, reset MAC directly. */
						rtw_write8(adapt, REG_SYS_FUNC_EN + 1, 0x50);	/* Reset MAC and Enable 8051 */
						rtw_mdelay_os(10);
					}

				}
			}

			rtw_write8(adapt, REG_SYS_FUNC_EN + 1, 0x54);	/* Reset MAC and Enable 8051 */
			rtw_write8(adapt, REG_MCUFWDL, 0);
		}
	}

	/* if(pDevice->RegUsbSS) */
	/* bWithoutHWSM = true;	// Sugest by Filen and Issau. */

	if (bWithoutHWSM) {
		/* struct hal_com_data		*pHalData	= GET_HAL_DATA(adapt); */
		/*****************************
			Without HW auto state machine
		g.	SYS_CLKR 0x08[15:0] = 0x30A3
		h.	AFE_PLL_CTRL 0x28[7:0] = 0x80
		i.	AFE_XTAL_CTRL 0x24[15:0] = 0x880F
		j.	SYS_ISO_CTRL 0x00[7:0] = 0xF9
		******************************/
		/* rtw_write16(adapt, REG_SYS_CLKR, 0x30A3); */
		/* if(!pDevice->RegUsbSS) */
		/* 2011/01/26 MH SD4 Scott suggest to fix UNC-B cut bug. */
		rtw_write16(adapt, REG_SYS_CLKR, 0x70A3);  /* modify to 0x70A3 by Scott. */
		rtw_write8(adapt, REG_AFE_PLL_CTRL, 0x80);
		rtw_write16(adapt, REG_AFE_XTAL_CTRL, 0x880F);
		/* if(!pDevice->RegUsbSS) */
		rtw_write8(adapt, REG_SYS_ISO_CTRL, 0xF9);
	} else {
		/* Disable all RF/BB power */
		rtw_write8(adapt, REG_RF_CTRL, 0x00);
	}

}

static void _ResetDigitalProcedure1(struct adapter * adapt, bool bWithoutHWSM)
{
	_ResetDigitalProcedure1_8723D(adapt, bWithoutHWSM);
}

static void _ResetDigitalProcedure2(struct adapter * adapt)
{
	/* struct hal_com_data		*pHalData	= GET_HAL_DATA(adapt);
	*****************************
	k.	SYS_FUNC_EN 0x03[7:0] = 0x44
	l.	SYS_CLKR 0x08[15:0] = 0x3083
	m.	SYS_ISO_CTRL 0x01[7:0] = 0x83
	******************************/
	/* rtw_write8(adapt, REG_SYS_FUNC_EN+1, 0x44); //marked by Scott. */
	/* 2011/01/26 MH SD4 Scott suggest to fix UNC-B cut bug. */
	rtw_write16(adapt, REG_SYS_CLKR, 0x70a3); /* modify to 0x70a3 by Scott. */
	rtw_write8(adapt, REG_SYS_ISO_CTRL + 1, 0x82); /* modify to 0x82 by Scott. */
}

static void _DisableAnalog(struct adapter * adapt, bool bWithoutHWSM)
{
	u16 value16 = 0;
	u8 value8 = 0;


	if (bWithoutHWSM) {
		/*****************************
		n.	LDOA15_CTRL 0x20[7:0] = 0x04
		o.	LDOV12D_CTRL 0x21[7:0] = 0x54
		r.	When driver call disable, the ASIC will turn off remaining clock automatically
		******************************/

		rtw_write8(adapt, REG_LDOA15_CTRL, 0x04);
		/* rtw_write8(adapt, REG_LDOV12D_CTRL, 0x54); */

		value8 = rtw_read8(adapt, REG_LDOV12D_CTRL);
		value8 &= (~LDV12_EN);
		rtw_write8(adapt, REG_LDOV12D_CTRL, value8);
	}

	/*****************************
	h.	SPS0_CTRL 0x11[7:0] = 0x23
	i.	APS_FSMCO 0x04[15:0] = 0x4802
	******************************/
	value8 = 0x23;

	rtw_write8(adapt, REG_SPS0_CTRL, value8);

	if (bWithoutHWSM) {
		/* value16 |= (APDM_HOST | AFSM_HSUS |PFM_ALDN); */
		/* 2010/08/31 According to Filen description, we need to use HW to shut down 8051 automatically. */
		/* Because suspend operation need the asistance of 8051 to wait for 3ms. */
		value16 |= (APDM_HOST | AFSM_HSUS | PFM_ALDN);
	} else
		value16 |= (APDM_HOST | AFSM_HSUS | PFM_ALDN);

	rtw_write16(adapt, REG_APS_FSMCO, value16);/* 0x4802 */

	rtw_write8(adapt, REG_RSV_CTRL, 0x0e);
}

/* HW Auto state machine */
int CardDisableHWSM(struct adapter * adapt, u8 resetMCU)
{
	int rtStatus = _SUCCESS;


	if (RTW_CANNOT_RUN(adapt))
		return rtStatus;

	/* ==== RF Off Sequence ==== */
	_DisableRFAFEAndResetBB(adapt);

	/* ==== Reset digital sequence   ====== */
	_ResetDigitalProcedure1(adapt, false);

	/* ==== Pull GPIO PIN to balance level and LED control ====== */
	_DisableGPIO(adapt);

	/* ==== Disable analog sequence === */
	_DisableAnalog(adapt, false);


	return rtStatus;
}

/* without HW Auto state machine */
int CardDisableWithoutHWSM(struct adapter * adapt)
{
	int rtStatus = _SUCCESS;


	if (RTW_CANNOT_RUN(adapt))
		return rtStatus;


	/* ==== RF Off Sequence ==== */
	_DisableRFAFEAndResetBB(adapt);

	/* ==== Reset digital sequence   ====== */
	_ResetDigitalProcedure1(adapt, true);

	/* ==== Pull GPIO PIN to balance level and LED control ====== */
	_DisableGPIO(adapt);

	/* ==== Reset digital sequence   ====== */
	_ResetDigitalProcedure2(adapt);

	/* ==== Disable analog sequence === */
	_DisableAnalog(adapt, true);

	return rtStatus;
}

void
Hal_InitPGData(
	struct adapter *	adapt,
	u8			*PROMContent)
{

	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);
	u32			i;

	if (false == pHalData->bautoload_fail_flag) {
		/* autoload OK.
		*		if (IS_BOOT_FROM_EEPROM(adapt)) */
		if (pHalData->EepromOrEfuse) {
			/* Read all Content from EEPROM or EFUSE. */
			for (i = 0; i < HWSET_MAX_SIZE_8723D; i += 2) {
				/*				value16 = EF2Byte(ReadEEprom(pAdapter, (u16) (i>>1)));
				 *				*((u16*)(&PROMContent[i])) = value16; */
			}
		} else {
			/* Read EFUSE real map to shadow. */
			EFUSE_ShadowMapUpdate(adapt, EFUSE_WIFI, false);
			memcpy((void *)PROMContent, (void *)pHalData->efuse_eeprom_data, HWSET_MAX_SIZE_8723D);
		}
	} else {
		/* autoload fail */
		/*		pHalData->AutoloadFailFlag = true; */
		/* update to default value 0xFF */
		if (false == pHalData->EepromOrEfuse)
			EFUSE_ShadowMapUpdate(adapt, EFUSE_WIFI, false);
		memcpy((void *)PROMContent, (void *)pHalData->efuse_eeprom_data, HWSET_MAX_SIZE_8723D);
	}

#ifdef CONFIG_EFUSE_CONFIG_FILE
	if (!check_phy_efuse_tx_power_info_valid(adapt)) {
		if (Hal_readPGDataFromConfigFile(adapt) != _SUCCESS)
			RTW_ERR("invalid phy efuse and read from file fail, will use driver default!!\n");
	}
#endif
}

void
Hal_EfuseParseIDCode(
	struct adapter *	adapt,
	u8			*hwinfo
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);
	u16			EEPROMId;


	/* Checl 0x8129 again for making sure autoload status!! */
	EEPROMId = le16_to_cpu(*((__le16 *)hwinfo));
	if (EEPROMId != RTL_EEPROM_ID) {
		RTW_INFO("EEPROM ID(%#x) is invalid!!\n", EEPROMId);
		pHalData->bautoload_fail_flag = true;
	} else
		pHalData->bautoload_fail_flag = false;

}

void
Hal_EfuseParseTxPowerInfo_8723D(
	struct adapter *		adapt,
	u8			*PROMContent,
	bool			AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);
	struct TxPowerInfo24G	pwrInfo24G;

	hal_load_txpwr_info(adapt, &pwrInfo24G, NULL, PROMContent);

	/* 2010/10/19 MH Add Regulator recognize for CU. */
	if (!AutoLoadFail) {
		pHalData->EEPROMRegulatory = (PROMContent[EEPROM_RF_BOARD_OPTION_8723D] & 0x7);	/* bit0~2 */
		if (PROMContent[EEPROM_RF_BOARD_OPTION_8723D] == 0xFF)
			pHalData->EEPROMRegulatory = (EEPROM_DEFAULT_BOARD_OPTION & 0x7);	/* bit0~2 */
	} else
		pHalData->EEPROMRegulatory = 0;
}

void
Hal_EfuseParseBoardType_8723D(
	struct adapter *	Adapter,
	u8			*PROMContent,
	bool		AutoloadFail
)
{


	struct hal_com_data	*pHalData = GET_HAL_DATA(Adapter);

	if (!AutoloadFail) {
		pHalData->InterfaceSel = (PROMContent[EEPROM_RF_BOARD_OPTION_8723D] & 0xE0) >> 5;
		if (PROMContent[EEPROM_RF_BOARD_OPTION_8723D] == 0xFF)
			pHalData->InterfaceSel = (EEPROM_DEFAULT_BOARD_OPTION & 0xE0) >> 5;
	} else
		pHalData->InterfaceSel = 0;

}

void
Hal_EfuseParseBTCoexistInfo_8723D(
	struct adapter * adapt,
	u8 *hwinfo,
	bool AutoLoadFail
)
{
	struct hal_com_data * pHalData = GET_HAL_DATA(adapt);
	u8 tempval;
	u32 tmpu4;

	if (!AutoLoadFail) {
		tmpu4 = rtw_read32(adapt, REG_MULTI_FUNC_CTRL);
		if (tmpu4 & BT_FUNC_EN)
			pHalData->EEPROMBluetoothCoexist = true;
		else
			pHalData->EEPROMBluetoothCoexist = false;

		pHalData->EEPROMBluetoothType = BT_RTL8723D;

		tempval = hwinfo[EEPROM_RF_BT_SETTING_8723D];
		if (tempval != 0xFF) {
			/* 0:Ant_x2, 1:Ant_x1 */
			pHalData->EEPROMBluetoothAntNum = tempval & BIT(0);
			/*
			 * EFUSE_0xC3[6] == 0, Wi-Fi at BTGS1(Main, Ant2) - RF_PATH_A (default)
			 * EFUSE_0xC3[6] == 1, Wi-Fi at BTGS0( Aux, Ant1) - RF_PATH_B
			 */
			pHalData->ant_path = (tempval & BIT(6)) ? RF_PATH_B : RF_PATH_A;
		} else {
			pHalData->EEPROMBluetoothAntNum = Ant_x1;
			pHalData->ant_path = RF_PATH_A;
		}
	} else {
		if (adapt->registrypriv.mp_mode == 1)
			pHalData->EEPROMBluetoothCoexist = true;
		else
			pHalData->EEPROMBluetoothCoexist = false;

		pHalData->EEPROMBluetoothType = BT_RTL8723D;
		pHalData->EEPROMBluetoothAntNum = Ant_x1;
		pHalData->ant_path = RF_PATH_A;
	}

	if (adapt->registrypriv.ant_num > 0) {
		RTW_INFO("%s: Apply driver defined antenna number(%d) to replace origin(%d)\n"
			 , __func__
			 , adapt->registrypriv.ant_num
			 , pHalData->EEPROMBluetoothAntNum == Ant_x2 ? 2 : 1);

		switch (adapt->registrypriv.ant_num) {
		case 1:
			pHalData->EEPROMBluetoothAntNum = Ant_x1;
			break;
		case 2:
			pHalData->EEPROMBluetoothAntNum = Ant_x2;
			break;
		default:
			RTW_INFO("%s: Discard invalid driver defined antenna number(%d)!\n"
				 , __func__, adapt->registrypriv.ant_num);
			break;
		}
	}

	RTW_INFO("%s: %s BT-coex, ant_num=%d\n"
		 , __func__
		, pHalData->EEPROMBluetoothCoexist ? "Enable" : "Disable"
		 , pHalData->EEPROMBluetoothAntNum == Ant_x2 ? 2 : 1);
}

void
Hal_EfuseParseEEPROMVer_8723D(
	struct adapter *		adapt,
	u8			*hwinfo,
	bool			AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);

	if (!AutoLoadFail)
		pHalData->EEPROMVersion = hwinfo[EEPROM_VERSION_8723D];
	else
		pHalData->EEPROMVersion = 1;
}


void
Hal_EfuseParsePackageType_8723D(
	struct adapter *		pAdapter,
	u8				*hwinfo,
	bool		AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(pAdapter);
	u8			package;
	u8 efuseContent;

	Efuse_PowerSwitch(pAdapter, false, true);
	efuse_OneByteRead(pAdapter, 0x1FB, &efuseContent, false);
	RTW_INFO("%s phy efuse read 0x1FB =%x\n", __func__, efuseContent);
	Efuse_PowerSwitch(pAdapter, false, false);

	package = efuseContent & 0x7;
	switch (package) {
	case 0x4:
		pHalData->PackageType = PACKAGE_TFBGA79;
		break;
	case 0x5:
		pHalData->PackageType = PACKAGE_TFBGA90;
		break;
	case 0x6:
		pHalData->PackageType = PACKAGE_QFN68;
		break;
	case 0x7:
		pHalData->PackageType = PACKAGE_TFBGA80;
		break;

	default:
		pHalData->PackageType = PACKAGE_DEFAULT;
		break;
	}

	RTW_INFO("PackageType = 0x%X\n", pHalData->PackageType);
}

void
Hal_EfuseParseVoltage_8723D(
	struct adapter *		pAdapter,
	u8			*hwinfo,
	bool	AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(pAdapter);

	/* memcpy(pHalData->adjuseVoltageVal, &hwinfo[EEPROM_Voltage_ADDR_8723D], 1); */
	RTW_INFO("%s hwinfo[EEPROM_Voltage_ADDR_8723D] =%02x\n", __func__, hwinfo[EEPROM_Voltage_ADDR_8723D]);
	pHalData->adjuseVoltageVal = (hwinfo[EEPROM_Voltage_ADDR_8723D] & 0xf0) >> 4;
	RTW_INFO("%s pHalData->adjuseVoltageVal =%x\n", __func__, pHalData->adjuseVoltageVal);
}

void
Hal_EfuseParseChnlPlan_8723D(
	struct adapter *		adapt,
	u8			*hwinfo,
	bool			AutoLoadFail
)
{
	hal_com_config_channel_plan(
		adapt
		, hwinfo ? &hwinfo[EEPROM_COUNTRY_CODE_8723D] : NULL
		, hwinfo ? hwinfo[EEPROM_ChannelPlan_8723D] : 0xFF
		, adapt->registrypriv.alpha2
		, adapt->registrypriv.channel_plan
		, RTW_CHPLAN_WORLD_NULL
		, AutoLoadFail
	);
}

void
Hal_EfuseParseCustomerID_8723D(
	struct adapter *		adapt,
	u8			*hwinfo,
	bool			AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(adapt);

	if (!AutoLoadFail)
		pHalData->EEPROMCustomerID = hwinfo[EEPROM_CustomID_8723D];
	else
		pHalData->EEPROMCustomerID = 0;
}

void
Hal_EfuseParseAntennaDiversity_8723D(
	struct adapter *		pAdapter,
	u8				*hwinfo,
	bool			AutoLoadFail
)
{
}

void
Hal_EfuseParseXtal_8723D(
	struct adapter *		pAdapter,
	u8			*hwinfo,
	bool		AutoLoadFail
)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(pAdapter);

	if (!AutoLoadFail) {
		pHalData->crystal_cap = hwinfo[EEPROM_XTAL_8723D];
		if (pHalData->crystal_cap == 0xFF)
			pHalData->crystal_cap = EEPROM_Default_CrystalCap_8723D;	   /* what value should 8812 set? */
	} else
		pHalData->crystal_cap = EEPROM_Default_CrystalCap_8723D;
}


void Hal_EfuseParseThermalMeter_8723D(struct adapter * adapt, u8 *PROMContent,
				      bool AutoLoadFail)
{
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);

	/* */
	/* ThermalMeter from EEPROM */
	/* */
	if (false == AutoLoadFail)
		pHalData->eeprom_thermal_meter = PROMContent[EEPROM_THERMAL_METER_8723D];
	else
		pHalData->eeprom_thermal_meter = EEPROM_Default_ThermalMeter_8723D;

	if ((pHalData->eeprom_thermal_meter == 0xff) || (AutoLoadFail)) {
		pHalData->odmpriv.rf_calibrate_info.is_apk_thermal_meter_ignore = true;
		pHalData->eeprom_thermal_meter = EEPROM_Default_ThermalMeter_8723D;
	}
}

void Hal_ReadRFGainOffset(
		struct adapter *		Adapter,
		u8			*PROMContent,
		bool		AutoloadFail)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(Adapter);
	struct kfree_data_t *kfree_data = &pHalData->kfree_data;
	u8 pg_pwrtrim = 0xFF, pg_therm = 0xFF;

	efuse_OneByteRead(Adapter,
		PPG_BB_GAIN_2G_TX_OFFSET_8723D, &pg_pwrtrim, false);
	efuse_OneByteRead(Adapter,
		PPG_THERMAL_OFFSET_8723D, &pg_therm, false);

	if (pg_pwrtrim != 0xFF) {
		kfree_data->bb_gain[BB_GAIN_2G][PPG_8723D_S1]
			= KFREE_BB_GAIN_2G_TX_OFFSET(pg_pwrtrim & PPG_BB_GAIN_2G_TX_OFFSET_MASK);
		kfree_data->bb_gain[BB_GAIN_2G][PPG_8723D_S0]
			= KFREE_BB_GAIN_2G_TXB_OFFSET(pg_pwrtrim & PPG_BB_GAIN_2G_TXB_OFFSET_MASK);
		kfree_data->flag |= KFREE_FLAG_ON;
	}

	if (pg_therm != 0xFF) {
		kfree_data->thermal
			= KFREE_THERMAL_OFFSET(pg_therm  & PPG_THERMAL_OFFSET_MASK);
		/* kfree_data->flag |= KFREE_FLAG_THERMAL_K_ON; */ /* Default disable thermel kfree by realsil Alan 20160428 */
	}

	if (kfree_data->flag & KFREE_FLAG_THERMAL_K_ON)
		pHalData->eeprom_thermal_meter += kfree_data->thermal;

	RTW_INFO("kfree Pwr Trim flag:%u\n", kfree_data->flag);
	if (kfree_data->flag & KFREE_FLAG_ON) {
		RTW_INFO("bb_gain(S1):%d\n", kfree_data->bb_gain[BB_GAIN_2G][PPG_8723D_S1]);
		RTW_INFO("bb_gain(S0):%d\n", kfree_data->bb_gain[BB_GAIN_2G][PPG_8723D_S0]);
	}
	if (kfree_data->flag & KFREE_FLAG_THERMAL_K_ON)
		RTW_INFO("thermal:%d\n", kfree_data->thermal);
}

u8 BWMapping_8723D(struct adapter * Adapter, struct pkt_attrib *pattrib)
{
	u8	BWSettingOfDesc = 0;
	struct hal_com_data *	pHalData = GET_HAL_DATA(Adapter);

	/* RTW_INFO("BWMapping pHalData->current_channel_bw %d, pattrib->bwmode %d\n",pHalData->current_channel_bw,pattrib->bwmode); */

	if (pHalData->current_channel_bw == CHANNEL_WIDTH_80) {
		if (pattrib->bwmode == CHANNEL_WIDTH_80)
			BWSettingOfDesc = 2;
		else if (pattrib->bwmode == CHANNEL_WIDTH_40)
			BWSettingOfDesc = 1;
		else
			BWSettingOfDesc = 0;
	} else if (pHalData->current_channel_bw == CHANNEL_WIDTH_40) {
		if ((pattrib->bwmode == CHANNEL_WIDTH_40) || (pattrib->bwmode == CHANNEL_WIDTH_80))
			BWSettingOfDesc = 1;
		else
			BWSettingOfDesc = 0;
	} else
		BWSettingOfDesc = 0;

	/* if(pTcb->bBTTxPacket) */
	/*	BWSettingOfDesc = 0; */

	return BWSettingOfDesc;
}

u8 SCMapping_8723D(struct adapter * Adapter, struct pkt_attrib *pattrib)
{
	u8 desc_setting = 0;
	struct hal_com_data *	pHalData = GET_HAL_DATA(Adapter);

	if (pHalData->current_channel_bw == CHANNEL_WIDTH_80) {
		if (pattrib->bwmode == CHANNEL_WIDTH_80)
			desc_setting = VHT_DATA_SC_DONOT_CARE;
		else if (pattrib->bwmode == CHANNEL_WIDTH_40) {
			if (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER)
				desc_setting = VHT_DATA_SC_40_LOWER_OF_80MHZ;
			else if (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER)
				desc_setting = VHT_DATA_SC_40_UPPER_OF_80MHZ;
			else
				RTW_INFO("SCMapping: DONOT CARE Mode Setting\n");
		} else {
			if ((pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER) && (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER))
				desc_setting = VHT_DATA_SC_20_LOWEST_OF_80MHZ;
			else if ((pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER) && (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER))
				desc_setting = VHT_DATA_SC_20_LOWER_OF_80MHZ;
			else if ((pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER) && (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER))
				desc_setting = VHT_DATA_SC_20_UPPER_OF_80MHZ;
			else if ((pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER) && (pHalData->nCur80MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER))
				desc_setting = VHT_DATA_SC_20_UPPERST_OF_80MHZ;
			else
				RTW_INFO("SCMapping: DONOT CARE Mode Setting\n");
		}
	} else if (pHalData->current_channel_bw == CHANNEL_WIDTH_40) {
		if (pattrib->bwmode == CHANNEL_WIDTH_40)
			desc_setting = VHT_DATA_SC_DONOT_CARE;
		else if (pattrib->bwmode == CHANNEL_WIDTH_20) {
			if (pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_UPPER)
				desc_setting = VHT_DATA_SC_20_UPPER_OF_80MHZ;
			else if (pHalData->nCur40MhzPrimeSC == HAL_PRIME_CHNL_OFFSET_LOWER)
				desc_setting = VHT_DATA_SC_20_LOWER_OF_80MHZ;
			else
				desc_setting = VHT_DATA_SC_DONOT_CARE;
		}
	} else {
		desc_setting = VHT_DATA_SC_DONOT_CARE;
	}
	return desc_setting;
}

#if defined(CONFIG_CONCURRENT_MODE)
void fill_txdesc_force_bmc_camid(struct pkt_attrib *pattrib, u8 *ptxdesc)
{
	if ((pattrib->encrypt > 0) && (!pattrib->bswenc)
	    && (pattrib->bmc_camid != INVALID_SEC_MAC_CAM_ID)) {

		SET_TX_DESC_EN_DESC_ID_8723D(ptxdesc, 1);
		SET_TX_DESC_MACID_8723D(ptxdesc, pattrib->bmc_camid);
	}
}
#endif
void fill_txdesc_bmc_tx_rate(struct pkt_attrib *pattrib, u8 *ptxdesc)
{
	SET_TX_DESC_USE_RATE_8723D(ptxdesc, 1);
	SET_TX_DESC_TX_RATE_8723D(ptxdesc, MRateToHwRate(pattrib->rate));
	SET_TX_DESC_DISABLE_FB_8723D(ptxdesc, 1);
}

static u8 fill_txdesc_sectype(struct pkt_attrib *pattrib)
{
	u8 sectype = 0;

	if ((pattrib->encrypt > 0) && !pattrib->bswenc) {
		switch (pattrib->encrypt) {
		/* SEC_TYPE */
		case _WEP40_:
		case _WEP104_:
		case _TKIP_:
		case _TKIP_WTMIC_:
			sectype = 1;
			break;
		case _AES_:
			sectype = 3;
			break;
		case _NO_PRIVACY_:
		default:
			break;
		}
	}
	return sectype;
}

static void fill_txdesc_vcs_8723d(struct adapter * adapt, struct pkt_attrib *pattrib, u8 *ptxdesc)
{
	/* RTW_INFO("cvs_mode=%d\n", pattrib->vcs_mode); */

	if (pattrib->vcs_mode) {
		switch (pattrib->vcs_mode) {
		case RTS_CTS:
			SET_TX_DESC_RTS_ENABLE_8723D(ptxdesc, 1);
			SET_TX_DESC_HW_RTS_ENABLE_8723D(ptxdesc, 1);
			break;

		case CTS_TO_SELF:
			SET_TX_DESC_CTS2SELF_8723D(ptxdesc, 1);
			break;

		case NONE_VCS:
		default:
			break;
		}

		SET_TX_DESC_RTS_RATE_8723D(ptxdesc, 8); /* RTS Rate=24M */
		SET_TX_DESC_RTS_RATE_FB_LIMIT_8723D(ptxdesc, 0xF);

		if (adapt->mlmeextpriv.mlmext_info.preamble_mode == PREAMBLE_SHORT)
			SET_TX_DESC_RTS_SHORT_8723D(ptxdesc, 1);

		/* Set RTS BW */
		if (pattrib->ht_en)
			SET_TX_DESC_RTS_SC_8723D(ptxdesc, SCMapping_8723D(adapt, pattrib));
	}
}

static void fill_txdesc_phy_8723d(struct adapter * adapt, struct pkt_attrib *pattrib, u8 *ptxdesc)
{
	/* RTW_INFO("bwmode=%d, ch_off=%d\n", pattrib->bwmode, pattrib->ch_offset); */

	if (pattrib->ht_en) {
		SET_TX_DESC_DATA_BW_8723D(ptxdesc, BWMapping_8723D(adapt, pattrib));
		SET_TX_DESC_DATA_SC_8723D(ptxdesc, SCMapping_8723D(adapt, pattrib));
	}
}

static void rtl8723d_fill_default_txdesc(
	struct xmit_frame *pxmitframe,
	u8 *pbuf)
{
	struct adapter * adapt;
	struct hal_com_data *pHalData;
	struct mlme_ext_priv *pmlmeext;
	struct mlme_ext_info *pmlmeinfo;
	struct pkt_attrib *pattrib;
	int bmcst;

	memset(pbuf, 0, TXDESC_SIZE);

	adapt = pxmitframe->adapt;
	pHalData = GET_HAL_DATA(adapt);
	pmlmeext = &adapt->mlmeextpriv;
	pmlmeinfo = &(pmlmeext->mlmext_info);

	pattrib = &pxmitframe->attrib;
	bmcst = IS_MCAST(pattrib->ra);

	if (pHalData->rf_type == RF_1T1R)
		SET_TX_DESC_PATH_A_EN_8723D(pbuf, 1);

	if (pxmitframe->frame_tag == DATA_FRAMETAG) {
		u8 drv_userate = 0;

		SET_TX_DESC_MACID_8723D(pbuf, pattrib->mac_id);
		SET_TX_DESC_RATE_ID_8723D(pbuf, pattrib->raid);
		SET_TX_DESC_QUEUE_SEL_8723D(pbuf, pattrib->qsel);
		SET_TX_DESC_SEQ_8723D(pbuf, pattrib->seqnum);

		SET_TX_DESC_SEC_TYPE_8723D(pbuf, fill_txdesc_sectype(pattrib));
#if defined(CONFIG_CONCURRENT_MODE)
		if (bmcst)
			fill_txdesc_force_bmc_camid(pattrib, pbuf);
#endif
		fill_txdesc_vcs_8723d(adapt, pattrib, pbuf);

		if (!rtw_p2p_chk_state(&adapt->wdinfo, P2P_STATE_NONE)) {
			if (pattrib->icmp_pkt == 1 && adapt->registrypriv.wifi_spec == 1)
				drv_userate = 1;
		}

		if ((pattrib->ether_type != 0x888e) &&
		    (pattrib->ether_type != 0x0806) &&
		    (pattrib->ether_type != 0x88B4) &&
		    (pattrib->dhcp_pkt != 1) &&
		    (drv_userate != 1)) {
			/* Non EAP & ARP & DHCP type data packet */

			if (pattrib->ampdu_en) {
				SET_TX_DESC_AGG_ENABLE_8723D(pbuf, 1);
				SET_TX_DESC_MAX_AGG_NUM_8723D(pbuf, 0x1F);
				SET_TX_DESC_AMPDU_DENSITY_8723D(pbuf, pattrib->ampdu_spacing);
			} else
				SET_TX_DESC_BK_8723D(pbuf, 1);

			fill_txdesc_phy_8723d(adapt, pattrib, pbuf);

			SET_TX_DESC_DATA_RATE_FB_LIMIT_8723D(pbuf, 0x1F);

			if (!pHalData->fw_ractrl) {
				SET_TX_DESC_USE_RATE_8723D(pbuf, 1);

				if (pHalData->INIDATA_RATE[pattrib->mac_id] & BIT(7))
					SET_TX_DESC_DATA_SHORT_8723D(pbuf, 1);

				SET_TX_DESC_TX_RATE_8723D(pbuf, pHalData->INIDATA_RATE[pattrib->mac_id] & 0x7F);
			}
			if (bmcst)
				fill_txdesc_bmc_tx_rate(pattrib, pbuf);

			/* modify data rate by iwpriv */
			if (adapt->fix_rate != 0xFF) {
				SET_TX_DESC_USE_RATE_8723D(pbuf, 1);
				if (adapt->fix_rate & BIT(7))
					SET_TX_DESC_DATA_SHORT_8723D(pbuf, 1);
				SET_TX_DESC_TX_RATE_8723D(pbuf, adapt->fix_rate & 0x7F);
				if (!adapt->data_fb)
					SET_TX_DESC_DISABLE_FB_8723D(pbuf, 1);
			}

			if (pattrib->stbc)
				SET_TX_DESC_DATA_STBC_8723D(pbuf, 1);

#ifdef CONFIG_CMCC_TEST
			SET_TX_DESC_DATA_SHORT_8723D(pbuf, 1); /* use cck short premble */
#endif
		} else {
			/* EAP data packet and ARP packet. */
			/* Use the 1M data rate to send the EAP/ARP packet. */
			/* This will maybe make the handshake smooth. */

			SET_TX_DESC_BK_8723D(pbuf, 1);
			SET_TX_DESC_USE_RATE_8723D(pbuf, 1);
			if (pmlmeinfo->preamble_mode == PREAMBLE_SHORT)
				SET_TX_DESC_DATA_SHORT_8723D(pbuf, 1);
			SET_TX_DESC_TX_RATE_8723D(pbuf, MRateToHwRate(pmlmeext->tx_rate));

			RTW_INFO(FUNC_ADPT_FMT ": SP Packet(0x%04X) rate=0x%x\n",
				FUNC_ADPT_ARG(adapt), pattrib->ether_type, MRateToHwRate(pmlmeext->tx_rate));
		}
	} else if (pxmitframe->frame_tag == MGNT_FRAMETAG) {
		SET_TX_DESC_MACID_8723D(pbuf, pattrib->mac_id);
		SET_TX_DESC_QUEUE_SEL_8723D(pbuf, pattrib->qsel);
		SET_TX_DESC_RATE_ID_8723D(pbuf, pattrib->raid);
		SET_TX_DESC_SEQ_8723D(pbuf, pattrib->seqnum);
		SET_TX_DESC_USE_RATE_8723D(pbuf, 1);

		SET_TX_DESC_MBSSID_8723D(pbuf, pattrib->mbssid & 0xF);

		SET_TX_DESC_RETRY_LIMIT_ENABLE_8723D(pbuf, 1);
		if (pattrib->retry_ctrl)
			SET_TX_DESC_DATA_RETRY_LIMIT_8723D(pbuf, 6);
		else
			SET_TX_DESC_DATA_RETRY_LIMIT_8723D(pbuf, 12);

		SET_TX_DESC_TX_RATE_8723D(pbuf, MRateToHwRate(pattrib->rate));

		/* CCX-TXRPT ack for xmit mgmt frames. */
		if (pxmitframe->ack_report) {
			SET_TX_DESC_CCX_8723D(pbuf, 1);
			SET_TX_DESC_SW_DEFINE_8723D(pbuf, (u8)(GET_PRIMARY_ADAPTER(adapt)->xmitpriv.seq_no));
		}
	} else if (pxmitframe->frame_tag == TXAGG_FRAMETAG) {
	} else {
		SET_TX_DESC_MACID_8723D(pbuf, pattrib->mac_id);
		SET_TX_DESC_RATE_ID_8723D(pbuf, pattrib->raid);
		SET_TX_DESC_QUEUE_SEL_8723D(pbuf, pattrib->qsel);
		SET_TX_DESC_SEQ_8723D(pbuf, pattrib->seqnum);
		SET_TX_DESC_USE_RATE_8723D(pbuf, 1);
		SET_TX_DESC_TX_RATE_8723D(pbuf, MRateToHwRate(pmlmeext->tx_rate));
	}

	SET_TX_DESC_PKT_SIZE_8723D(pbuf, pattrib->last_txcmdsz);

	{
		u8 pkt_offset, offset;

		pkt_offset = 0;
		offset = TXDESC_SIZE;
		pkt_offset = pxmitframe->pkt_offset;
		offset += (pxmitframe->pkt_offset >> 3);

		SET_TX_DESC_PKT_OFFSET_8723D(pbuf, pkt_offset);
		SET_TX_DESC_OFFSET_8723D(pbuf, offset);
	}

	if (bmcst)
		SET_TX_DESC_BMC_8723D(pbuf, 1);

	/* 2009.11.05. tynli_test. Suggested by SD4 Filen for FW LPS. */
	/* (1) The sequence number of each non-Qos frame / broadcast / multicast / */
	/* mgnt frame should be controlled by Hw because Fw will also send null data */
	/* which we cannot control when Fw LPS enable. */
	/* --> default enable non-Qos data sequense number. 2010.06.23. by tynli. */
	/* (2) Enable HW SEQ control for beacon packet, because we use Hw beacon. */
	/* (3) Use HW Qos SEQ to control the seq num of Ext port non-Qos packets. */
	/* 2010.06.23. Added by tynli. */
	if (!pattrib->qos_en)
		SET_TX_DESC_HWSEQ_EN_8723D(pbuf, 1);
}

/*
 *	Description:
 *
 *	Parameters:
 *		pxmitframe	xmitframe
 *		pbuf		where to fill tx desc
 */
void rtl8723d_update_txdesc(struct xmit_frame *pxmitframe, u8 *pbuf)
{
	rtl8723d_fill_default_txdesc(pxmitframe, pbuf);

	rtl8723d_cal_txdesc_chksum((struct tx_desc *)pbuf);
}

static void hw_var_set_monitor(struct adapter * Adapter, u8 variable, u8 *val)
{
	u32	rcr_bits;
	u16	value_rxfltmap2;
	struct hal_com_data *pHalData = GET_HAL_DATA(Adapter);

	if (*((u8 *)val) == _HW_STATE_MONITOR_) {


		/* Receive all type */
		rcr_bits = RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_APWRMGT | RCR_ADF | RCR_ACF | RCR_AMF | RCR_APP_PHYST_RXFF;

		/* Append FCS */
		rcr_bits |= RCR_APPFCS;

		rtw_hal_get_hwreg(Adapter, HW_VAR_RCR, (u8 *)&pHalData->rcr_backup);
		rtw_hal_set_hwreg(Adapter, HW_VAR_RCR, (u8 *)&rcr_bits);

		/* Receive all data frames */
		value_rxfltmap2 = 0xFFFF;
		rtw_write16(Adapter, REG_RXFLTMAP2, value_rxfltmap2);
	} else {
		/* do nothing */
	}

}

static void hw_var_set_opmode(struct adapter * adapt, u8 variable, u8 *val)
{
	u8 val8;
	u8 mode = *((u8 *)val);
	static u8 isMonitor = false;

	struct hal_com_data			*pHalData = GET_HAL_DATA(adapt);

	if (isMonitor) {
		/* reset RCR from backup */
		rtw_hal_set_hwreg(adapt, HW_VAR_RCR, (u8 *)&pHalData->rcr_backup);
		rtw_hal_rcr_set_chk_bssid(adapt, MLME_ACTION_NONE);
		isMonitor = false;
	}

	if (mode == _HW_STATE_MONITOR_) {
		isMonitor = true;
		/* set net_type */
		Set_MSR(adapt, _HW_STATE_NOLINK_);

		hw_var_set_monitor(adapt, variable, val);
		return;
	}
	/* set mac addr to mac register */
	rtw_hal_set_hwreg(adapt, HW_VAR_MAC_ADDR,
			  adapter_mac_addr(adapt));

#ifdef CONFIG_CONCURRENT_MODE
	if (adapt->hw_port == HW_PORT1) {
		/* disable Port1 TSF update */
		val8 = rtw_read8(adapt, REG_BCN_CTRL_1);
		val8 |= DIS_TSF_UDT;
		rtw_write8(adapt, REG_BCN_CTRL_1, val8);

		Set_MSR(adapt, mode);

		RTW_INFO("#### %s()-%d hw_port(%d) mode=%d ####\n",
			 __func__, __LINE__, adapt->hw_port, mode);

		if ((mode == _HW_STATE_STATION_) || (mode == _HW_STATE_NOLINK_)) {
			if (!rtw_mi_get_ap_num(adapt) && !rtw_mi_get_mesh_num(adapt))
				StopTxBeacon(adapt);
			/* disable atim wnd */
			rtw_write8(adapt, REG_BCN_CTRL_1, DIS_TSF_UDT | DIS_ATIM | EN_BCN_FUNCTION);
		} else if (mode == _HW_STATE_ADHOC_) {
			ResumeTxBeacon(adapt);
			rtw_write8(adapt, REG_BCN_CTRL_1, DIS_TSF_UDT | EN_BCN_FUNCTION | DIS_BCNQ_SUB);
		} else if (mode == _HW_STATE_AP_) {
			ResumeTxBeacon(adapt);

			rtw_write8(adapt, REG_BCN_CTRL_1, DIS_TSF_UDT | DIS_BCNQ_SUB);

			/* enable to rx data frame*/
			rtw_write16(adapt, REG_RXFLTMAP2, 0xFFFF);
			/* enable to rx ps-poll */
			rtw_write16(adapt, REG_RXFLTMAP1, 0x0400);

			/* Beacon Control related register for first time */
			rtw_write8(adapt, REG_BCNDMATIM, 0x02); /* 2ms */

			/* rtw_write8(adapt, REG_BCN_MAX_ERR, 0xFF); */
			rtw_write8(adapt, REG_ATIMWND_1, 0x0c); /* 13ms for port1 */
			rtw_write16(adapt, REG_BCNTCFG, 0x00);

			rtw_write16(adapt, REG_TSFTR_SYN_OFFSET, 0x7fff);/* +32767 (~32ms) */

			/* reset TSF2 */
			rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(1));

			/* enable BCN1 Function for if2 */
			/* don't enable update TSF1 for if2 (due to TSF update when beacon/probe rsp are received) */
			rtw_write8(adapt, REG_BCN_CTRL_1, (DIS_TSF_UDT | EN_BCN_FUNCTION | EN_TXBCN_RPT | DIS_BCNQ_SUB));

			/* SW_BCN_SEL - Port1 */
			/* rtw_write8(Adapter, REG_DWBCN1_CTRL_8192E+2, rtw_read8(Adapter, REG_DWBCN1_CTRL_8192E+2)|BIT4); */
			rtw_hal_set_hwreg(adapt, HW_VAR_DL_BCN_SEL, NULL);

			/* select BCN on port 1 */
			rtw_write8(adapt, REG_CCK_CHECK_8723D,
				(rtw_read8(adapt, REG_CCK_CHECK_8723D) | BIT_BCN_PORT_SEL));

			/* BCN1 TSF will sync to BCN0 TSF with offset(0x518) if if1_sta linked */
			/* rtw_write8(adapt, REG_BCN_CTRL_1, rtw_read8(adapt, REG_BCN_CTRL_1)|BIT(5)); */
			/* rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(3)); */

			/* dis BCN0 ATIM  WND if if1 is station */
			rtw_write8(adapt, REG_BCN_CTRL, rtw_read8(adapt, REG_BCN_CTRL) | DIS_ATIM);

#ifdef CONFIG_TSF_RESET_OFFLOAD
			/* Reset TSF for STA+AP concurrent mode */
			if (rtw_mi_buddy_check_fwstate(adapt,
				(WIFI_STATION_STATE | WIFI_ASOC_STATE))) {
				if (rtw_hal_reset_tsf(adapt, HW_PORT1) == _FAIL)
					RTW_INFO("ERROR! %s()-%d: Reset port1 TSF fail\n",
						 __func__, __LINE__);
			}
#endif /* CONFIG_TSF_RESET_OFFLOAD */
		}
	} else /* else for port0 */
#endif /* CONFIG_CONCURRENT_MODE */
	{
#ifdef CONFIG_MI_WITH_MBSSID_CAM /*For Port0 - MBSS CAM*/
		hw_var_set_opmode_mbid(adapt, mode);
#else
		/* disable Port0 TSF update */
		val8 = rtw_read8(adapt, REG_BCN_CTRL);
		val8 |= DIS_TSF_UDT;
		rtw_write8(adapt, REG_BCN_CTRL, val8);

		/* set net_type */
		Set_MSR(adapt, mode);
		RTW_INFO("#### %s() -%d hw_port(0) mode = %d ####\n",
			 __func__, __LINE__, mode);

		if ((mode == _HW_STATE_STATION_) || (mode == _HW_STATE_NOLINK_)) {
#ifdef CONFIG_CONCURRENT_MODE
			if (!rtw_mi_get_ap_num(adapt) && !rtw_mi_get_mesh_num(adapt)) {
#else
			{
#endif /* CONFIG_CONCURRENT_MODE */
				StopTxBeacon(adapt);
			}

			/* disable atim wnd */
			rtw_write8(adapt, REG_BCN_CTRL, DIS_TSF_UDT | EN_BCN_FUNCTION | DIS_ATIM);
			/* rtw_write8(adapt,REG_BCN_CTRL, 0x18); */
		} else if (mode == _HW_STATE_ADHOC_) {
			ResumeTxBeacon(adapt);
			rtw_write8(adapt, REG_BCN_CTRL, DIS_TSF_UDT | EN_BCN_FUNCTION | DIS_BCNQ_SUB);
		} else if (mode == _HW_STATE_AP_) {
			ResumeTxBeacon(adapt);

			rtw_write8(adapt, REG_BCN_CTRL, DIS_TSF_UDT | DIS_BCNQ_SUB);

			/* enable to rx data frame */
			rtw_write16(adapt, REG_RXFLTMAP2, 0xFFFF);
			/* enable to rx ps-poll */
			rtw_write16(adapt, REG_RXFLTMAP1, 0x0400);

			/* Beacon Control related register for first time */
			rtw_write8(adapt, REG_BCNDMATIM, 0x02); /* 2ms */

			/* rtw_write8(adapt, REG_BCN_MAX_ERR, 0xFF); */
			rtw_write8(adapt, REG_ATIMWND, 0x0c); /* 13ms */
			rtw_write16(adapt, REG_BCNTCFG, 0x00);

			rtw_write16(adapt, REG_TSFTR_SYN_OFFSET, 0x7fff);/* +32767 (~32ms) */

			/* reset TSF */
			rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(0));

			/* enable BCN0 Function for if1 */
			/* don't enable update TSF0 for if1 (due to TSF update when beacon/probe rsp are received) */
			rtw_write8(adapt, REG_BCN_CTRL, (DIS_TSF_UDT | EN_BCN_FUNCTION | EN_TXBCN_RPT | DIS_BCNQ_SUB));

			/* SW_BCN_SEL - Port0 */
			/* rtw_write8(Adapter, REG_DWBCN1_CTRL_8192E+2, rtw_read8(Adapter, REG_DWBCN1_CTRL_8192E+2) & ~BIT4); */
			rtw_hal_set_hwreg(adapt, HW_VAR_DL_BCN_SEL, NULL);

			/* select BCN on port 0 */
			rtw_write8(adapt, REG_CCK_CHECK_8723D,
				(rtw_read8(adapt, REG_CCK_CHECK_8723D) & ~BIT_BCN_PORT_SEL));

			/* dis BCN1 ATIM  WND if if2 is station */
			val8 = rtw_read8(adapt, REG_BCN_CTRL_1);
			val8 |= DIS_ATIM;
			rtw_write8(adapt, REG_BCN_CTRL_1, val8);
#ifdef CONFIG_TSF_RESET_OFFLOAD
			/* Reset TSF for STA+AP concurrent mode */
			if (rtw_mi_buddy_check_fwstate(adapt,
				(WIFI_STATION_STATE | WIFI_ASOC_STATE))) {
				if (rtw_hal_reset_tsf(adapt, HW_PORT0) == _FAIL)
					RTW_INFO("ERROR! %s()-%d: Reset port0 TSF fail\n",
						 __func__, __LINE__);
			}
#endif /* CONFIG_TSF_RESET_OFFLOAD */
		}
#endif /* !CONFIG_MI_WITH_MBSSID_CAM */
	}
}

static void hw_var_set_bcn_func(struct adapter * adapt, u8 variable, u8 *val)
{
	u32 bcn_ctrl_reg;

#ifdef CONFIG_CONCURRENT_MODE
	if (adapt->hw_port == HW_PORT1)
		bcn_ctrl_reg = REG_BCN_CTRL_1;
	else
#endif
	{
		bcn_ctrl_reg = REG_BCN_CTRL;
	}

	if (*(u8 *)val)
		rtw_write8(adapt, bcn_ctrl_reg, (EN_BCN_FUNCTION | EN_TXBCN_RPT));
	else {
		u8 val8;

		val8 = rtw_read8(adapt, bcn_ctrl_reg);
		val8 &= ~(EN_BCN_FUNCTION | EN_TXBCN_RPT);
		/* Always enable port0 beacon function for PSTDMA */
		if (REG_BCN_CTRL == bcn_ctrl_reg)
			val8 |= EN_BCN_FUNCTION;
		rtw_write8(adapt, bcn_ctrl_reg, val8);
	}
}

static void hw_var_set_mlme_disconnect(struct adapter * adapt, u8 variable, u8 *val)
{
	u8 val8;

	/* reject all data frames */
#ifdef CONFIG_CONCURRENT_MODE
	if (!rtw_mi_check_status(adapt, MI_LINKED))
#endif
		rtw_write16(adapt, REG_RXFLTMAP2, 0);

#ifdef CONFIG_CONCURRENT_MODE
	if (adapt->hw_port == HW_PORT1) {
	/* reset TSF1 */
		rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(1));

	/* disable update TSF1 */
		val8 = rtw_read8(adapt, REG_BCN_CTRL_1);
		val8 |= DIS_TSF_UDT;
		rtw_write8(adapt, REG_BCN_CTRL_1, val8);
	} else
#endif
	{
	/* reset TSF */
		rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(0));

	/* disable update TSF */
		val8 = rtw_read8(adapt, REG_BCN_CTRL);
		val8 |= DIS_TSF_UDT;
		rtw_write8(adapt, REG_BCN_CTRL, val8);
	}
}

static void hw_var_set_mlme_join(struct adapter * adapt, u8 variable, u8 *val)
{
	u8 val8;
	u16 val16;
	u8 RetryLimit;
	u8 type;
	struct hal_com_data * pHalData;
	struct mlme_priv *pmlmepriv;

	RetryLimit = RL_VAL_STA;
	type = *(u8 *)val;
	pHalData = GET_HAL_DATA(adapt);
	pmlmepriv = &adapt->mlmepriv;
#ifdef CONFIG_CONCURRENT_MODE
	if (type == 0) {
		/* prepare to join */
		if (rtw_mi_get_ap_num(adapt) || rtw_mi_get_mesh_num(adapt))
			StopTxBeacon(adapt);

		/* enable to rx data frame.Accept all data frame */
		rtw_write16(adapt, REG_RXFLTMAP2, 0xFFFF);

		if (check_fwstate(pmlmepriv, WIFI_STATION_STATE))
			RetryLimit = (pHalData->CustomerID == RT_CID_CCX) ? RL_VAL_AP : RL_VAL_STA;
		else /* Ad-hoc Mode */
			RetryLimit = RL_VAL_AP;
		} else if (type == 1) {
			/* joinbss_event call back when join res < 0 */
			if (!rtw_mi_check_status(adapt, MI_LINKED))
				rtw_write16(adapt, REG_RXFLTMAP2, 0x00);

			if (rtw_mi_get_ap_num(adapt) || rtw_mi_get_mesh_num(adapt)) {
				ResumeTxBeacon(adapt);
				/* reset TSF 1/2 after ResumeTxBeacon */
				rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(1) | BIT(0));
			}
		} else if (type == 2) {
			/* sta add event call back */
#ifdef CONFIG_MI_WITH_MBSSID_CAM
			/*if (check_fwstate(pmlmepriv, WIFI_STATION_STATE) && (rtw_mi_get_assoced_sta_num(adapt) == 1))
				rtw_write8(adapt, REG_BCN_CTRL, rtw_read8(adapt, REG_BCN_CTRL)&(~DIS_TSF_UDT));*/
#else
			/* enable update TSF */
			if (adapt->hw_port == HW_PORT1) {
				val8 = rtw_read8(adapt, REG_BCN_CTRL_1);
				val8 &= ~DIS_TSF_UDT;
				rtw_write8(adapt, REG_BCN_CTRL_1, val8);
			} else {
				val8 = rtw_read8(adapt, REG_BCN_CTRL);
				val8 &= ~DIS_TSF_UDT;
				rtw_write8(adapt, REG_BCN_CTRL, val8);
			}
#endif
			if (check_fwstate(pmlmepriv,
				WIFI_ADHOC_STATE | WIFI_ADHOC_MASTER_STATE)) {
				rtw_write8(adapt, 0x542 , 0x02);
				RetryLimit = RL_VAL_AP;
			}

			if (rtw_mi_get_ap_num(adapt) || rtw_mi_get_mesh_num(adapt)) {
				ResumeTxBeacon(adapt);

			/* reset TSF 1/2 after ResumeTxBeacon */
				rtw_write8(adapt, REG_DUAL_TSF_RST, BIT(1) | BIT(0));
			}
		}

		val16 = (RetryLimit << RETRY_LIMIT_SHORT_SHIFT) | (RetryLimit << RETRY_LIMIT_LONG_SHIFT);
		rtw_write16(adapt, REG_RL, val16);
#else /* !CONFIG_CONCURRENT_MODE */
		if (type == 0) { /* prepare to join */
			/* enable to rx data frame.Accept all data frame */
			rtw_write16(adapt, REG_RXFLTMAP2, 0xFFFF);

			if (check_fwstate(pmlmepriv, WIFI_STATION_STATE))
				RetryLimit = (pHalData->CustomerID == RT_CID_CCX) ? RL_VAL_AP : RL_VAL_STA;
			else /* Ad-hoc Mode */
				RetryLimit = RL_VAL_AP;
		} else if (type == 1) /* joinbss_event call back when join res < 0 */
			rtw_write16(adapt, REG_RXFLTMAP2, 0x00);
		else if (type == 2) { /* sta add event call back */
			/* enable update TSF */
			val8 = rtw_read8(adapt, REG_BCN_CTRL);
			val8 &= ~DIS_TSF_UDT;
			rtw_write8(adapt, REG_BCN_CTRL, val8);

			if (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE | WIFI_ADHOC_MASTER_STATE))
				RetryLimit = RL_VAL_AP;
		}

	val16 = (RetryLimit << RETRY_LIMIT_SHORT_SHIFT) | (RetryLimit << RETRY_LIMIT_LONG_SHIFT);
	rtw_write16(adapt, REG_RL, val16);
#endif /* !CONFIG_CONCURRENT_MODE */
}

void CCX_FwC2HTxRpt_8723d(struct adapter * adapt, u8 *pdata, u8 len)
{
	u8 seq_no;

#define	GET_8723D_C2H_TX_RPT_LIFE_TIME_OVER(_Header)	LE_BITS_TO_1BYTE((_Header + 0), 6, 1)
#define	GET_8723D_C2H_TX_RPT_RETRY_OVER(_Header)	LE_BITS_TO_1BYTE((_Header + 0), 7, 1)

	/* RTW_INFO("%s, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", __func__,  */
	/**pdata, *(pdata+1), *(pdata+2), *(pdata+3), *(pdata+4), *(pdata+5), *(pdata+6), *(pdata+7)); */

	seq_no = *(pdata + 6);

	if (GET_8723D_C2H_TX_RPT_RETRY_OVER(pdata) | GET_8723D_C2H_TX_RPT_LIFE_TIME_OVER(pdata))
		rtw_ack_tx_done(&adapt->xmitpriv, RTW_SCTX_DONE_CCX_PKT_FAIL);
	else
		rtw_ack_tx_done(&adapt->xmitpriv, RTW_SCTX_DONE_SUCCESS);
}

static int c2h_handler_8723d(struct adapter *adapter, u8 id, u8 seq, u8 plen, u8 *payload)
{
	int ret = _SUCCESS;

	switch (id) {
	case C2H_CCX_TX_RPT:
		CCX_FwC2HTxRpt_8723d(adapter, payload, plen);
		break;
	default:
		ret = _FAIL;
		break;
	}
	return ret;
}

u8 SetHwReg8723D(struct adapter * adapt, u8 variable, u8 *val)
{
	struct hal_com_data *	pHalData = GET_HAL_DATA(adapt);
	u8 ret = _SUCCESS;
	u8 val8;
	u32 val32;

	switch (variable) {
	case HW_VAR_SET_OPMODE:
		hw_var_set_opmode(adapt, variable, val);
		break;

	case HW_VAR_BASIC_RATE:
	{
		struct mlme_ext_info *mlmext_info = &adapt->mlmeextpriv.mlmext_info;
		u16 input_b = 0, masked = 0, ioted = 0, BrateCfg = 0;
		u16 rrsr_2g_force_mask = RRSR_CCK_RATES;
		u16 rrsr_2g_allow_mask = (RRSR_24M | RRSR_12M | RRSR_6M | RRSR_CCK_RATES);

		HalSetBrateCfg(adapt, val, &BrateCfg);
		input_b = BrateCfg;

		/* apply force and allow mask */
		BrateCfg |= rrsr_2g_force_mask;
		BrateCfg &= rrsr_2g_allow_mask;
		masked = BrateCfg;

#ifdef CONFIG_CMCC_TEST
		BrateCfg |= (RRSR_11M | RRSR_5_5M | RRSR_1M); /* use 11M to send ACK */
		BrateCfg |= (RRSR_24M | RRSR_18M | RRSR_12M); /* CMCC_OFDM_ACK 12/18/24M */
#endif

		/* IOT consideration */
		if (mlmext_info->assoc_AP_vendor == HT_IOT_PEER_CISCO) {
			/* if peer is cisco and didn't use ofdm rate, we enable 6M ack */
			if ((BrateCfg & (RRSR_24M | RRSR_12M | RRSR_6M)) == 0)
				BrateCfg |= RRSR_6M;
		}
		ioted = BrateCfg;

		pHalData->BasicRateSet = BrateCfg;

		RTW_INFO("HW_VAR_BASIC_RATE: %#x->%#x->%#x\n", input_b, masked, ioted);

		/* Set RRSR rate table. */
		rtw_write16(adapt, REG_RRSR, BrateCfg);
		rtw_write8(adapt, REG_RRSR + 2, rtw_read8(adapt, REG_RRSR + 2) & 0xf0);
	}
		break;

	case HW_VAR_TXPAUSE:
		rtw_write8(adapt, REG_TXPAUSE, *val);
		break;

	case HW_VAR_BCN_FUNC:
		hw_var_set_bcn_func(adapt, variable, val);
		break;

	case HW_VAR_MLME_DISCONNECT:
		hw_var_set_mlme_disconnect(adapt, variable, val);
		break;

	case HW_VAR_MLME_JOIN:
		hw_var_set_mlme_join(adapt, variable, val);

		switch (*val) {
		case 0:
			/* Notify coex. mechanism before join */
			rtw_btcoex_ConnectNotify(adapt, true);
			break;
		case 1:
		case 2:
			/* Notify coex. mechanism after join, whether successful or failed */
			rtw_btcoex_ConnectNotify(adapt, false);
			break;
		}
		break;
	case HW_VAR_BEACON_INTERVAL:
		{
			u16 bcn_interval = *((u16 *)val);

			rtw_write16(adapt, REG_BCN_INTERVAL, bcn_interval);
		}
		break;

	case HW_VAR_SLOT_TIME:
		rtw_write8(adapt, REG_SLOT, *val);
		break;

	case HW_VAR_RESP_SIFS:
		/* SIFS_Timer = 0x0a0a0808; */
		/* RESP_SIFS for CCK */
		rtw_write8(adapt, REG_RESP_SIFS_CCK, val[0]); /* SIFS_T2T_CCK (0x08) */
		rtw_write8(adapt, REG_RESP_SIFS_CCK + 1, val[1]); /* SIFS_R2T_CCK(0x08) */
		/* RESP_SIFS for OFDM */
		rtw_write8(adapt, REG_RESP_SIFS_OFDM, val[2]); /* SIFS_T2T_OFDM (0x0a) */
		rtw_write8(adapt, REG_RESP_SIFS_OFDM + 1, val[3]); /* SIFS_R2T_OFDM(0x0a) */
		break;

	case HW_VAR_ACK_PREAMBLE: {
		u8 regTmp;
		u8 bShortPreamble = *val;

		/* Joseph marked out for Netgear 3500 TKIP channel 7 issue.(Temporarily) */
		/* regTmp = (pHalData->nCur40MhzPrimeSC)<<5; */
		regTmp = 0;
		if (bShortPreamble)
			regTmp |= 0x80;
		rtw_write8(adapt, REG_RRSR + 2, regTmp);
	}
		break;

	case HW_VAR_CAM_EMPTY_ENTRY: {
		u8	ucIndex = *val;
		u8	i;
		u32	ulCommand = 0;
		u32	ulContent = 0;
		u32	ulEncAlgo = CAM_AES;

		for (i = 0; i < CAM_CONTENT_COUNT; i++) {
		/* filled id in CAM config 2 byte */
			if (i == 0) {
				ulContent |= (ucIndex & 0x03) | ((u16)(ulEncAlgo) << 2);
				/* ulContent |= CAM_VALID; */
			} else
				ulContent = 0;

			/* polling bit, and No Write enable, and address */
			ulCommand = CAM_CONTENT_COUNT * ucIndex + i;
			ulCommand = ulCommand | CAM_POLLINIG | CAM_WRITE;
			/* write content 0 is equall to mark invalid */
			rtw_write32(adapt, WCAMI, ulContent);  /* delay_ms(40); */
			rtw_write32(adapt, RWCAM, ulCommand);  /* delay_ms(40); */
		}
	}
		break;

	case HW_VAR_CAM_INVALID_ALL:
		rtw_write32(adapt, RWCAM, BIT(31) | BIT(30));
		break;

	case HW_VAR_AC_PARAM_VO:
		rtw_write32(adapt, REG_EDCA_VO_PARAM, *((u32 *)val));
		break;

	case HW_VAR_AC_PARAM_VI:
		rtw_write32(adapt, REG_EDCA_VI_PARAM, *((u32 *)val));
		break;

	case HW_VAR_AC_PARAM_BE:
		pHalData->ac_param_be = ((u32 *)(val))[0];
		rtw_write32(adapt, REG_EDCA_BE_PARAM, *((u32 *)val));
		break;

	case HW_VAR_AC_PARAM_BK:
		rtw_write32(adapt, REG_EDCA_BK_PARAM, *((u32 *)val));
		break;

	case HW_VAR_ACM_CTRL: {
		u8 ctrl = *((u8 *)val);
		u8 hwctrl = 0;

		if (ctrl != 0) {
			hwctrl |= AcmHw_HwEn;

		if (ctrl & BIT(3)) /* BE */
			hwctrl |= AcmHw_BeqEn;

		if (ctrl & BIT(2)) /* VI */
			hwctrl |= AcmHw_ViqEn;

		if (ctrl & BIT(1)) /* VO */
			hwctrl |= AcmHw_VoqEn;
		}

		RTW_INFO("[HW_VAR_ACM_CTRL] Write 0x%02X\n", hwctrl);
		rtw_write8(adapt, REG_ACMHWCTRL, hwctrl);
	}
		break;

	case HW_VAR_AMPDU_FACTOR: {
		u32	AMPDULen = (*((u8 *)val));

		if (AMPDULen < HT_AGG_SIZE_32K)
			AMPDULen = (0x2000 << (*((u8 *)val))) - 1;
		else
			AMPDULen = 0x7fff;

		rtw_write32(adapt, REG_AMPDU_MAX_LENGTH_8723D, AMPDULen);
	}
		break;

	case HW_VAR_H2C_FW_PWRMODE: {
		u8 psmode = *val;

		/* if (psmode != PS_MODE_ACTIVE)	{ */
			/*rtl8723d_set_lowpwr_lps_cmd(adapt, true); */
		/* } else { */
			/*	rtl8723d_set_lowpwr_lps_cmd(adapt, false); */
		/* } */
		rtl8723d_set_FwPwrMode_cmd(adapt, psmode);
	}
		break;
	case HW_VAR_H2C_PS_TUNE_PARAM:
		rtl8723d_set_FwPsTuneParam_cmd(adapt);
		break;

	case HW_VAR_H2C_FW_JOINBSSRPT:
		rtl8723d_set_FwJoinBssRpt_cmd(adapt, *val);
		break;
	case HW_VAR_H2C_FW_P2P_PS_OFFLOAD:
		rtl8723d_set_p2p_ps_offload_cmd(adapt, *val);
		break;
	case HW_VAR_EFUSE_USAGE:
		pHalData->EfuseUsedPercentage = *val;
		break;

	case HW_VAR_EFUSE_BYTES:
		pHalData->EfuseUsedBytes = *((u16 *)val);
		break;

	case HW_VAR_EFUSE_BT_USAGE:
#ifdef HAL_EFUSE_MEMORY
		pHalData->EfuseHal.BTEfuseUsedPercentage = *val;
#endif
		break;

	case HW_VAR_EFUSE_BT_BYTES:
#ifdef HAL_EFUSE_MEMORY
		pHalData->EfuseHal.BTEfuseUsedBytes = *((u16 *)val);
#else
		BTEfuseUsedBytes = *((u16 *)val);
#endif
		break;

	case HW_VAR_FIFO_CLEARN_UP: {
#define RW_RELEASE_EN		BIT(18)
#define RXDMA_IDLE			BIT(17)

		struct pwrctrl_priv *pwrpriv = adapter_to_pwrctl(adapt);
		u8 trycnt = 100;

		/* pause tx */
		rtw_write8(adapt, REG_TXPAUSE, 0xff);

		/* keep sn */
		adapt->xmitpriv.nqos_ssn = rtw_read16(adapt, REG_NQOS_SEQ);

		if (!pwrpriv->bkeepfwalive) {
			/* RX DMA stop */
			val32 = rtw_read32(adapt, REG_RXPKT_NUM);
			val32 |= RW_RELEASE_EN;
			rtw_write32(adapt, REG_RXPKT_NUM, val32);
			do {
				val32 = rtw_read32(adapt, REG_RXPKT_NUM);
				val32 &= RXDMA_IDLE;
				if (val32)
					break;

				RTW_INFO("%s: [HW_VAR_FIFO_CLEARN_UP] val=%x times:%d\n", __func__, val32, trycnt);
			} while (--trycnt);
			if (trycnt == 0)
				RTW_INFO("[HW_VAR_FIFO_CLEARN_UP] Stop RX DMA failed......\n");

			/* RQPN Load 0 */
			rtw_write16(adapt, REG_RQPN_NPQ, 0);
			rtw_write32(adapt, REG_RQPN, 0x80000000);
			rtw_mdelay_os(2);
		}
	}
	break;

	case HW_VAR_RESTORE_HW_SEQ:
		/* restore Sequence No. */
		rtw_write8(adapt, 0x4dc, adapt->xmitpriv.nqos_ssn);
		break;

#ifdef CONFIG_CONCURRENT_MODE
	case HW_VAR_CHECK_TXBUF: {
		u32 i;
		u8 RetryLimit = 0x01;
		u32 reg_200, reg_204;
		u16 val16;

		val16 = RetryLimit << RETRY_LIMIT_SHORT_SHIFT | RetryLimit << RETRY_LIMIT_LONG_SHIFT;
		rtw_write16(adapt, REG_RL, val16);

		for (i = 0; i < 200; i++) { /* polling 200x10=2000 msec */
			reg_200 = rtw_read32(adapt, 0x200);
			reg_204 = rtw_read32(adapt, 0x204);
			if (reg_200 != reg_204) {
			/* RTW_INFO("packet in tx packet buffer - 0x204=%x, 0x200=%x (%d)\n", rtw_read32(adapt, 0x204), rtw_read32(adapt, 0x200), i); */
				rtw_msleep_os(10);
			} else {
				RTW_INFO("[HW_VAR_CHECK_TXBUF] no packet in tx packet buffer (%d)\n", i);
				break;
			}
		}

		if (reg_200 != reg_204)
			RTW_INFO("packets in tx buffer - 0x204=%x, 0x200=%x\n", reg_204, reg_200);

		RetryLimit = RL_VAL_STA;
		val16 = RetryLimit << RETRY_LIMIT_SHORT_SHIFT | RetryLimit << RETRY_LIMIT_LONG_SHIFT;
		rtw_write16(adapt, REG_RL, val16);
	}
		break;
#endif /* CONFIG_CONCURRENT_MODE */

	case HW_VAR_NAV_UPPER: {
		u32 usNavUpper = *((u32 *)val);

		if (usNavUpper > HAL_NAV_UPPER_UNIT_8723D * 0xFF) {
		break;
	}

		/* The value of ((usNavUpper + HAL_NAV_UPPER_UNIT_8723D - 1) / HAL_NAV_UPPER_UNIT_8723D) */
		/* is getting the upper integer. */
		usNavUpper = (usNavUpper + HAL_NAV_UPPER_UNIT_8723D - 1) / HAL_NAV_UPPER_UNIT_8723D;
		rtw_write8(adapt, REG_NAV_UPPER, (u8)usNavUpper);
	}
		break;

	case HW_VAR_BCN_VALID:
#ifdef CONFIG_CONCURRENT_MODE
		if (adapt->hw_port == HW_PORT1) {
			val8 = rtw_read8(adapt,  REG_DWBCN1_CTRL_8723D + 2);
			val8 |= BIT(0);
			rtw_write8(adapt, REG_DWBCN1_CTRL_8723D + 2, val8);
		} else
#endif /* CONFIG_CONCURRENT_MODE */
		{
		/* BCN_VALID, BIT16 of REG_TDECTRL = BIT0 of REG_TDECTRL+2, write 1 to clear, Clear by sw */
			val8 = rtw_read8(adapt, REG_TDECTRL + 2);
			val8 |= BIT(0);
			rtw_write8(adapt, REG_TDECTRL + 2, val8);
		}
		break;
	case HW_VAR_DL_BCN_SEL:
#ifdef CONFIG_CONCURRENT_MODE
		if (adapt->hw_port == HW_PORT1) {
			/* SW_BCN_SEL - Port1 */
			val8 = rtw_read8(adapt, REG_DWBCN1_CTRL_8723D + 2);
			val8 |= BIT(4);
			rtw_write8(adapt, REG_DWBCN1_CTRL_8723D + 2, val8);
		} else
#endif /* CONFIG_CONCURRENT_MODE */
		{
			/* SW_BCN_SEL - Port0 */
			val8 = rtw_read8(adapt, REG_DWBCN1_CTRL_8723D + 2);
			val8 &= ~BIT(4);
			rtw_write8(adapt, REG_DWBCN1_CTRL_8723D + 2, val8);
		}
		break;
	case HW_VAR_DO_IQK:
		if (*val)
			pHalData->bNeedIQK = true;
		else
			pHalData->bNeedIQK = false;
		break;

	case HW_VAR_DL_RSVD_PAGE:
		if (check_fwstate(&adapt->mlmepriv, WIFI_AP_STATE))
			rtl8723d_download_BTCoex_AP_mode_rsvd_page(adapt);
		else
			rtl8723d_download_rsvd_page(adapt, RT_MEDIA_CONNECT);
		break;
	default:
		ret = SetHwReg(adapt, variable, val);
		break;
	}

	return ret;
}

struct qinfo_8723d {
	u32 head:8;
	u32 pkt_num:7;
	u32 tail:8;
	u32 ac:2;
	u32 macid:7;
};

struct bcn_qinfo_8723d {
	u16 head:8;
	u16 pkt_num:8;
};

static void dump_qinfo_8723d(void *sel, struct qinfo_8723d *info, const char *tag)
{
	/* if (info->pkt_num) */
	RTW_PRINT_SEL(sel, "%shead:0x%02x, tail:0x%02x, pkt_num:%u, macid:%u, ac:%u\n"
		, tag ? tag : "", info->head, info->tail,
		info->pkt_num, info->macid, info->ac);
}

static void dump_bcn_qinfo_8723d(void *sel, struct bcn_qinfo_8723d *info, const char *tag)
{
	/* if (info->pkt_num) */
	RTW_PRINT_SEL(sel, "%shead:0x%02x, pkt_num:%u\n"
		      , tag ? tag : "", info->head, info->pkt_num);
}

static void dump_mac_qinfo_8723d(void *sel, struct adapter *adapter)
{
	u32 q0_info;
	u32 q1_info;
	u32 q2_info;
	u32 q3_info;
	u32 q4_info;
	u32 q5_info;
	u32 q6_info;
	u32 q7_info;
	u32 mg_q_info;
	u32 hi_q_info;
	u16 bcn_q_info;

	q0_info = rtw_read32(adapter, REG_Q0_INFO);
	q1_info = rtw_read32(adapter, REG_Q1_INFO);
	q2_info = rtw_read32(adapter, REG_Q2_INFO);
	q3_info = rtw_read32(adapter, REG_Q3_INFO);
	q4_info = rtw_read32(adapter, REG_Q4_INFO);
	q5_info = rtw_read32(adapter, REG_Q5_INFO);
	q6_info = rtw_read32(adapter, REG_Q6_INFO);
	q7_info = rtw_read32(adapter, REG_Q7_INFO);
	mg_q_info = rtw_read32(adapter, REG_MGQ_INFO);
	hi_q_info = rtw_read32(adapter, REG_HGQ_INFO);
	bcn_q_info = rtw_read16(adapter, REG_BCNQ_INFO);

	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q0_info, "Q0 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q1_info, "Q1 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q2_info, "Q2 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q3_info, "Q3 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q4_info, "Q4 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q5_info, "Q5 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q6_info, "Q6 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&q7_info, "Q7 ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&mg_q_info, "MG ");
	dump_qinfo_8723d(sel, (struct qinfo_8723d *)&hi_q_info, "HI ");
	dump_bcn_qinfo_8723d(sel, (struct bcn_qinfo_8723d *)&bcn_q_info, "BCN ");
}

static void dump_mac_txfifo_8723d(void *sel, struct adapter *adapter)
{
	u32 rqpn, rqpn_npq;
	u32 hpq, lpq, npq, epq, pubq;

	rqpn = rtw_read32(adapter, REG_FIFOPAGE);
	rqpn_npq = rtw_read32(adapter, REG_RQPN_NPQ);

	hpq = (rqpn & 0xFF);
	lpq = ((rqpn & 0xFF00)>>8);
	pubq = ((rqpn & 0xFF0000)>>16);
	npq = ((rqpn_npq & 0xFF00)>>8);
	epq = ((rqpn_npq & 0xFF000000)>>24);

	RTW_PRINT_SEL(sel, "Tx: available page num: ");
	if ((hpq == 0xEA) && (hpq == lpq) && (hpq == pubq))
		RTW_PRINT_SEL(sel, "N/A (reg val = 0xea)\n");
	else
		RTW_PRINT_SEL(sel, "HPQ: %d, LPQ: %d, NPQ: %d, EPQ: %d, PUBQ: %d\n"
			, hpq, lpq, npq, epq, pubq);
}

void GetHwReg8723D(struct adapter * adapt, u8 variable, u8 *val)
{
	struct hal_com_data * pHalData = GET_HAL_DATA(adapt);
	u8 val8;
	u16 val16;

	switch (variable) {
	case HW_VAR_TXPAUSE:
		*val = rtw_read8(adapt, REG_TXPAUSE);
		break;

	case HW_VAR_BCN_VALID:
#ifdef CONFIG_CONCURRENT_MODE
		if (adapt->hw_port == HW_PORT1) {
			val8 = rtw_read8(adapt, REG_DWBCN1_CTRL_8723D + 2);
			*val = (BIT(0) & val8) ? true : false;
		} else
#endif
		{
			/* BCN_VALID, BIT16 of REG_TDECTRL = BIT0 of REG_TDECTRL+2 */
			val8 = rtw_read8(adapt, REG_TDECTRL + 2);
			*val = (BIT(0) & val8) ? true : false;
		}
		break;

	case HW_VAR_EFUSE_USAGE:
		*val = pHalData->EfuseUsedPercentage;
		break;

	case HW_VAR_EFUSE_BYTES:
		*((u16 *)val) = pHalData->EfuseUsedBytes;
		break;

	case HW_VAR_EFUSE_BT_USAGE:
#ifdef HAL_EFUSE_MEMORY
		*val = pHalData->EfuseHal.BTEfuseUsedPercentage;
#endif
		break;

	case HW_VAR_EFUSE_BT_BYTES:
#ifdef HAL_EFUSE_MEMORY
		*((u16 *)val) = pHalData->EfuseHal.BTEfuseUsedBytes;
#else
		*((u16 *)val) = BTEfuseUsedBytes;
#endif
		break;

	case HW_VAR_CHK_HI_QUEUE_EMPTY:
		val16 = rtw_read16(adapt, REG_TXPKT_EMPTY);
		*val = (val16 & BIT(10)) ? true : false;
		break;
	case HW_VAR_CHK_MGQ_CPU_EMPTY:
		val16 = rtw_read16(adapt, REG_TXPKT_EMPTY);
		*val = (val16 & BIT(8)) ? true : false;
		break;
	case HW_VAR_DUMP_MAC_QUEUE_INFO:
		dump_mac_qinfo_8723d(val, adapt);
		break;
	case HW_VAR_DUMP_MAC_TXFIFO:
		dump_mac_txfifo_8723d(val, adapt);
		break;
	default:
		GetHwReg(adapt, variable, val);
		break;
	}
}

/*
 *	Description:
 *		Change default setting of specified variable.
 */
u8 SetHalDefVar8723D(struct adapter * adapt, enum hal_def_variable variable, void *pval)
{
	u8 bResult;

	bResult = _SUCCESS;

	switch (variable) {
	default:
		bResult = SetHalDefVar(adapt, variable, pval);
		break;
	}

	return bResult;
}

static void hal_ra_info_dump(struct adapter *adapt , void *sel)
{
	int i;
	u8 mac_id;
	u32 cmd;
	u32 ra_info1, ra_info2, bw_set;
	u32 rate_mask1, rate_mask2;
	u8 curr_tx_rate, curr_tx_sgi, hight_rate, lowest_rate;
	struct dvobj_priv *dvobj = adapter_to_dvobj(adapt);
	struct macid_ctl_t *macid_ctl = dvobj_to_macidctl(dvobj);

	for (i = 0; i < macid_ctl->num; i++) {

		if (rtw_macid_is_used(macid_ctl, i) && !rtw_macid_is_bmc(macid_ctl, i)) {

			mac_id = (u8) i;
			_RTW_PRINT_SEL(sel , "============ RA status check  Mac_id:%d ===================\n", mac_id);

			cmd = 0x40000100 | mac_id;
			rtw_write32(adapt, REG_HMEBOX_DBG_2_8723D, cmd);
			rtw_msleep_os(10);
			ra_info1 = rtw_read32(adapt, 0x2F0);
			curr_tx_rate = ra_info1 & 0x7F;
			curr_tx_sgi = (ra_info1 >> 7) & 0x01;

			_RTW_PRINT_SEL(sel , "[ ra_info1:0x%08x ] =>cur_tx_rate= %s,cur_sgi:%d\n", ra_info1, HDATA_RATE(curr_tx_rate), curr_tx_sgi);
			_RTW_PRINT_SEL(sel , "[ ra_info1:0x%08x ] => PWRSTS = 0x%02x\n", ra_info1, (ra_info1 >> 8)  & 0x07);

			cmd = 0x40000400 | mac_id;
			rtw_write32(adapt, REG_HMEBOX_DBG_2_8723D, cmd);
			rtw_msleep_os(10);
			ra_info1 = rtw_read32(adapt, 0x2F0);
			ra_info2 = rtw_read32(adapt, 0x2F4);
			rate_mask1 = rtw_read32(adapt, 0x2F8);
			rate_mask2 = rtw_read32(adapt, 0x2FC);
			hight_rate = ra_info2 & 0xFF;
			lowest_rate = (ra_info2 >> 8)  & 0xFF;
			bw_set = (ra_info1 >> 8)  & 0xFF;

			_RTW_PRINT_SEL(sel , "[ ra_info1:0x%08x ] => VHT_EN=0x%02x, ", ra_info1, (ra_info1 >> 24) & 0xFF);


			switch (bw_set) {

			case CHANNEL_WIDTH_20:
				_RTW_PRINT_SEL(sel , "BW_setting=20M\n");
				break;

			case CHANNEL_WIDTH_40:
				_RTW_PRINT_SEL(sel , "BW_setting=40M\n");
				break;

			case CHANNEL_WIDTH_80:
				_RTW_PRINT_SEL(sel , "BW_setting=80M\n");
				break;

			case CHANNEL_WIDTH_160:
				_RTW_PRINT_SEL(sel , "BW_setting=160M\n");
				break;

			default:
				_RTW_PRINT_SEL(sel , "BW_setting=0x%02x\n", bw_set);
				break;

			}

			_RTW_PRINT_SEL(sel , "[ ra_info1:0x%08x ] =>RSSI=%d, DISRA=0x%02x\n",
				       ra_info1,
				       ra_info1 & 0xFF,
				       (ra_info1 >> 16) & 0xFF);

			_RTW_PRINT_SEL(sel , "[ ra_info2:0x%08x ] =>hight_rate=%s, lowest_rate=%s, SGI=0x%02x, RateID=%d\n",
				       ra_info2,
				       HDATA_RATE(hight_rate),
				       HDATA_RATE(lowest_rate),
				       (ra_info2 >> 16) & 0xFF,
				       (ra_info2 >> 24) & 0xFF);

			_RTW_PRINT_SEL(sel , "rate_mask2=0x%08x, rate_mask1=0x%08x\n", rate_mask2, rate_mask1);

		}
	}
}

/*
 *	Description:
 *		Query setting of specified variable.
 */
u8 GetHalDefVar8723D(struct adapter * adapt, enum hal_def_variable variable, void *pval)
{
	struct hal_com_data * pHalData;
	u8 bResult;

	pHalData = GET_HAL_DATA(adapt);
	bResult = _SUCCESS;

	switch (variable) {
	case HAL_DEF_MAX_RECVBUF_SZ:
		*((u32 *)pval) = MAX_RECVBUF_SZ;
		break;
	case HAL_DEF_RX_PACKET_OFFSET:
#ifdef CONFIG_TRX_BD_ARCH
		*((u32 *)pval) = RX_WIFI_INFO_SIZE + DRVINFO_SZ * 8;
#else
		*((u32 *)pval) = RXDESC_SIZE + DRVINFO_SZ * 8;
#endif
		break;

	case HW_VAR_MAX_RX_AMPDU_FACTOR:
		/* Stanley@BB.SD3 suggests 16K can get stable performance */
		/* The experiment was done on SDIO interface */
		/* coding by Lucas@20130730 */
		*(enum ht_cap_ampdu_factor *)pval = MAX_AMPDU_FACTOR_16K;
		break;
	case HW_VAR_BEST_AMPDU_DENSITY:
		*((u32 *)pval) = AMPDU_DENSITY_VALUE_7;
		break;
	case HAL_DEF_TX_LDPC:
	case HAL_DEF_RX_LDPC:
		*((u8 *)pval) = false;
		break;
	case HAL_DEF_TX_STBC:
		*((u8 *)pval) = 0;
		break;
	case HAL_DEF_RX_STBC:
		*((u8 *)pval) = 1;
		break;
	case HAL_DEF_EXPLICIT_BEAMFORMER:
	case HAL_DEF_EXPLICIT_BEAMFORMEE:
		*((u8 *)pval) = false;
		break;

	case HW_DEF_RA_INFO_DUMP:
		hal_ra_info_dump(adapt, pval);
		break;

	case HAL_DEF_TX_PAGE_BOUNDARY:
		if (!adapt->registrypriv.wifi_spec)
			*(u8 *)pval = TX_PAGE_BOUNDARY_8723D;
		else
			*(u8 *)pval = WMM_NORMAL_TX_PAGE_BOUNDARY_8723D;
		break;
	case HAL_DEF_TX_PAGE_SIZE:
		*((u32 *)pval) = PAGE_SIZE_128;
		break;
	case HAL_DEF_RX_DMA_SZ_WOW:
		*(u32 *)pval = RX_DMA_SIZE_8723D - RESV_FMWF;
		break;
	case HAL_DEF_RX_DMA_SZ:
		*(u32 *)pval = RX_DMA_BOUNDARY_8723D + 1;
		break;
	case HAL_DEF_RX_PAGE_SIZE:
		*((u32 *)pval) = 8;
		break;
	default:
		bResult = GetHalDefVar(adapt, variable, pval);
		break;
	}

	return bResult;
}

void rtl8723d_start_thread(struct adapter *adapt)
{
}

void rtl8723d_stop_thread(struct adapter *adapt)
{
}

#if defined(CONFIG_CHECK_BT_HANG)
void rtl8723ds_init_checkbthang_workqueue(struct adapter *adapter)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 37))
	adapter->priv_checkbt_wq = alloc_workqueue("sdio_wq", 0, 0);
#else
	adapter->priv_checkbt_wq = create_workqueue("sdio_wq");
#endif
	INIT_DELAYED_WORK(&adapter->checkbt_work, (void *)check_bt_status_work);
}

void rtl8723ds_free_checkbthang_workqueue(struct adapter *adapter)
{
	if (adapter->priv_checkbt_wq) {
		cancel_delayed_work_sync(&adapter->checkbt_work);
		flush_workqueue(adapter->priv_checkbt_wq);
		destroy_workqueue(adapter->priv_checkbt_wq);
		adapter->priv_checkbt_wq = NULL;
	}
}

void rtl8723ds_cancle_checkbthang_workqueue(struct adapter *adapter)
{
	if (adapter->priv_checkbt_wq)
		cancel_delayed_work_sync(&adapter->checkbt_work);
}

void rtl8723ds_hal_check_bt_hang(struct adapter *adapter)
{
	if (adapter->priv_checkbt_wq)
		queue_delayed_work(adapter->priv_checkbt_wq, &(adapter->checkbt_work), 0);
}
#endif

void rtl8723d_set_hal_ops(struct hal_ops *pHalFunc)
{
	pHalFunc->dm_init = &rtl8723d_init_dm_priv;
	pHalFunc->dm_deinit = &rtl8723d_deinit_dm_priv;
	pHalFunc->read_chip_version = read_chip_version_8723d;
	pHalFunc->set_chnl_bw_handler = &PHY_SetSwChnlBWMode8723D;
	pHalFunc->set_tx_power_level_handler = &PHY_SetTxPowerLevel8723D;
	pHalFunc->get_tx_power_level_handler = &PHY_GetTxPowerLevel8723D;
	pHalFunc->get_tx_power_index_handler = &PHY_GetTxPowerIndex_8723D;
	pHalFunc->hal_dm_watchdog = &rtl8723d_HalDmWatchDog;

	pHalFunc->SetBeaconRelatedRegistersHandler = &rtl8723d_SetBeaconRelatedRegisters;
	pHalFunc->run_thread = &rtl8723d_start_thread;
	pHalFunc->cancel_thread = &rtl8723d_stop_thread;
	pHalFunc->read_bbreg = &PHY_QueryBBReg_8723D;
	pHalFunc->write_bbreg = &PHY_SetBBReg_8723D;
	pHalFunc->read_rfreg = &PHY_QueryRFReg_8723D;
	pHalFunc->write_rfreg = &PHY_SetRFReg_8723D;

	/* Efuse related function */
	pHalFunc->BTEfusePowerSwitch = &Hal_BT_EfusePowerSwitch;
	pHalFunc->EfusePowerSwitch = &Hal_EfusePowerSwitch;
	pHalFunc->ReadEFuse = &Hal_ReadEFuse;
	pHalFunc->EFUSEGetEfuseDefinition = &Hal_GetEfuseDefinition;
	pHalFunc->EfuseGetCurrentSize = &Hal_EfuseGetCurrentSize;
	pHalFunc->Efuse_PgPacketRead = &Hal_EfusePgPacketRead;
	pHalFunc->Efuse_PgPacketWrite = &Hal_EfusePgPacketWrite;
	pHalFunc->Efuse_WordEnableDataWrite = &Hal_EfuseWordEnableDataWrite;
	pHalFunc->Efuse_PgPacketWrite_BT = &Hal_EfusePgPacketWrite_BT;

	pHalFunc->GetHalODMVarHandler = GetHalODMVar;
	pHalFunc->SetHalODMVarHandler = SetHalODMVar;

	pHalFunc->hal_notch_filter = &hal_notch_filter_8723d;
	pHalFunc->c2h_handler = c2h_handler_8723d;
	pHalFunc->fill_h2c_cmd = &FillH2CCmd8723D;
	pHalFunc->fill_fake_txdesc = &rtl8723d_fill_fake_txdesc;
	pHalFunc->fw_dl = &rtl8723d_FirmwareDownload;
	pHalFunc->hal_get_tx_buff_rsvd_page_num = &GetTxBufferRsvdPageNum8723D;
}

