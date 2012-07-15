
/*!
 * \file 	sccp_line.c
 * \brief 	SCCP Line
 * \author 	Sergio Chersovani <mlists [at] c-net.it>
 * \note		Reworked, but based on chan_sccp code.
 *        	The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *        	Modified by Jan Czmok and Julien Goodwin
 * \note		This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 *
 * $Date$
 * $Revision$
 */

#include <config.h>
#include "common.h"

SCCP_FILE_VERSION(__FILE__, "$Revision$")
#ifdef CS_DYNAMIC_CONFIG
static void regcontext_exten(sccp_line_t * l, struct subscriptionId *subscriptionId, int onoff);
int __sccp_line_destroy(const void *ptr);
int __sccp_lineDevice_destroy(const void *ptr);

/*!
 * \brief run before reload is start on lines
 * \note See \ref sccp_config_reload
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- lines
 */
void sccp_line_pre_reload(void)
{
	sccp_line_t *l;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		/* Don't want to include the hotline line */
		if (GLOB(hotline)->line != l
#    ifdef CS_SCCP_REALTIME
		    && l->realtime == FALSE
#    endif
		    ) {
			l->pendingDelete = 1;
			sccp_log(DEBUGCAT_NEWCODE) (VERBOSE_PREFIX_3 "%s: Setting Line to Pending Delete=1\n", l->name);
		}
		l->pendingUpdate = 0;
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));
}

/*!
 * \brief run after the new line config is loaded during the reload process
 * \note See \ref sccp_config_reload
 * \todo to be implemented correctly (***)
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- lines
 * 	  - line
 * 	     - line->devices
 * 	       - device
 * 	  - see sccp_line_clean()
 */
void sccp_line_post_reload(void)
{
	sccp_line_t *l;
	sccp_linedevices_t *linedevice;

	SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(lines), l, list) {
		if (!l->pendingDelete && !l->pendingUpdate)
			continue;

		if ((l = sccp_line_retain(l))) {
			SCCP_LIST_LOCK(&l->devices);
			SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
				if ((linedevice->device = sccp_device_retain(linedevice->device))) {
					linedevice->device->pendingUpdate = 1;
					linedevice->device = sccp_device_release(linedevice->device);
				}
			}
			SCCP_LIST_UNLOCK(&l->devices);

			if (l->pendingDelete) {
				sccp_line_clean(l, TRUE);
			}
			l = sccp_line_release(l);
		}
	}
	SCCP_RWLIST_TRAVERSE_SAFE_END;
}
#endif										/* CS_DYNAMIC_CONFIG */

/*!
 * \brief Build Default SCCP Line.
 *
 * Creates an SCCP Line with default/global values
 *
 * \return Default SCCP Line
 *
 * \callgraph
 * \callergraph
 */
sccp_line_t *sccp_line_create(const char *name)
{
	sccp_line_t *l = (sccp_line_t *) sccp_refcount_object_alloc(sizeof(sccp_line_t), "line", name, __sccp_line_destroy);

	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "Unable to allocate memory for a line\n");
		return NULL;
	}
	memset(l, 0, sizeof(sccp_line_t));
	pbx_mutex_init(&l->lock);
	sccp_copy_string(l->name, name, sizeof(l->name));
	
#if DEBUG
	sccp_line_retain(l);
#endif
	SCCP_LIST_HEAD_INIT(&l->channels);
	SCCP_LIST_HEAD_INIT(&l->devices);
	SCCP_LIST_HEAD_INIT(&l->mailboxes);
#if DEBUG
	sccp_line_release(l);
#endif
	return l;
}

/*!
 * Add a line to global line list.
 * \param line line pointer
 * \since 20091202 - MC
 * 
 * \note needs to be called with a retained line
 * \note adds a retained line to the list (refcount + 1)
 * \lock
 * 	- lines
 * 	- see sccp_mwi_linecreatedEvent() via sccp_event_fire()
 */
sccp_line_t *sccp_line_addToGlobals(sccp_line_t * line)
{
	sccp_line_t *l = NULL;

	if (!line) {
		pbx_log(LOG_ERROR, "Adding null to global line list is not allowed!\n");
		return NULL;
	}

	SCCP_RWLIST_WRLOCK(&GLOB(lines));
	/* does the line already exist (created by an other thread) ? */
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		if (!strcasecmp(l->name, line->name)) {
			break;
		}
	}

	if (l) {
		pbx_log(LOG_NOTICE, "SCCP: line '%s' was already created by an other thread, cleaning up new line\n", line->name);
//		sccp_free(line);		// this should never be (refcounted)
		line = sccp_line_release(line);	// cleanup newly created line, to prevent doubles
		SCCP_RWLIST_UNLOCK(&GLOB(lines));
		return l;			// return previous instance
	}

	/* line was not created */
	line = sccp_line_retain(line);
	SCCP_RWLIST_INSERT_HEAD(&GLOB(lines), line, list);
	SCCP_RWLIST_UNLOCK(&GLOB(lines));
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "Added line '%s' to Glob(lines)\n", line->name);

	sccp_event_t event;
	memset(&event, 0, sizeof(sccp_event_t));
	event.type = SCCP_EVENT_LINE_CREATED;
	event.event.lineCreated.line = sccp_line_retain(line);
	sccp_event_fire(&event);

	return line;
}

/*!
 * Remove a line from the global line list.
 * \param line SCCP line pointer
 * 
 * \note needs to be called with a retained line
 * \note removes the retained line withing the list (refcount - 1)
 * \lock
 * 	- lines
 * 	- see sccp_mwi_linecreatedEvent() via sccp_event_fire()
 */
sccp_line_t *sccp_line_removeFromGlobals(sccp_line_t * line)
{
	if (!line) {
		pbx_log(LOG_ERROR, "Removing null from global line list is not allowed!\n");
		return NULL;
	}

	SCCP_RWLIST_WRLOCK(&GLOB(lines));
	line = SCCP_RWLIST_REMOVE(&GLOB(lines), line, list);
	SCCP_RWLIST_UNLOCK(&GLOB(lines));
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "Removed line '%s' from Glob(lines)\n", line->name);

	/* not sure if we should fire an event like this ? */
/*	
	sccp_event_t event;
	memset(&event, 0, sizeof(sccp_event_t));
	event.type = SCCP_EVENT_LINE_DELETED;
	event.event.lineCreated.line = sccp_line_retain(line);
	sccp_event_fire(&event);
*/
	sccp_line_release(line);

	return line;
}

/*!
 * \brief Kill all Channels of a specific Line
 * \param l SCCP Line
 * \note Should be Called with a lock on l->lock
 *
 * \callgraph
 * \callergraph
 *
 * \lock
 * 	- line->channels
 * 	  - see sccp_channel_endcall();
 */
void sccp_line_kill(sccp_line_t * l)
{
	sccp_channel_t *c;

	if (!l)
		return;

//	SCCP_LIST_LOCK(&l->channels);
	SCCP_LIST_TRAVERSE_SAFE_BEGIN(&l->channels, c, list) {
		sccp_channel_retain(c);
		sccp_channel_endcall(c);
		sccp_channel_release(c);
	}
	SCCP_LIST_TRAVERSE_SAFE_END;
//	SCCP_LIST_UNLOCK(&l->channels);
}

/*!
 * \brief Clean Line
 *
 *  clean up memory allocated by the line.
 *  if destroy is true, line will be removed from global device list
 *
 * \param l SCCP Line
 * \param remove_from_global as boolean_t
 *
 * \todo integrate sccp_line_clean and sccp_line_delete_nolock into sccp_line_delete
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- lines
 * 	- see sccp_line_kill()
 * 	- line->devices
 * 	- see sccp_line_destroy()
 */
void sccp_line_clean(sccp_line_t * l, boolean_t remove_from_global)
{
	sccp_linedevices_t *linedevice;

	if (!l)
		return;

	if (remove_from_global) {
		l = sccp_line_removeFromGlobals(l);
	}

	sccp_line_kill(l);

//	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE_SAFE_BEGIN(&l->devices, linedevice, list) {
		sccp_line_removeDevice(linedevice->line, linedevice->device);
	}	
	SCCP_LIST_TRAVERSE_SAFE_END;
//	SCCP_LIST_UNLOCK(&l->devices);

	sccp_line_destroy(l);
}

/*!
 * \brief Free a Line as scheduled command
 * \param ptr SCCP Line Pointer
 * \return success as int
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- line
 * 	  - see sccp_mwi_unsubscribeMailbox()
 */
int __sccp_line_destroy(const void *ptr)
{
	sccp_line_t *l = (sccp_line_t *) ptr;

	sccp_log((DEBUGCAT_NEWCODE | DEBUGCAT_CONFIG)) (VERBOSE_PREFIX_1 "%s: Line FREE\n", l->name);

	sccp_mutex_lock(&l->lock);

	// cleanup linedevices
	sccp_linedevices_t *linedevice = NULL;
	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE_SAFE_BEGIN(&l->devices, linedevice, list) {
		sccp_line_removeDevice(linedevice->line, linedevice->device);
	}	
	SCCP_LIST_TRAVERSE_SAFE_END;
	SCCP_LIST_UNLOCK(&l->devices);
	if (SCCP_LIST_EMPTY(&l->devices))
		SCCP_LIST_HEAD_DESTROY(&l->devices);

	// cleanup mailboxes
	if (l->trnsfvm)
		sccp_free(l->trnsfvm);

	sccp_mailbox_t *mailbox = NULL;
	SCCP_LIST_LOCK(&l->mailboxes);
	while ((mailbox = SCCP_LIST_REMOVE_HEAD(&l->mailboxes, list))) {
		if (!mailbox)
			break;

		sccp_mwi_unsubscribeMailbox(&mailbox);
		if (mailbox->mailbox)
			sccp_free(mailbox->mailbox);
		if (mailbox->context)
			sccp_free(mailbox->context);
		sccp_free(mailbox);
	}
	SCCP_LIST_UNLOCK(&l->mailboxes);
	if (SCCP_LIST_EMPTY(&l->mailboxes))
		SCCP_LIST_HEAD_DESTROY(&l->mailboxes);

	// cleanup channels
	sccp_channel_t *c = NULL;
	SCCP_LIST_LOCK(&l->channels);
	while ((c = SCCP_LIST_REMOVE_HEAD(&l->channels, list))) {
		sccp_channel_endcall(c);
		sccp_channel_release(c);		// release channel retain in list
	}
	SCCP_LIST_UNLOCK(&l->channels);
	if (SCCP_LIST_EMPTY(&l->channels))
		SCCP_LIST_HEAD_DESTROY(&l->channels);

	if (l->variables) {
		pbx_variables_destroy(l->variables);
		l->variables = NULL;
	}

	sccp_mutex_unlock(&l->lock);
	pbx_mutex_destroy(&l->lock);
	return 0;
}

/*!
 * \brief Free a Line as scheduled command
 * \param ptr SCCP Line Pointer
 * \return success as int
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- line
 * 	  - see sccp_mwi_unsubscribeMailbox()
 */
int __sccp_lineDevice_destroy(const void *ptr)
{
	sccp_linedevices_t *linedevice = (sccp_linedevices_t *) ptr;

	sccp_log((DEBUGCAT_NEWCODE | DEBUGCAT_CONFIG)) (VERBOSE_PREFIX_1 "LineDevice FREE %p\n", linedevice);
	if (linedevice->line)
		linedevice->line = sccp_line_release(linedevice->line);
	if (linedevice->device)
		linedevice->device = sccp_device_release(linedevice->device);
	return 0;
}

/*!
 * \brief Free a Line as scheduled command
 * \param ptr SCCP Line Pointer
 * \return success as int
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- line
 * 	  - see sccp_mwi_unsubscribeMailbox()
 */
int sccp_line_destroy(const void *ptr)
{
	sccp_line_t *l = (sccp_line_t *) ptr;
	sccp_line_removeFromGlobals(l);
	sccp_line_release(l);
	return 0;
}

/*!
 * \brief Delete an SCCP line
 * \param l SCCP Line
 * \note Should be Called without a lock on l->lock
 */
void sccp_line_delete_nolock(sccp_line_t * l)
{
	sccp_line_clean(l, TRUE);
}

/*!
 * \brief Set a Call Forward on a specific Line
 * \param l SCCP Line
 * \param device device that requested the forward
 * \param type Call Forward Type as uint8_t
 * \param number Number to which should be forwarded
 * \todo we should check, that extension is reachable on line
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- line->devices
 * 	- see sccp_feat_changed()
 *
 * \todo implement cfwd_noanswer
 */
void sccp_line_cfwd(sccp_line_t * l, sccp_device_t * device, uint8_t type, char *number)
{
	sccp_linedevices_t *linedevice;

	if (!l || !device)
		return;

//	SCCP_LIST_LOCK(&l->devices);
//	SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
//		if (linedevice->device == device)
//			break;
//	}
//	SCCP_LIST_UNLOCK(&l->devices);
//
//	if (!linedevice) {
//		pbx_log(LOG_ERROR, "%s: Device does not have line configured \n", DEV_ID_LOG(device));
//		return;
//	}
	if (!(linedevice = sccp_linedevice_find(device, l))) {
		pbx_log(LOG_ERROR, "%s: Device does not have line configured (linedevice not found)\n", DEV_ID_LOG(device));
		return;
	}

	if (type == SCCP_CFWD_NONE) {
		linedevice->cfwdAll.enabled = 0;
		linedevice->cfwdBusy.enabled = 0;
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Call Forward disabled on line %s\n", DEV_ID_LOG(device), l->name);
	} else {
		if (!number || sccp_strlen_zero(number)) {
			linedevice->cfwdAll.enabled = 0;
			linedevice->cfwdBusy.enabled = 0;
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Call Forward to an empty number. Invalid\n", DEV_ID_LOG(device));
		} else {
			switch (type) {
			case SCCP_CFWD_ALL:
				linedevice->cfwdAll.enabled = 1;
				sccp_copy_string(linedevice->cfwdAll.number, number, sizeof(linedevice->cfwdAll.number));
				break;
			case SCCP_CFWD_BUSY:
				linedevice->cfwdBusy.enabled = 1;
				sccp_copy_string(linedevice->cfwdBusy.number, number, sizeof(linedevice->cfwdBusy.number));
				break;
			default:
				linedevice->cfwdAll.enabled = 0;
				linedevice->cfwdBusy.enabled = 0;
			}
			sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_3 "%s: Call Forward enabled on line %s to number %s\n", DEV_ID_LOG(device), l->name, number);
		}
	}
	if (linedevice && linedevice->device) {
		sccp_dev_starttone(linedevice->device, SKINNY_TONE_ZIPZIP, 0, 0, 0);
		switch (type) {
		case SCCP_CFWD_ALL:
			sccp_feat_changed(linedevice->device, SCCP_FEATURE_CFWDALL);
			break;
		case SCCP_CFWD_BUSY:
			sccp_feat_changed(linedevice->device, SCCP_FEATURE_CFWDBUSY);
			break;
		case SCCP_CFWD_NONE:
			sccp_feat_changed(linedevice->device, SCCP_FEATURE_CFWDNONE);
			break;
		default:
			sccp_feat_changed(linedevice->device, SCCP_FEATURE_CFWDNONE);
			break;
		}
		sccp_dev_forward_status(l, linedevice->lineInstance, device);
	}
	linedevice = sccp_linedevice_release(linedevice);
}

/*!
 * \brief Attach a Device to a line
 * \param l SCCP Line
 * \param device SCCP Device
 * \param lineInstance lineInstance as uint8_t
 * \param subscriptionId Subscription ID for addressing individual devices on the line
 * 
 * \lock
 * 	- line->devices
 * 	  - see register_exten()
 * 	- line
 * 	- see sccp_feat_changed()
 * 	- see sccp_dev_forward_status() via sccp_event_fire()
 * 	- see sccp_mwi_deviceAttachedEvent() via sccp_event_fire
 */
void sccp_line_addDevice(sccp_line_t * l, sccp_device_t * device, uint8_t lineInstance, struct subscriptionId *subscriptionId)
{
	sccp_linedevices_t *linedevice = NULL;

	if (!device || !l) {
		pbx_log(LOG_ERROR, "SCCP: sccp_line_addDevice: No line or device provided\n");
		return;
	}
		
	if ((linedevice = sccp_linedevice_find(device, l))) {
		sccp_log(DEBUGCAT_LINE) (VERBOSE_PREFIX_3 "%s: device already registered for line '%s'\n", DEV_ID_LOG(device), l->name);
		sccp_linedevice_release(linedevice);
		// early exit
		return;
	}

	if (!(device = sccp_device_retain(device))) {
		pbx_log(LOG_ERROR, "SCCP: sccp_line_addDevice: Device could not be retained for line : %s\n", l ? l->name : "UNDEF");
		return;
	}

	if (!(l = sccp_line_retain(l))) {
		pbx_log(LOG_ERROR, "%s: sccp_line_addDevice: Line could not be retained\n", DEV_ID_LOG(device));
		device = sccp_device_release(device);		
		return;
	}
	sccp_log(DEBUGCAT_LINE) (VERBOSE_PREFIX_3 "%s: add device to line %s\n", DEV_ID_LOG(device), l->name);

	linedevice = (sccp_linedevices_t *) sccp_refcount_object_alloc(sizeof(sccp_linedevices_t), "linedevice", "", __sccp_lineDevice_destroy);
	memset(linedevice, 0, sizeof(sccp_linedevices_t));

	linedevice->device = sccp_device_retain(device);
	linedevice->line = sccp_line_retain(l);
	linedevice->lineInstance = lineInstance;

	if (NULL != subscriptionId) {
		sccp_copy_string(linedevice->subscriptionId.name, subscriptionId->name, sizeof(linedevice->subscriptionId.name));
		sccp_copy_string(linedevice->subscriptionId.number, subscriptionId->number, sizeof(linedevice->subscriptionId.number));
		sccp_copy_string(linedevice->subscriptionId.aux, subscriptionId->aux, sizeof(linedevice->subscriptionId.aux));
	}

	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_INSERT_HEAD(&l->devices, linedevice, list);
	SCCP_LIST_UNLOCK(&l->devices);

	linedevice->line->statistic.numberOfActiveDevices++;
	linedevice->device->configurationStatistic.numberOfLines++;

	/* read cfwd status from db */
#ifndef ASTDB_FAMILY_KEY_LEN
#    define ASTDB_FAMILY_KEY_LEN 100
#endif
#ifndef ASTDB_RESULT_LEN
#    define ASTDB_RESULT_LEN 80
#endif
	char family[ASTDB_FAMILY_KEY_LEN];
	char buffer[ASTDB_RESULT_LEN];

	memset(family, 0, ASTDB_FAMILY_KEY_LEN);
	sprintf(family, "SCCP/%s/%s", device->id, l->name);
	if (PBX(feature_getFromDatabase) (family, "cfwdAll", buffer, sizeof(buffer)) && strcmp(buffer, "")) {
		linedevice->cfwdAll.enabled = TRUE;
		sccp_copy_string(linedevice->cfwdAll.number, buffer, sizeof(linedevice->cfwdAll.number));
		sccp_feat_changed(device, SCCP_FEATURE_CFWDALL);
	}

	if (PBX(feature_getFromDatabase) (family, "cfwdBusy", buffer, sizeof(buffer)) && strcmp(buffer, "")) {
		linedevice->cfwdBusy.enabled = TRUE;
		sccp_copy_string(linedevice->cfwdBusy.number, buffer, sizeof(linedevice->cfwdAll.number));
		sccp_feat_changed(device, SCCP_FEATURE_CFWDBUSY);
	}

	if (linedevice->cfwdAll.enabled || linedevice->cfwdBusy.enabled) {
		sccp_dev_forward_status(l, lineInstance, device);
	}
	// fire event for new device
	sccp_event_t event;

	memset(&event, 0, sizeof(sccp_event_t));
	event.type = SCCP_EVENT_DEVICE_ATTACHED;
	event.event.deviceAttached.linedevice = sccp_linedevice_retain(linedevice);
	sccp_event_fire(&event);

#ifdef CS_DYNAMIC_CONFIG
	regcontext_exten(l, &(linedevice->subscriptionId), 1);
#endif
	sccp_log(DEBUGCAT_LINE) (VERBOSE_PREFIX_3 "%s: added linedevice: %p with device: %s\n", l->name, linedevice, DEV_ID_LOG(device));
	l = sccp_line_release(l);
	device = sccp_device_release(device);
}

/*!
 * \brief Remove a Device from a Line
 *
 * Fire SCCP_EVENT_DEVICE_DETACHED event after removing device.
 *
 * \param l SCCP Line
 * \param device SCCP Device
 * 
 * \lock
 * 	- line
 * 	  - line->devices
 * 	    - see unregister_exten()
 * 	- see sccp_hint_eventListener() via sccp_event_fire()
 */
void sccp_line_removeDevice(sccp_line_t * l, sccp_device_t * device)
{
	sccp_linedevices_t *linedevice;

	if (!l || !device)
		return;

	sccp_log((DEBUGCAT_HIGH + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: remove device from line %s\n", DEV_ID_LOG(device), l->name);

	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE_SAFE_BEGIN(&l->devices, linedevice, list) {
		if (linedevice->device == device) {
#ifdef CS_DYNAMIC_CONFIG
			regcontext_exten(l, &(linedevice->subscriptionId), 0);
#endif
			SCCP_LIST_REMOVE_CURRENT(list);
			l->statistic.numberOfActiveDevices--;

			sccp_event_t event;

			memset(&event, 0, sizeof(sccp_event_t));

			event.type = SCCP_EVENT_DEVICE_DETACHED;
			event.event.deviceAttached.linedevice = sccp_linedevice_retain(linedevice);
			sccp_event_fire(&event);
			sccp_linedevice_release(linedevice);
		}
	}
	SCCP_LIST_TRAVERSE_SAFE_END;
	SCCP_LIST_UNLOCK(&l->devices);

	sccp_hint_lineStatusChanged(l, device, NULL, SCCP_CHANNELSTATE_CONGESTION, SCCP_CHANNELSTATE_CONGESTION);
}

/*!
 * \brief Add a Channel to a Line
 *
 * \param l SCCP Line
 * \param channel SCCP Channel
 * 
 * \warning
 * 	- line->channels is not always locked
 * 
 * \lock
 * 	- line
 */
void sccp_line_addChannel(sccp_line_t * l, sccp_channel_t * channel)
{
	if (!l || !channel)
		return;

	if ((l = sccp_line_retain(l))) {
		l->statistic.numberOfActiveChannels++;
		SCCP_LIST_LOCK(&l->channels);
		if ((channel = sccp_channel_retain(channel))) {				// Add into list retained
			sccp_channel_updateChannelDesignator(channel);
			sccp_log((DEBUGCAT_LINE)) (VERBOSE_PREFIX_1 "SCCP: Adding channel %d to line %s", channel->callid, l->name);
			if (GLOB(callanswerorder) == ANSWER_OLDEST_FIRST)
				SCCP_LIST_INSERT_TAIL(&l->channels, channel, list);
			else
				SCCP_LIST_INSERT_HEAD(&l->channels, channel, list);
		}		
		SCCP_LIST_UNLOCK(&l->channels);
		l = sccp_line_release(l);
	}
}

/*!
 * \brief Remove a Channel from a Line
 *
 * \param l SCCP Line
 * \param channel SCCP Channel
 * 
 * \warning
 * 	- line->channels is not always locked
 * 
 * \lock
 * 	- line
 */
void sccp_line_removeChannel(sccp_line_t * l, sccp_channel_t * c)
{
	sccp_channel_t *channel;
	if (!l || !c)
		return;

	if ((l = sccp_line_retain(l))) {
		SCCP_LIST_LOCK(&l->channels);
		if ((channel = SCCP_LIST_REMOVE(&l->channels, c, list))) {
			sccp_log((DEBUGCAT_LINE)) (VERBOSE_PREFIX_1 "SCCP: Removing channel %d from line %s", channel->callid, l->name);
			l->statistic.numberOfActiveChannels--;
			channel = sccp_channel_release(channel);			// Remove retain from list
		}			
		SCCP_LIST_UNLOCK(&l->channels);
		l = sccp_line_release(l);
	}
}

/*!
 * \brief Register Extension to Asterisk regextension
 * \param l SCCP Line
 * \param subscriptionId subscriptionId
 * \param onoff On/Off as int
 * \note used for DUNDi Discovery \ref DUNDi
 */
static void regcontext_exten(sccp_line_t * l, struct subscriptionId *subscriptionId, int onoff)
{
	char multi[256] = "";
	char *stringp, *ext = "", *context = "";

//      char extension[AST_MAX_CONTEXT]="";
//      char name[AST_MAX_CONTEXT]="";

	struct pbx_context *con;
	struct pbx_find_info q = {.stacklen = 0 };

	if (sccp_strlen_zero(GLOB(regcontext)))
		return;

	sccp_copy_string(multi, S_OR(l->regexten, l->name), sizeof(multi));
	stringp = multi;
	while ((ext = strsep(&stringp, "&"))) {
		if ((context = strchr(ext, '@'))) {
			*context++ = '\0';					/* split ext@context */
			if (!pbx_context_find(context)) {
				pbx_log(LOG_WARNING, "Context specified in regcontext=%s (sccp.conf) must exist\n", context);
				continue;
			}
		} else {
			context = GLOB(regcontext);
		}
		con = pbx_context_find_or_create(NULL, NULL, context, "SCCP");	/* make sure the context exists */
		if (con) {
			if (onoff) {
				/* register */

				if (!pbx_exists_extension(NULL, context, ext, 1, NULL) && pbx_add_extension(context, 0, ext, 1, NULL, NULL, "Noop", sccp_strdup(l->name), sccp_free_ptr, "SCCP")) {
					sccp_log((DEBUGCAT_LINE | DEBUGCAT_NEWCODE)) (VERBOSE_PREFIX_1 "Registered RegContext: %s, Extension: %s, Line: %s\n", context, ext, l->name);
				}

/*				if (subscriptionId && subscriptionId->number && !sccp_strlen_zero(subscriptionId->number) && !sccp_strlen_zero(subscriptionId->name)) {
					snprintf(extension, sizeof(extension), "%s@%s", ext, subscriptionId->number);
					snprintf(name, sizeof(name), "%s%s", l->name, subscriptionId->name);
					if (!pbx_exists_extension(NULL, context, extension, 2, NULL) && pbx_add_extension(context, 0, extension, 2, NULL, NULL, "Noop", sccp_strdup(name), sccp_free_ptr, "SCCP")) {
						sccp_log((DEBUGCAT_LINE | DEBUGCAT_NEWCODE)) (VERBOSE_PREFIX_1 "Registered RegContext: %s, Extension: %s, Line: %s\n", context, extension, name);
					}
				}*/
			} else {
				/* un-register */

				if (l->devices.size == 1) {			// only remove entry if it is the last one (shared line)
					if (pbx_find_extension(NULL, NULL, &q, context, ext, 1, NULL, "", E_MATCH)) {
						ast_context_remove_extension(context, ext, 1, NULL);
						sccp_log((DEBUGCAT_LINE | DEBUGCAT_NEWCODE)) (VERBOSE_PREFIX_1 "Unregistered RegContext: %s, Extension: %s\n", context, ext);
					}
				}

/*				if (subscriptionId && subscriptionId->number && !sccp_strlen_zero(subscriptionId->number) && !sccp_strlen_zero(subscriptionId->name)) {
					snprintf(extension, sizeof(extension), "%s@%s", ext, subscriptionId->number);
//					if (pbx_exists_extension(NULL, context, extension, 2, NULL)) {
					if (pbx_find_extension(NULL, NULL, &q, context, extension, 2, NULL, "", E_MATCH)) {
						ast_context_remove_extension(context, extension, 2, NULL);
						sccp_log((DEBUGCAT_LINE | DEBUGCAT_NEWCODE)) (VERBOSE_PREFIX_1 "Unregistered RegContext: %s, Extension: %s\n", context, extension);
					}
				}*/
			}
		} else {
			pbx_log(LOG_ERROR, "SCCP: context '%s' does not exist and could not be created\n", context);
		}
	}
}
