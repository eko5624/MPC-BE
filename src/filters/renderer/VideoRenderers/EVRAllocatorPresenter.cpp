/*
 * (C) 2006-2022 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include <map>
#include "EVRAllocatorPresenter.h"
#include "OuterEVR.h"
#include <Mferror.h>
#include "IPinHook.h"
#include "MacrovisionKicker.h"
#include "Variables.h"
#include "Utils.h"
#include <IMediaSideData.h>
#include <clsids.h>

#if (0)		// Set to 1 to activate EVR traces
	#define TRACE_EVR	TRACE
#else
	#define TRACE_EVR	__noop
#endif

#define MIN_FRAME_TIME 15000

// Guid to tag IMFSample with DirectX surface index
static const GUID GUID_SURFACE_INDEX = { 0x30c8e9f6, 0x415, 0x4b81, { 0xa3, 0x15, 0x1, 0xa, 0xc6, 0xa9, 0xda, 0x19 } };

MFOffset MakeOffset(float v)
{
	MFOffset offset;
	offset.value = short(v);
	offset.fract = WORD(65536 * (v-offset.value));
	return offset;
}

MFVideoArea MakeArea(float x, float y, DWORD width, DWORD height)
{
	MFVideoArea area;
	area.OffsetX = MakeOffset(x);
	area.OffsetY = MakeOffset(y);
	area.Area.cx = width;
	area.Area.cy = height;
	return area;
}

using namespace DSObjects;

CEVRAllocatorPresenter::CEVRAllocatorPresenter(HWND hWnd, bool bFullscreen, HRESULT& hr, CString &_Error)
	: CDX9AllocatorPresenter(hWnd, bFullscreen, hr, _Error)
{
	ResetQualProps();

	if (FAILED(hr)) {
		_Error += L"DX9AllocatorPresenter failed\n";
		return;
	}

	// Load EVR specifics DLLs
	if (m_hDxva2Lib) {
		pfDXVA2CreateDirect3DDeviceManager9 = (PTR_DXVA2CreateDirect3DDeviceManager9)GetProcAddress(m_hDxva2Lib, "DXVA2CreateDirect3DDeviceManager9");
	}

	// Load EVR functions
	m_hEvrLib = LoadLibraryW(L"evr.dll");
	if (m_hEvrLib) {
		pfMFCreateVideoSampleFromSurface = (PTR_MFCreateVideoSampleFromSurface)GetProcAddress(m_hEvrLib, "MFCreateVideoSampleFromSurface");
		pfMFCreateVideoMediaType = (PTR_MFCreateVideoMediaType)GetProcAddress(m_hEvrLib, "MFCreateVideoMediaType");
	}

	if (!pfDXVA2CreateDirect3DDeviceManager9 || !pfMFCreateVideoSampleFromSurface || !pfMFCreateVideoMediaType) {
		if (!pfDXVA2CreateDirect3DDeviceManager9) {
			_Error += L"Could not find DXVA2CreateDirect3DDeviceManager9 (dxva2.dll)\n";
		}
		if (!pfMFCreateVideoSampleFromSurface) {
			_Error += L"Could not find MFCreateVideoSampleFromSurface (evr.dll)\n";
		}
		if (!pfMFCreateVideoMediaType) {
			_Error += L"Could not find MFCreateVideoMediaType (Mfplat.dll)\n";
		}
		hr = E_FAIL;
		return;
	}

	// Load Vista specifics DLLs
	m_hAvrtLib = LoadLibraryW(L"avrt.dll");
	if (m_hAvrtLib) {
		pfAvSetMmThreadCharacteristicsW   = (PTR_AvSetMmThreadCharacteristicsW)GetProcAddress(m_hAvrtLib, "AvSetMmThreadCharacteristicsW");
		pfAvSetMmThreadPriority           = (PTR_AvSetMmThreadPriority)GetProcAddress(m_hAvrtLib, "AvSetMmThreadPriority");
		pfAvRevertMmThreadCharacteristics = (PTR_AvRevertMmThreadCharacteristics)GetProcAddress(m_hAvrtLib, "AvRevertMmThreadCharacteristics");
	}
}

CEVRAllocatorPresenter::~CEVRAllocatorPresenter(void)
{
	StopWorkerThreads(); // If not already done...
	m_pMediaType.Release();
	m_pClock.Release();
	m_pD3DManager.Release();

	if (m_hAvrtLib) {
		FreeLibrary(m_hAvrtLib);
	}
	if (m_hEvrLib) {
		FreeLibrary(m_hEvrLib);
	}
}

STDMETHODIMP CEVRAllocatorPresenter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
	HRESULT hr;
	if (riid == __uuidof(IMFClockStateSink)) {
		hr = GetInterface((IMFClockStateSink*)this, ppv);
	} else if (riid == __uuidof(IMFVideoPresenter)) {
		hr = GetInterface((IMFVideoPresenter*)this, ppv);
	} else if (riid == __uuidof(IMFTopologyServiceLookupClient)) {
		hr = GetInterface((IMFTopologyServiceLookupClient*)this, ppv);
	} else if (riid == __uuidof(IMFVideoDeviceID)) {
		hr = GetInterface((IMFVideoDeviceID*)this, ppv);
	} else if (riid == __uuidof(IMFGetService)) {
		hr = GetInterface((IMFGetService*)this, ppv);
	} else if (riid == __uuidof(IMFAsyncCallback)) {
		hr = GetInterface((IMFAsyncCallback*)this, ppv);
	} else if (riid == __uuidof(IMFVideoDisplayControl)) {
		hr = GetInterface((IMFVideoDisplayControl*)this, ppv);
	} else if (riid == __uuidof(IMFVideoMixerBitmap)) {
		hr = GetInterface((IMFVideoMixerBitmap*)this, ppv);
	} else if (riid == __uuidof(IEVRTrustedVideoPlugin)) {
		hr = GetInterface((IEVRTrustedVideoPlugin*)this, ppv);
	} else if (riid == IID_IQualProp) {
		hr = GetInterface((IQualProp*)this, ppv);
	} else if (riid == __uuidof(IMFRateSupport)) {
		hr = GetInterface((IMFRateSupport*)this, ppv);
	} else if (riid == __uuidof(IDirect3DDeviceManager9)) {
		//hr = GetInterface((IDirect3DDeviceManager9*)this, ppv);
		hr = m_pD3DManager->QueryInterface(__uuidof(IDirect3DDeviceManager9), (void**) ppv);
	} else if (riid == __uuidof(ID3DFullscreenControl)) {
		hr = GetInterface((ID3DFullscreenControl*)this, ppv);
	} else if (riid == __uuidof(IMediaSideData)) {
		hr = GetInterface((IMediaSideData*)this, ppv);
	} else if (riid == __uuidof(IPlaybackNotify)) {
		hr = GetInterface((IPlaybackNotify*)this, ppv);
	} else {
		hr = __super::NonDelegatingQueryInterface(riid, ppv);
	}

	return hr;
}

void CEVRAllocatorPresenter::ResetQualProps()
{
	m_pcFrames			= 0;
	m_nDroppedUpdate	= 0;
	m_pcFramesDrawn		= 0;
	m_piAvg				= 0;
	m_piDev				= 0;
}

HRESULT CEVRAllocatorPresenter::CheckShutdown() const
{
	if (m_nRenderState == Shutdown) {
		return MF_E_SHUTDOWN;
	} else {
		return S_OK;
	}
}

void CEVRAllocatorPresenter::StartWorkerThreads()
{
	DWORD dwThreadId;

	if (m_nRenderState == Shutdown) {
		m_hEvtQuit			= CreateEventW(nullptr, TRUE, FALSE, nullptr);
		m_hEvtFlush			= CreateEventW(nullptr, TRUE, FALSE, nullptr);

		m_hRenderThread		= ::CreateThread(nullptr, 0, PresentThread, (LPVOID)this, 0, &dwThreadId);
		SetThreadPriority(m_hRenderThread, THREAD_PRIORITY_TIME_CRITICAL);
		m_hGetMixerThread	= ::CreateThread(nullptr, 0, GetMixerThreadStatic, (LPVOID)this, 0, &dwThreadId);
		SetThreadPriority(m_hGetMixerThread, THREAD_PRIORITY_HIGHEST);
		m_hVSyncThread		= ::CreateThread(nullptr, 0, VSyncThreadStatic, (LPVOID)this, 0, &dwThreadId);
		SetThreadPriority(m_hVSyncThread, THREAD_PRIORITY_HIGHEST);

		m_nRenderState		= Stopped;
		m_bChangeMT			= true;
		TRACE_EVR("EVR: Worker threads started...\n");
	}
}

void CEVRAllocatorPresenter::StopWorkerThreads()
{
	if (m_nRenderState != Shutdown) {
		if (m_pClock) {// if m_pClock is active, everything has been initialized from the above InitServicePointers()
			SetEvent(m_hEvtFlush);
			m_bEvtFlush = true;
			SetEvent(m_hEvtQuit);
			m_bEvtQuit = true;
			if (m_hRenderThread && WaitForSingleObject(m_hRenderThread, 1000) == WAIT_TIMEOUT) {
				ASSERT(FALSE);
				TerminateThread(m_hRenderThread, 0xDEAD);
			}
			if (m_hGetMixerThread && WaitForSingleObject(m_hGetMixerThread, 1000) == WAIT_TIMEOUT) {
				ASSERT(FALSE);
				TerminateThread(m_hGetMixerThread, 0xDEAD);
			}
			if (m_hVSyncThread && WaitForSingleObject(m_hVSyncThread, 1000) == WAIT_TIMEOUT) {
				ASSERT(FALSE);
				TerminateThread(m_hVSyncThread, 0xDEAD);
			}
		}

		SAFE_CLOSE_HANDLE(m_hRenderThread);
		SAFE_CLOSE_HANDLE(m_hGetMixerThread);
		SAFE_CLOSE_HANDLE(m_hVSyncThread);
		SAFE_CLOSE_HANDLE(m_hEvtFlush);
		SAFE_CLOSE_HANDLE(m_hEvtQuit);

		m_bEvtFlush	= false;
		m_bEvtQuit	= false;

		TRACE_EVR("EVR: Worker threads stopped...\n");
	}

	m_nRenderState = Shutdown;
}

// IAllocatorPresenter

STDMETHODIMP CEVRAllocatorPresenter::CreateRenderer(IUnknown** ppRenderer)
{
	ASSERT(m_pD3D9Ex && m_pDevice9Ex == nullptr && m_pD3DManager == nullptr);

	CheckPointer(ppRenderer, E_POINTER);
	*ppRenderer = nullptr;

	HRESULT hr = E_FAIL;
	CStringW _Error;

	if (!m_bPreviewMode) {
		hr = RegisterWindowClass();
		ASSERT(SUCCEEDED(hr));
	}

	// Init DXVA manager
	hr = pfDXVA2CreateDirect3DDeviceManager9(&m_nResetToken, &m_pD3DManager);
	if (FAILED(hr)) {
		_Error = L"DXVA2CreateDirect3DDeviceManager9 failed\n";
		DLog(_Error);
		return hr;
	}

	hr = CreateDevice(_Error);
	if (FAILED(hr)) {
		DLog(_Error);
		return hr;
	}

	hr = m_pD3DManager->ResetDevice(m_pDevice9Ex, m_nResetToken);
	if (FAILED(hr)) {
		_Error = L"m_pD3DManager->ResetDevice failed\n";
		DLog(_Error);
		return hr;
	}

	do {
		CMacrovisionKicker*	pMK  = DNew CMacrovisionKicker(L"CMacrovisionKicker", nullptr);
		CComPtr<IUnknown>	pUnk = (IUnknown*)(INonDelegatingUnknown*)pMK;

		COuterEVR *pOuterEVR = DNew COuterEVR(L"COuterEVR", pUnk, hr, this);
		m_pOuterEVR = pOuterEVR;

		pMK->SetInner((IUnknown*)(INonDelegatingUnknown*)pOuterEVR);
		CComQIPtr<IBaseFilter> pBF(pUnk);

		if (FAILED(hr)) {
			break;
		}

		// Set EVR custom presenter
		CComPtr<IMFVideoPresenter>	pVP;
		CComPtr<IMFVideoRenderer>	pMFVR;
		CComQIPtr<IMFGetService>	pMFGS(pBF);
		CComQIPtr<IEVRFilterConfig>	pConfig(pBF);

		hr = pMFGS->GetService(MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&pMFVR));
		if (SUCCEEDED(hr)) {
			hr = QueryInterface(IID_PPV_ARGS(&pVP));
		}
		if (SUCCEEDED(hr)) {
			hr = pMFVR->InitializeRenderer(nullptr, pVP);
		}

		if (m_bEnableSubPic) {
			m_fUseInternalTimer = HookNewSegmentAndReceive(GetFirstPin(pBF));
		}

		if (FAILED(hr)) {
			*ppRenderer = nullptr;
		} else {
			*ppRenderer = pBF.Detach();
		}

	} while (0);

	DwmEnableMMCSS(TRUE);

	return hr;
}

STDMETHODIMP_(CLSID) CEVRAllocatorPresenter::GetAPCLSID()
{
	return CLSID_EVRAllocatorPresenter;
}

STDMETHODIMP_(bool) CEVRAllocatorPresenter::ResizeDevice()
{
	CAutoLock lock(this);
	CAutoLock lock2(&m_ImageProcessingLock);
	CAutoLock cRenderLock(&m_RenderLock);

	return __super::ResizeDevice();
}

STDMETHODIMP_(bool) CEVRAllocatorPresenter::ResetDevice()
{
	DLog(L"CEVRAllocatorPresenter::ResetDevice()");

	StopWorkerThreads();

	CAutoLock lock(this);
	CAutoLock lock2(&m_ImageProcessingLock);
	CAutoLock cRenderLock(&m_RenderLock);

	RemoveAllSamples();

	bool bResult = __super::ResetDevice();

	for (unsigned i = 0; i < m_nSurfaces; i++) {
		CComPtr<IMFSample> pMFSample;
		HRESULT hr = pfMFCreateVideoSampleFromSurface(m_pVideoSurfaces[i], &pMFSample);

		if (SUCCEEDED(hr)) {
			pMFSample->SetUINT32(GUID_SURFACE_INDEX, i);
			m_FreeSamples.emplace_back(pMFSample);
		}
		ASSERT(SUCCEEDED(hr));
	}

	if (bResult) {
		StartWorkerThreads();
	}

	return bResult;
}

STDMETHODIMP_(bool) CEVRAllocatorPresenter::DisplayChange()
{
	CAutoLock lock(this);
	CAutoLock lock2(&m_ImageProcessingLock);
	CAutoLock cRenderLock(&m_RenderLock);

	return __super::DisplayChange();
}

// IMFClockStateSink

STDMETHODIMP CEVRAllocatorPresenter::OnClockStart(MFTIME hnsSystemTime,  LONGLONG llClockStartOffset)
{
	TRACE_EVR("EVR: OnClockStart  hnsSystemTime = %I64d,   llClockStartOffset = %I64d\n", hnsSystemTime, llClockStartOffset);

	m_nRenderState       = Started;
	m_ModeratedTimeLast  = -1;
	m_ModeratedClockLast = -1;

	if (m_pSS) {
		// Get the selected subtitles stream
		DWORD cStreams = 0;
		if (SUCCEEDED(m_pSS->Count(&cStreams)) && cStreams > 0) {
			int selectedStream = 0;
			for (long i = 0; i < (long)cStreams; i++) {
				DWORD dwFlags = DWORD_MAX;
				DWORD dwGroup = DWORD_MAX;

				if (FAILED(m_pSS->Info(i, nullptr, &dwFlags, nullptr, &dwGroup, nullptr, nullptr, nullptr))) {
					continue;
				}

				if (dwGroup != 2) {
					continue;
				}

				if (dwFlags & (AMSTREAMSELECTINFO_ENABLED | AMSTREAMSELECTINFO_EXCLUSIVE)) {
					m_nCurrentSubtitlesStream = selectedStream;
					break;
				}

				selectedStream++;
			}
		}
	}

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::OnClockStop(MFTIME hnsSystemTime)
{
	TRACE_EVR("EVR: OnClockStop  hnsSystemTime = %I64d\n", hnsSystemTime);

	m_nRenderState       = Stopped;
	m_ModeratedTimeLast  = -1;
	m_ModeratedClockLast = -1;

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::OnClockPause(MFTIME hnsSystemTime)
{
	TRACE_EVR("EVR: OnClockPause  hnsSystemTime = %I64d\n", hnsSystemTime);

	if (!m_bSignaledStarvation) {
		m_nRenderState   = Paused;
	}
	m_ModeratedTimeLast  = -1;
	m_ModeratedClockLast = -1;

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::OnClockRestart(MFTIME hnsSystemTime)
{
	TRACE_EVR("EVR: OnClockRestart  hnsSystemTime = %I64d\n", hnsSystemTime);

	m_nRenderState       = Started;
	m_ModeratedTimeLast  = -1;
	m_ModeratedClockLast = -1;

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::OnClockSetRate(MFTIME hnsSystemTime, float flRate)
{
	ASSERT(FALSE);
	return E_NOTIMPL;
}

// IBaseFilter delegate

bool CEVRAllocatorPresenter::GetState( DWORD dwMilliSecsTimeout, FILTER_STATE *State, HRESULT &_ReturnValue)
{
	CAutoLock lock(&m_SampleQueueLock);

	if (m_bSignaledStarvation) {
		size_t nSamples = std::max(m_nSurfaces / 2, 1u);
		if ((m_ScheduledSamples.size() < nSamples || m_LastSampleOffset < -m_rtTimePerFrame*2) && !g_bNoDuration) {
			*State = (FILTER_STATE)Paused;
			_ReturnValue = VFW_S_STATE_INTERMEDIATE;
			return true;
		}
		m_bSignaledStarvation = false;
	}
	return false;
}

// IQualProp

STDMETHODIMP CEVRAllocatorPresenter::get_FramesDroppedInRenderer(int *pcFrames)
{
	*pcFrames = m_pcFrames;
	return S_OK;
}
STDMETHODIMP CEVRAllocatorPresenter::get_FramesDrawn(int *pcFramesDrawn)
{
	*pcFramesDrawn = m_pcFramesDrawn;
	return S_OK;
}
STDMETHODIMP CEVRAllocatorPresenter::get_AvgFrameRate(int *piAvgFrameRate)
{
	*piAvgFrameRate = (int)(m_fAvrFps * 100);
	return S_OK;
}
STDMETHODIMP CEVRAllocatorPresenter::get_Jitter(int *iJitter)
{
	*iJitter = (int)((m_fJitterStdDev / 10000.0) + 0.5);
	return S_OK;
}
STDMETHODIMP CEVRAllocatorPresenter::get_AvgSyncOffset(int *piAvg)
{
	*piAvg = (int)((m_fSyncOffsetAvr / 10000.0) + 0.5);
	return S_OK;
}
STDMETHODIMP CEVRAllocatorPresenter::get_DevSyncOffset(int *piDev)
{
	*piDev = (int)((m_fSyncOffsetStdDev / 10000.0) + 0.5);
	return S_OK;
}

// IMFRateSupport

STDMETHODIMP CEVRAllocatorPresenter::GetSlowestRate(MFRATE_DIRECTION eDirection, BOOL fThin, float *pflRate)
{
	// TODO : not finished...
	*pflRate = 0;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetFastestRate(MFRATE_DIRECTION eDirection, BOOL fThin, float *pflRate)
{
	HRESULT hr = S_OK;

	CAutoLock lock(this);

	CheckPointer(pflRate, E_POINTER);
	CHECK_HR(CheckShutdown());

	// Get the maximum forward rate.
	float fMaxRate = GetMaxRate(fThin);

	// For reverse playback, swap the sign.
	if (eDirection == MFRATE_REVERSE) {
		fMaxRate = -fMaxRate;
	}

	*pflRate = fMaxRate;

	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::IsRateSupported(BOOL fThin, float flRate, float *pflNearestSupportedRate)
{
	// fRate can be negative for reverse playback.
	// pfNearestSupportedRate can be nullptr.

	CAutoLock lock(this);

	HRESULT hr = S_OK;

	CheckPointer(pflNearestSupportedRate, E_POINTER);
	CHECK_HR(CheckShutdown());

	float fNearestRate = flRate; // Default.
	// Find the maximum forward rate.
	float fMaxRate = GetMaxRate(fThin);

	if (fabsf(flRate) > fMaxRate) {
		// The (absolute) requested rate exceeds the maximum rate.
		hr = MF_E_UNSUPPORTED_RATE;

		// The nearest supported rate is fMaxRate.
		fNearestRate = fMaxRate;
		if (flRate < 0) {
			// For reverse playback, swap the sign.
			fNearestRate = -fNearestRate;
		}
	}

	// Return the nearest supported rate if the caller requested it.
	*pflNearestSupportedRate = fNearestRate;

	return hr;
}

float CEVRAllocatorPresenter::GetMaxRate(BOOL bThin)
{
	float  fMaxRate     = FLT_MAX;  // Default.
	UINT32 fpsNumerator = 0, fpsDenominator = 0;

	if (!bThin && (m_pMediaType != nullptr)) {
		// Non-thinned: Use the frame rate and monitor refresh rate.

		// Frame rate:
		MFGetAttributeRatio(m_pMediaType, MF_MT_FRAME_RATE,
							&fpsNumerator, &fpsDenominator);

		// Monitor refresh rate:
		UINT MonitorRateHz = m_refreshRate; // D3DDISPLAYMODE

		if (fpsDenominator && fpsNumerator && MonitorRateHz) {
			// Max Rate = Refresh Rate / Frame Rate
			fMaxRate = (float)MulDiv(MonitorRateHz, fpsDenominator, fpsNumerator);
		}
	}
	return fMaxRate;
}

void CEVRAllocatorPresenter::CompleteFrameStep(bool bCancel)
{
	if (m_nStepCount > 0) {
		if (bCancel || (m_nStepCount == 1)) {
			m_pSink->Notify(EC_STEP_COMPLETE, bCancel ? TRUE : FALSE, 0);
			m_nStepCount = 0;
		} else {
			m_nStepCount--;
		}
	}
}

// IMFVideoPresenter
STDMETHODIMP CEVRAllocatorPresenter::ProcessMessage(MFVP_MESSAGE_TYPE eMessage, ULONG_PTR ulParam)
{
	switch (eMessage) {
		case MFVP_MESSAGE_BEGINSTREAMING :			// The EVR switched from stopped to paused. The presenter should allocate resources
			TRACE_EVR("EVR: MFVP_MESSAGE_BEGINSTREAMING\n");
			m_nRenderState = Paused;
			ResetQualProps();
			m_bStreamChanged = TRUE;
			break;

		case MFVP_MESSAGE_CANCELSTEP :				// Cancels a frame step
			TRACE_EVR("EVR: MFVP_MESSAGE_CANCELSTEP\n");
			CompleteFrameStep(true);
			break;

		case MFVP_MESSAGE_ENDOFSTREAM :				// All input streams have ended.
			TRACE_EVR("EVR: MFVP_MESSAGE_ENDOFSTREAM\n");
			m_bPendingMediaFinished = true;
			m_bStreamChanged = TRUE;
			break;

		case MFVP_MESSAGE_ENDSTREAMING :			// The EVR switched from running or paused to stopped. The presenter should free resources
			TRACE_EVR("EVR: MFVP_MESSAGE_ENDSTREAMING\n");
			break;

		case MFVP_MESSAGE_FLUSH :					// The presenter should discard any pending samples
			TRACE_EVR("EVR: MFVP_MESSAGE_FLUSH\n");
			SetEvent(m_hEvtFlush);
			m_bEvtFlush = true;
			while (WaitForSingleObject(m_hEvtFlush, 1) == WAIT_OBJECT_0);
			break;

		case MFVP_MESSAGE_INVALIDATEMEDIATYPE :		// The mixer's output format has changed. The EVR will initiate format negotiation, as described previously
			TRACE_EVR("EVR: MFVP_MESSAGE_INVALIDATEMEDIATYPE\n");
			/*
				1) The EVR sets the media type on the reference stream.
				2) The EVR calls IMFVideoPresenter::ProcessMessage on the presenter with the MFVP_MESSAGE_INVALIDATEMEDIATYPE message.
				3) The presenter sets the media type on the mixer's output stream.
				4) The EVR sets the media type on the substreams.
			*/
			if (!m_hRenderThread) {// handle things here
				CAutoLock lock2(&m_ImageProcessingLock);
				CAutoLock cRenderLock(&m_RenderLock);
				RenegotiateMediaType();
			} else {// leave it to the other thread
				m_hEvtRenegotiate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				EXECUTE_ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(m_hEvtRenegotiate, INFINITE));
				EXECUTE_ASSERT(CloseHandle(m_hEvtRenegotiate));
				m_hEvtRenegotiate = nullptr;
			}

			break;

		case MFVP_MESSAGE_PROCESSINPUTNOTIFY :		// One input stream on the mixer has received a new sample
			//		GetImageFromMixer();
			break;

		case MFVP_MESSAGE_STEP :					// Requests a frame step.
			TRACE_EVR("EVR: MFVP_MESSAGE_STEP\n");
			m_nStepCount = (int)ulParam;
			break;

		default:
			ASSERT(FALSE);
			break;
	}

	return S_OK;
}

HRESULT CEVRAllocatorPresenter::IsMediaTypeSupported(IMFMediaType* pMixerType)
{
	// We support only video types
	GUID MajorType;
	HRESULT hr = pMixerType->GetMajorType(&MajorType);

	if (SUCCEEDED(hr)) {
		if (MajorType != MFMediaType_Video) {
			hr = MF_E_INVALIDMEDIATYPE;
		}
	}

	// We support only progressive formats
	MFVideoInterlaceMode InterlaceMode = MFVideoInterlace_Unknown;

	if (SUCCEEDED(hr)) {
		hr = pMixerType->GetUINT32(MF_MT_INTERLACE_MODE, (UINT32*)&InterlaceMode);
	}

	if (SUCCEEDED(hr)) {
		if (InterlaceMode != MFVideoInterlace_Progressive) {
			hr = MF_E_INVALIDMEDIATYPE;
		}
	}

	// Check whether we support the surface format
	int Merit = 0;

	if (SUCCEEDED(hr)) {
		hr = GetMixerMediaTypeMerit(pMixerType, Merit);
	}

	return hr;
}

HRESULT CEVRAllocatorPresenter::CreateProposedOutputType(IMFMediaType* pMixerType, IMFMediaType* pMixerInputType, IMFMediaType** pType)
{
	HRESULT        hr;
	AM_MEDIA_TYPE* pAMMedia = nullptr;
	MFVIDEOFORMAT* VideoFormat;

	CHECK_HR(pMixerType->GetRepresentation(FORMAT_MFVideoFormat, (void**)&pAMMedia));

	VideoFormat = (MFVIDEOFORMAT*)pAMMedia->pbFormat;
	CHECK_HR(pfMFCreateVideoMediaType(VideoFormat, &m_pMediaType));

	m_pMediaType->SetUINT32(MF_MT_PAN_SCAN_ENABLED, 0);

	UINT32 nominalRange;
	pMixerInputType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange);

	const int iOutputRange = m_ExtraSets.iEVROutputRange;
	if (nominalRange == MFNominalRange_0_255) {
		m_pMediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, iOutputRange == 1 ? MFNominalRange_48_208 : MFNominalRange_16_235); // fix EVR bug
	} else {
		m_pMediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, iOutputRange == 1 ? MFNominalRange_16_235 : MFNominalRange_0_255);
	}
	m_LastSetOutputRange = iOutputRange;

	ULARGE_INTEGER ui64FrameSize;
	m_pMediaType->GetUINT64(MF_MT_FRAME_SIZE, &ui64FrameSize.QuadPart);

	CSize videoSize((LONG)ui64FrameSize.HighPart, (LONG)ui64FrameSize.LowPart);
	MFVideoArea Area = MakeArea(0, 0, videoSize.cx, videoSize.cy);
	m_pMediaType->SetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8*)&Area, sizeof(MFVideoArea));

	ULARGE_INTEGER ui64AspectRatio;
	m_pMediaType->GetUINT64(MF_MT_PIXEL_ASPECT_RATIO, &ui64AspectRatio.QuadPart);

	__int64 darx = (__int64)ui64AspectRatio.HighPart * videoSize.cx;
	__int64 dary = (__int64)ui64AspectRatio.LowPart * videoSize.cy;
	ReduceDim(darx, dary);
	CSize aspectRatio((LONG)darx, (LONG)dary);

	if (videoSize != m_nativeVideoSize || aspectRatio != m_aspectRatio) {
		m_nativeVideoSize = videoSize;
		m_aspectRatio = aspectRatio;

		// Notify the graph about the change
		if (m_pSink) {
			m_pSink->Notify(EC_VIDEO_SIZE_CHANGED, MAKELPARAM(m_nativeVideoSize.cx, m_nativeVideoSize.cy), 0);
		}
	}

	pMixerType->FreeRepresentation(FORMAT_MFVideoFormat, (LPVOID)pAMMedia);
	m_pMediaType->QueryInterface(IID_PPV_ARGS(pType));

	return hr;
}

HRESULT CEVRAllocatorPresenter::SetMediaType(IMFMediaType* pType)
{
	CheckPointer(pType, E_POINTER);

	HRESULT hr;
	AM_MEDIA_TYPE* pAMMedia = nullptr;

	CHECK_HR(pType->GetRepresentation(FORMAT_VideoInfo2, (LPVOID*)&pAMMedia));

	hr = InitializeDevice(pType);

	pType->FreeRepresentation(FORMAT_VideoInfo2, (LPVOID)pAMMedia);

	return hr;
}

HRESULT CEVRAllocatorPresenter::GetMediaTypeD3DFormat(IMFMediaType* pType, D3DFORMAT& d3dformat)
{
	GUID subtype = GUID_NULL;
	HRESULT hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);

	if (SUCCEEDED(hr) && subtype == FOURCCMap(subtype.Data1)) { // {xxxxxxxx-0000-0010-8000-00AA00389B71}
		d3dformat = (D3DFORMAT)subtype.Data1;
	}

	return hr;
}

HRESULT CEVRAllocatorPresenter::GetMixerMediaTypeMerit(IMFMediaType* pType, int& merit)
{
	D3DFORMAT mix_fmt;
	HRESULT hr = GetMediaTypeD3DFormat(pType, mix_fmt);

	if (SUCCEEDED(hr)) {
		// Information about actual YUV formats - http://msdn.microsoft.com/en-us/library/windows/desktop/dd206750%28v=vs.85%29.aspx

		// Test result
		// EVR input formats: NV12, YV12, YUY2, UYVY, AYUV, RGB32, ARGB32, AI44 and P010.
		// EVR mixer formats:
		// Intel: YUY2, X8R8G8B8, A8R8G8B8 (HD 4000).
		// Nvidia: NV12, YUY2, X8R8G8B8 (GTX 660Ti, GTX 960).
		// ATI/AMD: NV12, X8R8G8B8 (HD 5770)
		// for Win8+ A2R10G10B10 additional A2R10G10B10

		switch (mix_fmt) {
			case D3DFMT_A2R10G10B10:
				merit = (m_SurfaceFmt != D3DFMT_X8R8G8B8) ? 90 : 70;
				break;
			case D3DFMT_X8R8G8B8:
				merit = 80;
				break;
			case FCC('NV12'):
				merit = 60;
				break;
			case FCC('YUY2'):
				merit = 50;
				break;
			case FCC('P010'):
				merit = 40;
				break;
			// an accepted format, but fails on most surface types
			case D3DFMT_A8R8G8B8:
			default:
				merit = 0;
				return MF_E_INVALIDMEDIATYPE;
		}
	}

	return hr;
}

LPCWSTR CEVRAllocatorPresenter::GetMediaTypeFormatDesc(IMFMediaType* pMediaType)
{
	D3DFORMAT Format = D3DFMT_UNKNOWN;
	GetMediaTypeD3DFormat(pMediaType, Format);
	return D3DFormatToString(Format);
}

HRESULT CEVRAllocatorPresenter::RenegotiateMediaType()
{
	if (!m_pMixer) {
		return MF_E_INVALIDREQUEST;
	}

	CComPtr<IMFMediaType> pType;
	CComPtr<IMFMediaType> pMixerType;
	CComPtr<IMFMediaType> pMixerInputType;

	CInterfaceArray<IMFMediaType> ValidMixerTypes;

	// Get the mixer's input type
	HRESULT hr = m_pMixer->GetInputCurrentType(0, &pMixerInputType);
	if (SUCCEEDED(hr)) {
		AM_MEDIA_TYPE* pMT;
		hr = pMixerInputType->GetRepresentation(FORMAT_VideoInfo2, (LPVOID*)&pMT);
		if (SUCCEEDED(hr)) {
			m_inputMediaType = *pMT;
			pMixerInputType->FreeRepresentation(FORMAT_VideoInfo2, (LPVOID)pMT);
		}
	}

	// Loop through all of the mixer's proposed output types.
	DWORD iTypeIndex = 0;
	while ((hr != MF_E_NO_MORE_TYPES)) {
		pMixerType.Release();
		pType.Release();
		m_pMediaType.Release();

		// Step 1. Get the next media type supported by mixer.
		hr = m_pMixer->GetOutputAvailableType(0, iTypeIndex++, &pMixerType);
		if (FAILED(hr)) {
			break;
		}

		// Step 2. Check if we support this media type.
		if (SUCCEEDED(hr)) {
			hr = IsMediaTypeSupported(pMixerType);
		}

		if (SUCCEEDED(hr)) {
			hr = CreateProposedOutputType(pMixerType, pMixerInputType, &pType);
		}

		// Step 4. Check if the mixer will accept this media type.
		if (SUCCEEDED(hr)) {
			hr = m_pMixer->SetOutputType(0, pType, MFT_SET_TYPE_TEST_ONLY);
		}

		int Merit;
		if (SUCCEEDED(hr)) {
			hr = GetMixerMediaTypeMerit(pType, Merit);
		}

		if (SUCCEEDED(hr)) {
			size_t nTypes = ValidMixerTypes.GetCount();
			size_t iInsertPos = 0;
			for (size_t i = 0; i < nTypes; ++i) {
				int ThisMerit;
				GetMixerMediaTypeMerit(ValidMixerTypes[i], ThisMerit);

				if (Merit > ThisMerit) {
					iInsertPos = i;
					break;
				} else {
					iInsertPos = i+1;
				}
			}

			ValidMixerTypes.InsertAt(iInsertPos, pType);
		}
	}

#ifdef DEBUG_OR_LOG
	CString dbgmsg = L"EVR: Valid mixer output types:";
	for (size_t i = 0; i < ValidMixerTypes.GetCount(); ++i) {
		dbgmsg.AppendFormat(L"\n - %s", GetMediaTypeFormatDesc(ValidMixerTypes[i]));
	}
	DLog(dbgmsg);
#endif

	for (size_t i = 0; i < ValidMixerTypes.GetCount(); ++i) {
		// Step 3. Adjust the mixer's type to match our requirements.
		pType = ValidMixerTypes[i];

		TRACE_EVR("EVR: Trying mixer output type: %ws\n", GetMediaTypeFormatDesc(pType));

		// Step 5. Try to set the media type on ourselves.
		hr = SetMediaType(pType);

		// Step 6. Set output media type on mixer.
		if (SUCCEEDED(hr)) {
			hr = m_pMixer->SetOutputType(0, pType, 0);

			// If something went wrong, clear the media type.
			if (FAILED(hr)) {
				SetMediaType(nullptr);
			} else {
				m_bStreamChanged = FALSE;
				m_bChangeMT = true;
				DLog(L"EVR: Type %s selected for mixer output", GetMediaTypeFormatDesc(pType));
				break;
			}
		}
	}

	return hr;
}

bool CEVRAllocatorPresenter::GetImageFromMixer()
{
	MFT_OUTPUT_DATA_BUFFER Buffer;
	HRESULT                hr = S_OK;
	DWORD                  dwStatus = 0;
	REFERENCE_TIME         nsSampleTime = 0;
	LONGLONG               llClockBefore = 0;
	LONGLONG               llClockAfter  = 0;
	LONGLONG               llMixerLatency = 0;
	UINT32                 iSurface = 0;

	bool bDoneSomething = false;

	while (SUCCEEDED(hr)) {
		CComPtr<IMFSample> pSample;

		if (FAILED(GetFreeSample(&pSample))) {
			m_bWaitingSample = true;
			break;
		}

		memset(&Buffer, 0, sizeof(Buffer));
		Buffer.pSample = pSample;
		pSample->GetUINT32(GUID_SURFACE_INDEX, &iSurface);

		{
			llClockBefore = GetPerfCounter();
			hr = m_pMixer->ProcessOutput(0 , 1, &Buffer, &dwStatus);
			llClockAfter = GetPerfCounter();
		}

		//if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT ) { // <-- old code
		if (FAILED(hr)) {
			MoveToFreeList(pSample, false);
			DLogIf(hr != MF_E_TRANSFORM_NEED_MORE_INPUT, L"EVR: GetImageFromMixer failed with error %s", HR2Str(hr));
			break;
		}

		if (m_pSink) {
			//CAutoLock autolock(this); We shouldn't need to lock here, m_pSink is thread safe
			llMixerLatency = llClockAfter - llClockBefore;
			m_pSink->Notify(EC_PROCESSING_LATENCY, (LONG_PTR)&llMixerLatency, 0);
		}

		pSample->GetSampleTime(&nsSampleTime);
		REFERENCE_TIME nsDuration;
		pSample->GetSampleDuration(&nsDuration);

		if (m_ExtraSets.bTearingTest) {
			RECT rcTearing;

			rcTearing.left		= m_nTearingPos;
			rcTearing.top		= 0;
			rcTearing.right		= rcTearing.left + 4;
			rcTearing.bottom	= m_nativeVideoSize.cy;
			m_pDevice9Ex->ColorFill(m_pVideoSurfaces[iSurface], &rcTearing, D3DCOLOR_ARGB(255, 255, 0, 0));

			rcTearing.left		= (rcTearing.right + 15) % m_nativeVideoSize.cx;
			rcTearing.right		= rcTearing.left + 4;
			m_pDevice9Ex->ColorFill(m_pVideoSurfaces[iSurface], &rcTearing, D3DCOLOR_ARGB(255, 255, 0, 0));
			m_nTearingPos		= (m_nTearingPos + 7) % m_nativeVideoSize.cx;
		}

		TRACE_EVR("EVR: Get from Mixer : %u  (%I64d) (%I64d)\n", iSurface, nsSampleTime, m_rtTimePerFrame ? nsSampleTime / m_rtTimePerFrame : 0);

		MoveToScheduledList(pSample, false);
		bDoneSomething = true;
		if (m_rtTimePerFrame == 0) {
			break;
		}
	}

	return bDoneSomething;
}

STDMETHODIMP CEVRAllocatorPresenter::GetCurrentMediaType(__deref_out  IMFVideoMediaType **ppMediaType)
{
	HRESULT hr = S_OK;
	CAutoLock lock(this);  // Hold the critical section.

	CheckPointer(ppMediaType, E_POINTER);
	CHECK_HR(CheckShutdown());

	if (m_pMediaType == nullptr) {
		CHECK_HR(MF_E_NOT_INITIALIZED);
	}

	CHECK_HR(m_pMediaType->QueryInterface(__uuidof(IMFVideoMediaType), (void**)&ppMediaType));

	return hr;
}

// IMFTopologyServiceLookupClient
STDMETHODIMP CEVRAllocatorPresenter::InitServicePointers(/* [in] */ __in  IMFTopologyServiceLookup *pLookup)
{
	TRACE_EVR("EVR: CEVRAllocatorPresenter::InitServicePointers\n");

	HRESULT hr;
	DWORD dwObjects = 1;

	ASSERT(!m_pMixer);
	hr = pLookup->LookupService(MF_SERVICE_LOOKUP_GLOBAL, 0, MR_VIDEO_MIXER_SERVICE, __uuidof(IMFTransform), (void**)&m_pMixer, &dwObjects);
	if (FAILED(hr)) {
		ASSERT(0);
		return hr;
	}

	ASSERT(!m_pSink);
	hr = pLookup->LookupService(MF_SERVICE_LOOKUP_GLOBAL, 0, MR_VIDEO_RENDER_SERVICE, __uuidof(IMediaEventSink ), (void**)&m_pSink, &dwObjects);
	if (FAILED(hr)) {
		ASSERT(0);
		m_pMixer.Release();
		return hr;
	}

	ASSERT(!m_pClock);
	hr = pLookup->LookupService(MF_SERVICE_LOOKUP_GLOBAL, 0, MR_VIDEO_RENDER_SERVICE, __uuidof(IMFClock ), (void**)&m_pClock, &dwObjects);
	if (FAILED(hr)) {	// IMFClock can't be guaranteed to exist during first initialization. After negotiating the media type, it should initialize okay.
		return S_OK;
	}

	StartWorkerThreads();
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::ReleaseServicePointers()
{
	TRACE_EVR("EVR: CEVRAllocatorPresenter::ReleaseServicePointers\n");
	StopWorkerThreads();
	m_pMixer.Release();
	m_pSink.Release();
	m_pClock.Release();
	return S_OK;
}

// IMFVideoDeviceID
STDMETHODIMP CEVRAllocatorPresenter::GetDeviceID(/* [out] */ __out  IID *pDeviceID)
{
	CheckPointer(pDeviceID, E_POINTER);
	*pDeviceID = IID_IDirect3DDevice9;
	return S_OK;
}

// IMFGetService
STDMETHODIMP CEVRAllocatorPresenter::GetService(/* [in] */ __RPC__in REFGUID guidService,
												/* [in] */ __RPC__in REFIID riid,
												/* [iid_is][out] */ __RPC__deref_out_opt LPVOID *ppvObject)
{
	if (guidService == MR_VIDEO_RENDER_SERVICE) {
		return NonDelegatingQueryInterface (riid, ppvObject);
	} else if (guidService == MR_VIDEO_ACCELERATION_SERVICE) {
		return m_pD3DManager->QueryInterface (__uuidof(IDirect3DDeviceManager9), (void**) ppvObject);
	}

	return E_NOINTERFACE;
}

// IMFAsyncCallback

STDMETHODIMP CEVRAllocatorPresenter::GetParameters(/* [out] */ __RPC__out DWORD *pdwFlags, /* [out] */ __RPC__out DWORD *pdwQueue)
{
	return E_NOTIMPL;
}

STDMETHODIMP CEVRAllocatorPresenter::Invoke(/* [in] */ __RPC__in_opt IMFAsyncResult *pAsyncResult)
{
	return E_NOTIMPL;
}

// IMFVideoDisplayControl

STDMETHODIMP CEVRAllocatorPresenter::GetNativeVideoSize(SIZE *pszVideo, SIZE *pszARVideo)
{
	if (pszVideo) {
		pszVideo->cx = m_nativeVideoSize.cx;
		pszVideo->cy = m_nativeVideoSize.cy;
	}
	if (pszARVideo) {
		pszARVideo->cx = m_nativeVideoSize.cx * m_aspectRatio.cx;
		pszARVideo->cy = m_nativeVideoSize.cy * m_aspectRatio.cy;
	}
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetIdealVideoSize(SIZE *pszMin, SIZE *pszMax)
{
	if (pszMin) {
		pszMin->cx = 1;
		pszMin->cy = 1;
	}

	if (pszMax) {
		D3DDISPLAYMODE d3ddm = { 0 };
		if (SUCCEEDED(m_pD3D9Ex->GetAdapterDisplayMode(GetAdapter(m_pD3D9Ex), &d3ddm))) {
			pszMax->cx = d3ddm.Width;
			pszMax->cy = d3ddm.Height;
		}
	}

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetVideoPosition(const MFVideoNormalizedRect *pnrcSource, const LPRECT prcDest)
{
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetVideoPosition(MFVideoNormalizedRect *pnrcSource, LPRECT prcDest)
{
	// Always all source rectangle ?
	if (pnrcSource) {
		pnrcSource->left	= 0.0;
		pnrcSource->top		= 0.0;
		pnrcSource->right	= 1.0;
		pnrcSource->bottom	= 1.0;
	}

	if (prcDest) {
		memcpy (prcDest, &m_videoRect, sizeof(m_videoRect));    //GetClientRect (m_hWnd, prcDest);
	}

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetAspectRatioMode(DWORD dwAspectRatioMode)
{
	m_dwVideoAspectRatioMode = (MFVideoAspectRatioMode)dwAspectRatioMode;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetAspectRatioMode(DWORD *pdwAspectRatioMode)
{
	CheckPointer(pdwAspectRatioMode, E_POINTER);
	*pdwAspectRatioMode = m_dwVideoAspectRatioMode;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetVideoWindow(HWND hwndVideo)
{
	CAutoLock lock(this);
	CAutoLock lock2(&m_ImageProcessingLock);
	CAutoLock cRenderLock(&m_RenderLock);

	if (m_hWnd != hwndVideo) {
		m_hWnd = hwndVideo;
		m_bPendingResetDevice = true;
		m_bNeedCreateWindow = !m_bPreviewMode;
		SendResetRequest();
	}
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetVideoWindow(HWND *phwndVideo)
{
	CheckPointer(phwndVideo, E_POINTER);
	*phwndVideo = m_hWnd;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::RepaintVideo()
{
	Paint(true);
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetCurrentImage(BITMAPINFOHEADER *pBih, BYTE **pDib, DWORD *pcbDib, LONGLONG *pTimeStamp)
{
	if (!pBih || !pDib || !pcbDib) {
		return E_POINTER;
	}
	CheckPointer(m_pDevice9Ex, E_ABORT);

	HRESULT hr = S_OK;
	const unsigned width  = m_windowRect.Width();
	const unsigned height = m_windowRect.Height();
	const unsigned len = width * height * 4;

	memset(pBih, 0, sizeof(BITMAPINFOHEADER));
	pBih->biSize      = sizeof(BITMAPINFOHEADER);
	pBih->biWidth     = width;
	pBih->biHeight    = height;
	pBih->biBitCount  = 32;
	pBih->biPlanes    = 1;
	pBih->biSizeImage = DIBSIZE(*pBih);

	BYTE* p = (BYTE*)CoTaskMemAlloc(len); // only this allocator can be used
	if (!p) {
		return E_OUTOFMEMORY;
	}

	CComPtr<IDirect3DSurface9> pBackBuffer;
	CComPtr<IDirect3DSurface9> pDestSurface;
	D3DLOCKED_RECT r;
	if (FAILED(hr = m_pDevice9Ex->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))
			|| FAILED(hr = m_pDevice9Ex->CreateRenderTarget(width, height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &pDestSurface, nullptr))
			|| (FAILED(hr = m_pDevice9Ex->StretchRect(pBackBuffer, m_windowRect, pDestSurface, nullptr, D3DTEXF_NONE)))
			|| (FAILED(hr = pDestSurface->LockRect(&r, nullptr, D3DLOCK_READONLY)))) {
		DLog(L"CEVRAllocatorPresenter::GetCurrentImage filed : %s", GetWindowsErrorMessage(hr, m_hD3D9));
		CoTaskMemFree(p);
		return hr;
	}

	RetrieveBitmapData(width, height, 32, p, (BYTE*)r.pBits, r.Pitch);

	pDestSurface->UnlockRect();

	*pDib = p;
	*pcbDib = len;

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetBorderColor(COLORREF Clr)
{
	m_BorderColor = Clr;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetBorderColor(COLORREF *pClr)
{
	CheckPointer(pClr, E_POINTER);
	*pClr = m_BorderColor;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetRenderingPrefs(DWORD dwRenderFlags)
{
	m_dwVideoRenderPrefs = (MFVideoRenderPrefs)dwRenderFlags;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetRenderingPrefs(DWORD *pdwRenderFlags)
{
	CheckPointer(pdwRenderFlags, E_POINTER);
	*pdwRenderFlags = m_dwVideoRenderPrefs;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetFullscreen(BOOL fFullscreen)
{
	ASSERT(FALSE);
	return E_NOTIMPL;
}

STDMETHODIMP CEVRAllocatorPresenter::GetFullscreen(BOOL *pfFullscreen)
{
	ASSERT(FALSE);
	return E_NOTIMPL;
}

// IMFVideoMixerBitmap
STDMETHODIMP CEVRAllocatorPresenter::ClearAlphaBitmap()
{
	CAutoLock cRenderLock(&m_RenderLock);
	m_bAlphaBitmapEnable = false;

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::GetAlphaBitmapParameters(MFVideoAlphaBitmapParams *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRenderLock(&m_RenderLock);

	if (m_bAlphaBitmapEnable && m_pAlphaBitmapTexture) {
		*pBmpParms = m_AlphaBitmapParams; // formal implementation, don't believe it
		return S_OK;
	} else {
		return MF_E_NOT_INITIALIZED;
	}
}

STDMETHODIMP CEVRAllocatorPresenter::SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRenderLock(&m_RenderLock);

	CheckPointer(m_pDevice9Ex, E_ABORT);
	HRESULT hr = S_OK;

	if (pBmpParms->GetBitmapFromDC && pBmpParms->bitmap.hdc) {
		HBITMAP hBitmap = (HBITMAP)GetCurrentObject(pBmpParms->bitmap.hdc, OBJ_BITMAP);
		if (!hBitmap) {
			return E_INVALIDARG;
		}
		DIBSECTION info = { 0 };
		if (!::GetObjectW(hBitmap, sizeof(DIBSECTION), &info)) {
			return E_INVALIDARG;
		}
		BITMAP& bm = info.dsBm;
		if (!bm.bmWidth || !bm.bmHeight || bm.bmBitsPixel != 32 || !bm.bmBits) {
			return E_INVALIDARG;
		}

		if (m_pAlphaBitmapTexture) {
			D3DSURFACE_DESC desc = {};
			m_pAlphaBitmapTexture->GetLevelDesc(0, &desc);
			if (bm.bmWidth != desc.Width || bm.bmHeight != desc.Height) {
				m_pAlphaBitmapTexture.Release();
			}
		}

		if (!m_pAlphaBitmapTexture) {
			hr = m_pDevice9Ex->CreateTexture(bm.bmWidth, bm.bmHeight, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pAlphaBitmapTexture, nullptr);
		}

		if (SUCCEEDED(hr)) {
			CComPtr<IDirect3DSurface9> pSurface;
			hr = m_pAlphaBitmapTexture->GetSurfaceLevel(0, &pSurface);
			if (SUCCEEDED(hr)) {
				D3DLOCKED_RECT lr;
				hr = pSurface->LockRect(&lr, nullptr, D3DLOCK_DISCARD);
				if (S_OK == hr) {
					if (bm.bmWidthBytes == lr.Pitch) {
						memcpy(lr.pBits, bm.bmBits, bm.bmWidthBytes * bm.bmHeight);
					}
					else {
						LONG linesize = std::min(bm.bmWidthBytes, (LONG)lr.Pitch);
						BYTE* src = (BYTE*)bm.bmBits;
						BYTE* dst = (BYTE*)lr.pBits;
						for (LONG y = 0; y < bm.bmHeight; ++y) {
							memcpy(dst, src, linesize);
							src += bm.bmWidthBytes;
							dst += lr.Pitch;
						}
					}
					hr = pSurface->UnlockRect();
				}
			}
		}
	} else {
		return E_INVALIDARG;
	}

	m_bAlphaBitmapEnable = SUCCEEDED(hr) && m_pAlphaBitmapTexture;

	if (m_bAlphaBitmapEnable) {
		hr = UpdateAlphaBitmapParameters(&pBmpParms->params);
	}

	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRenderLock(&m_RenderLock);

	m_AlphaBitmapParams = *pBmpParms; // formal implementation, don't believe it

	return S_OK;
}

// IEVRTrustedVideoPlugin
STDMETHODIMP CEVRAllocatorPresenter::IsInTrustedVideoMode(BOOL *pYes)
{
	CheckPointer(pYes, E_POINTER);
	*pYes = TRUE;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::CanConstrict(BOOL *pYes)
{
	CheckPointer(pYes, E_POINTER);
	*pYes = TRUE;
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::SetConstriction(DWORD dwKPix)
{
	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::DisableImageExport(BOOL bDisable)
{
	return S_OK;
}

// IDirect3DDeviceManager9
STDMETHODIMP CEVRAllocatorPresenter::ResetDevice(IDirect3DDevice9 *pDevice, UINT resetToken)
{
	HRESULT hr = m_pD3DManager->ResetDevice(pDevice, resetToken);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::OpenDeviceHandle(HANDLE *phDevice)
{
	HRESULT hr = m_pD3DManager->OpenDeviceHandle(phDevice);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::CloseDeviceHandle(HANDLE hDevice)
{
	HRESULT hr = m_pD3DManager->CloseDeviceHandle(hDevice);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::TestDevice(HANDLE hDevice)
{
	HRESULT hr = m_pD3DManager->TestDevice(hDevice);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::LockDevice(HANDLE hDevice, IDirect3DDevice9 **ppDevice, BOOL fBlock)
{
	HRESULT hr = m_pD3DManager->LockDevice(hDevice, ppDevice, fBlock);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::UnlockDevice(HANDLE hDevice, BOOL fSaveState)
{
	HRESULT hr = m_pD3DManager->UnlockDevice(hDevice, fSaveState);
	return hr;
}

STDMETHODIMP CEVRAllocatorPresenter::GetVideoService(HANDLE hDevice, REFIID riid, void **ppService)
{
	HRESULT hr = m_pD3DManager->GetVideoService(hDevice, riid, ppService);

	if (riid == __uuidof(IDirectXVideoDecoderService)) {
		UINT  nNbDecoder = 5;
		GUID* pDecoderGuid;
		IDirectXVideoDecoderService* pDXVAVideoDecoder = (IDirectXVideoDecoderService*)(*ppService);
		pDXVAVideoDecoder->GetDecoderDeviceGuids (&nNbDecoder, &pDecoderGuid);
	} else if (riid == __uuidof(IDirectXVideoProcessorService)) {
		IDirectXVideoProcessorService* pDXVAProcessor = (IDirectXVideoProcessorService*)(*ppService);
		UNREFERENCED_PARAMETER(pDXVAProcessor);
	}

	return hr;
}

// IMediaSideData
STDMETHODIMP CEVRAllocatorPresenter::SetSideData(GUID guidType, const BYTE *pData, size_t size)
{
	CheckPointer(pData, E_POINTER);
	if (guidType == IID_MediaOffset3D && size == sizeof(MediaOffset3D)) {
		std::unique_lock<std::mutex> lock(m_mutexOffsetQueue);
		MediaOffset3D offset3D;
		memcpy(&offset3D, pData, size);
		offset3D.timestamp += g_tSegmentStart;
		m_mediaOffsetQueue.push_back(offset3D);
		return S_OK;
	}

	return E_FAIL;
}

STDMETHODIMP CEVRAllocatorPresenter::GetSideData(GUID guidType, const BYTE **pData, size_t *pSize)
{
	return E_NOTIMPL;
}

// IPlaybackNotify

STDMETHODIMP CEVRAllocatorPresenter::Stop()
{
	for (unsigned i = 0; i < m_nSurfaces; i++) {
		if (m_pVideoSurfaces[i]) {
			m_pDevice9Ex->ColorFill(m_pVideoSurfaces[i], nullptr, 0);
		}
	}

	return S_OK;
}

STDMETHODIMP CEVRAllocatorPresenter::InitializeDevice(IMFMediaType* pMediaType)
{
	HRESULT hr;
	CAutoLock lock(this);
	CAutoLock lock2(&m_ImageProcessingLock);
	CAutoLock cRenderLock(&m_RenderLock);

	// Retrieve the surface size and format
	UINT32 width;
	UINT32 height;
	hr = MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &width, &height);

	if (SUCCEEDED(hr)) {
		CSize frameSize(width, height);
		m_bStreamChanged |= m_nativeVideoSize != frameSize;
		m_nativeVideoSize = frameSize;
		D3DFORMAT Format;
		hr = GetMediaTypeD3DFormat(pMediaType, Format);
	}

	if (!m_bStreamChanged && m_pVideoTextures[0]) {
		D3DSURFACE_DESC desc;
		if (SUCCEEDED(m_pVideoTextures[0]->GetLevelDesc(0, &desc))) {
			if (desc.Width != (UINT)m_nativeVideoSize.cx || desc.Height != (UINT)m_nativeVideoSize.cy) {
				m_bStreamChanged = TRUE;
			}
		}
	}

	if (m_bStreamChanged && SUCCEEDED(hr)) {
		DeleteSurfaces();
		RemoveAllSamples();
		hr = AllocSurfaces();

		if (SUCCEEDED(hr)) {
			for (unsigned i = 0; i < m_nSurfaces; i++) {
				CComPtr<IMFSample> pMFSample;
				hr = pfMFCreateVideoSampleFromSurface(m_pVideoSurfaces[i], &pMFSample);

				if (SUCCEEDED(hr)) {
					pMFSample->SetUINT32(GUID_SURFACE_INDEX, i);
					m_FreeSamples.emplace_back(pMFSample);
				}
				ASSERT(SUCCEEDED(hr));
			}
		}
	}

	{
		m_bMVC_Base_View_R_flag = false;
		m_stereo_subtitle_offset_ids.clear();

		CComPtr<IBaseFilter> pBF;
		if (SUCCEEDED(m_pOuterEVR->QueryInterface(IID_PPV_ARGS(&pBF)))) {
			while (pBF = GetUpStreamFilter(pBF)) {
				if (CComQIPtr<IPropertyBag> pPB = pBF.p) {
					CComVariant var;

					if (SUCCEEDED(pPB->Read(L"STEREOSCOPIC3DMODE", &var, nullptr)) && var.vt == VT_BSTR) {
						CString mode(var.bstrVal); mode.MakeLower();
						m_bMVC_Base_View_R_flag = mode == L"mvc_rl";
					}
					var.Clear();

					if (SUCCEEDED(pPB->Read(L"stereo_subtitle_offset_ids", &var, nullptr)) && var.vt == VT_BSTR) {
						const CString offset_ids(var.bstrVal);
						std::list<CString> list;
						Explode(offset_ids, list, L',');
						for (const auto& str : list) {
							const int value = _wtoi(str);
							m_stereo_subtitle_offset_ids.emplace_back(value);
						}
					}
				}

				m_pSS = pBF;
			}
		}
	}

	return hr;
}

DWORD WINAPI CEVRAllocatorPresenter::GetMixerThreadStatic(LPVOID lpParam)
{
	SetThreadName(DWORD_MAX, "CEVRPresenter::MixerThread");
	CEVRAllocatorPresenter*	pThis = (CEVRAllocatorPresenter*)lpParam;
	pThis->GetMixerThread();
	return 0;
}

DWORD WINAPI CEVRAllocatorPresenter::PresentThread(LPVOID lpParam)
{
	SetThreadName(DWORD_MAX, "CEVRPresenter::PresentThread");
	CEVRAllocatorPresenter* pThis = (CEVRAllocatorPresenter*)lpParam;
	pThis->RenderThread();
	return 0;
}

void CEVRAllocatorPresenter::CheckWaitingSampleFromMixer()
{
	if (m_bWaitingSample) {
		m_bWaitingSample = false;
		//GetImageFromMixer(); // Do this in processing thread instead
	}
}

void CEVRAllocatorPresenter::GetMixerThread()
{
	bool     bQuit = false;

	TIMECAPS tc = {};
	timeGetDevCaps(&tc, sizeof(TIMECAPS));
	const UINT wTimerRes = std::max(tc.wPeriodMin, 1u);
	timeBeginPeriod(wTimerRes);

	while (!bQuit) {
		DWORD dwObject = WaitForSingleObject(m_hEvtQuit, 1);
		switch (dwObject) {
			case WAIT_OBJECT_0: // Quit
				bQuit = true;
				break;
			case WAIT_TIMEOUT: {
				if (m_nRenderState == Stopped) {
					continue; // nothing sensible to do here
				}

				bool bDoneSomething = false;
				{
					CAutoLock AutoLock(&m_ImageProcessingLock);
					bDoneSomething = GetImageFromMixer();
				}
				if ((m_rtTimePerFrame == 0 && bDoneSomething) || m_bChangeMT) {
					//CAutoLock lock(this);
					//CAutoLock lock2(&m_ImageProcessingLock);
					//CAutoLock cRenderLock(&m_RenderLock);

					CComPtr<IPin> pPin;
					if (m_pOuterEVR->FindPin(L"EVR Input0", &pPin) == S_OK) {
						OnChangeInput(pPin);
					}

					// Update internal subtitle clock
					if (m_fUseInternalTimer && m_pSubPicQueue) {
						m_fps = 10000000.0 / m_rtTimePerFrame;
						m_pSubPicQueue->SetFPS(m_fps);
					}

					m_bChangeMT = false;
				}
			}
			break;
		}
	}

	timeEndPeriod(wTimerRes);
}

void ModerateFloat(double& Value, double Target, double& ValuePrim, double ChangeSpeed)
{
	double xbiss = (-(ChangeSpeed)*(ValuePrim) - (Value-Target)*(ChangeSpeed*ChangeSpeed)*0.25f);
	ValuePrim += xbiss;
	Value += ValuePrim;
}

LONGLONG CEVRAllocatorPresenter::GetClockTime(LONGLONG PerformanceCounter)
{
	LONGLONG			llClockTime;
	MFTIME				nsCurrentTime;
	m_pClock->GetCorrelatedTime(0, &llClockTime, &nsCurrentTime);
	DWORD Characteristics = 0;
	m_pClock->GetClockCharacteristics(&Characteristics);
	MFCLOCK_STATE State;
	m_pClock->GetState(0, &State);

	if (!(Characteristics & MFCLOCK_CHARACTERISTICS_FLAG_FREQUENCY_10MHZ)) {
		MFCLOCK_PROPERTIES Props;
		if (m_pClock->GetProperties(&Props) == S_OK) {
			llClockTime = (llClockTime * 10000000) / Props.qwClockFrequency;    // Make 10 MHz
		}

	}
	LONGLONG llPerf = PerformanceCounter;
	//	return llClockTime + (llPerf - nsCurrentTime);
	double Target = llClockTime + (llPerf - nsCurrentTime) * m_ModeratedTimeSpeed;

	bool bReset = false;
	if (m_ModeratedTimeLast < 0 || State != m_LastClockState || m_ModeratedClockLast < 0) {
		bReset = true;
		m_ModeratedTimeLast = llPerf;
		m_ModeratedClockLast = llClockTime;
	}

	m_LastClockState = State;

	LONGLONG TimeChangeM = llPerf - m_ModeratedTimeLast;
	LONGLONG ClockChangeM = llClockTime - m_ModeratedClockLast;
	UNREFERENCED_PARAMETER(ClockChangeM);

	m_ModeratedTimeLast = llPerf;
	m_ModeratedClockLast = llClockTime;

#if 1

	if (bReset) {
		m_ModeratedTimeSpeed = 1.0;
		m_ModeratedTimeSpeedPrim = 0.0;
		ZeroMemory(m_TimeChangeHistory, sizeof(m_TimeChangeHistory));
		ZeroMemory(m_ClockChangeHistory, sizeof(m_ClockChangeHistory));
		m_ClockTimeChangeHistoryPos = 0;
	}
	if (TimeChangeM) {
		unsigned Pos = m_ClockTimeChangeHistoryPos % 100u;
		unsigned nHistory = std::min(m_ClockTimeChangeHistoryPos, 100u);
		++m_ClockTimeChangeHistoryPos;
		if (nHistory > 50) {
			unsigned iLastPos = (nHistory < 100) ? 0 : Pos;

			double TimeChange = llPerf - m_TimeChangeHistory[iLastPos];
			double ClockChange = llClockTime - m_ClockChangeHistory[iLastPos];

			double ClockSpeedTarget = ClockChange / TimeChange;
			double ChangeSpeed = 0.1;
			if (ClockSpeedTarget > m_ModeratedTimeSpeed) {
				if (ClockSpeedTarget / m_ModeratedTimeSpeed > 0.1) {
					ChangeSpeed = 0.1;
				} else {
					ChangeSpeed = 0.01;
				}
			} else {
				if (m_ModeratedTimeSpeed / ClockSpeedTarget > 0.1) {
					ChangeSpeed = 0.1;
				} else {
					ChangeSpeed = 0.01;
				}
			}
			ModerateFloat(m_ModeratedTimeSpeed, ClockSpeedTarget, m_ModeratedTimeSpeedPrim, ChangeSpeed);
			//m_ModeratedTimeSpeed = TimeChange / ClockChange;
		}
		m_TimeChangeHistory[Pos] = (double)llPerf;
		m_ClockChangeHistory[Pos] = (double)llClockTime;
	}

	return (LONGLONG)Target;
#else
	double EstimateTime = m_ModeratedTime + TimeChange * m_ModeratedTimeSpeed + m_ClockDiffCalc;
	double Diff = Target - EstimateTime;

	// > 5 ms just set it
	if ((fabs(Diff) > 50000.0 || bReset)) {

		//TRACE_EVR("EVR: Reset clock at diff: %f ms\n", (m_ModeratedTime - Target) /10000.0);
		if (State == MFCLOCK_STATE_RUNNING) {
			if (bReset) {
				m_ModeratedTimeSpeed = 1.0;
				m_ModeratedTimeSpeedPrim = 0.0;
				m_ClockDiffCalc = 0;
				m_ClockDiffPrim = 0;
				m_ModeratedTime = Target;
				m_ModeratedTimer = llPerf;
			} else {
				EstimateTime = m_ModeratedTime + TimeChange * m_ModeratedTimeSpeed;
				Diff = Target - EstimateTime;
				m_ClockDiffCalc = Diff;
				m_ClockDiffPrim = 0;
			}
		} else {
			m_ModeratedTimeSpeed = 0.0;
			m_ModeratedTimeSpeedPrim = 0.0;
			m_ClockDiffCalc = 0;
			m_ClockDiffPrim = 0;
			m_ModeratedTime = Target;
			m_ModeratedTimer = llPerf;
		}
	}

	{
		LONGLONG ModerateTime = 10000;
		double ChangeSpeed = 1.00;
		/*		if (m_ModeratedTimeSpeedPrim != 0.0)
				{
					if (m_ModeratedTimeSpeedPrim < 0.1)
						ChangeSpeed = 0.1;
				}*/

		int nModerate = 0;
		double Change = 0;
		while (m_ModeratedTimer < llPerf - ModerateTime) {
			m_ModeratedTimer += ModerateTime;
			m_ModeratedTime += double(ModerateTime) * m_ModeratedTimeSpeed;

			double TimerDiff = llPerf - m_ModeratedTimer;

			double Diff = (double)(m_ModeratedTime - (Target - TimerDiff));

			double TimeSpeedTarget;
			double AbsDiff = fabs(Diff);
			TimeSpeedTarget = 1.0 - (Diff / 1000000.0);
			//			TimeSpeedTarget = m_ModeratedTimeSpeed - (Diff / 100000000000.0);
			//if (AbsDiff > 20000.0)
			//				TimeSpeedTarget = 1.0 - (Diff / 1000000.0);
			/*else if (AbsDiff > 5000.0)
				TimeSpeedTarget = 1.0 - (Diff / 100000000.0);
			else
				TimeSpeedTarget = 1.0 - (Diff / 500000000.0);*/
			double StartMod = m_ModeratedTimeSpeed;
			ModerateFloat(m_ModeratedTimeSpeed, TimeSpeedTarget, m_ModeratedTimeSpeedPrim, ChangeSpeed);
			m_ModeratedTimeSpeed = TimeSpeedTarget;
			++nModerate;
			Change += m_ModeratedTimeSpeed - StartMod;
		}
		if (nModerate) {
			m_ModeratedTimeSpeedDiff = Change / nModerate;
		}

		double Ret = m_ModeratedTime + double(llPerf - m_ModeratedTimer) * m_ModeratedTimeSpeed;
		double Diff = Target - Ret;
		ModerateFloat(m_ClockDiffCalc, Diff, m_ClockDiffPrim, ChangeSpeed*0.1);

		Ret += m_ClockDiffCalc;
		Diff = Target - Ret;
		m_ClockDiff = Diff;
		return LONGLONG(Ret + 0.5);
	}

	return Target;
	return LONGLONG(m_ModeratedTime + 0.5);
#endif
}

void CEVRAllocatorPresenter::OnVBlankFinished(bool fAll, LONGLONG PerformanceCounter)
{
	if (!m_OrderedPaint || !fAll) {
		return;
	}

	LONGLONG nsSampleTime   = m_CurrentSampleTime;
	LONGLONG SampleDuration = m_CurrentSampleDuration;
	if (SampleDuration < 0) {
		return;
	}

	LONGLONG llClockTime;
	if (!m_bSignaledStarvation) {
		llClockTime = GetClockTime(PerformanceCounter);
		m_StarvationClock = llClockTime;
	} else {
		llClockTime = m_StarvationClock;
	}

	if (nsSampleTime < 0) {
		nsSampleTime = llClockTime;
	}

	LONGLONG TimePerFrame = m_rtTimePerFrame;
	if (!TimePerFrame) {
		return;
	}
	if (SampleDuration > 1) {
		TimePerFrame = SampleDuration;
	}
	{
		m_nNextSyncOffset = (m_nNextSyncOffset+1) % NB_JITTER;
		LONGLONG SyncOffset = nsSampleTime - llClockTime;

		m_pllSyncOffset[m_nNextSyncOffset] = SyncOffset;
		//TRACE_EVR("EVR: SyncOffset(%u, %d): %8I64d     %8I64d     %8I64d \n", m_iCurSurface, m_VSyncMode, m_LastPredictedSync, -SyncOffset, m_LastPredictedSync - (-SyncOffset));

		m_MaxSyncOffset = MINLONG64;
		m_MinSyncOffset = MAXLONG64;

		LONGLONG AvrageSum = 0;
		for (int i=0; i<NB_JITTER; i++) {
			LONGLONG Offset = m_pllSyncOffset[i];
			AvrageSum += Offset;
			expand_range(Offset, m_MinSyncOffset, m_MaxSyncOffset);
		}
		double MeanOffset = double(AvrageSum)/NB_JITTER;
		double DeviationSum = 0;
		for (int i=0; i<NB_JITTER; i++) {
			double Deviation = double(m_pllSyncOffset[i]) - MeanOffset;
			DeviationSum += Deviation*Deviation;
		}
		double StdDev = sqrt(DeviationSum/NB_JITTER);

		m_fSyncOffsetAvr = MeanOffset;
		m_bSyncStatsAvailable = true;
		m_fSyncOffsetStdDev = StdDev;
	}
}

void CEVRAllocatorPresenter::RenderThread()
{
	HANDLE   hEvts[]      = { m_hEvtQuit, m_hEvtFlush, g_hNewSegmentEvent };
	bool     bQuit        = false;
	bool     bForcePaint  = true; // needs to be true in some rare cases
	MFTIME   nsSampleTime = 0;
	LONGLONG llClockTime;
	DWORD    dwObject;

	// Tell Multimedia Class Scheduler we are a playback thread (increase priority)
	HANDLE hAvrt = 0;
	if (pfAvSetMmThreadCharacteristicsW) {
		DWORD dwTaskIndex = 0;
		hAvrt = pfAvSetMmThreadCharacteristicsW (L"Playback", &dwTaskIndex);
		if (pfAvSetMmThreadPriority) {
			pfAvSetMmThreadPriority (hAvrt, AVRT_PRIORITY_HIGH /*AVRT_PRIORITY_CRITICAL*/);
		}
	}

	TIMECAPS tc = {};
	timeGetDevCaps(&tc, sizeof(TIMECAPS));
	const UINT wTimerRes = std::max(tc.wPeriodMin, 1u);
	timeBeginPeriod(wTimerRes);

	auto SubPicSetTime = [&] {
		if (!g_bExternalSubtitleTime) {
			CAllocatorPresenterImpl::SetTime(g_tSegmentStart + nsSampleTime * (g_bExternalSubtitle ? g_dRate : 1));
		}
	};

	int NextSleepTime = 1;
	while (!bQuit) {
		LONGLONG llPerf = GetPerfCounter();
		UNREFERENCED_PARAMETER(llPerf);
		dwObject = WaitForMultipleObjects(std::size(hEvts), hEvts, FALSE, std::max(NextSleepTime < 0 ? 1 : NextSleepTime, 0));
		if (m_hEvtRenegotiate) {
			CAutoLock Lock(&m_csExternalMixerLock);
			CAutoLock cRenderLock(&m_RenderLock);
			RenegotiateMediaType();
			SetEvent(m_hEvtRenegotiate);
		}
		if (NextSleepTime > 1) {
			NextSleepTime = 0;
		} else if (NextSleepTime == 0) {
			NextSleepTime = -1;
		}
		switch (dwObject) {
			case WAIT_OBJECT_0: // Quit
				bQuit = true;
				break;
			case WAIT_OBJECT_0 + 1: // Flush
				// Flush pending samples!
				FlushSamples();
				m_bEvtFlush = false;
				ResetEvent(m_hEvtFlush);
				bForcePaint = true;
				TRACE_EVR("EVR: Flush done!\n");
				break;
			case WAIT_OBJECT_0 + 2: // NewSegment
				{
					std::unique_lock<std::mutex> lock(m_mutexOffsetQueue);
					m_mediaOffsetQueue.clear();
				}
				TRACE_EVR("EVR: NewSegment\n");
				break;
			case WAIT_TIMEOUT :
				if (m_LastSetOutputRange != -1 && m_LastSetOutputRange != m_ExtraSets.iEVROutputRange) {
					{
						CAutoLock Lock(&m_csExternalMixerLock);
						CAutoLock cRenderLock(&m_RenderLock);
						FlushSamples();
						RenegotiateMediaType();
					}
				}
				if (m_bPendingResetDevice) {
					SendResetRequest();
				}

				// Discard timer events if playback stop
				//if ((dwObject == WAIT_OBJECT_0 + 3) && (m_nRenderState != Started)) continue;

				//TRACE_EVR("EVR: RenderThread ==>> Waiting buffer\n");

				//if (WaitForMultipleObjects(std::size(hEvtsBuff), hEvtsBuff, FALSE, INFINITE) == WAIT_OBJECT_0+2)
				{
					CComPtr<IMFSample> pMFSample;
					LONGLONG	llPerf = GetPerfCounter();
					UNREFERENCED_PARAMETER(llPerf);
					int nSamplesLeft = 0;
					if (SUCCEEDED(GetScheduledSample(&pMFSample, nSamplesLeft))) {
						//pMFSample->GetUINT32(GUID_SURFACE_INDEX, &m_iCurSurface);

						LONGLONG SampleDuration = 0;

						HRESULT hrGetSampleTime = pMFSample->GetSampleTime(&nsSampleTime);
						pMFSample->GetSampleDuration(&SampleDuration); // We assume that all samples have the same duration
						{
							m_CurrentSampleTime = nsSampleTime;
							m_CurrentSampleDuration = SampleDuration;
						}

						// The sample does not have a presentation time or first sample
						const bool bForcePaint2 = (hrGetSampleTime != S_OK || nsSampleTime == 0);

						//TRACE_EVR("EVR: RenderThread ==>> Presenting surface %u  (%I64d)\n", m_iCurSurface, nsSampleTime);

						bool bStepForward = false;

						if (m_nStepCount < 0) {
							// Drop frame
							TRACE_EVR("EVR: Dropped frame\n");
							m_pcFrames++;
							bStepForward = true;
							m_nStepCount = 0;
							/*
							} else if (m_nStepCount > 0) {
								pMFSample->GetUINT32(GUID_SURFACE_INDEX, &m_iCurSurface);
								++m_OrderedPaint;
								SubPicSetTime();
								Paint(true);
								m_nDroppedUpdate = 0;
								CompleteFrameStep (false);
								bStepForward = true;
							*/
						} else if (m_nRenderState == Started) {
							LONGLONG CurrentCounter = GetPerfCounter();
							// Calculate wake up timer
							if (!m_bSignaledStarvation) {
								llClockTime = GetClockTime(CurrentCounter);
								m_StarvationClock = llClockTime;
							} else {
								llClockTime = m_StarvationClock;
							}

							if (bForcePaint2) {
								// Just play as fast as possible
								bStepForward = true;
								pMFSample->GetUINT32(GUID_SURFACE_INDEX, &m_iCurSurface);
								++m_OrderedPaint;
								SubPicSetTime();
								Paint(true);
							} else {
								LONGLONG TimePerFrame = (LONGLONG)(GetFrameTime() * 10000000.0);

								LONGLONG SyncOffset = 0;
								LONGLONG VSyncTime = 0;
								LONGLONG TimeToNextVSync = -1;
								bool bVSyncCorrection = false;
								double DetectedRefreshTime;
								double DetectedScanlinesPerFrame;
								double DetectedScanlineTime;
								unsigned DetectedRefreshRatePos;
								{
									CAutoLock Lock(&m_RefreshRateLock);
									DetectedRefreshTime = m_DetectedRefreshTime;
									DetectedRefreshRatePos = m_DetectedRefreshRatePos;
									DetectedScanlinesPerFrame = m_DetectedScanlinesPerFrame;
									DetectedScanlineTime = m_DetectedScanlineTime;
								}

								if (DetectedRefreshRatePos < 20 || !DetectedRefreshTime || !DetectedScanlinesPerFrame) {
									DetectedRefreshTime = 1.0/m_refreshRate;
									DetectedScanlinesPerFrame = m_ScreenSize.cy;
									DetectedScanlineTime = DetectedRefreshTime / double(m_ScreenSize.cy);
								}

								if (m_ExtraSets.bVSyncInternal) {
									bVSyncCorrection = true;
									double TargetVSyncPos = GetVBlackPos();
									double RefreshLines = DetectedScanlinesPerFrame;
									double ScanlinesPerSecond = 1.0/DetectedScanlineTime;
									double CurrentVSyncPos = fmod(double(m_VBlankStartMeasure) + ScanlinesPerSecond * ((CurrentCounter - m_VBlankStartMeasureTime) / 10000000.0), RefreshLines);
									double LinesUntilVSync = 0;
									//TargetVSyncPos -= ScanlinesPerSecond * (DrawTime/10000000.0);
									//TargetVSyncPos -= 10;
									TargetVSyncPos = fmod(TargetVSyncPos, RefreshLines);
									if (TargetVSyncPos < 0) {
										TargetVSyncPos += RefreshLines;
									}
									if (TargetVSyncPos > CurrentVSyncPos) {
										LinesUntilVSync = TargetVSyncPos - CurrentVSyncPos;
									} else {
										LinesUntilVSync = (RefreshLines - CurrentVSyncPos) + TargetVSyncPos;
									}
									double TimeUntilVSync = LinesUntilVSync * DetectedScanlineTime;
									TimeToNextVSync = (LONGLONG)(TimeUntilVSync * 10000000.0);
									VSyncTime = (LONGLONG)(DetectedRefreshTime * 10000000.0);

									LONGLONG ClockTimeAtNextVSync = llClockTime + (LONGLONG)(TimeUntilVSync * 10000000.0 * m_ModeratedTimeSpeed);

									SyncOffset = (nsSampleTime - ClockTimeAtNextVSync);

									//if (SyncOffset < 0)
									//	TRACE_EVR("EVR: SyncOffset(%u): %I64d     %I64d     %I64d\n", m_iCurSurface, SyncOffset, TimePerFrame, VSyncTime);
								} else {
									SyncOffset = (nsSampleTime - llClockTime);
								}

								//LONGLONG SyncOffset = nsSampleTime - llClockTime;
								TRACE_EVR("EVR: SyncOffset: %I64d SampleFrame: %I64d ClockFrame: %I64d\n", SyncOffset, TimePerFrame!=0 ? nsSampleTime/TimePerFrame : 0, TimePerFrame!=0 ? llClockTime /TimePerFrame : 0);
								if (SampleDuration > 1 && !m_DetectedLock) {
									TimePerFrame = SampleDuration;
								}

								LONGLONG MinMargin;
								if (m_FrameTimeCorrection == 0) {
									MinMargin = MIN_FRAME_TIME;
								} else {
									MinMargin = MIN_FRAME_TIME + std::min(LONGLONG(m_DetectedFrameTimeStdDev), 20000LL);
								}
								LONGLONG TimePerFrameMargin = std::clamp(MinMargin, TimePerFrame*2/100, TimePerFrame*11/100); // (0.02..0.11)TimePerFrame
								LONGLONG TimePerFrameMargin0 = TimePerFrameMargin / 2;
								LONGLONG TimePerFrameMargin1 = 0;

								if (m_DetectedLock && TimePerFrame < VSyncTime) {
									VSyncTime = TimePerFrame;
								}

								if (m_VSyncMode == 1) {
									TimePerFrameMargin1 = -TimePerFrameMargin;
								} else if (m_VSyncMode == 2) {
									TimePerFrameMargin1 = TimePerFrameMargin;
								}

								m_LastSampleOffset = SyncOffset;
								m_bLastSampleOffsetValid = true;

								LONGLONG VSyncOffset0 = 0;
								bool bDoVSyncCorrection = false;
								if ((SyncOffset < -(TimePerFrame + TimePerFrameMargin0 - TimePerFrameMargin1)) && nSamplesLeft > 0) { // Only drop if we have something else to display at once
									// Drop frame
									TRACE_EVR("EVR: Dropped frame\n");
									m_pcFrames++;
									bStepForward = true;
									++m_nDroppedUpdate;
									NextSleepTime = 0;
									//VSyncOffset0 = (-SyncOffset) - VSyncTime;
									//VSyncOffset0 = (-SyncOffset) - VSyncTime + TimePerFrameMargin1;
									//m_LastPredictedSync = VSyncOffset0;
									bDoVSyncCorrection = false;
								} else if (SyncOffset < TimePerFrameMargin1) {

									if (bVSyncCorrection) {
										VSyncOffset0 = -SyncOffset;
										bDoVSyncCorrection = true;
									}

									// Paint and prepare for next frame
									TRACE_EVR("EVR: Normalframe\n");
									m_nDroppedUpdate = 0;
									bStepForward = true;
									pMFSample->GetUINT32(GUID_SURFACE_INDEX, &m_iCurSurface);
									m_LastFrameDuration = nsSampleTime - m_LastSampleTime;
									m_LastSampleTime = nsSampleTime;
									m_LastPredictedSync = VSyncOffset0;

									if (m_nStepCount > 0) {
										CompleteFrameStep (false);
									}

									++m_OrderedPaint;

									SubPicSetTime();
									Paint(true);

									NextSleepTime = 0;
									m_pcFramesDrawn++;
								} else {
									if (TimeToNextVSync >= 0 && SyncOffset > 0) {
										NextSleepTime = (int)(TimeToNextVSync/10000 - 2);
									} else {
										NextSleepTime = (int)(SyncOffset/10000 - 2);
									}

									if (NextSleepTime > TimePerFrame) {
										NextSleepTime = 1;
									}

									if (NextSleepTime < 0) {
										NextSleepTime = 0;
									}
									//TRACE_EVR("EVR: Delay\n");
								}

								if (bDoVSyncCorrection) {
									//LONGLONG VSyncOffset0 = (((SyncOffset) % VSyncTime) + VSyncTime) % VSyncTime;
									LONGLONG Margin = TimePerFrameMargin;

									LONGLONG VSyncOffsetMin = 30000000000000;
									LONGLONG VSyncOffsetMax = -30000000000000;
									for (int i = 0; i < 5; ++i) {
										expand_range(m_VSyncOffsetHistory[i], VSyncOffsetMin, VSyncOffsetMax);
									}

									m_VSyncOffsetHistory[m_VSyncOffsetHistoryPos] = VSyncOffset0;
									m_VSyncOffsetHistoryPos = (m_VSyncOffsetHistoryPos + 1) % 5u;

									//LONGLONG VSyncTime2 = VSyncTime2 + (VSyncOffsetMax - VSyncOffsetMin);
									//VSyncOffsetMin; = (((VSyncOffsetMin) % VSyncTime) + VSyncTime) % VSyncTime;
									//VSyncOffsetMax = (((VSyncOffsetMax) % VSyncTime) + VSyncTime) % VSyncTime;

									//TRACE_EVR("EVR: SyncOffset(%u, %d): %8I64d     %8I64d     %8I64d     %8I64d\n", m_iCurSurface, m_VSyncMode,VSyncOffset0, VSyncOffsetMin, VSyncOffsetMax, VSyncOffsetMax - VSyncOffsetMin);

									if (m_VSyncMode == 0) {
										// 23.976 in 60 Hz
										if (VSyncOffset0 < Margin && VSyncOffsetMax > (VSyncTime - Margin)) {
											m_VSyncMode = 2;
										} else if (VSyncOffset0 > (VSyncTime - Margin) && VSyncOffsetMin < Margin) {
											m_VSyncMode = 1;
										}
									} else if (m_VSyncMode == 2) {
										if (VSyncOffsetMin > (Margin)) {
											m_VSyncMode = 0;
										}
									} else if (m_VSyncMode == 1) {
										if (VSyncOffsetMax < (VSyncTime - Margin)) {
											m_VSyncMode = 0;
										}
									}
								}
							}
						} else if (m_nRenderState == Paused) {
							if (bForcePaint) {
								bStepForward = true;
								// Ensure that the renderer is properly updated after seeking when paused
								SubPicSetTime();
								Paint(false);
							}
							NextSleepTime = int(SampleDuration / 10000 - 2);
						}

						{
							m_CurrentSampleTime = INVALID_TIME;
							m_CurrentSampleDuration = INVALID_TIME;
						}
						if (bStepForward) {
							MoveToFreeList(pMFSample, true);
							CheckWaitingSampleFromMixer();
							m_MaxSampleDuration = std::max(SampleDuration, m_MaxSampleDuration);
						} else {
							MoveToScheduledList(pMFSample, true);
						}

						bForcePaint = false;
					} else if (m_bLastSampleOffsetValid && m_LastSampleOffset < -10000000) { // Only starve if we are 1 seconds behind
						if (m_nRenderState == Started && !g_bNoDuration) {
							m_pSink->Notify(EC_STARVATION, 0, 0);
							m_bSignaledStarvation = true;
						}
					}
					//GetImageFromMixer();
				}
				//else
				//{
				//	TRACE_EVR("EVR: RenderThread ==>> Flush before rendering frame!\n");
				//}

				break;
		}
	}

	timeEndPeriod(wTimerRes);
	if (pfAvRevertMmThreadCharacteristics) {
		pfAvRevertMmThreadCharacteristics(hAvrt);
	}
}

void CEVRAllocatorPresenter::VSyncThread()
{
	struct {
		LONGLONG time;
		UINT scanline;
	} ScanLines[250] = {};
	unsigned ScanLinePos = 0;
	bool filled = false;
	UINT prevSL = UINT_MAX;

	TIMECAPS tc = {};
	timeGetDevCaps(&tc, sizeof(TIMECAPS));
	const UINT wTimerRes = std::max(tc.wPeriodMin, 1u);
	timeBeginPeriod(wTimerRes);

	bool bQuit = false;
	LONGLONG start = 0;
	while (!bQuit) {
		DWORD dwObject = WaitForSingleObject(m_hEvtQuit, 1);
		switch (dwObject) {
			case WAIT_OBJECT_0 :
				bQuit = true;
				break;
			case WAIT_TIMEOUT : {
				if (m_bDisplayChanged) {
					m_bDisplayChanged = false;
					ScanLinePos = 0;
					filled = false;
					prevSL = UINT_MAX;
				}
				// Do our stuff
				if (m_pDevice9Ex && m_ExtraSets.bVSyncInternal) {
					ScanLinePos = 0;
					filled = false;

					if (m_nRenderState == Started) {
						int VSyncPos = GetVBlackPos();
						int WaitRange = std::max(m_ScreenSize.cy / 40, 5L);
						int MinRange = std::clamp(long(0.003 * double(m_ScreenSize.cy) * double(m_refreshRate) + 0.5), 5L, m_ScreenSize.cy/3); // 1.8  ms or max 33 % of Time

						VSyncPos += MinRange + WaitRange;

						VSyncPos = VSyncPos % m_ScreenSize.cy;
						if (VSyncPos < 0) {
							VSyncPos += m_ScreenSize.cy;
						}

						int ScanLine = (VSyncPos + 1) % m_ScreenSize.cy;
						if (ScanLine < 0) {
							ScanLine += m_ScreenSize.cy;
						}
						int ScanLineMiddle = ScanLine + m_ScreenSize.cy / 2;
						ScanLineMiddle = ScanLineMiddle % m_ScreenSize.cy;
						if (ScanLineMiddle < 0) {
							ScanLineMiddle += m_ScreenSize.cy;
						}

						int ScanlineStart = ScanLine;
						HANDLE lockOwner = nullptr;
						WaitForVBlankRange(ScanlineStart, 5, true, true, false, lockOwner);
						LONGLONG TimeStart = GetPerfCounter();

						WaitForVBlankRange(ScanLineMiddle, 5, true, true, false, lockOwner);
						LONGLONG TimeMiddle = GetPerfCounter();

						int ScanlineEnd = ScanLine;
						WaitForVBlankRange(ScanlineEnd, 5, true, true, false, lockOwner);
						LONGLONG TimeEnd = GetPerfCounter();

						double nSeconds = double(TimeEnd - TimeStart) / 10000000.0;
						LONGLONG DiffMiddle = TimeMiddle - TimeStart;
						LONGLONG DiffEnd = TimeEnd - TimeMiddle;
						double DiffDiff;
						if (DiffEnd > DiffMiddle) {
							DiffDiff = double(DiffEnd) / double(DiffMiddle);
						} else {
							DiffDiff = double(DiffMiddle) / double(DiffEnd);
						}
						if (nSeconds > 0.003 && DiffDiff < 1.3) {
							double ScanLineSeconds;
							double nScanLines;
							if (ScanLineMiddle > ScanlineEnd) {
								ScanLineSeconds = double(TimeMiddle - TimeStart) / 10000000.0;
								nScanLines = ScanLineMiddle - ScanlineStart;
							} else {
								ScanLineSeconds = double(TimeEnd - TimeMiddle) / 10000000.0;
								nScanLines = ScanlineEnd - ScanLineMiddle;
							}

							double ScanLineTime = ScanLineSeconds / nScanLines;

							unsigned iPos = m_DetectedRefreshRatePos % 100u;
							m_ldDetectedScanlineRateList[iPos] = ScanLineTime;
							if (m_DetectedScanlineTime && ScanlineStart != ScanlineEnd) {
								int Diff = ScanlineEnd - ScanlineStart;
								nSeconds -= double(Diff) * m_DetectedScanlineTime;
							}
							m_ldDetectedRefreshRateList[iPos] = nSeconds;
							double Average = 0;
							double AverageScanline = 0;
							unsigned nPos = std::min(iPos + 1, 100u);
							for (unsigned i = 0; i < nPos; ++i) {
								Average += m_ldDetectedRefreshRateList[i];
								AverageScanline += m_ldDetectedScanlineRateList[i];
							}

							Average /= double(nPos);
							AverageScanline /= double(nPos);

							double ThisValue = Average;

							if (Average > 0.0 && AverageScanline > 0.0) {
								CAutoLock Lock(&m_RefreshRateLock);
								++m_DetectedRefreshRatePos;
								if (m_DetectedRefreshTime == 0 || m_DetectedRefreshTime / ThisValue > 1.01 || m_DetectedRefreshTime / ThisValue < 0.99) {
									m_DetectedRefreshTime = ThisValue;
									m_DetectedRefreshTimePrim = 0;
								}
								if (_isnan(m_DetectedRefreshTime)) {
									m_DetectedRefreshTime = 0.0;
								}
								if (_isnan(m_DetectedRefreshTimePrim)) {
									m_DetectedRefreshTimePrim = 0.0;
								}

								ModerateFloat(m_DetectedRefreshTime, ThisValue, m_DetectedRefreshTimePrim, 1.5);
								if (m_DetectedRefreshTime > 0.0) {
									m_DetectedRefreshRate = 1.0/m_DetectedRefreshTime;
								} else {
									m_DetectedRefreshRate = 0.0;
								}

								if (m_DetectedScanlineTime == 0 || m_DetectedScanlineTime / AverageScanline > 1.01 || m_DetectedScanlineTime / AverageScanline < 0.99) {
									m_DetectedScanlineTime = AverageScanline;
									m_DetectedScanlineTimePrim = 0;
								}
								ModerateFloat(m_DetectedScanlineTime, AverageScanline, m_DetectedScanlineTimePrim, 1.5);
								if (m_DetectedScanlineTime > 0.0) {
									m_DetectedScanlinesPerFrame = m_DetectedRefreshTime / m_DetectedScanlineTime;
								} else {
									m_DetectedScanlinesPerFrame = 0;
								}
							}
						}
					}
				}
				else if (m_pDevice9ExRefresh && m_ExtraSets.iDisplayStats == 1) {
					if (prevSL == UINT_MAX) {
						D3DRASTER_STATUS rasterStatus;
						if (S_OK == m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus)) {
							while (rasterStatus.ScanLine == 0) { // skip zero scanline with unknown start time
								Sleep(1);
								m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus);
							}
							while (rasterStatus.ScanLine != 0) { // find new zero scanline
								m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus);
							}
							LONGLONG times0 = GetPerfCounter();
							while (rasterStatus.ScanLine == 0) {
								m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus);
							}
							LONGLONG times1 = GetPerfCounter();

							Sleep(1);
							prevSL = 0;
							while (rasterStatus.ScanLine != 0) {
								prevSL = rasterStatus.ScanLine;
								m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus);
							}
							LONGLONG timesLast = GetPerfCounter();

							CAutoLock Lock(&m_RefreshRateLock);
							m_DetectedScanlinesPerFrame = (double)(prevSL * (timesLast - times0)) / (timesLast - times1);
						}
					}
					else {
						D3DRASTER_STATUS rasterStatus;
						if (S_OK == m_pDevice9ExRefresh->GetRasterStatus(0, &rasterStatus)) {
							const LONGLONG time = GetPerfCounter();
							if (rasterStatus.ScanLine) { // ignore the zero scan line, it coincides with VBlanc and therefore is very long in time
								if (rasterStatus.ScanLine < prevSL) {
									ScanLines[ScanLinePos].time = time;
									ScanLines[ScanLinePos].scanline = rasterStatus.ScanLine;
									const UINT lastpos = ScanLinePos++;
									if (ScanLinePos >= std::size(ScanLines)) {
										ScanLinePos = 0;
										filled = true;
									}

									if ((time - start) >= UNITS / 10) {
										CAutoLock Lock(&m_RefreshRateLock);
										if (filled) {
											m_DetectedRefreshRate = (m_DetectedScanlinesPerFrame * (std::size(ScanLines) - 1) + ScanLines[lastpos].scanline - ScanLines[ScanLinePos].scanline) * UNITS / (m_DetectedScanlinesPerFrame * (ScanLines[lastpos].time - ScanLines[ScanLinePos].time));
										} else if (lastpos) {
											m_DetectedRefreshRate = (m_DetectedScanlinesPerFrame * lastpos + ScanLines[lastpos].scanline - ScanLines[0].scanline) * UNITS / (m_DetectedScanlinesPerFrame * (ScanLines[lastpos].time - ScanLines[0].time));
										}

										start = time;
									}
								}
								prevSL = rasterStatus.ScanLine;
							}
						}
					}
				}
				else {
					ScanLinePos = 0;
					filled = false;
				}
			}
			break;
		}
	}

	timeEndPeriod(wTimerRes);
}

DWORD WINAPI CEVRAllocatorPresenter::VSyncThreadStatic(LPVOID lpParam)
{
	SetThreadName(DWORD_MAX, "CEVRAllocatorPresenter::VSyncThread");
	CEVRAllocatorPresenter* pThis = (CEVRAllocatorPresenter*)lpParam;
	pThis->VSyncThread();
	return 0;
}

void CEVRAllocatorPresenter::OnResetDevice()
{
	CAutoLock cRenderLock(&m_RenderLock);

	// Reset DXVA Manager, and get new buffers
	HRESULT hr = m_pD3DManager->ResetDevice(m_pDevice9Ex, m_nResetToken);

	// Not necessary, but Microsoft documentation say Presenter should send this message...
	if (m_pSink) {
		EXECUTE_ASSERT(S_OK == (hr = m_pSink->Notify(EC_DISPLAY_CHANGED, 0, 0)));
	}
}

void CEVRAllocatorPresenter::RemoveAllSamples()
{
	CAutoLock AutoLock(&m_ImageProcessingLock);
	CAutoLock Lock(&m_csExternalMixerLock);

	FlushSamples();
	m_ScheduledSamples.clear();
	m_FreeSamples.clear();
	m_LastScheduledSampleTime = -1;
	m_LastScheduledUncorrectedSampleTime = -1;
	m_nUsedBuffer = 0;
}

HRESULT CEVRAllocatorPresenter::GetFreeSample(IMFSample** ppSample)
{
	CAutoLock lock(&m_SampleQueueLock);
	HRESULT hr = S_OK;

	if (m_FreeSamples.size() > 1) { // <= Cannot use first free buffer (can be currently displayed)
		InterlockedIncrement(&m_nUsedBuffer);
		*ppSample = m_FreeSamples.front().Detach(); m_FreeSamples.pop_front();
	} else {
		hr = MF_E_SAMPLEALLOCATOR_EMPTY;
	}

	return hr;
}

HRESULT CEVRAllocatorPresenter::GetScheduledSample(IMFSample** ppSample, int &_Count)
{
	CAutoLock lock(&m_SampleQueueLock);
	HRESULT hr = S_OK;

	_Count = (int)m_ScheduledSamples.size();
	if (_Count > 0) {
		*ppSample = m_ScheduledSamples.front().Detach();
		m_ScheduledSamples.pop_front();
		--_Count;
	} else {
		hr = MF_E_SAMPLEALLOCATOR_EMPTY;
	}

	return hr;
}

void CEVRAllocatorPresenter::MoveToFreeList(IMFSample* pSample, const bool bBack)
{
	CAutoLock lock(&m_SampleQueueLock);
	InterlockedDecrement(&m_nUsedBuffer);
	if (m_bPendingMediaFinished && m_nUsedBuffer == 0) {
		m_bPendingMediaFinished = false;
		m_pSink->Notify(EC_COMPLETE, 0, 0);
	}
	if (bBack) {
		m_FreeSamples.emplace_back(pSample);
	} else {
		m_FreeSamples.emplace_front(pSample);
	}
}

void CEVRAllocatorPresenter::MoveToScheduledList(IMFSample* pSample, const bool bSorted)
{
	if (bSorted) {
		CAutoLock lock(&m_SampleQueueLock);
		// Insert sorted
		/*
		POSITION Iterator = m_ScheduledSamples.GetHeadPosition();

		LONGLONG NewSampleTime;
		pSample->GetSampleTime(&NewSampleTime);

		while (Iterator != nullptr)
		{
			POSITION CurrentPos = Iterator;
			IMFSample *pIter = m_ScheduledSamples.GetNext(Iterator);
			LONGLONG SampleTime;
			pIter->GetSampleTime(&SampleTime);
			if (NewSampleTime < SampleTime)
			{
				m_ScheduledSamples.InsertBefore(CurrentPos, pSample);
				return;
			}
		}
		*/

		m_ScheduledSamples.emplace_front(pSample);
	} else {

		CAutoLock lock(&m_SampleQueueLock);

		/*
		double ForceFPS = 0.0;
		//double ForceFPS = 59.94;
		//double ForceFPS = 23.976;
		if (ForceFPS != 0.0) {
			m_rtTimePerFrame = (REFERENCE_TIME)(10000000.0 / ForceFPS);
		}
		*/
		LONGLONG Duration = m_rtTimePerFrame;
		UNREFERENCED_PARAMETER(Duration);
		LONGLONG PrevTime = m_LastScheduledUncorrectedSampleTime;
		LONGLONG Time;
		LONGLONG SetDuration;
		pSample->GetSampleDuration(&SetDuration);
		pSample->GetSampleTime(&Time);
		m_LastScheduledUncorrectedSampleTime = Time;

		m_bCorrectedFrameTime = false;

		LONGLONG Diff2 = PrevTime - (LONGLONG)(m_LastScheduledSampleTimeFP * 10000000.0);
		LONGLONG Diff = Time - PrevTime;
		if (PrevTime == -1) {
			Diff = 0;
		}
		if (Diff < 0) {
			Diff = -Diff;
		}
		if (Diff2 < 0) {
			Diff2 = -Diff2;
		}
		if (Diff < m_rtTimePerFrame*8 && m_rtTimePerFrame && Diff2 < m_rtTimePerFrame*8) { // Detect seeking
			int iPos = (m_DetectedFrameTimePos++) % 60u;
			LONGLONG Diff = Time - PrevTime;
			if (PrevTime == -1) {
				Diff = 0;
			}
			m_DetectedFrameTimeHistory[iPos] = Diff;

			if (m_DetectedFrameTimePos >= 10) {
				const unsigned nFrames = std::min(m_DetectedFrameTimePos, 60u);
				LONGLONG DectedSum = 0;
				for (unsigned i = 0; i < nFrames; ++i) {
					DectedSum += m_DetectedFrameTimeHistory[i];
				}

				const double Average = double(DectedSum) / nFrames;
				double DeviationSum = 0.0;
				for (unsigned i = 0; i < nFrames; ++i) {
					double Deviation = m_DetectedFrameTimeHistory[i] - Average;
					DeviationSum += Deviation*Deviation;
				}

				m_DetectedFrameTimeStdDev = sqrt(DeviationSum / nFrames);

				double DetectedRate = 1.0 / (double(DectedSum) / (nFrames * 10000000.0) );

				const double AllowedError = 0.0003;
				static const double AllowedValues[] = {60.0, 60/1.001, 50.0, 48.0, 48/1.001, 30.0, 30/1.001, 25.0, 24.0, 24/1.001};

				for (const auto& AllowedValue : AllowedValues) {
					if (fabs(1.0 - DetectedRate / AllowedValue) < AllowedError) {
						DetectedRate = AllowedValue;
						break;
					}
				}

				m_DetectedFrameTimeHistoryHistory[m_DetectedFrameTimePos % 500u] = DetectedRate;

				std::map<double, unsigned> Map;

				for (unsigned i = 0; i < std::size(m_DetectedFrameTimeHistoryHistory); ++i) {
					++Map[m_DetectedFrameTimeHistoryHistory[i]];
				}

				double BestVal = 0.0;
				unsigned BestNum = 5;
				for (const auto& [Key, Value] : Map) {
					if (Value > BestNum && Key != 0.0) {
						BestNum = Value;
						BestVal = Key;
					}
				}

				m_DetectedLock = false;
				for (const auto& AllowedValue : AllowedValues) {
					if (BestVal == AllowedValue) {
						m_DetectedLock = true;
						break;
					}
				}
				if (BestVal != 0.0) {
					m_DetectedFrameRate = BestVal;
					m_DetectedFrameTime = 1.0 / BestVal;
				}
			}

			LONGLONG PredictedNext = PrevTime + m_rtTimePerFrame;
			LONGLONG PredictedDiff = PredictedNext - Time;
			if (PredictedDiff < 0) {
				PredictedDiff = -PredictedDiff;
			}

			if (m_DetectedFrameTime != 0.0
					//&& PredictedDiff > MIN_FRAME_TIME
					&& m_DetectedLock && m_ExtraSets.bEVRFrameTimeCorrection) {
				double CurrentTime = Time / 10000000.0;
				double LastTime = m_LastScheduledSampleTimeFP;
				double PredictedTime = LastTime + m_DetectedFrameTime;
				if (fabs(PredictedTime - CurrentTime) > 0.0015) { // 1.5 ms wrong, lets correct
					CurrentTime = PredictedTime;
					Time = (LONGLONG)(CurrentTime * 10000000.0);
					pSample->SetSampleTime(Time);
					pSample->SetSampleDuration(LONGLONG(m_DetectedFrameTime * 10000000.0));
					m_bCorrectedFrameTime = true;
					m_FrameTimeCorrection = 30;
				}
				m_LastScheduledSampleTimeFP = CurrentTime;
			} else {
				m_LastScheduledSampleTimeFP = Time / 10000000.0;
			}
		} else {
			m_LastScheduledSampleTimeFP = Time / 10000000.0;
			if (Diff > m_rtTimePerFrame*8) {
				// Seek
				m_bSignaledStarvation = false;
				m_DetectedFrameTimePos = 0;
				m_DetectedLock = false;
			}
		}

		//TRACE_EVR("EVR: Time: %f %f %f\n", Time / 10000000.0, SetDuration / 10000000.0, m_DetectedFrameRate);
		if (!m_bCorrectedFrameTime && m_FrameTimeCorrection) {
			--m_FrameTimeCorrection;
		}

#if 0
		if (Time <= m_LastScheduledUncorrectedSampleTime && m_LastScheduledSampleTime >= 0) {
			PrevTime = m_LastScheduledSampleTime;
		}

		m_bCorrectedFrameTime = false;
		if (PrevTime != -1 && (Time >= PrevTime - ((Duration*20)/9) || Time == 0) || ForceFPS != 0.0) {
			if (Time - PrevTime > ((Duration*20)/9) && Time - PrevTime < Duration * 8 || Time == 0 || ((Time - PrevTime) < (Duration / 11)) || ForceFPS != 0.0) {
				// Error!!!!
				Time = PrevTime + Duration;
				pSample->SetSampleTime(Time);
				pSample->SetSampleDuration(Duration);
				m_bCorrectedFrameTime = true;
				TRACE_EVR("EVR: Corrected invalid sample time\n");
			}
		}
		if (Time+Duration*10 < m_LastScheduledSampleTime) {
			// Flush when repeating movie
			FlushSamplesInternal();
		}
#endif

#if 0
		static LONGLONG LastDuration = 0;
		LONGLONG SetDuration = m_rtTimePerFrame;
		pSample->GetSampleDuration(&SetDuration);
		if (SetDuration != LastDuration) {
			TRACE_EVR("EVR: Old duration: %I64d New duration: %I64d\n", LastDuration, SetDuration);
		}
		LastDuration = SetDuration;
#endif
		m_LastScheduledSampleTime = Time;

		m_ScheduledSamples.emplace_back(pSample);
	}
}

void CEVRAllocatorPresenter::FlushSamples()
{
	CAutoLock lock(this);
	CAutoLock lock2(&m_SampleQueueLock);

	FlushSamplesInternal();
	m_LastScheduledSampleTime = -1;
}

void CEVRAllocatorPresenter::FlushSamplesInternal()
{
	CAutoLock Lock(&m_csExternalMixerLock);

	for (const auto& sample : m_ScheduledSamples) {
		MoveToFreeList(sample, true);
	}
	m_ScheduledSamples.clear();

	m_LastSampleOffset       = 0;
	m_bLastSampleOffsetValid = false;
	m_bSignaledStarvation    = false;
}
