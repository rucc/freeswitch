#include "google/cloud/speech/v1/cloud_speech.pb.h"
#include "google/cloud/speech/v1/cloud_speech.grpc.pb.h"
#include <windows.h>
#include <grpc++/grpc++.h>
#include <string>
#include <thread>
#include <mutex>
#include "GStreamer.h"

using google::cloud::speech::v1::Speech;
using google::cloud::speech::v1::StreamingRecognizeRequest;
using google::cloud::speech::v1::StreamingRecognizeResponse;
using google::cloud::speech::v1::RecognitionConfig;
using google::cloud::speech::v1::RecognitionConfig_AudioEncoding;

struct RecogAlternative
{
	float confidence;
	std::string transcript;
};
struct RecogResult
{
	float stability;
	std::vector<RecogAlternative> alternatives;
};
struct RecogResults
{
	std::vector<RecogResult> results;
};

class GStreamer
{
public:
	std::mutex m_resmtx;
	std::vector<RecogResults> m_resultholder;

	void Init(unsigned int sample_rate)
	{
		m_ccreds = grpc::GoogleDefaultCredentials();
		m_chan = grpc::CreateChannel("speech.googleapis.com", m_ccreds);
		m_speech = Speech::NewStub(m_chan);
		StreamingRecognizeRequest request;
		auto* streaming_config = request.mutable_streaming_config();
		m_strm = m_speech->StreamingRecognize(&m_ctx);
		streaming_config->set_interim_results(true);
		auto* cfg = streaming_config->mutable_config();
		cfg->set_language_code("hu");
		cfg->set_encoding(RecognitionConfig_AudioEncoding::RecognitionConfig_AudioEncoding_LINEAR16);
		cfg->set_sample_rate_hertz(sample_rate);
		m_strm->Write(request);

		m_rdrThread = std::thread(&GStreamer::ReaderFunc, this);
	}

	void ShutDown()
	{
		m_strm->WritesDone();
		m_rdrThread.join();
	}

	GStreamer()
	{
	}

	virtual ~GStreamer()
	{
	}

	void ReaderFunc()
	{
		while (GetResult()) {}
	}

	bool GetResult()
	{
		StreamingRecognizeResponse response;
		if (m_strm->Read(&response))
		{
			std::lock_guard<std::mutex> grd(m_resmtx);
			RecogResults rrs;
			for (int r = 0; r < response.results_size(); ++r)
			{
				RecogResult rr;
				const auto& result = response.results(r);
				rr.stability = result.stability();
				for (int a = 0; a < result.alternatives_size(); ++a)
				{
					RecogAlternative ra;
					const auto& alternative = result.alternatives(a);
					ra.confidence = alternative.confidence();
					ra.transcript = alternative.transcript();
					rr.alternatives.push_back(ra);
				}
				rrs.results.push_back(rr);
			}
			m_resultholder.push_back(rrs);
			return true;
		}
		else
		{
			auto status = m_strm->Finish();
			return false;
		}
	}

	void StreamFunc(void* datap, int size)
	{
		StreamingRecognizeRequest request;
		request.set_audio_content(datap, size);
		m_strm->Write(request);
	}

private:
	std::shared_ptr<::grpc::Channel> m_chan;
	std::shared_ptr<grpc_impl::ChannelCredentials> m_ccreds;
	grpc::ClientContext m_ctx;
	std::unique_ptr<Speech::Stub> m_speech;
	std::unique_ptr<::grpc::ClientReaderWriter<::google::cloud::speech::v1::StreamingRecognizeRequest, ::google::cloud::speech::v1::StreamingRecognizeResponse>> m_strm;
	std::thread m_rdrThread;
};

void* gspeech_init(unsigned int sample_rate)
{
	GStreamer* ret = new GStreamer;
	ret->Init(sample_rate);
	return ret;
}

GStreamer* Cast(void* gstrmr)
{
	return (GStreamer*)gstrmr;
}

void gspeech_shutdown(void* gstrmr)
{
	auto gs = Cast(gstrmr);
	gs->ShutDown();
	delete gs;
}

void gspeech_stream_func(void* gstrmr, void* datap, int size)
{
	Cast(gstrmr)->StreamFunc(datap, size);
}

int gspeech_get_result_count(void* gstrmr)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return (int)gs->m_resultholder.size();
}

int gspeech_get_subresult_count(void* gstrmr, int resIdx)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return (int)gs->m_resultholder[resIdx].results.size();
}

float gspeech_get_result_stability(void* gstrmr, int resIdx, int subResIdx)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return gs->m_resultholder[resIdx].results[subResIdx].stability;
}

int gspeech_get_alternative_count(void* gstrmr, int resIdx, int subResIdx)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return (int)gs->m_resultholder[resIdx].results[subResIdx].alternatives.size();
}

float gspeech_get_alternative_confidence(void* gstrmr, int resIdx, int subResIdx, int altIdx)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return gs->m_resultholder[resIdx].results[subResIdx].alternatives[altIdx].confidence;
}

const char* gspeech_get_alternative_transcript(void* gstrmr, int resIdx, int subResIdx, int altIdx)
{
	auto gs = Cast(gstrmr);
	std::lock_guard<std::mutex> grd(gs->m_resmtx);
	return gs->m_resultholder[resIdx].results[subResIdx].alternatives[altIdx].transcript.c_str();
}
