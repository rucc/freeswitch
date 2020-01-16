#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	void* gspeech_init(unsigned int sample_rate);

	void gspeech_shutdown(void* gstrmr);

	void gspeech_stream_func(void* gstrmr, void* datap, int size);

	int gspeech_get_result_count(void* gstrmr);

	int gspeech_get_subresult_count(void* gstrm, int resIdxr);

	float gspeech_get_result_stability(void* gstrmr, int resIdx, int subResIdx);

	int gspeech_get_alternative_count(void* gstrmr, int resIdx, int subResIdx);

	float gspeech_get_alternative_confidence(void* gstrmr, int resIdx, int subResIdx, int altIdx);

	const char* gspeech_get_alternative_transcript(void* gstrmr, int resIdx, int subResIdx, int altIdx);

#ifdef __cplusplus
}
#endif

