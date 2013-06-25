/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "protocol.h"

extern struct sr_dev_driver kecheng_kc_330b_driver_info;
static struct sr_dev_driver *di = &kecheng_kc_330b_driver_info;
extern const uint64_t kecheng_kc_330b_sample_intervals[][2];

SR_PRIV int kecheng_kc_330b_handle_events(int fd, int revents, void *cb_data)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_datafeed_packet packet;
	struct sr_dev_inst *sdi;
	struct sr_usb_dev_inst *usb;
	struct timeval tv;
	const uint64_t *intv_entry;
	gint64 now, interval;
	int len, ret, i;
	unsigned char cmd;

	(void)fd;
	(void)revents;

	drvc = di->priv;
	sdi = cb_data;
	devc = sdi->priv;
	usb = sdi->conn;

	memset(&tv, 0, sizeof(struct timeval));
	libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &tv,
					       NULL);

	if (sdi->status == SR_ST_STOPPING) {
		libusb_free_transfer(devc->xfer);
		for (i = 0; devc->usbfd[i] != -1; i++)
			sr_source_remove(devc->usbfd[i]);
		packet.type = SR_DF_END;
		sr_session_send(cb_data, &packet);
		sdi->status = SR_ST_ACTIVE;
		return TRUE;
	}

	if (devc->state == LIVE_SPL_IDLE) {
		now = g_get_monotonic_time() / 1000;
		intv_entry = kecheng_kc_330b_sample_intervals[devc->sample_interval];
		interval = intv_entry[0] * 1000 / intv_entry[1];
		if (now - devc->last_live_request > interval) {
			cmd = CMD_GET_LIVE_SPL;
			ret = libusb_bulk_transfer(usb->devhdl, EP_OUT, &cmd, 1, &len, 5);
			if (ret != 0 || len != 1) {
				sr_dbg("Failed to request new acquisition: %s",
						libusb_error_name(ret));
				sdi->driver->dev_acquisition_stop((struct sr_dev_inst *)sdi,
						devc->cb_data);
				return TRUE;
			}
			libusb_submit_transfer(devc->xfer);
			devc->last_live_request = now;
			devc->state = LIVE_SPL_WAIT;
		}
	}

	return TRUE;
}

static void send_data(const struct sr_dev_inst *sdi, void *buf, unsigned int buf_len)
{
	struct dev_context *devc;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_analog analog;

	devc = sdi->priv;

	memset(&analog, 0, sizeof(struct sr_datafeed_analog));
	analog.mq = SR_MQ_SOUND_PRESSURE_LEVEL;
	analog.mqflags = devc->mqflags;
	analog.unit = SR_UNIT_DECIBEL_SPL;
	analog.probes = sdi->probes;
	analog.num_samples = buf_len;
	analog.data = buf;
	packet.type = SR_DF_ANALOG;
	packet.payload = &analog;
	sr_session_send(devc->cb_data, &packet);

}

SR_PRIV void kecheng_kc_330b_receive_transfer(struct libusb_transfer *transfer)
{
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	float fvalue;
	int packet_has_error;

	sdi = transfer->user_data;
	devc = sdi->priv;

	packet_has_error = FALSE;
	switch (transfer->status) {
	case LIBUSB_TRANSFER_NO_DEVICE:
		/* USB device was unplugged. */
		sdi->driver->dev_acquisition_stop((struct sr_dev_inst *)sdi,
				devc->cb_data);
		return;
	case LIBUSB_TRANSFER_COMPLETED:
	case LIBUSB_TRANSFER_TIMED_OUT: /* We may have received some data though */
		break;
	default:
		packet_has_error = TRUE;
		break;
	}

	if (!packet_has_error) {
		if (devc->state == LIVE_SPL_WAIT) {
			if (transfer->actual_length != 3 || transfer->buffer[0] != 0x88) {
				sr_dbg("Received invalid SPL packet.");
			} else {
				fvalue = ((transfer->buffer[1] << 8) + transfer->buffer[2]) / 10.0;
				send_data(sdi, &fvalue, 1);
				devc->num_samples++;
				if (devc->limit_samples && devc->num_samples >= devc->limit_samples)
					sdi->driver->dev_acquisition_stop((struct sr_dev_inst *)sdi,
							devc->cb_data);

				/* let USB event handler fire off another
				 * request when the time is right. */
				devc->state = LIVE_SPL_IDLE;
			}
		}
	}


}

SR_PRIV int kecheng_kc_330b_configure(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;
	int len, ret;
	unsigned char buf[7];

	sr_dbg("Configuring device.");

	usb = sdi->conn;
	devc = sdi->priv;

	buf[0] = CMD_CONFIGURE;
	buf[1] = devc->sample_interval;
	buf[2] = devc->alarm_low;
	buf[3] = devc->alarm_high;
	buf[4] = devc->mqflags & SR_MQFLAG_SPL_TIME_WEIGHT_F ? 0 : 1;
	buf[5] = devc->mqflags & SR_MQFLAG_SPL_FREQ_WEIGHT_A ? 0 : 1;
	buf[6] = devc->data_source;
	ret = libusb_bulk_transfer(usb->devhdl, EP_OUT, buf, 7, &len, 5);
	if (ret != 0 || len != 7) {
		sr_dbg("Failed to configure device: %s", libusb_error_name(ret));
		return SR_ERR;
	}

	/* The configure command ack takes about 32ms to come in. */
	ret = libusb_bulk_transfer(usb->devhdl, EP_IN, buf, 1, &len, 40);
	if (ret != 0 || len != 1) {
		sr_dbg("Failed to configure device (no ack): %s", libusb_error_name(ret));
		return SR_ERR;
	}
	if (buf[0] != (CMD_CONFIGURE | 0x80)) {
		sr_dbg("Failed to configure device: invalid response 0x%2.x", buf[0]);
		return SR_ERR;
	}

	return SR_OK;
}

SR_PRIV int kecheng_kc_330b_set_date_time(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb;
	GDateTime *dt;
	int len, ret;
	unsigned char buf[7];

	sr_dbg("Setting device date/time.");

	usb = sdi->conn;

	dt = g_date_time_new_now_local();
	buf[0] = CMD_SET_DATE_TIME;
	buf[1] = g_date_time_get_year(dt) - 2000;
	buf[2] = g_date_time_get_month(dt);
	buf[3] = g_date_time_get_day_of_month(dt);
	buf[4] = g_date_time_get_hour(dt);
	buf[5] = g_date_time_get_minute(dt);
	buf[6] = g_date_time_get_second(dt);
	g_date_time_unref(dt);
	ret = libusb_bulk_transfer(usb->devhdl, EP_OUT, buf, 7, &len, 5);
	if (ret != 0 || len != 7) {
		sr_dbg("Failed to set date/time: %s", libusb_error_name(ret));
		return SR_ERR;
	}

	ret = libusb_bulk_transfer(usb->devhdl, EP_IN, buf, 1, &len, 10);
	if (ret != 0 || len != 1) {
		sr_dbg("Failed to set date/time (no ack): %s", libusb_error_name(ret));
		return SR_ERR;
	}
	if (buf[0] != (CMD_SET_DATE_TIME | 0x80)) {
		sr_dbg("Failed to set date/time: invalid response 0x%2.x", buf[0]);
		return SR_ERR;
	}



	return SR_OK;
}

SR_PRIV int kecheng_kc_330b_recording_get(const struct sr_dev_inst *sdi,
		gboolean *tmp)
{

	return SR_OK;
}