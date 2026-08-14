#include "../PsyX_audio_internal.h"
#include "../PsyX_audio_sink.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace psyx::audio
{

template <typename T>
class ComHolder
{
public:
	~ComHolder() { reset(); }
	T* get() const { return value_; }
	T** put()
	{
		reset();
		return &value_;
	}
	T* operator->() const { return value_; }
	explicit operator bool() const { return value_ != nullptr; }
	void reset(T* value = nullptr)
	{
		if (value_)
			value_->Release();
		value_ = value;
	}

private:
	T* value_ = nullptr;
};

struct ComApartment
{
	ComApartment()
	{
		result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		uninitialize = result == S_OK || result == S_FALSE;
	}
	~ComApartment()
	{
		if (uninitialize)
			CoUninitialize();
	}
	HRESULT result = E_FAIL;
	bool uninitialize = false;
};

struct WasapiFormat
{
	WAVEFORMATEXTENSIBLE wave{};
	PsyXAudioSampleFormat publicFormat = PSYX_AUDIO_FORMAT_UNKNOWN;
	PackedFormat packedFormat = PackedFormat::S16;
	uint16_t containerBits = 0;
	uint16_t validBits = 0;
	bool plainPcm = false;
};

static std::string wideToUtf8(const wchar_t* source)
{
	if (!source || !source[0])
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1)
		return {};
	std::string output(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, source, -1, output.data(), size, nullptr, nullptr);
	output.resize(static_cast<size_t>(size - 1));
	return output;
}

static std::wstring utf8ToWide(const char* source)
{
	if (!source || !source[0])
		return {};
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1, nullptr, 0);
	if (size <= 1)
		return {};
	std::wstring output(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1, output.data(), size);
	output.resize(static_cast<size_t>(size - 1));
	return output;
}

static PsyXAudioResult resultFromHresult(HRESULT result)
{
	if (result == AUDCLNT_E_DEVICE_IN_USE)
		return PSYX_AUDIO_ERROR_DEVICE_BUSY;
	if (result == AUDCLNT_E_UNSUPPORTED_FORMAT)
		return PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT;
	if (result == AUDCLNT_E_DEVICE_INVALIDATED)
		return PSYX_AUDIO_ERROR_DEVICE_INVALIDATED;
#ifdef AUDCLNT_E_RESOURCES_INVALIDATED
	if (result == AUDCLNT_E_RESOURCES_INVALIDATED)
		return PSYX_AUDIO_ERROR_DEVICE_INVALIDATED;
#endif
	return PSYX_AUDIO_ERROR_BACKEND;
}

static WasapiFormat makePcmFormat(
	uint32_t rate,
	uint16_t containerBits,
	uint16_t validBits,
	PsyXAudioSampleFormat publicFormat,
	PackedFormat packedFormat,
	bool plainPcm = false)
{
	WasapiFormat format{};
	format.publicFormat = publicFormat;
	format.packedFormat = packedFormat;
	format.containerBits = containerBits;
	format.validBits = validBits;
	format.plainPcm = plainPcm;
	WAVEFORMATEX& base = format.wave.Format;
	base.wFormatTag = plainPcm ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
	base.nChannels = 2;
	base.nSamplesPerSec = rate;
	base.wBitsPerSample = containerBits;
	base.nBlockAlign = static_cast<WORD>(2u * containerBits / 8u);
	base.nAvgBytesPerSec = base.nSamplesPerSec * base.nBlockAlign;
	base.cbSize = plainPcm ? 0 : sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	format.wave.Samples.wValidBitsPerSample = validBits;
	format.wave.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	format.wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	return format;
}

static std::vector<WasapiFormat> exclusiveCandidates(const PsyXAudioConfig& config)
{
	std::vector<WasapiFormat> formats;
	const uint32_t rates[] = {44100, 88200, 176400, 352800};
	for (uint32_t rate : rates)
	{
		if ((config.allowed_rate_mask & rateMaskFor(rate)) == 0)
			continue;
		if (rate != 44100 &&
			(config.flags & PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION) == 0)
			continue;
		const bool highPrecision =
			(config.flags & PSYX_AUDIO_FLAG_ALLOW_SHARED_FLOAT) != 0;

		if (highPrecision)
		{
			if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_F32) != 0)
			{
				WasapiFormat format = makePcmFormat(
					rate, 32, 32, PSYX_AUDIO_FORMAT_F32, PackedFormat::F32);
				format.wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
				formats.push_back(format);
			}
			if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S32) != 0)
				formats.push_back(makePcmFormat(
					rate, 32, 32, PSYX_AUDIO_FORMAT_S32, PackedFormat::S32));
			if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24_IN_32) != 0)
				formats.push_back(makePcmFormat(
					rate, 32, 24, PSYX_AUDIO_FORMAT_S24_IN_32, PackedFormat::S24In32));
			if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24) != 0)
				formats.push_back(makePcmFormat(
					rate, 24, 24, PSYX_AUDIO_FORMAT_S24, PackedFormat::S24));
			if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S16) != 0)
				formats.push_back(makePcmFormat(
					rate, 16, 16, PSYX_AUDIO_FORMAT_S16, PackedFormat::S16));
			continue;
		}

		if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S16) != 0)
		{
			formats.push_back(makePcmFormat(
				rate, 16, 16, PSYX_AUDIO_FORMAT_S16, PackedFormat::S16));
			formats.push_back(makePcmFormat(
				rate, 16, 16, PSYX_AUDIO_FORMAT_S16, PackedFormat::S16, true));
		}
		if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24) != 0)
			formats.push_back(makePcmFormat(
				rate, 24, 24, PSYX_AUDIO_FORMAT_S24, PackedFormat::S24));
		if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24_IN_32) != 0)
			formats.push_back(makePcmFormat(
				rate, 32, 24, PSYX_AUDIO_FORMAT_S24_IN_32, PackedFormat::S24In32));
		if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S32) != 0)
			formats.push_back(makePcmFormat(
				rate, 32, 32, PSYX_AUDIO_FORMAT_S32, PackedFormat::S32));
		if ((config.allowed_container_mask & PSYX_AUDIO_CONTAINER_F32) != 0)
		{
			WasapiFormat format = makePcmFormat(
				rate, 32, 32, PSYX_AUDIO_FORMAT_F32, PackedFormat::F32);
			format.wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
			formats.push_back(format);
		}
	}
	return formats;
}

static bool classifySharedFormat(
	const WAVEFORMATEX* input,
	const PsyXAudioConfig& config,
	WasapiFormat& output)
{
	if (!input || input->nChannels != 2 ||
		(config.allowed_rate_mask & rateMaskFor(input->nSamplesPerSec)) == 0)
		return false;
	if (input->nSamplesPerSec != 44100 &&
		(config.flags & PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION) == 0)
		return false;

	GUID subtype{};
	uint16_t validBits = input->wBitsPerSample;
	if (input->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		input->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
	{
		const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(input);
		subtype = extended->SubFormat;
		validBits = extended->Samples.wValidBitsPerSample;
	}
	else if (input->wFormatTag == WAVE_FORMAT_PCM)
	{
		subtype = KSDATAFORMAT_SUBTYPE_PCM;
	}
	else if (input->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
	{
		subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
	}
	else
	{
		return false;
	}

	output.wave = {};
	const size_t formatBytes = std::min<size_t>(
		sizeof(output.wave),
		sizeof(WAVEFORMATEX) + input->cbSize);
	std::memcpy(&output.wave, input, formatBytes);
	output.containerBits = input->wBitsPerSample;
	output.validBits = validBits;

	if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
		input->wBitsPerSample == 32 &&
		input->nBlockAlign == 8 &&
		(config.flags & PSYX_AUDIO_FLAG_ALLOW_SHARED_FLOAT) != 0)
	{
		output.publicFormat = PSYX_AUDIO_FORMAT_F32;
		output.packedFormat = PackedFormat::F32;
		return true;
	}

	if (!IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM))
		return false;

	if (input->wBitsPerSample == 16 && validBits == 16 &&
		input->nBlockAlign == 4 &&
		(config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S16) != 0)
	{
		output.publicFormat = PSYX_AUDIO_FORMAT_S16;
		output.packedFormat = PackedFormat::S16;
		return true;
	}
	if (input->wBitsPerSample == 24 && validBits == 24 &&
		input->nBlockAlign == 6 &&
		(config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24) != 0)
	{
		output.publicFormat = PSYX_AUDIO_FORMAT_S24;
		output.packedFormat = PackedFormat::S24;
		return true;
	}
	if (input->wBitsPerSample == 32 && validBits == 24 &&
		input->nBlockAlign == 8 &&
		(config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S24_IN_32) != 0)
	{
		output.publicFormat = PSYX_AUDIO_FORMAT_S24_IN_32;
		output.packedFormat = PackedFormat::S24In32;
		return true;
	}
	if (input->wBitsPerSample == 32 && validBits == 32 &&
		input->nBlockAlign == 8 &&
		(config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S32) != 0)
	{
		output.publicFormat = PSYX_AUDIO_FORMAT_S32;
		output.packedFormat = PackedFormat::S32;
		return true;
	}
	return false;
}

static HRESULT createEnumerator(IMMDeviceEnumerator** enumerator)
{
	return CoCreateInstance(
		CLSID_MMDeviceEnumerator,
		nullptr,
		CLSCTX_ALL,
		IID_IMMDeviceEnumerator,
		reinterpret_cast<void**>(enumerator));
}

static HRESULT activateAudioClient(IMMDevice* device, IAudioClient** client)
{
	return device->Activate(
		IID_IAudioClient,
		CLSCTX_ALL,
		nullptr,
		reinterpret_cast<void**>(client));
}

static std::string endpointId(IMMDevice* device)
{
	LPWSTR id = nullptr;
	if (FAILED(device->GetId(&id)))
		return {};
	const std::string result = wideToUtf8(id);
	CoTaskMemFree(id);
	return result;
}

static std::string endpointName(IMMDevice* device)
{
	ComHolder<IPropertyStore> properties;
	if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put())))
		return {};
	PROPVARIANT value;
	PropVariantInit(&value);
	std::string result;
	if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
		value.vt == VT_LPWSTR)
		result = wideToUtf8(value.pwszVal);
	PropVariantClear(&value);
	return result;
}

class WasapiSink final : public AudioSink
{
public:
	~WasapiSink() override
	{
		stop();
	}

	PsyXAudioResult start(const SinkStartParams& params) override
	{
		config_ = params.config;
		deviceId_ = params.config.device_id_utf8 ? params.config.device_id_utf8 : "";
		config_.device_id_utf8 = deviceId_.empty() ? nullptr : deviceId_.c_str();
		renderCallback_ = params.renderCallback;
		renderCallbackFloat64_ = params.renderCallbackFloat64;
		renderUser_ = params.renderUser;
		sourceRate_ = params.sourceRate;
		dither_ = params.dither;
		shared_ = params.shared;
		stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!stopEvent_ || !audioEvent_)
		{
			const DWORD error = GetLastError();
			closeEvents();
			shared_->setFailure(PSYX_AUDIO_ERROR_BACKEND, error, "CreateEvent failed");
			return PSYX_AUDIO_ERROR_BACKEND;
		}

		{
			std::lock_guard<std::mutex> lock(startMutex_);
			startComplete_ = false;
			startResult_ = PSYX_AUDIO_ERROR_THREAD;
		}
		stopping_.store(false, std::memory_order_release);
		try
		{
			thread_ = std::thread(&WasapiSink::threadMain, this);
		}
		catch (...)
		{
			closeEvents();
			shared_->setFailure(PSYX_AUDIO_ERROR_THREAD, 0, "WASAPI thread creation failed");
			return PSYX_AUDIO_ERROR_THREAD;
		}

		std::unique_lock<std::mutex> lock(startMutex_);
		startCondition_.wait(lock, [this] { return startComplete_; });
		const PsyXAudioResult result = startResult_;
		lock.unlock();
		if (result != PSYX_AUDIO_OK)
		{
			if (thread_.joinable())
				thread_.join();
			closeEvents();
		}
		return result;
	}

	void stop() override
	{
		stopping_.store(true, std::memory_order_release);
		if (stopEvent_)
			SetEvent(stopEvent_);
		if (thread_.joinable())
			thread_.join();
		closeEvents();
	}

private:
	void completeStart(PsyXAudioResult result)
	{
		std::lock_guard<std::mutex> lock(startMutex_);
		startResult_ = result;
		startComplete_ = true;
		startCondition_.notify_one();
	}

	void failStart(HRESULT result, const char* diagnostic)
	{
		const PsyXAudioResult mapped = resultFromHresult(result);
		shared_->setFailure(mapped, result, diagnostic);
		completeStart(mapped);
	}

	bool selectEndpoint(IMMDeviceEnumerator* enumerator, IMMDevice** endpoint)
	{
		HRESULT result;
		if (deviceId_.empty())
		{
			result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint);
		}
		else
		{
			const std::wstring id = utf8ToWide(deviceId_.c_str());
			if (id.empty())
				return false;
			result = enumerator->GetDevice(id.c_str(), endpoint);
		}
		return SUCCEEDED(result);
	}

	bool initializeExclusive(
		IMMDevice* endpoint,
		ComHolder<IAudioClient>& client,
		WasapiFormat& selected,
		UINT32& bufferFrames,
		HRESULT& failure)
	{
		failure = activateAudioClient(endpoint, client.put());
		if (FAILED(failure))
			return false;

		bool supported = false;
		for (const WasapiFormat& candidate : exclusiveCandidates(config_))
		{
			failure = client->IsFormatSupported(
				AUDCLNT_SHAREMODE_EXCLUSIVE,
				reinterpret_cast<const WAVEFORMATEX*>(&candidate.wave),
				nullptr);
			if (failure == S_OK)
			{
				selected = candidate;
				supported = true;
				break;
			}
		}
		if (!supported)
		{
			failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
			return false;
		}

		REFERENCE_TIME defaultPeriod = 0;
		REFERENCE_TIME minimumPeriod = 0;
		failure = client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
		if (FAILED(failure))
			return false;

		const uint32_t rate = selected.wave.Format.nSamplesPerSec;
		REFERENCE_TIME duration = std::max<REFERENCE_TIME>(
			static_cast<REFERENCE_TIME>(config_.target_latency_ms) * 10000,
			minimumPeriod);
		failure = client->Initialize(
			AUDCLNT_SHAREMODE_EXCLUSIVE,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
			duration,
			duration,
			reinterpret_cast<const WAVEFORMATEX*>(&selected.wave),
			nullptr);
		if (failure == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			UINT32 alignedFrames = 0;
			if (FAILED(client->GetBufferSize(&alignedFrames)))
				return false;
			duration = static_cast<REFERENCE_TIME>(
				(10000000ull * alignedFrames + rate - 1) / rate);
			client.reset();
			failure = activateAudioClient(endpoint, client.put());
			if (FAILED(failure))
				return false;
			failure = client->Initialize(
				AUDCLNT_SHAREMODE_EXCLUSIVE,
				AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
				duration,
				duration,
				reinterpret_cast<const WAVEFORMATEX*>(&selected.wave),
				nullptr);
		}
		if (FAILED(failure))
			return false;
		failure = client->GetBufferSize(&bufferFrames);
		return SUCCEEDED(failure);
	}

	bool initializeShared(
		IMMDevice* endpoint,
		ComHolder<IAudioClient>& client,
		WasapiFormat& selected,
		UINT32& bufferFrames,
		HRESULT& failure)
	{
		failure = activateAudioClient(endpoint, client.put());
		if (FAILED(failure))
			return false;

		WAVEFORMATEX* mix = nullptr;
		failure = client->GetMixFormat(&mix);
		if (FAILED(failure))
			return false;
		const bool accepted = classifySharedFormat(mix, config_, selected);
		if (!accepted)
		{
			CoTaskMemFree(mix);
			failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
			return false;
		}

		const REFERENCE_TIME duration =
			static_cast<REFERENCE_TIME>(config_.target_latency_ms) * 10000;
		failure = client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
			duration,
			0,
			mix,
			nullptr);
		CoTaskMemFree(mix);
		if (FAILED(failure))
			return false;
		failure = client->GetBufferSize(&bufferFrames);
		return SUCCEEDED(failure);
	}

	void threadMain()
	{
		ComApartment apartment;
		if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
		{
			failStart(apartment.result, "CoInitializeEx failed");
			return;
		}

		DWORD taskIndex = 0;
		HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		ComHolder<IMMDeviceEnumerator> enumerator;
		ComHolder<IMMDevice> endpoint;
		ComHolder<IAudioClient> client;
		ComHolder<IAudioRenderClient> renderClient;
		ComHolder<IAudioClock> clock;
		HRESULT result = createEnumerator(enumerator.put());
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "MMDeviceEnumerator creation failed");
			return;
		}
		if (!selectEndpoint(enumerator.get(), endpoint.put()))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			shared_->setFailure(PSYX_AUDIO_ERROR_NO_DEVICE, 0, "WASAPI endpoint was not found");
			completeStart(PSYX_AUDIO_ERROR_NO_DEVICE);
			return;
		}

		WasapiFormat format{};
		UINT32 bufferFrames = 0;
		const bool exclusive = config_.mode == PSYX_AUDIO_MODE_EXCLUSIVE_EVENT;
		const bool initialized = exclusive
			? initializeExclusive(endpoint.get(), client, format, bufferFrames, result)
			: initializeShared(endpoint.get(), client, format, bufferFrames, result);
		if (!initialized)
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, exclusive
				? "No exact WASAPI exclusive format was accepted"
				: "WASAPI shared mix format violates the strict policy");
			return;
		}

		result = client->SetEventHandle(audioEvent_);
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "IAudioClient::SetEventHandle failed");
			return;
		}
		result = client->GetService(
			IID_IAudioRenderClient,
			reinterpret_cast<void**>(renderClient.put()));
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "IAudioRenderClient acquisition failed");
			return;
		}
		client->GetService(IID_IAudioClock, reinterpret_cast<void**>(clock.put()));

		const uint32_t rate = format.wave.Format.nSamplesPerSec;
		const uint32_t multiplier = renderCallbackFloat64_
			? sourceRate_ / rate : rateMultiplier(rate);
		const uint32_t bufferSourceFrames = renderCallbackFloat64_
			? bufferFrames * multiplier
			: (bufferFrames + multiplier - 1) / multiplier;
		const uint32_t effectiveRingFrames = std::max<uint32_t>(
			config_.ring_capacity_frames,
			bufferSourceFrames * 2u + 1u);
		const uint32_t targetLeadFrames = std::min<uint32_t>(
			effectiveRingFrames - 1,
			std::max<uint32_t>(bufferSourceFrames * 2, 256u));
		RenderPump pump(
			effectiveRingFrames,
			renderCallback_,
			renderCallbackFloat64_,
			renderUser_,
			sourceRate_,
			*shared_);
		OutputPacker packer(pump, format.packedFormat, rate, dither_);

		const std::string id = endpointId(endpoint.get());
		const std::string name = endpointName(endpoint.get());
		const bool integerFormat = format.publicFormat != PSYX_AUDIO_FORMAT_F32;
		const bool nativeRate = rate == sourceRate_;
		shared_->setOpened(
			PSYX_AUDIO_BACKEND_WASAPI,
			exclusive ? PSYX_AUDIO_MODE_EXCLUSIVE_EVENT : PSYX_AUDIO_MODE_SHARED,
			format.publicFormat,
			rate,
			format.containerBits,
			format.validBits,
			multiplier,
			exclusive && nativeRate && integerFormat,
			integerFormat,
			id.c_str(),
			name.c_str());
		shared_->setState(PSYX_AUDIO_STATE_PRIMING);

		pump.fillTo(std::max<uint64_t>(
			targetLeadFrames,
			packer.sourceFramesNeeded(bufferFrames)));
		BYTE* initialData = nullptr;
		result = renderClient->GetBuffer(bufferFrames, &initialData);
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "Initial WASAPI buffer acquisition failed");
			return;
		}
		packer.pack(initialData, bufferFrames);
		result = renderClient->ReleaseBuffer(bufferFrames, 0);
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "Initial WASAPI buffer release failed");
			return;
		}
		shared_->deviceFramesSubmitted.fetch_add(bufferFrames, std::memory_order_relaxed);

		result = client->Start();
		if (FAILED(result))
		{
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			failStart(result, "IAudioClient::Start failed");
			return;
		}
		shared_->setState(PSYX_AUDIO_STATE_RUNNING);
		completeStart(PSYX_AUDIO_OK);

		UINT64 clockFrequency = 0;
		UINT64 clockOrigin = 0;
		bool haveClockOrigin = false;
		if (clock)
			clock->GetFrequency(&clockFrequency);

		HANDLE events[2] = {stopEvent_, audioEvent_};
		while (!stopping_.load(std::memory_order_acquire))
		{
			const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
			if (waitResult == WAIT_OBJECT_0)
				break;
			if (waitResult != WAIT_OBJECT_0 + 1)
			{
				shared_->setFailure(
					PSYX_AUDIO_ERROR_BACKEND,
					GetLastError(),
					"WASAPI event wait failed");
				break;
			}

			uint64_t playedDeviceFrames = 0;
			if (clock && clockFrequency != 0)
			{
				UINT64 position = 0;
				UINT64 qpc = 0;
				if (SUCCEEDED(clock->GetPosition(&position, &qpc)))
				{
					if (!haveClockOrigin)
					{
						clockOrigin = position;
						haveClockOrigin = true;
					}
					const UINT64 delta = position - clockOrigin;
					playedDeviceFrames =
						(delta / clockFrequency) * rate +
						((delta % clockFrequency) * rate) / clockFrequency;
					shared_->deviceFramesPlayed.store(
						playedDeviceFrames,
						std::memory_order_relaxed);
				}
			}

			UINT32 framesAvailable = bufferFrames;
			if (!exclusive)
			{
				UINT32 padding = 0;
				result = client->GetCurrentPadding(&padding);
				if (FAILED(result))
				{
					shared_->setFailure(
						resultFromHresult(result),
						result,
						"IAudioClient::GetCurrentPadding failed");
					break;
				}
				framesAvailable = bufferFrames - padding;
			}
			if (framesAvailable == 0)
				continue;

			const uint64_t playedSourceFrames = renderCallbackFloat64_
				? playedDeviceFrames * multiplier
				: playedDeviceFrames / multiplier;
			const uint64_t minimumTarget =
				pump.consumedFrames() + packer.sourceFramesNeeded(framesAvailable);
			pump.fillTo(std::max<uint64_t>(
				playedSourceFrames + targetLeadFrames,
				minimumTarget));

			BYTE* output = nullptr;
			result = renderClient->GetBuffer(framesAvailable, &output);
			if (FAILED(result))
			{
				shared_->setFailure(
					resultFromHresult(result),
					result,
					"IAudioRenderClient::GetBuffer failed");
				break;
			}
			packer.pack(output, framesAvailable);
			result = renderClient->ReleaseBuffer(framesAvailable, 0);
			if (FAILED(result))
			{
				shared_->setFailure(
					resultFromHresult(result),
					result,
					"IAudioRenderClient::ReleaseBuffer failed");
				break;
			}
			shared_->deviceFramesSubmitted.fetch_add(
				framesAvailable,
				std::memory_order_relaxed);
		}

		client->Stop();
		if (mmcss)
			AvRevertMmThreadCharacteristics(mmcss);
	}

	void closeEvents()
	{
		if (audioEvent_)
		{
			CloseHandle(audioEvent_);
			audioEvent_ = nullptr;
		}
		if (stopEvent_)
		{
			CloseHandle(stopEvent_);
			stopEvent_ = nullptr;
		}
	}

	PsyXAudioConfig config_{};
	std::string deviceId_;
	PsyXAudioRenderCallback renderCallback_ = nullptr;
	PsyXAudioRenderCallbackFloat64 renderCallbackFloat64_ = nullptr;
	void* renderUser_ = nullptr;
	uint32_t sourceRate_ = 44100;
	PsyXAudioDither dither_ = PSYX_AUDIO_DITHER_NONE;
	AudioSharedState* shared_ = nullptr;
	HANDLE stopEvent_ = nullptr;
	HANDLE audioEvent_ = nullptr;
	std::thread thread_;
	std::atomic<bool> stopping_{false};
	std::mutex startMutex_;
	std::condition_variable startCondition_;
	bool startComplete_ = false;
	PsyXAudioResult startResult_ = PSYX_AUDIO_ERROR_THREAD;
};

std::unique_ptr<AudioSink> createWasapiSink()
{
	return std::make_unique<WasapiSink>();
}

PsyXAudioResult enumerateWasapiDevices(std::vector<PsyXAudioDeviceInfo>& devices)
{
	ComApartment apartment;
	if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
		return PSYX_AUDIO_ERROR_BACKEND;

	ComHolder<IMMDeviceEnumerator> enumerator;
	if (FAILED(createEnumerator(enumerator.put())))
		return PSYX_AUDIO_ERROR_BACKEND;

	ComHolder<IMMDevice> defaultDevice;
	std::string defaultId;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultDevice.put())))
		defaultId = endpointId(defaultDevice.get());

	ComHolder<IMMDeviceCollection> collection;
	if (FAILED(enumerator->EnumAudioEndpoints(
			eRender,
			DEVICE_STATE_ACTIVE,
			collection.put())))
		return PSYX_AUDIO_ERROR_BACKEND;

	UINT count = 0;
	if (FAILED(collection->GetCount(&count)))
		return PSYX_AUDIO_ERROR_BACKEND;

	PsyXAudioConfig probeConfig{};
	probeConfig.allowed_rate_mask =
		PSYX_AUDIO_RATE_44100 | PSYX_AUDIO_RATE_88200 | PSYX_AUDIO_RATE_176400;
	probeConfig.allowed_container_mask =
		PSYX_AUDIO_CONTAINER_S16 | PSYX_AUDIO_CONTAINER_S24 |
		PSYX_AUDIO_CONTAINER_S24_IN_32 | PSYX_AUDIO_CONTAINER_S32;
	probeConfig.flags = PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION |
		PSYX_AUDIO_FLAG_ALLOW_SHARED_FLOAT;

	for (UINT i = 0; i < count; ++i)
	{
		ComHolder<IMMDevice> endpoint;
		if (FAILED(collection->Item(i, endpoint.put())))
			continue;

		PsyXAudioDeviceInfo info{};
		info.backend = PSYX_AUDIO_BACKEND_WASAPI;
		info.state = DEVICE_STATE_ACTIVE;
		const std::string id = endpointId(endpoint.get());
		const std::string name = endpointName(endpoint.get());
		info.is_default = id == defaultId ? 1u : 0u;
		copyText(info.id_utf8, sizeof(info.id_utf8), id.c_str());
		copyText(info.name_utf8, sizeof(info.name_utf8), name.c_str());

		ComHolder<IAudioClient> client;
		if (SUCCEEDED(activateAudioClient(endpoint.get(), client.put())))
		{
			WAVEFORMATEX* mix = nullptr;
			if (SUCCEEDED(client->GetMixFormat(&mix)))
			{
				WasapiFormat classified{};
				info.mix_rate = mix->nSamplesPerSec;
				info.mix_channels = mix->nChannels;
				if (classifySharedFormat(mix, probeConfig, classified))
					info.mix_format = classified.publicFormat;
				CoTaskMemFree(mix);
			}

			for (const WasapiFormat& candidate : exclusiveCandidates(probeConfig))
			{
				if (client->IsFormatSupported(
						AUDCLNT_SHAREMODE_EXCLUSIVE,
						reinterpret_cast<const WAVEFORMATEX*>(&candidate.wave),
						nullptr) != S_OK)
					continue;
				info.exclusive_rate_mask |=
					rateMaskFor(candidate.wave.Format.nSamplesPerSec);
				switch (candidate.publicFormat)
				{
				case PSYX_AUDIO_FORMAT_S16:
					info.exclusive_container_mask |= PSYX_AUDIO_CONTAINER_S16;
					break;
				case PSYX_AUDIO_FORMAT_S24:
					info.exclusive_container_mask |= PSYX_AUDIO_CONTAINER_S24;
					break;
				case PSYX_AUDIO_FORMAT_S24_IN_32:
					info.exclusive_container_mask |= PSYX_AUDIO_CONTAINER_S24_IN_32;
					break;
				case PSYX_AUDIO_FORMAT_S32:
					info.exclusive_container_mask |= PSYX_AUDIO_CONTAINER_S32;
					break;
				default:
					break;
				}
			}
		}
		devices.push_back(info);
	}

	return devices.empty() ? PSYX_AUDIO_ERROR_NO_DEVICE : PSYX_AUDIO_OK;
}

} // namespace psyx::audio

#endif
