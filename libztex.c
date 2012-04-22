/**
 *   libztex.c - Ztex 1.15x fpga board support library
 *
 *   Copyright (c) 2012 nelisky.btc@gmail.com
 *
 *   This work is based upon the Java SDK provided by ztex which is
 *   Copyright (C) 2009-2011 ZTEX GmbH.
 *   http://www.ztex.de
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2 as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see http://www.gnu.org/licenses/.
**/

#include <stdio.h>
#include <unistd.h>
#include "miner.h"
#include "libztex.h"

#define BUFSIZE 256

//* Capability index for EEPROM support.
#define CAPABILITY_EEPROM 0,0
//* Capability index for FPGA configuration support. 
#define CAPABILITY_FPGA 0,1
//* Capability index for FLASH memory support.
#define CAPABILITY_FLASH 0,2
//* Capability index for DEBUG helper support.
#define CAPABILITY_DEBUG 0,3
//* Capability index for AVR XMEGA support.
#define CAPABILITY_XMEGA 0,4
//* Capability index for AVR XMEGA support.
#define CAPABILITY_HS_FPGA 0,5
//* Capability index for AVR XMEGA support.
#define CAPABILITY_MAC_EEPROM 0,6



static bool libztex_checkDevice(struct libusb_device *dev)
{
	struct libusb_device_descriptor desc;
	int err;

	err = libusb_get_device_descriptor(dev, &desc);
	if (unlikely(err != 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to open read descriptor with error %d", err);
		return false;
	}
	if (!(desc.idVendor == LIBZTEX_IDVENDOR && desc.idProduct == LIBZTEX_IDPRODUCT)) {
		applog(LOG_DEBUG, "Not a ZTEX device %0.4x:%0.4x", desc.idVendor, desc.idProduct);
		return false;
	}
	return true;
}

static bool libztex_checkCapability(struct libztex_device *ztex, int i, int j)
{
	if (!((i >= 0) && (i <= 5) && (j >= 0) && (j < 8) &&
	     (((ztex->interfaceCapabilities[i] & 255) & (1 << j)) != 0))) {
		applog(LOG_ERR, "%s: capability missing: %d %d", ztex->repr, i, i);
		return false;
	}
	return true;
}

static int libztex_detectBitstreamBitOrder(const unsigned char *buf, int size)
{
	int i;

	for (i = 0; i < size - 4; i++) {
		if (((buf[i] & 255) == 0xaa) && ((buf[i + 1] & 255) == 0x99) && ((buf[i + 2] & 255) == 0x55) && ((buf[i + 3] & 255) == 0x66))
			return 1;
		if (((buf[i] & 255) == 0x55) && ((buf[i + 1] & 255) == 0x99) && ((buf[i + 2] & 255) == 0xaa) && ((buf[i + 3] & 255) == 0x66))
			return 0;
	} 
	applog(LOG_WARNING, "Unable to determine bitstream bit order: no signature found");
	return 0;
}

static void libztex_swapBits(unsigned char *buf, int size)
{
	unsigned char c;
	int i;

	for (i = 0; i < size; i++) {
		c = buf[i];
		buf[i] = ((c & 128) >> 7) |
		         ((c & 64) >> 5) |
		         ((c & 32) >> 3) |
		         ((c & 16) >> 1) |
		         ((c & 8) << 1) |
		         ((c & 4) << 3) |
		         ((c & 2) << 5) |
		         ((c & 1) << 7);
	}
}

static int libztex_getFpgaState(struct libztex_device *ztex, struct libztex_fpgastate *state)
{
	unsigned char buf[9];
	int cnt;

	if (!libztex_checkCapability(ztex, CAPABILITY_FPGA))
		return -1;
	cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x30, 0, 0, buf, 9, 1000);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "%s: Failed getFpgaState with err %d", ztex->repr, cnt);
		return cnt;
	}
	state->fpgaConfigured = (buf[0] == 0);
	state->fpgaChecksum = buf[1] & 0xff;
	state->fpgaBytes = ((buf[5] & 0xff) << 24) | ((buf[4] & 0xff) << 16) | ((buf[3] & 0xff) << 8) | (buf[2] & 0xff);
	state->fpgaInitB = buf[6] & 0xff;
	state->fpgaFlashResult = buf[7];
	state->fpgaFlashBitSwap = (buf[8] != 0);
	return 0;
}

static int libztex_configureFpgaLS(struct libztex_device *ztex, const char* firmware, bool force, char bs)
{
	struct libztex_fpgastate state;
	const int transactionBytes = 2048;
	unsigned char buf[transactionBytes], cs;
	int tries, cnt, buf_p, i;
	ssize_t pos = 0;
	FILE *fp;

	if (!libztex_checkCapability(ztex, CAPABILITY_FPGA))
		return -1;

	libztex_getFpgaState(ztex, &state);
	if (!force && state.fpgaConfigured) {
		applog(LOG_DEBUG, "Bitstream already configured");
		return 1;
	}

	for (tries = 10; tries > 0; tries--) {
		fp = fopen(firmware, "rb");
		if (!fp) {
			applog(LOG_ERR, "%s: failed to read firmware '%s'", ztex->repr, firmware);
			return -2;
		}

		cs = 0;
		while (pos < transactionBytes && !feof(fp)) {
			buf[pos] = getc(fp);
			cs += buf[pos++];
		}

		if (feof(fp))
			pos--;

		if (bs != 0 && bs != 1)
			bs = libztex_detectBitstreamBitOrder(buf, transactionBytes < pos? transactionBytes: pos);

		//* Reset fpga
		cnt = libztex_resetFpga(ztex);
		if (unlikely(cnt < 0)) {
			applog(LOG_ERR, "%s: Failed reset fpga with err %d", ztex->repr, cnt);
			continue;
		}

		if (bs == 1)
			libztex_swapBits(buf, pos);
	 
		buf_p = pos;
		while (1) {
			i = 0;
			while (i < buf_p) {
				cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x32, 0, 0, &buf[i], buf_p - i, 5000);
				if (unlikely(cnt < 0)) {
					applog(LOG_ERR, "%s: Failed send fpga data with err %d", ztex->repr, cnt);
					break;
				}
				i += cnt;
			}
			if (i < buf_p || buf_p < transactionBytes)
				break;
			buf_p = 0;
			while (buf_p < transactionBytes && !feof(fp)) {
				buf[buf_p] = getc(fp);
				cs += buf[buf_p++];
			}
			if (feof(fp))
				buf_p--;
			pos += buf_p;
			if (buf_p == 0)
				break;
			if (bs == 1)
				libztex_swapBits(buf, buf_p);
		}
		if (cnt >= 0)
			tries = 0;

		fclose(fp);
	}
	libztex_getFpgaState(ztex, &state);
	if (!state.fpgaConfigured) {
		applog(LOG_ERR, "%s: FPGA configuration failed: DONE pin does not go high", ztex->repr);
		return 3;
	}
	usleep(200000);
	applog(LOG_INFO, "%s: FPGA configuration done", ztex->repr);
	return 0;
}

int libztex_configureFpga(struct libztex_device *ztex)
{
	char buf[256] = "bitstreams/";

	memset(&buf[11], 0, 245);
	strcpy(&buf[11], ztex->bitFileName);
	strcpy(&buf[strlen(buf)], ".bit");

	return libztex_configureFpgaLS(ztex, buf, true, 2);
}

int libztex_setFreq(struct libztex_device *ztex, uint16_t freq)
{
	int cnt;

	if (freq > ztex->freqMaxM)
		freq = ztex->freqMaxM;

	cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x83, freq, 0, NULL, 0, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to set frequency with err %d", cnt);
		return cnt;
	}
	ztex->freqM = freq;
	applog(LOG_WARNING, "%s: Frequency change to %0.2f Mhz", ztex->repr, ztex->freqM1 * (ztex->freqM + 1));

	return 0;
}

int libztex_resetFpga(struct libztex_device *ztex)
{
	return libusb_control_transfer(ztex->hndl, 0x40, 0x31, 0, 0, NULL, 0, 1000);
}

int libztex_prepare_device(struct libusb_device *dev, struct libztex_device** ztex)
{
	struct libztex_device *newdev;
	unsigned char buf[64];
	int cnt, err;

	newdev = malloc(sizeof(struct libztex_device));
	newdev->bitFileName = NULL;
	newdev->valid = false;
	newdev->hndl = NULL;
	*ztex = newdev;

	err = libusb_get_device_descriptor(dev, &newdev->descriptor);
	if (unlikely(err != 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to open read descriptor with error %d", err);
		return err;
	}

	// Check vendorId and productId
	if (!(newdev->descriptor.idVendor == LIBZTEX_IDVENDOR &&
				newdev->descriptor.idProduct == LIBZTEX_IDPRODUCT)) {
		applog(LOG_ERR, "Not a ztex device? %0.4X, %0.4X", newdev->descriptor.idVendor, newdev->descriptor.idProduct);
		return 1;
	}

	err = libusb_open(dev, &newdev->hndl);
	if (unlikely(err != 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to open handle with error %d", err);
		return err;
	}

	cnt = libusb_get_string_descriptor_ascii (newdev->hndl, newdev->descriptor.iSerialNumber, newdev->snString,
	                                          LIBZTEX_SNSTRING_LEN + 1);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read device snString with err %d", cnt);
		return cnt;
	}

	cnt = libusb_control_transfer(newdev->hndl, 0xc0, 0x22, 0, 0, buf, 40, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read ztex descriptor with err %d", cnt);
		return cnt;
	}
	
	if ( buf[0] != 40 || buf[1] != 1 || buf[2] != 'Z' || buf[3] != 'T' || buf[4] != 'E' || buf[5] != 'X' ) {
		applog(LOG_ERR, "Ztex check device: Error reading ztex descriptor");
		return 2;
	}

	newdev->productId[0] = buf[6];
	newdev->productId[1] = buf[7];
	newdev->productId[2] = buf[8];
	newdev->productId[3] = buf[9];
	newdev->fwVersion = buf[10];
	newdev->interfaceVersion = buf[11];
	newdev->interfaceCapabilities[0] = buf[12];
	newdev->interfaceCapabilities[1] = buf[13];
	newdev->interfaceCapabilities[2] = buf[14];
	newdev->interfaceCapabilities[3] = buf[15];
	newdev->interfaceCapabilities[4] = buf[16];
	newdev->interfaceCapabilities[5] = buf[17];
	newdev->moduleReserved[0] = buf[18];
	newdev->moduleReserved[1] = buf[19];
	newdev->moduleReserved[2] = buf[20];
	newdev->moduleReserved[3] = buf[21];
	newdev->moduleReserved[4] = buf[22];
	newdev->moduleReserved[5] = buf[23];
	newdev->moduleReserved[6] = buf[24];
	newdev->moduleReserved[7] = buf[25];
	newdev->moduleReserved[8] = buf[26];
	newdev->moduleReserved[9] = buf[27];
	newdev->moduleReserved[10] = buf[28];
	newdev->moduleReserved[11] = buf[29];


	cnt = libusb_control_transfer(newdev->hndl, 0xc0, 0x82, 0, 0, buf, 64, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read ztex descriptor with err %d", cnt);
		return cnt;
	}

	if (unlikely(buf[0] != 4)) {
		if (unlikely(buf[0] != 2)) {
			applog(LOG_ERR, "Invalid BTCMiner descriptor version. Firmware must be updated (%d).", buf[0]);
			return 3;
		}
		applog(LOG_WARNING, "Firmware out of date");
	}

	newdev->numNonces = buf[1] + 1;
	newdev->offsNonces = ((buf[2] & 255) | ((buf[3] & 255) << 8)) - 10000;
	newdev->freqM1 = ((buf[4] & 255) | ((buf[5] & 255) << 8) ) * 0.01;
	newdev->freqMaxM = (buf[7] & 255);
	newdev->freqM = (buf[6] & 255);
	newdev->freqMDefault = newdev->freqM;

	for (cnt=0; cnt < 255; cnt++) {
		newdev->errorCount[cnt] = 0;
		newdev->errorWeight[cnt] = 0;
		newdev->errorRate[cnt] = 0;
		newdev->maxErrorRate[cnt] = 0;
	}

	cnt = strlen((char *)&buf[buf[0] == 4? 10: 8]);
	newdev->bitFileName = malloc(sizeof(char) * (cnt + 1));
	memcpy(newdev->bitFileName, &buf[buf[0] == 4? 10: 8], cnt + 1);

	newdev->usbbus = libusb_get_bus_number(dev);
	newdev->usbaddress = libusb_get_device_address(dev);
	sprintf(newdev->repr, "ZTEX %.3d:%.3d-%s", newdev->usbbus, newdev->usbaddress, newdev->snString);
	newdev->valid = true;
	return 0;
}

void libztex_destroy_device(struct libztex_device* ztex)
{
	if (ztex->hndl != NULL) {
		libusb_close(ztex->hndl);
		ztex->hndl = NULL;
	}
	if (ztex->bitFileName != NULL) {
		free(ztex->bitFileName);
		ztex->bitFileName = NULL;
	}
	free(ztex);
}

int libztex_scanDevices(struct libztex_dev_list*** devs_p)
{
	int usbdevices[LIBZTEX_MAX_DESCRIPTORS];
	struct libztex_dev_list **devs;
	struct libztex_device *ztex;
	int found = 0, pos = 0, err;
	libusb_device **list;
	ssize_t cnt, i = 0;

	cnt = libusb_get_device_list(NULL, &list);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex scan devices: Failed to list usb devices with err %d", cnt);
		return 0;
	}

	for (i = 0; i < cnt; i++) {
		if (libztex_checkDevice(list[i])) {
			// Got one!
			usbdevices[found] = i;
			found++;
		}
	}

	devs = malloc(sizeof(struct libztex_dev_list *) * found);
	if (devs == NULL) {
		applog(LOG_ERR, "Ztex scan devices: Failed to allocate memory");
		return 0;
	}

	for (i = 0; i < found; i++) {
		err = libztex_prepare_device(list[usbdevices[i]], &ztex);
		if (unlikely(err != 0))
			applog(LOG_ERR, "prepare device: %d", err);
		// check if valid
		if (!ztex->valid) {
			libztex_destroy_device(ztex);
			continue;
		}
		devs[pos] = malloc(sizeof(struct libztex_dev_list));
		devs[pos]->dev = ztex;
		devs[pos]->next = NULL;
		if (pos > 0)
			devs[pos - 1]->next = devs[pos];
		pos++;
	}

	libusb_free_device_list(list, 1);
	*devs_p = devs;
	return pos;
}

int libztex_sendHashData(struct libztex_device *ztex, unsigned char *sendbuf)
{
	int cnt;

	if (ztex == NULL || ztex->hndl == NULL)
		return 0;
	cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x80, 0, 0, sendbuf, 44, 1000);
	if (unlikely(cnt < 0))
		applog(LOG_ERR, "%s: Failed sendHashData with err %d", ztex->repr, cnt);
	
	return cnt;
}

int libztex_readHashData(struct libztex_device *ztex, struct libztex_hash_data nonces[])
{
	// length of buf must be 8 * (numNonces + 1)
	unsigned char rbuf[12 * 8];
	int cnt, i;

	if (ztex->hndl == NULL)
		return 0;
	
	cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x81, 0, 0, rbuf, 12 * ztex->numNonces, 1000);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "%s: Failed readHashData with err %d", ztex->repr, cnt);
		return cnt;
	}

	for (i = 0; i < ztex->numNonces; i++) {
		memcpy((char*)&nonces[i].goldenNonce, &rbuf[i * 12], 4);
		nonces[i].goldenNonce -= ztex->offsNonces;
		memcpy((char*)&nonces[i].nonce, &rbuf[(i * 12) + 4], 4);
		nonces[i].nonce -= ztex->offsNonces;
		memcpy((char*)&nonces[i].hash7, &rbuf[(i * 12) + 8], 4);
	}
	
	return cnt;
}

void libztex_freeDevList(struct libztex_dev_list **devs)
{
	bool done = false;
	ssize_t cnt = 0;

	while (!done) {
		if (devs[cnt]->next == NULL)
			done = true;
		free(devs[cnt++]);
	}
	free(devs);
}
