#include "GStreamer.h"
#include <stdio.h>
#include <switch.h>
#include <windows.h>

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_gspeech_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_gspeech_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_gspeech_load);
SWITCH_MODULE_DEFINITION(mod_gspeech, mod_gspeech_load, mod_gspeech_shutdown, NULL);

static struct {
	char* ssl_pem;
	char* credentials_json;
	int rec;
	char* recdir;
} globals;

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM("ssl-root-path", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.ssl_pem, "D:\\dev\\grpc\\etc\\roots.pem",
					   NULL, NULL, "Specifies the root ssl cert file to use for grpc"),
	SWITCH_CONFIG_ITEM("credentials-json-path", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.credentials_json, "D:\\dev\\grpc\\tmp\\Gsp2txt-04bb38aba460.json",
					   NULL, NULL, "Specifies the google speech api credentials file"),
	SWITCH_CONFIG_ITEM("stream-rec-dir", SWITCH_CONFIG_STRING, CONFIG_RELOADABLE, &globals.recdir, "",
					   NULL, NULL, "strem rec dir, no rec if empty"),
	SWITCH_CONFIG_ITEM_END()};

static switch_status_t do_config(switch_bool_t reload)
{
	memset(&globals, 0, sizeof(globals));
	if (switch_xml_config_parse_module_settings("gspeech.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open gspeech.conf\n");
		return SWITCH_STATUS_FALSE;
	}
	globals.rec = globals.recdir && strcasecmp("", globals.recdir);
	return SWITCH_STATUS_SUCCESS;
}

struct gspeech_bug_helper {
	uint32_t packet_len;
	int rready;
	int wready;
	switch_time_t last_read_time;
	switch_time_t last_write_time;
	switch_bool_t hangup_on_error;
	switch_codec_implementation_t read_impl;
	switch_bool_t speech_detected;
	switch_buffer_t *thread_buffer;
	switch_thread_t *thread;
	switch_mutex_t *buffer_mutex;
	int thread_ready;
	uint32_t writes;
	uint32_t vwrites;
	const char *completion_cause;
	int start_event_sent;
	void* gstrmr;
	int last_res_cnt;
	FILE* fp;
};

static void logfunc(char* msg)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, msg);
}

static switch_bool_t gspeech_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct gspeech_bug_helper *rh = (struct gspeech_bug_helper *)user_data;
	switch_event_t *event;
	switch_size_t len = 0;
	int mask = switch_core_media_bug_test_flag(bug, SMBF_MASK);
	unsigned char null_data[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};

	switch (type) {
	case SWITCH_ABC_TYPE_INIT: {
		if (rh->start_event_sent == 0) {
			switch_codec_implementation_t read_impl = { 0 };
			switch_core_session_get_read_impl(session, &read_impl);
			unsigned int rate = read_impl.actual_samples_per_second;

			rh->start_event_sent = 1;
			if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
				switch_channel_event_set_data(channel, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "GSpeech_Start");
				switch_event_fire(&event);
			}
			if (globals.rec)
			{
				char path[500];
				sprintf(path, "%s\\%s_gs_strm_L16_%d.raw", globals.recdir, switch_channel_get_uuid(channel), rate);
				rh->fp = fopen(path, "ab");
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "opened %s\n", path);
			}
			else
			{
				rh->fp = NULL;
			}

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "gspeech_init with sampling rate %d\n", rate);
			rh->gstrmr = gspeech_init(rate);
			gspeech_register_logfunc(&logfunc, rh->gstrmr);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "gspeech_init done\n");
			rh->last_res_cnt = 0;
		}
	} break;
	case SWITCH_ABC_TYPE_TAP_NATIVE_READ: {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "gspeech mod SWITCH_ABC_TYPE_TAP_NATIVE_READ ");
	} break;
	case SWITCH_ABC_TYPE_TAP_NATIVE_WRITE: {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "gspeech mod SWITCH_ABC_TYPE_TAP_NATIVE_WRITE ");
	} break;
	case SWITCH_ABC_TYPE_CLOSE: {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "gspeech_shutdown\n");
		gspeech_shutdown_async(rh->gstrmr);
		if (globals.rec && rh->fp)
		{
			fclose(rh->fp);
			rh->fp = NULL;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "closed stream rec file\n");
		}
	} break;
	case SWITCH_ABC_TYPE_READ_PING: {
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = {0};
		switch_status_t status;
		int i = 0;
		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		for (;;) {
			status = switch_core_media_bug_read(bug, &frame, i++ == 0 ? SWITCH_FALSE : SWITCH_TRUE);
			if (status != SWITCH_STATUS_SUCCESS || !frame.datalen) {
				break;
			} else {
				gspeech_stream_func(rh->gstrmr, data, frame.datalen);
				fwrite(data, sizeof(uint8_t), frame.datalen, rh->fp);
				rh->writes++;
			}
		}

		while (rh->last_res_cnt < gspeech_get_result_count(rh->gstrmr))
		{
			rh->last_res_cnt++;
			int resIdx = rh->last_res_cnt - 1;
			int subResCnt = gspeech_get_subresult_count(rh->gstrmr, resIdx);
			for (int subResIdx = 0; subResIdx < subResCnt; subResIdx++)
			{
				int altCnt = gspeech_get_alternative_count(rh->gstrmr, resIdx, subResIdx);
				for (int altIdx = 0; altIdx < altCnt; altIdx++)
				{
					float stab = gspeech_get_result_stability(rh->gstrmr, resIdx, subResIdx);
					float conf = gspeech_get_alternative_confidence(rh->gstrmr, resIdx, subResIdx, altIdx);
					char* term = gspeech_get_alternative_transcript(rh->gstrmr, resIdx, subResIdx, altIdx);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "gspeech alternative received stability: %f confidence: %f text: %s ", stab, conf, term);

					if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
						switch_channel_event_set_data(channel, event);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "GSpeech_Result");
						switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Gspeech-Stability", "%f", stab);
						switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Gspeech-Confidence", "%f", conf);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Gspeech-Term", term);
						switch_event_fire(&event);
					}
				}
			}
		}

	} break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING:
	case SWITCH_ABC_TYPE_STREAM_VIDEO_PING:
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

#define GSPEECH_SYNTAX "<uuid> start|stop"
SWITCH_STANDARD_API(session_gspeech_stream_function)
{
	char* mycmd = NULL, * argv[2] = { 0 };
	char* uuid = NULL, * action = NULL;
	int argc = 0;
	if (zstr(cmd)) {
		goto usage;
	}
	if (!(mycmd = strdup(cmd))) {
		goto usage;
	}
	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 2) {
		goto usage;
	}

	uuid = argv[0];
	action = argv[1];

	switch_core_session_t *rsession = NULL;

	if (!(rsession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto done;
	}
	switch_channel_t* channel = switch_core_session_get_channel(rsession);

	switch_media_bug_t* bug;
	bug = switch_channel_get_private(channel, "gspeech_bug");

	if (!strcasecmp(action, "start"))
	{
		switch_status_t status;
		time_t to = 0;
		switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_READ_PING;
		uint8_t channels;
		switch_codec_implementation_t read_impl = { 0 };
		struct gspeech_bug_helper* rh = NULL;
		if ((status = switch_channel_pre_answer(channel)) != SWITCH_STATUS_SUCCESS) { return SWITCH_STATUS_FALSE; }

		if (!switch_channel_media_up(channel) || !switch_core_session_get_read_codec(rsession)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_ERROR,
				"Can not record session.  Media not enabled on channel\n");
			goto done;
		}
		switch_core_session_get_read_impl(rsession, &read_impl);
		channels = read_impl.number_of_channels;

		if (bug != NULL) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_WARNING, "Gspeech session already started\n");
			goto done;
		}

		rh = switch_core_session_alloc(rsession, sizeof(*rh));
		if ((status = switch_core_media_bug_add(rsession, "session_record", "gspeech_bug", gspeech_callback, rh, to,
			flags, &bug)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_ERROR,
				"Error adding media bug for gspeech");
			goto done;
		}
		switch_channel_set_private(channel, "gspeech_bug", bug);

		stream->write_function(stream, "+OK Success\n");
		goto done;
	}
	else if (!strcasecmp(action, "stop")) {
		if (bug == NULL)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_WARNING, "Gspeech session does not exist\n");
			goto done;
		}
		return switch_core_media_bug_remove(rsession, &bug);
	}
	else {
		goto usage;
	}
usage:
	stream->write_function(stream, "-USAGE: %s\n", GSPEECH_SYNTAX);

done:
	if (rsession) { switch_core_session_rwunlock(rsession); }

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_gspeech_load)
{
	switch_api_interface_t *api_interface;
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Mod gspeech load\n");
	if (do_config(SWITCH_FALSE) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Mod gspeech conf error\n");
		return SWITCH_STATUS_FALSE;
	}

	SetEnvironmentVariableA("GOOGLE_APPLICATION_CREDENTIALS", globals.credentials_json);
	SetEnvironmentVariableA("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", globals.ssl_pem);

	SWITCH_ADD_API(api_interface, "uuid_gspeech", "Sppech to text using Google cloud speech API",
				   session_gspeech_stream_function, GSPEECH_SYNTAX); 
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_gspeech_shutdown)
{
	switch_xml_config_cleanup(instructions);
	return SWITCH_STATUS_SUCCESS;
}
