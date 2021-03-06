/*
 * Copyright © 2011-2013 Martin Pieuchot <mpi@openbsd.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "libusbi.h"

struct device_priv {
	char *devname;				/* name of the ugen(4) node */
	int fd;					/* device file descriptor */

	unsigned char *cdesc;			/* active config descriptor */
	usb_device_descriptor_t ddesc;		/* usb device descriptor */
};

struct handle_priv {
	int endpoints[USB_MAX_ENDPOINTS];
};

/*
 * Backend functions
 */
static int obsd_get_device_list(struct libusb_context *,
    struct discovered_devs **);
static int obsd_open(struct libusb_device_handle *);
static void obsd_close(struct libusb_device_handle *);

static int obsd_get_device_descriptor(struct libusb_device *, unsigned char *,
    int *);
static int obsd_get_active_config_descriptor(struct libusb_device *,
    unsigned char *, size_t, int *);
static int obsd_get_config_descriptor(struct libusb_device *, uint8_t,
    unsigned char *, size_t, int *);

static int obsd_get_configuration(struct libusb_device_handle *, int *);
static int obsd_set_configuration(struct libusb_device_handle *, int);

static int obsd_claim_interface(struct libusb_device_handle *, int);
static int obsd_release_interface(struct libusb_device_handle *, int);

static int obsd_set_interface_altsetting(struct libusb_device_handle *, int,
    int);
static int obsd_clear_halt(struct libusb_device_handle *, unsigned char);
static int obsd_reset_device(struct libusb_device_handle *);
static void obsd_destroy_device(struct libusb_device *);

static int obsd_submit_transfer(struct usbi_transfer *);
static int obsd_cancel_transfer(struct usbi_transfer *);
static void obsd_clear_transfer_priv(struct usbi_transfer *);
static int obsd_handle_events(struct libusb_context *ctx, struct pollfd *,
    nfds_t, int);
static int obsd_handle_transfer_completion(struct usbi_transfer *);
static int obsd_clock_gettime(int, struct timespec *);

/*
 * Private functions
 */
static int _errno_to_libusb(int);
static int _cache_active_config_descriptor(struct libusb_device *);
static int _sync_control_transfer(struct usbi_transfer *);
static int _sync_bulk_transfer(struct usbi_transfer *itransfer);
static int _cancel_control_transfer(struct usbi_transfer *);
static int _cancel_bulk_transfer(struct usbi_transfer *);
static int _sync_gen_transfer(struct usbi_transfer *);
static int _access_endpoint(struct libusb_transfer *);

static int _bus_open(int);


const struct usbi_os_backend openbsd_backend = {
	"Synchronous OpenBSD backend",
	0,
	NULL,				/* init() */
	NULL,				/* exit() */
	obsd_get_device_list,
	NULL,				/* hotplug_poll */
	obsd_open,
	obsd_close,

	obsd_get_device_descriptor,
	obsd_get_active_config_descriptor,
	obsd_get_config_descriptor,
	NULL,				/* get_config_descriptor_by_value() */

	obsd_get_configuration,
	obsd_set_configuration,

	obsd_claim_interface,
	obsd_release_interface,

	obsd_set_interface_altsetting,
	obsd_clear_halt,
	obsd_reset_device,

	NULL,				/* alloc_streams */
	NULL,				/* free_streams */

	NULL,				/* kernel_driver_active() */
	NULL,				/* detach_kernel_driver() */
	NULL,				/* attach_kernel_driver() */

	obsd_destroy_device,

	obsd_submit_transfer,
	obsd_cancel_transfer,
	obsd_clear_transfer_priv,

	obsd_handle_events,
	obsd_handle_transfer_completion,

	obsd_clock_gettime,
	sizeof(struct device_priv),
	sizeof(struct handle_priv),
	0,				/* transfer_priv_size */
};

#define DEVPATH	"/dev/"
#define USBDEV	DEVPATH "usb"

int
obsd_get_device_list(struct libusb_context * ctx,
	struct discovered_devs **discdevs)
{
	struct discovered_devs *ddd;
	struct libusb_device *dev;
	struct device_priv *dpriv;
	struct usb_device_info di;
	struct usb_device_ddesc dd;
	unsigned long session_id;
	char devices[USB_MAX_DEVICES];
	char busnode[16];
	char *udevname;
	int fd, addr, i, j;

	usbi_dbg("");

	for (i = 0; i < 8; i++) {
		snprintf(busnode, sizeof(busnode), USBDEV "%d", i);

		if ((fd = open(busnode, O_RDWR)) < 0) {
			if (errno != ENOENT && errno != ENXIO)
				usbi_err(ctx, "could not open %s", busnode);
			continue;
		}

		bzero(devices, sizeof(devices));
		for (addr = 1; addr < USB_MAX_DEVICES; addr++) {
			if (devices[addr])
				continue;

			di.udi_addr = addr;
			if (ioctl(fd, USB_DEVICEINFO, &di) < 0)
				continue;

			/*
			 * XXX If ugen(4) is attached to the USB device
			 * it will be used.
			 */
			udevname = NULL;
			for (j = 0; j < USB_MAX_DEVNAMES; j++)
				if (!strncmp("ugen", di.udi_devnames[j], 4)) {
					udevname = strdup(di.udi_devnames[j]);
					break;
				}

			session_id = (di.udi_bus << 8 | di.udi_addr);
			dev = usbi_get_device_by_session_id(ctx, session_id);

			if (dev == NULL) {
				dev = usbi_alloc_device(ctx, session_id);
				if (dev == NULL) {
					close(fd);
					return (LIBUSB_ERROR_NO_MEM);
				}

				dev->bus_number = di.udi_bus;
				dev->device_address = di.udi_addr;
				dev->speed = di.udi_speed;

				dpriv = (struct device_priv *)dev->os_priv;
				dpriv->fd = -1;
				dpriv->cdesc = NULL;
				dpriv->devname = udevname;

				dd.udd_bus = di.udi_bus;
				dd.udd_addr = di.udi_addr;
				if (ioctl(fd, USB_DEVICE_GET_DDESC, &dd) < 0) {
					libusb_unref_device(dev);
					continue;
				}
				dpriv->ddesc = dd.udd_desc;

				if (_cache_active_config_descriptor(dev)) {
					libusb_unref_device(dev);
					continue;
				}

				if (usbi_sanitize_device(dev)) {
					libusb_unref_device(dev);
					continue;
				}
			}

			ddd = discovered_devs_append(*discdevs, dev);
			if (ddd == NULL) {
				close(fd);
				return (LIBUSB_ERROR_NO_MEM);
			}
			libusb_unref_device(dev);

			*discdevs = ddd;
			devices[addr] = 1;
		}

		close(fd);
	}

	return (LIBUSB_SUCCESS);
}

int
obsd_open(struct libusb_device_handle *handle)
{
	struct handle_priv *hpriv = (struct handle_priv *)handle->os_priv;
	struct device_priv *dpriv = (struct device_priv *)handle->dev->os_priv;
	char devnode[16];

	if (dpriv->devname) {
		/*
		 * Only open ugen(4) attached devices read-write, all
		 * read-only operations are done through the bus node.
		 */
		snprintf(devnode, sizeof(devnode), DEVPATH "%s.00",
		    dpriv->devname);
		dpriv->fd = open(devnode, O_RDWR);
		if (dpriv->fd < 0)
			return _errno_to_libusb(errno);

		usbi_add_pollfd(HANDLE_CTX(handle), dpriv->fd, POLLIN | POLLRDNORM);
		usbi_dbg("open %s: fd %d", devnode, dpriv->fd);
	}

	return (LIBUSB_SUCCESS);
}

void
obsd_close(struct libusb_device_handle *handle)
{
	struct handle_priv *hpriv = (struct handle_priv *)handle->os_priv;
	struct device_priv *dpriv = (struct device_priv *)handle->dev->os_priv;

	if (dpriv->devname) {
		usbi_dbg("close: fd %d", dpriv->fd);

		usbi_remove_pollfd(HANDLE_CTX(handle), dpriv->fd);
		close(dpriv->fd);
		dpriv->fd = -1;
	}
}

int
obsd_get_device_descriptor(struct libusb_device *dev, unsigned char *buf,
    int *host_endian)
{
	struct device_priv *dpriv = (struct device_priv *)dev->os_priv;

	usbi_dbg("");

	memcpy(buf, &dpriv->ddesc, DEVICE_DESC_LENGTH);

	*host_endian = 0;

	return (LIBUSB_SUCCESS);
}

int
obsd_get_active_config_descriptor(struct libusb_device *dev,
    unsigned char *buf, size_t len, int *host_endian)
{
	struct device_priv *dpriv = (struct device_priv *)dev->os_priv;
	usb_config_descriptor_t *ucd = (usb_config_descriptor_t *)dpriv->cdesc;

	len = MIN(len, UGETW(ucd->wTotalLength));

	usbi_dbg("len %d", len);

	memcpy(buf, dpriv->cdesc, len);

	*host_endian = 0;

	return (len);
}

int
obsd_get_config_descriptor(struct libusb_device *dev, uint8_t idx,
    unsigned char *buf, size_t len, int *host_endian)
{
	struct usb_device_fdesc udf;
	int fd, err;

	if ((fd = _bus_open(dev->bus_number)) < 0)
		return _errno_to_libusb(errno);

	udf.udf_bus = dev->bus_number;
	udf.udf_addr = dev->device_address;
	udf.udf_config_index = idx;
	udf.udf_size = len;
	udf.udf_data = buf;

	usbi_dbg("index %d, len %d", udf.udf_config_index, len);

	if (ioctl(fd, USB_DEVICE_GET_FDESC, &udf) < 0) {
		err = errno;
		close(fd);
		return _errno_to_libusb(err);
	}
	close(fd);

	*host_endian = 0;

	return (len);
}

int
obsd_get_configuration(struct libusb_device_handle *handle, int *config)
{
	struct device_priv *dpriv = (struct device_priv *)handle->dev->os_priv;
	usb_config_descriptor_t *ucd = (usb_config_descriptor_t *)dpriv->cdesc;

	*config = ucd->bConfigurationValue;

	usbi_dbg("bConfigurationValue %d", *config);

	return (LIBUSB_SUCCESS);
}

int
obsd_set_configuration(struct libusb_device_handle *handle, int config)
{
	struct device_priv *dpriv = (struct device_priv *)handle->dev->os_priv;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	usbi_dbg("bConfigurationValue %d", config);

	if (ioctl(dpriv->fd, USB_SET_CONFIG, &config) < 0)
		return _errno_to_libusb(errno);

	return _cache_active_config_descriptor(handle->dev);
}

int
obsd_claim_interface(struct libusb_device_handle *handle, int iface)
{
	struct handle_priv *hpriv = (struct handle_priv *)handle->os_priv;
	int i;

	for (i = 0; i < USB_MAX_ENDPOINTS; i++)
		hpriv->endpoints[i] = -1;

	return (LIBUSB_SUCCESS);
}

int
obsd_release_interface(struct libusb_device_handle *handle, int iface)
{
	struct handle_priv *hpriv = (struct handle_priv *)handle->os_priv;
	int i;

	for (i = 0; i < USB_MAX_ENDPOINTS; i++) {
		if (hpriv->endpoints[i] >= 0) {
			usbi_remove_pollfd(HANDLE_CTX(handle), hpriv->endpoints[i]);
			close(hpriv->endpoints[i]);
		}
	}

	return (LIBUSB_SUCCESS);
}

int
obsd_set_interface_altsetting(struct libusb_device_handle *handle, int iface,
    int altsetting)
{
	struct device_priv *dpriv = (struct device_priv *)handle->dev->os_priv;
	struct usb_alt_interface intf;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	usbi_dbg("iface %d, setting %d", iface, altsetting);

	memset(&intf, 0, sizeof(intf));

	intf.uai_interface_index = iface;
	intf.uai_alt_no = altsetting;

	if (ioctl(dpriv->fd, USB_SET_ALTINTERFACE, &intf) < 0)
		return _errno_to_libusb(errno);

	return (LIBUSB_SUCCESS);
}

int
obsd_clear_halt(struct libusb_device_handle *handle, unsigned char endpoint)
{
	struct usb_ctl_request req;
	int fd, err;

	if ((fd = _bus_open(handle->dev->bus_number)) < 0)
		return _errno_to_libusb(errno);

	usbi_dbg("");

	req.ucr_addr = handle->dev->device_address;
	req.ucr_request.bmRequestType = UT_WRITE_ENDPOINT;
	req.ucr_request.bRequest = UR_CLEAR_FEATURE;
	USETW(req.ucr_request.wValue, UF_ENDPOINT_HALT);
	USETW(req.ucr_request.wIndex, endpoint);
	USETW(req.ucr_request.wLength, 0);

	if (ioctl(fd, USB_REQUEST, &req) < 0) {
		err = errno;
		close(fd);
		return _errno_to_libusb(err);
	}
	close(fd);

	return (LIBUSB_SUCCESS);
}

int
obsd_reset_device(struct libusb_device_handle *handle)
{
	usbi_dbg("");

	return (LIBUSB_ERROR_NOT_SUPPORTED);
}

void
obsd_destroy_device(struct libusb_device *dev)
{
	struct device_priv *dpriv = (struct device_priv *)dev->os_priv;

	usbi_dbg("");

	free(dpriv->cdesc);
	free(dpriv->devname);
}

int
obsd_submit_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer;
	struct handle_priv *hpriv;
	struct device_priv *dpriv;
	int err = 0;

	usbi_dbg("");

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	hpriv = (struct handle_priv *)transfer->dev_handle->os_priv;
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	switch (transfer->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		err = _sync_control_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
		if (IS_XFEROUT(transfer)) {
			/* Isochronous write is not supported */
			err = LIBUSB_ERROR_NOT_SUPPORTED;
			break;
		}
		err = _sync_gen_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_BULK:
		err = _sync_bulk_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
		if (IS_XFEROUT(transfer) &&
		    transfer->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET) {
			err = LIBUSB_ERROR_NOT_SUPPORTED;
			break;
		}
		err = _sync_gen_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
		err = LIBUSB_ERROR_NOT_SUPPORTED;
		break;
	}

	if (err)
		return (err);

	if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
		if (dpriv->devname == NULL)
			usbi_signal_transfer_completion(itransfer);
	} else if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
		/* do nothing */
	} else {
		usbi_signal_transfer_completion(itransfer);
	}

	return (LIBUSB_SUCCESS);
}

int
obsd_cancel_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer;
	struct handle_priv *hpriv;
	struct device_priv *dpriv;
	int err = 0;

	usbi_dbg("");

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	hpriv = (struct handle_priv *)transfer->dev_handle->os_priv;
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	switch (transfer->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		err = _cancel_control_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
		err = LIBUSB_ERROR_NOT_SUPPORTED;
		break;
	case LIBUSB_TRANSFER_TYPE_BULK:
		err = _cancel_bulk_transfer(itransfer);
		break;
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
		err = LIBUSB_ERROR_NOT_SUPPORTED;
		break;
	case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
		err = LIBUSB_ERROR_NOT_SUPPORTED;
		break;
	}

	if (err)
		return (err);

	return (LIBUSB_SUCCESS);
}

void
obsd_clear_transfer_priv(struct usbi_transfer *itransfer)
{
	usbi_dbg("");

	/* Nothing to do */
}

int
obsd_handle_events(struct libusb_context *ctx, struct pollfd *fds, nfds_t nfds,
    int num_ready)
{
	struct libusb_device_handle *handle;
	struct handle_priv *hpriv = NULL;
	struct device_priv *dpriv = NULL;
	struct usbi_transfer *itransfer;
	struct usb_ctl_request req;
	struct pollfd *pollfd;
	int i, err = 0;
	int endpt;
	int error_code = LIBUSB_TRANSFER_COMPLETED;
	int fd;

	usbi_dbg("");

	pthread_mutex_lock(&ctx->open_devs_lock);
	for (i = 0; i < nfds && num_ready > 0; i++) {
		pollfd = &fds[i];

		if (!pollfd->revents)
			continue;

		hpriv = NULL;
		num_ready--;
		list_for_each_entry(handle, &ctx->open_devs, list,
		    struct libusb_device_handle) {
			hpriv = (struct handle_priv *)handle->os_priv;
			dpriv = (struct device_priv *)handle->dev->os_priv;

			if (dpriv->fd == pollfd->fd) {
				fd = dpriv->fd;
				break;
			}

			for (endpt = 0; endpt < USB_MAX_ENDPOINTS; endpt++) {
				if (hpriv->endpoints[endpt] == pollfd->fd) {
					fd = hpriv->endpoints[endpt];
					goto out;
				}
			}

			hpriv = NULL;
		}

out:
		if (NULL == hpriv) {
			usbi_dbg("fd %d is not an event pipe!", pollfd->fd);
			err = ENOENT;
			break;
		}

		if (pollfd->revents & POLLERR) {
			usbi_dbg("got a disconnect event");
			for (endpt = 0; endpt < USB_MAX_ENDPOINTS; endpt++) {
				if (hpriv->endpoints[endpt] >= 0) {
					usbi_remove_pollfd(HANDLE_CTX(handle), hpriv->endpoints[endpt]);
					close(hpriv->endpoints[endpt]);
				}
			}
			usbi_remove_pollfd(HANDLE_CTX(handle), dpriv->fd);
			usbi_handle_disconnect(handle);
			continue;
		}

		while (1) {
repeat:
			if (ioctl(fd, USB_GET_COMPLETED, &req)) {
				err = 0;
				break;
			}
			itransfer = req.ucr_context;

			switch(req.ucr_status) {
			case USBD_NORMAL_COMPLETION:
				usbi_mutex_lock(&itransfer->lock);
				itransfer->transferred += req.ucr_actlen;
				usbi_dbg("transferred %d", itransfer->transferred);
				usbi_mutex_unlock(&itransfer->lock);

				error_code = LIBUSB_TRANSFER_COMPLETED;
				break;
			case USBD_SHORT_XFER:
				error_code = LIBUSB_TRANSFER_ERROR;
				break;
			case USBD_IN_PROGRESS:
				goto repeat;
			/* errors */
			case USBD_CANCELLED:
				error_code = LIBUSB_TRANSFER_CANCELLED;
				break;
			case USBD_STALLED:
				error_code = LIBUSB_TRANSFER_STALL;
				break;
			default:
				error_code = LIBUSB_TRANSFER_ERROR;
				break;
			}
			if (error_code == LIBUSB_TRANSFER_CANCELLED) {
				usbi_dbg("cancelling the transfer");
				if ((err = usbi_handle_transfer_cancellation(itransfer)))
					break;
			} else {
				if ((err = usbi_handle_transfer_completion(itransfer, error_code)))
					break;
			}
		}
		if (err) {
			err = errno;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->open_devs_lock);

	if (err)
		return _errno_to_libusb(err);

	return (LIBUSB_SUCCESS);
}

int
obsd_handle_transfer_completion(struct usbi_transfer *itransfer)
{
	return usbi_handle_transfer_completion(itransfer, LIBUSB_TRANSFER_COMPLETED);
}

int
obsd_clock_gettime(int clkid, struct timespec *tp)
{
	usbi_dbg("clock %d", clkid);

	if (clkid == USBI_CLOCK_REALTIME)
		return clock_gettime(CLOCK_REALTIME, tp);

	if (clkid == USBI_CLOCK_MONOTONIC)
		return clock_gettime(CLOCK_MONOTONIC, tp);

	return (LIBUSB_ERROR_INVALID_PARAM);
}

int
_errno_to_libusb(int err)
{
	usbi_dbg("error: %s (%d)", strerror(err), err);

	switch (err) {
	case EIO:
		return (LIBUSB_ERROR_IO);
	case EACCES:
		return (LIBUSB_ERROR_ACCESS);
	case ENOENT:
		return (LIBUSB_ERROR_NO_DEVICE);
	case ENOMEM:
		return (LIBUSB_ERROR_NO_MEM);
	case ETIMEDOUT:
		return (LIBUSB_ERROR_TIMEOUT);
	}

	return (LIBUSB_ERROR_OTHER);
}

int
_cache_active_config_descriptor(struct libusb_device *dev)
{
	struct device_priv *dpriv = (struct device_priv *)dev->os_priv;
	struct usb_device_cdesc udc;
	struct usb_device_fdesc udf;
	unsigned char* buf;
	int fd, len, err;

	if ((fd = _bus_open(dev->bus_number)) < 0)
		return _errno_to_libusb(errno);

	usbi_dbg("fd %d, addr %d", fd, dev->device_address);

	udc.udc_bus = dev->bus_number;
	udc.udc_addr = dev->device_address;
	udc.udc_config_index = USB_CURRENT_CONFIG_INDEX;
	if (ioctl(fd, USB_DEVICE_GET_CDESC, &udc) < 0) {
		err = errno;
		close(fd);
		return _errno_to_libusb(errno);
	}

	usbi_dbg("active bLength %d", udc.udc_desc.bLength);

	len = UGETW(udc.udc_desc.wTotalLength);
	buf = malloc(len);
	if (buf == NULL)
		return (LIBUSB_ERROR_NO_MEM);

	udf.udf_bus = dev->bus_number;
	udf.udf_addr = dev->device_address;
	udf.udf_config_index = udc.udc_config_index;
	udf.udf_size = len;
	udf.udf_data = buf;

	usbi_dbg("index %d, len %d", udf.udf_config_index, len);

	if (ioctl(fd, USB_DEVICE_GET_FDESC, &udf) < 0) {
		err = errno;
		close(fd);
		free(buf);
		return _errno_to_libusb(err);
	}
	close(fd);

	if (dpriv->cdesc)
		free(dpriv->cdesc);
	dpriv->cdesc = buf;

	return (LIBUSB_SUCCESS);
}

int
_sync_control_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer;
	struct libusb_control_setup *setup;
	struct device_priv *dpriv;
	struct usb_ctl_request req;

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;
	setup = (struct libusb_control_setup *)transfer->buffer;

	usbi_dbg("type %x request %x value %x index %d length %d timeout %d",
	    setup->bmRequestType, setup->bRequest,
	    libusb_le16_to_cpu(setup->wValue),
	    libusb_le16_to_cpu(setup->wIndex),
	    libusb_le16_to_cpu(setup->wLength), transfer->timeout);

	req.ucr_addr = transfer->dev_handle->dev->device_address;
	req.ucr_request.bmRequestType = setup->bmRequestType;
	req.ucr_request.bRequest = setup->bRequest;
	/* Don't use USETW, libusb already deals with the endianness */
	(*(uint16_t *)req.ucr_request.wValue) = setup->wValue;
	(*(uint16_t *)req.ucr_request.wIndex) = setup->wIndex;
	(*(uint16_t *)req.ucr_request.wLength) = setup->wLength;
	req.ucr_data = transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE;

	req.ucr_flags = 0;
	if ((transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) == 0)
		req.ucr_flags |= USBD_SHORT_XFER_OK;

	if (dpriv->devname == NULL) {
		/*
		 * XXX If the device is not attached to ugen(4) it is
		 * XXX still possible to submit a control transfer but
		 * XXX with the default timeout only.
		 */
		int fd, err;

		if ((fd = _bus_open(transfer->dev_handle->dev->bus_number)) < 0)
			return _errno_to_libusb(errno);

		if ((ioctl(fd, USB_REQUEST, &req)) < 0) {
			err = errno;
			close(fd);
			return _errno_to_libusb(err);
		}
		close(fd);
	} else {
		req.ucr_context = itransfer;
		req.ucr_timeout = transfer->timeout;
		req.ucr_read = req.ucr_request.bmRequestType & UT_READ;

		if ((ioctl(dpriv->fd, USB_DO_REQUEST, &req)) < 0)
			return _errno_to_libusb(errno);

		return (0);
	}

	itransfer->transferred = req.ucr_actlen;

	usbi_dbg("transferred %d", itransfer->transferred);

	return (0);
}

int
_cancel_control_transfer(struct usbi_transfer *itransfer) {
	struct libusb_transfer *transfer;
	struct handle_priv *hpriv;
	struct device_priv *dpriv;
	struct usb_ctl_request req;
	int err = 0;

	usbi_dbg("");

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	hpriv = (struct handle_priv *)transfer->dev_handle->os_priv;
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	req.ucr_context = itransfer;
	if (ioctl(dpriv->fd, USB_CANCEL, &req)) {
		usbi_dbg("transfer not found");
		return _errno_to_libusb(errno);
	}

	return (LIBUSB_SUCCESS);
}

int
_cancel_bulk_transfer(struct usbi_transfer *itransfer) {
	struct libusb_transfer *transfer;
	struct handle_priv *hpriv;
	struct device_priv *dpriv;
	struct usb_ctl_request req;
	int fd;
	int err = 0;

	usbi_dbg("");

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	hpriv = (struct handle_priv *)transfer->dev_handle->os_priv;
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	/*
	 * Bulk, Interrupt or Isochronous transfer depends on the
	 * endpoint and thus the node to open.
	 */
	if ((fd = _access_endpoint(transfer)) < 0)
		return _errno_to_libusb(errno);

	req.ucr_context = itransfer;
	if (ioctl(fd, USB_CANCEL, &req)) {
		usbi_dbg("transfer not found");
		return _errno_to_libusb(errno);
	}
	usbi_dbg("transfer found");

	return (LIBUSB_SUCCESS);
}

int
_access_endpoint(struct libusb_transfer *transfer)
{
	struct handle_priv *hpriv;
	struct device_priv *dpriv;
	struct libusb_device_handle *handle;
	char devnode[16];
	int fd, endpt;
	mode_t mode;

	handle = (struct libusb_device_handle *)transfer->dev_handle;
	hpriv = (struct handle_priv *)transfer->dev_handle->os_priv;
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	endpt = UE_GET_ADDR(transfer->endpoint);
	mode = IS_XFERIN(transfer) ? O_RDONLY : O_WRONLY;

	usbi_dbg("endpoint %d mode %d", endpt, mode);

	if (hpriv->endpoints[endpt] < 0) {
		/* Pick the right endpoint node */
		snprintf(devnode, sizeof(devnode), DEVPATH "%s.%02d",
		    dpriv->devname, endpt);

		usbi_dbg("devnode %s", devnode);

		/* We may need to read/write to the same endpoint later. */
		if (((fd = open(devnode, O_RDWR)) < 0) && (errno == ENXIO)) {
			if ((fd = open(devnode, mode)) < 0)
				return (-1);
		} else if (fd < 0) {
			return (-1);
		}

		hpriv->endpoints[endpt] = fd;
		usbi_add_pollfd(HANDLE_CTX(handle), fd, POLLIN | POLLRDNORM);
	}

	return (hpriv->endpoints[endpt]);
}

int
_sync_bulk_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer;
	struct device_priv *dpriv;
	struct usb_ctl_request req;
	int fd;

	usbi_dbg("");

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	/*
	 * Bulk, Interrupt or Isochronous transfer depends on the
	 * endpoint and thus the node to open.
	 */
	if ((fd = _access_endpoint(transfer)) < 0)
		return _errno_to_libusb(errno);

	req.ucr_timeout = transfer->timeout;
	req.ucr_flags = 0;
	if ((transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) == 0)
		req.ucr_flags |= USBD_SHORT_XFER_OK;

	if (transfer->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET)
		req.ucr_flags |= USBD_FORCE_SHORT_XFER;

	req.ucr_read = IS_XFERIN(transfer);
	req.ucr_data = transfer->buffer;
	req.ucr_actlen = transfer->length;
	req.ucr_context = itransfer;
	if (ioctl(fd, USB_DO_REQUEST, &req))
		return _errno_to_libusb(errno);

	return (0);
}

int
_sync_gen_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer;
	struct device_priv *dpriv;
	int fd, nr = 1;

	transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	dpriv = (struct device_priv *)transfer->dev_handle->dev->os_priv;

	if (dpriv->devname == NULL)
		return (LIBUSB_ERROR_NOT_SUPPORTED);

	/*
	 * Bulk, Interrupt or Isochronous transfer depends on the
	 * endpoint and thus the node to open.
	 */
	if ((fd = _access_endpoint(transfer)) < 0)
		return _errno_to_libusb(errno);

	if ((ioctl(fd, USB_SET_TIMEOUT, &transfer->timeout)) < 0)
		return _errno_to_libusb(errno);

	if (IS_XFERIN(transfer)) {
		if ((transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) == 0)
			if ((ioctl(fd, USB_SET_SHORT_XFER, &nr)) < 0)
				return _errno_to_libusb(errno);

		nr = read(fd, transfer->buffer, transfer->length);
	} else {
		nr = write(fd, transfer->buffer, transfer->length);
	}

	if (nr < 0)
		return _errno_to_libusb(errno);

	itransfer->transferred = nr;

	return (0);
}

int
_bus_open(int number)
{
	char busnode[16];

	snprintf(busnode, sizeof(busnode), USBDEV "%d", number);

	return open(busnode, O_RDWR);
}
