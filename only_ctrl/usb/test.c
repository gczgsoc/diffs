/*
 * Copyright (c) 2015 Grant Czajkowski <czajkow2@illinois.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <poll.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

void usage(void);
int test_sync_control(char *);
int test_async_control(char *);
int main(int, char **);

extern char *__progname;

void
usage(void)
{
	fprintf(stderr, "usage: %s [-d devnode]\n", __progname);
	exit(1);
}

int
test_sync_control(char *dev)
{
	struct usb_request_block urb;
	char buf[2];
	int fd, err;
	
	if ((fd = open(dev, O_RDWR)) < 0)
		return (-1);

	urb.urb_addr = 0;
	urb.urb_endpt = 0;
	urb.urb_request.bmRequestType = UT_READ_DEVICE;
	urb.urb_request.bRequest = UR_GET_STATUS;
	USETW(urb.urb_request.wValue, 0);
	USETW(urb.urb_request.wIndex, 0);
	USETW(urb.urb_request.wLength, 2);
	urb.urb_data = &buf;
	urb.urb_flags = USBD_SYNCHRONOUS;
	urb.urb_actlen = 2;
	urb.urb_timeout = USBD_DEFAULT_TIMEOUT;
	urb.urb_read = 1;

	if (ioctl(fd, USB_DO_REQUEST, &urb)) {
		err = errno;
		close(fd);
		return (err);
	}

	close(fd);
	return (0);
}

int
test_async_control(char *dev)
{
	struct usb_request_block urb;
	struct usb_request_block rurb;
	char buf[2];
	int fd, err;
	struct pollfd pfd;
	
	if ((fd = open(dev, O_RDWR)) < 0)
		return (-1);

	urb.urb_addr = 0;
	urb.urb_endpt = 0;
	urb.urb_request.bmRequestType = UT_READ_DEVICE;
	urb.urb_request.bRequest = UR_GET_STATUS;
	USETW(urb.urb_request.wValue, 0);
	USETW(urb.urb_request.wIndex, 0);
	USETW(urb.urb_request.wLength, 2);
	urb.urb_data = &buf;
	urb.urb_flags = 0;
	urb.urb_actlen = 2;
	urb.urb_timeout = USBD_DEFAULT_TIMEOUT;
	urb.urb_read = 1;

	if (ioctl(fd, USB_DO_REQUEST, &urb)) {
		err = errno;
		close(fd);
		return (err);
	}

	pfd.fd = fd;
	pfd.events = POLLIN | POLLRDNORM;
	if ((poll(&pfd, 1, INFTIM)) < 0) {
		err = errno;
		close(fd);
		return (err);
	}

	if (ioctl(fd, USB_GET_COMPLETED, &rurb)) {
		err = errno;
		close(fd);
		return (err);
	}

	close(fd);
	return (0);
}

int
main(int argc, char **argv)
{
	int ch;
	char *dev = 0;

	/* give the address for the device we want to run
	 * the tests on */
	/* try sending a request to it */

	while ((ch = getopt(argc, argv, "d:?")) != -1) {
		switch (ch) {
		case 'd':
			dev = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (dev == 0)
		usage();

	if (test_sync_control(dev))
		err(1, "synchronous control transfer");

	if (test_async_control(dev))
		err(1, "asynchronous control transfer");

	exit(0);
}
