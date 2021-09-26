/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include "common/Console.h"
#include "common/RedtapeWindows.h"
#include "cubeb/cubeb.h"

#include "Global.h"
#include "SndOut.h"

extern bool CfgReadBool(const wchar_t* Section, const wchar_t* Name, bool Default);
extern int CfgReadInt(const wchar_t* Section, const wchar_t* Name, int Default);

class Cubeb : public SndOutModule
{
private:
	//////////////////////////////////////////////////////////////////////////////////////////
	// Configuration Vars (unused still)
	bool m_com_initialized_by_us = false;
	bool m_SuggestedLatencyMinimal = false;
	int m_SuggestedLatencyMS = 20;

	//////////////////////////////////////////////////////////////////////////////////////////
	// Instance vars
	u64 writtenSoFar = 0;
	u64 writtenLastTime = 0;
	u64 positionLastTime = 0;

	u32 channels = 0;
	cubeb* m_context = nullptr;
	cubeb_stream* stream = nullptr;

	//////////////////////////////////////////////////////////////////////////////////////////
	// Stuff necessary for speaker expansion
	class SampleReader
	{
	public:
		virtual void ReadSamples(void* outputBuffer, long frames) = 0;
	};

	template <class T>
	class ConvertedSampleReader : public SampleReader
	{
		u64* written;

	public:
		ConvertedSampleReader(u64* pWritten)
		{
			written = pWritten;
		}

		virtual void ReadSamples(void* outputBuffer, long frames) override
		{
			T* p1 = (T*)outputBuffer;

			while (frames > 0)
			{
				const long frames_to_read = std::min<long>(frames, SndOutPacketSize);
				SndBuffer::ReadSamples(p1, frames_to_read);
				p1 += frames_to_read;
				frames -= frames_to_read;
			}

			(*written) += frames;
		}
	};

	void DestroyContextAndStream()
	{
		if (stream)
		{
			cubeb_stream_stop(stream);
			cubeb_stream_destroy(stream);
			stream = nullptr;
		}

		if (m_context)
		{
			cubeb_destroy(m_context);
			m_context = nullptr;
		}

#ifdef _WIN32
		if (m_com_initialized_by_us)
			CoUninitialize();
#endif
	}

	SampleReader* ActualReader = nullptr;

public:
	Cubeb() = default;

	~Cubeb()
	{
		DestroyContextAndStream();
	}

	s32 Init() override
	{
		ReadSettings();

#ifdef _WIN32
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		m_com_initialized_by_us = SUCCEEDED(hr);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
		{
			ConLog("Failed to initialize COM");
			return -1;
		}
#endif

		int rv = cubeb_init(&m_context, "PCSX2", nullptr);
		if (rv != CUBEB_OK)
		{
			ConLog("Could not initialize cubeb context: %d", rv);
			return false;
		}

		switch (numSpeakers) // speakers = (numSpeakers + 1) *2; ?
		{
			case 0:
				channels = 2;
				break; // Stereo
			case 1:
				channels = 4;
				break; // Quadrafonic
			case 2:
				channels = 6;
				break; // Surround 5.1
			case 3:
				channels = 8;
				break; // Surround 7.1
			default:
				channels = 2;
				break;
		}

		switch (channels)
		{
			case 2:
				ConLog("* SPU2 > Using normal 2 speaker stereo output.\n");
				ActualReader = new ConvertedSampleReader<StereoOut16>(&writtenSoFar);
				break;

			case 3:
				ConLog("* SPU2 > 2.1 speaker expansion enabled.\n");
				ActualReader = new ConvertedSampleReader<Stereo21Out16>(&writtenSoFar);
				break;

			case 4:
				ConLog("* SPU2 > 4 speaker expansion enabled [quadraphenia]\n");
				ActualReader = new ConvertedSampleReader<Stereo40Out16>(&writtenSoFar);
				break;

			case 5:
				ConLog("* SPU2 > 4.1 speaker expansion enabled.\n");
				ActualReader = new ConvertedSampleReader<Stereo41Out16>(&writtenSoFar);
				break;

			case 6:
			case 7:
				switch (dplLevel)
				{
					case 0:
						ConLog("* SPU2 > 5.1 speaker expansion enabled.\n");
						ActualReader = new ConvertedSampleReader<Stereo51Out16>(&writtenSoFar); //"normal" stereo upmix
						break;
					case 1:
						ConLog("* SPU2 > 5.1 speaker expansion with basic ProLogic dematrixing enabled.\n");
						ActualReader = new ConvertedSampleReader<Stereo51Out16Dpl>(&writtenSoFar); // basic Dpl decoder without rear stereo balancing
						break;
					case 2:
						ConLog("* SPU2 > 5.1 speaker expansion with experimental ProLogicII dematrixing enabled.\n");
						ActualReader = new ConvertedSampleReader<Stereo51Out16DplII>(&writtenSoFar); //gigas PLII
						break;
				}
				channels = 6; // we do not support 7.0 or 6.2 configurations, downgrade to 5.1
				break;

			default: // anything 8 or more gets the 7.1 treatment!
				ConLog("* SPU2 > 7.1 speaker expansion enabled.\n");
				ActualReader = new ConvertedSampleReader<Stereo71Out16>(&writtenSoFar);
				channels = 8; // we do not support 7.2 or more, downgrade to 7.1
				break;
		}

		cubeb_stream_params params = {};
		params.format = CUBEB_SAMPLE_S16LE;
		params.rate = SampleRate;
		params.channels = channels;
		params.layout = CUBEB_LAYOUT_UNDEFINED;
		params.prefs = CUBEB_STREAM_PREF_PERSIST;

		const u32 requested_latency_frames = static_cast<u32>((m_SuggestedLatencyMS * SampleRate) / 1000);
		u32 latency_frames = 0;
		rv = cubeb_get_min_latency(m_context, &params, &latency_frames);
		if (rv == CUBEB_ERROR_NOT_SUPPORTED)
		{
			ConLog("Cubeb backend does not support latency queries, using buffer size of %u.", requested_latency_frames);
			latency_frames = requested_latency_frames;
		}
		else
		{
			if (rv != CUBEB_OK)
			{
				ConLog("Could not get minimum latency: %d", rv);
				DestroyContextAndStream();
				return -1;
			}

			ConLog("Minimum latency in frames: %u", latency_frames);
			if (!m_SuggestedLatencyMinimal)
			{
				if (latency_frames > requested_latency_frames)
					ConLog("Minimum latency is above buffer size: %u vs %u, adjusting to compensate.", latency_frames, requested_latency_frames);
				else
					latency_frames = requested_latency_frames;
			}
		}

		char stream_name[32];
		std::snprintf(stream_name, sizeof(stream_name), "%p", this);

		rv = cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, nullptr, &params,
			latency_frames, &Cubeb::DataCallback, &Cubeb::StateCallback, this);
		if (rv != CUBEB_OK)
		{
			ConLog("Could not create stream: %d", rv);
			DestroyContextAndStream();
			return -1;
		}

		rv = cubeb_stream_start(stream);
		if (rv != CUBEB_OK)
		{
			ConLog("Could not start stream: %d", rv);
			DestroyContextAndStream();
			return -1;
		}

		return 0;
	}

	void Close() override
	{
		DestroyContextAndStream();
	}

	static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state)
	{
	}

	static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer, long nframes)
	{
		static_cast<Cubeb*>(user_ptr)->ActualReader->ReadSamples(output_buffer, nframes);
		return nframes;
	}


	void Configure(uptr parent) override
	{
	}

	s32 Test() const override
	{
		return 0;
	}

	int GetEmptySampleCount() override
	{
		u64 pos;
		if (cubeb_stream_get_position(stream, &pos) != CUBEB_OK)
			pos = 0;

		int playedSinceLastTime = (writtenSoFar - writtenLastTime) + (pos - positionLastTime);
		writtenLastTime = writtenSoFar;
		positionLastTime = pos;

		// Lowest resolution here is the SndOutPacketSize we use.
		return playedSinceLastTime;
	}

	const wchar_t* GetIdent() const override
	{
		return L"cubeb";
	}

	const wchar_t* GetLongName() const override
	{
		return L"Cubeb (Cross-platform)";
	}

	void ReadSettings() override
	{
		m_SuggestedLatencyMinimal = CfgReadBool(L"Cubeb", L"MinimalSuggestedLatency", false);
		m_SuggestedLatencyMS = CfgReadInt(L"Cubeb", L"ManualSuggestedLatencyMS", 20);

		if (m_SuggestedLatencyMS < 10)
			m_SuggestedLatencyMS = 10;
		if (m_SuggestedLatencyMS > 200)
			m_SuggestedLatencyMS = 200;
	}

	void SetApiSettings(wxString api) override
	{
	}

	void WriteSettings() const override
	{
	}
};

static Cubeb s_Cubeb;
SndOutModule* CubebOut = &s_Cubeb;
