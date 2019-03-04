/*
GOALS:
have one thread pulling audio data and pushing it into a queue
FFT the data and sort into buckets to update main threads visualizer
aim is to have frequency resolution > 20 Hz which short of any
data interpolation is calculated by
	resolution = (sample-rate / number of data points)
so at 44100 samples per second needs >2205 data points, or roughly 50ms
which would give an animation frame rate of 20, can possibly update
audio data every 3 frames to get a smooth animation (60fps)

Console window should be set to match width and height specified (20x60)
in console properties for bars to display properly

*/


#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <complex>
#include <chrono>
#include <queue>

#include <Windows.h>
#include <MMDeviceAPI.h>
#include <AudioClient.h>
#include <AudioPolicy.h>
#include <functiondiscoverykeys.h>

using namespace std;

#define SAMPLE_RATE 44100
#define MAX_PACKET_SIZE 1024
#define N_CHANNELS 2

#define NUM_BARS 10
int nScreenHeight = 20;
int nScreenWidth = 60;

#define PI 3.14159f
complex<float> fi = { 0.0, 1.0 };

struct AudioPacket {
	int packetSize;
	float data[MAX_PACKET_SIZE][N_CHANNELS];
};

bool bDone = FALSE;
bool bInit = FALSE;
mutex mInit;
condition_variable cvInit;

atomic<bool> bPacketsReady = FALSE;
mutex mPacketsReady;
condition_variable cvPacketsReady;

queue<AudioPacket> audioData;
UINT32 framesQueued = 0;
mutex mtxData;

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

void getAudioData();
int FastFourierTransform(complex<float> *pDataIn, UINT32 nDataPoints, UINT32 stride, complex<float> *pDataOut);

int main() {
	HRESULT hr = S_OK;
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(hr)) { return -1; }

	thread listener(getAudioData);

	float barHeightOld[NUM_BARS] = { 0 };
	float barHeightNew[NUM_BARS] = { 0 };
	int freqBands[NUM_BARS + 1]; // 0 | 50 | 100 | 200 ... 12800 | 25600 Hz
	freqBands[0] = 0;
	freqBands[1] = 50;
	for (int i = 2; i < NUM_BARS + 1; i++) {
		freqBands[i] = 2 * freqBands[i - 1];
	}

	wchar_t *screen = new wchar_t[nScreenWidth*nScreenHeight];
	HANDLE hConsole = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
	SetConsoleActiveScreenBuffer(hConsole);
	SetConsoleTitle(L"Visualizer");
	DWORD dwBytesWritten = 0;

	while (!bInit) {
		unique_lock<mutex> mlk(mInit);
		cvInit.wait(mlk);
	}

	while (!bDone) {
		while (!bPacketsReady) {
			unique_lock<mutex> mlk(mPacketsReady);
			cvPacketsReady.wait_for(mlk, chrono::milliseconds(16)); //16ms to approximate 60fps animation
		}

		if (bPacketsReady) {
			mtxData.lock();
			int nPackets = audioData.size();
			AudioPacket *rawData = new AudioPacket[nPackets];

			for (int i = 0; i < nPackets; i++) {
				rawData[i] = audioData.front();
				audioData.pop();
			}
			int nFrames = framesQueued;
			framesQueued = 0;
			mtxData.unlock();

			complex<float> *cfDataIn = new complex<float>[nFrames];
			complex<float> *cfDataOut = new complex<float>[nFrames];
			ZeroMemory(cfDataOut, sizeof(complex<float>)*nFrames);
			//convert raw multi channel data to complex mono data
			int frame = 0;
			for (int i = 0; i < nPackets; i++) {
				for (int j = 0; j < rawData[i].packetSize; j++) {
					cfDataIn[frame] = { 0, 0 };
					for (int k = 0; k < 1; k++) {
						cfDataIn[frame] += (rawData[i].data[j][k] / N_CHANNELS);
					}
					frame++;
				}
			}

			FastFourierTransform(cfDataIn, nFrames, 1, cfDataOut);

			for (int i = 0; i < nFrames; i++) {
				int freq = i * SAMPLE_RATE / nFrames;
				float freqMag = sqrt(real(cfDataOut[i])*real(cfDataOut[i]) + imag(cfDataOut[i])*imag(cfDataOut[i]));
				for (int j = 0; j < NUM_BARS; j++) {
					if (freqBands[j] < freq && freq <= freqBands[j + 1])
						barHeightNew[j] += freqMag;
				}
			}

			float max = 0;
			for (int i = 0; i < NUM_BARS; i++) {
				if (barHeightNew[i] > max)
					max = barHeightNew[i];
			}

			if (max != 0) {
				for (int i = 0; i < NUM_BARS; i++) {
					barHeightNew[i] = barHeightNew[i] / max;
				}
			}

			delete[] rawData;
			delete[] cfDataIn;
			delete[] cfDataOut;
		}

		for (int i = 0; i < NUM_BARS; i++) {
			barHeightOld[i] = barHeightOld[i] * 0.9f;
			if (barHeightOld[i] >= barHeightNew[i])
				barHeightNew[i] = barHeightOld[i];
		}

		for (int x = 0; x < nScreenWidth; x++) {
			int nBar = (int)(((float)x * NUM_BARS) / (float)nScreenWidth);
			for (int y = 0; y < nScreenHeight; y++) {
				if (y <= nScreenHeight * (1 - barHeightNew[nBar]))
					screen[y*nScreenWidth + x] = ' ';
				else
					screen[y*nScreenWidth + x] = 'x';
			}
		}

		for (int i = 0; i < NUM_BARS; i++) {
			barHeightOld[i] = barHeightNew[i];
			barHeightNew[i] = 0;
		}

		WriteConsoleOutputCharacter(hConsole, screen, nScreenWidth*nScreenHeight, { 0,0 }, &dwBytesWritten);
		bPacketsReady = FALSE;
	}
	listener.join();
	return 0;
}

void getAudioData() {
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDevice *pEndpoint = NULL;

	IAudioClient *pAudioClient = NULL;
	IAudioCaptureClient *pCaptureClient = NULL;
	WAVEFORMATEX *pwfx = NULL;

	UINT32 packetLength = 0;
	UINT32 numFramesAvailable;
	BYTE *pData;
	DWORD flags;

	AudioPacket packet = { 0, {0} };

	hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);
	if (FAILED(hr)) { goto Exit; }
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pEndpoint);
	if (FAILED(hr)) { goto Exit; }
	hr = pEndpoint->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
	if (FAILED(hr)) { goto Exit; }
	hr = pAudioClient->GetMixFormat(&pwfx);
	if (FAILED(hr)) { goto Exit; }
	hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,			//share mode
		AUDCLNT_STREAMFLAGS_LOOPBACK,		//flags
		10000000,							//buffer duration in 100 nanosecond units (1sec buffer)
		0,								//buffer periodicity (0 for default)
		pwfx,								//WAVEFORMATEX (mix format)
		NULL);							//Session GUID, NULL for new session
	if (FAILED(hr)) { goto Exit; }
	hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
	if (FAILED(hr)) { goto Exit; }
	hr = pAudioClient->Start();
	if (FAILED(hr)) { goto Exit; }

	bInit = TRUE;
	cvInit.notify_all();

	while (!bDone) {
		Sleep(100); //wait for buffer to fill

		hr = pCaptureClient->GetNextPacketSize(&packetLength);
		if (FAILED(hr)) { goto Exit; }

		while (packetLength != 0) {
			hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
			if (FAILED(hr)) { goto Exit; }
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { break; }

			packet.packetSize = numFramesAvailable;
			memcpy(packet.data, pData, sizeof(float)*numFramesAvailable*N_CHANNELS);;

			mtxData.lock();
			audioData.push(packet);
			framesQueued += packet.packetSize;
			if (framesQueued >= 2205) {
				bPacketsReady = TRUE;
				cvPacketsReady.notify_all();
			}
			mtxData.unlock();

			hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
			if (FAILED(hr)) { goto Exit; }
			hr = pCaptureClient->GetNextPacketSize(&packetLength);
			if (FAILED(hr)) { goto Exit; }
		}
	}

Exit:
	if (pEnumerator != NULL) { pEnumerator->Release(); pEnumerator = NULL; }
	if (pEndpoint != NULL) { pEndpoint->Release(); pEndpoint = NULL; }
	if (pAudioClient != NULL) { pAudioClient->Release(); pAudioClient = NULL; }
	if (pCaptureClient != NULL) { pCaptureClient->Release(); pCaptureClient = NULL; }
	CoTaskMemFree(pwfx);
	return;
}

int FastFourierTransform(complex<float> *pDataIn, UINT32 nDataPoints, UINT32 stride, complex<float> *pDataOut) {
	if (nDataPoints == 1) {
		pDataOut[0] = pDataIn[0];
	}
	else {
		FastFourierTransform(pDataIn, nDataPoints / 2, 2 * stride, pDataOut);
		FastFourierTransform(&pDataIn[stride], nDataPoints / 2, 2 * stride, &pDataOut[nDataPoints / 2]);
		for (UINT32 k = 0; k < nDataPoints / 2; k++) {
			complex<float> t = pDataOut[k];
			complex<float> scal = { (float)(-2.0)*PI*(float)k / nDataPoints, 0 };
			pDataOut[k] = t + exp((fi)*scal)*pDataOut[k + (nDataPoints / 2)];
			pDataOut[k + (nDataPoints / 2)] = t - exp((fi)*scal)*pDataOut[k + (nDataPoints / 2)];
		}
	}
	return 0;
}