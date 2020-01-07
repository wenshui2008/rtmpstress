#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include <signal.h>		// to catch Ctrl-C

#ifdef _MSC_VER
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>

#define inline __inline
#endif

#include "librtmp/rtmp_sys.h"
#include "librtmp/log.h"

#ifdef _MSC_VER
#define fseeko _fseeki64
#define ftello _ftelli64
#define	SET_BINMODE(f)	_setmode(_fileno(f), O_BINARY)
typedef __int64 int64_t;
#else
#define	SET_BINMODE(f)
typedef long long int64_t;
#endif

#define RTMPDUMP_VERSION "2.40"

#define RD_SUCCESS		0
#define RD_FAILED		1
#define RD_INCOMPLETE	2
#define RD_NO_CONNECT	3

#define DEF_TIMEOUT	30	/* seconds */
#define DEF_BUFTIME	(10 * 60 * 60 * 1000)	/* 10 hours default */
#define DEF_SKIPFRM	0


volatile int64_t g_TotalSize = 0;
volatile int64_t g_timestamp = 0;
volatile int     g_bExit = FALSE;

int	Rtmp_PlayStream(RTMP * rtmp,
	uint32_t dSeek, uint32_t dStopOffset, double duration, int bResume, 
	int bLiveStream, int bRealtimeStream,  int bHashes, int bOverrideBufferTime,
	uint32_t bufferTime, double *percent)
{
	int32_t now, lastUpdate;
	int bufferSize = 64 * 1024;
	char *buffer;
	int nRead = 0;
	off_t size = 0;
	unsigned long lastPercent = 0;

	rtmp->m_read.timestamp = dSeek;

	*percent = 0.0;

	if (rtmp->m_read.timestamp) {
		RTMP_Log(RTMP_LOGDEBUG, "Continuing at TS: %d ms\n", rtmp->m_read.timestamp);
	}

	if (bLiveStream) {
		RTMP_LogPrintf("Starting Live Stream\n");
	}
	else {
		// print initial status
		// Workaround to exit with 0 if the file is fully (> 99.9%) downloaded
		if (duration > 0) {
			if ((double) rtmp->m_read.timestamp >= (double) duration * 999.0) {
				RTMP_LogPrintf("Already Completed at: %.3f sec Duration=%.3f sec\n",
					(double) rtmp->m_read.timestamp / 1000.0,
					(double) duration / 1000.0);
				return RD_SUCCESS;
			}
			else {
				*percent = ((double) rtmp->m_read.timestamp) / (duration * 1000.0) * 100.0;
				*percent = ((double) (int) (*percent * 10.0)) / 10.0;
				RTMP_LogPrintf("%s download at: %.3f kB / %.3f sec (%.1f%%)\n",
					bResume ? "Resuming" : "Starting",
					(double) size / 1024.0, (double) rtmp->m_read.timestamp / 1000.0,
					*percent);
			}
		}
		else {
			RTMP_LogPrintf("%s download at: %.3f kB\n",
				bResume ? "Resuming" : "Starting",
				(double) size / 1024.0);
		}
		if (bRealtimeStream)
			RTMP_LogPrintf("  in approximately realtime (disabled BUFX speedup hack)\n");
	}

	if (dStopOffset > 0) {
		RTMP_LogPrintf("For duration: %.3f sec\n", (double) (dStopOffset - dSeek) / 1000.0);
	}

	rtmp->m_read.initialFrameType = 0;
	rtmp->m_read.nResumeTS = dSeek;
	rtmp->m_read.metaHeader = NULL;
	rtmp->m_read.initialFrame = NULL;
	rtmp->m_read.nMetaHeaderSize = 0;
	rtmp->m_read.nInitialFrameSize = 0;

	buffer = (char *) malloc(bufferSize);

	now = RTMP_GetTime();
	lastUpdate = now - 1000;
	do {
		nRead = RTMP_Read(rtmp, buffer, bufferSize);
		//RTMP_LogPrintf("nRead: %d\n", nRead);
		if (nRead > 0) {
			size += nRead;

			//RTMP_LogPrintf("write %dbytes (%.1f kB)\n", nRead, nRead/1024.0);
			if (duration <= 0)	// if duration unknown try to get it from the stream (onMetaData)
				duration = RTMP_GetDuration(rtmp);

			if (duration > 0)
			{
				// make sure we claim to have enough buffer time!
				if (!bOverrideBufferTime && bufferTime < (duration * 1000.0))
				{
					bufferTime = (uint32_t) (duration * 1000.0) + 5000;	// extra 5sec to make sure we've got enough

					RTMP_Log(RTMP_LOGDEBUG,
						"Detected that buffer time is less than duration, resetting to: %dms",
						bufferTime);
					RTMP_SetBufferMS(rtmp, bufferTime);
					RTMP_UpdateBufferMS(rtmp);
				}
				*percent = ((double) rtmp->m_read.timestamp) / (duration * 1000.0) * 100.0;
				*percent = ((double) (int) (*percent * 10.0)) / 10.0;
				if (bHashes)
				{
					if (lastPercent + 1 <= *percent)
					{
						RTMP_LogStatus("#");
						lastPercent = (unsigned long) *percent;
					}
				}
				else
				{
					now = RTMP_GetTime();
					if (abs(now - lastUpdate) > 200)
					{
						RTMP_LogStatus("\r%.3f kB / %.2f sec (%.1f%%)",
							(double) size / 1024.0,
							(double) (rtmp->m_read.timestamp) / 1000.0, *percent);
						lastUpdate = now;
					}
				}
			}
			else
			{
				now = RTMP_GetTime();
				if (abs(now - lastUpdate) > 200) {
					if (bHashes)
						RTMP_LogStatus("#");
					
					lastUpdate = now;
				}
			}

			g_TotalSize += nRead;
		}
		else {
#ifdef _DEBUG
			RTMP_Log(RTMP_LOGDEBUG, "zero read!");
#endif
			if (rtmp->m_read.status == RTMP_READ_EOF)
				break;
		}

		if (g_bExit)
			break;
	} while (!RTMP_ctrlC && nRead > -1 && RTMP_IsConnected(rtmp) && !RTMP_IsTimedout(rtmp));

	free(buffer);
	if (nRead < 0)
		nRead = rtmp->m_read.status;

	/* Final status update */
	if (!bHashes) {
		if (duration > 0) {
			*percent = ((double) rtmp->m_read.timestamp) / (duration * 1000.0) * 100.0;
			*percent = ((double) (int) (*percent * 10.0)) / 10.0;
			RTMP_LogStatus("\r%.3f kB / %.2f sec (%.1f%%)",
				(double) size / 1024.0,
				(double) (rtmp->m_read.timestamp) / 1000.0, *percent);
		}
		else {
			RTMP_LogStatus("\r%.3f kB / %.2f sec", (double) size / 1024.0,
				(double) (rtmp->m_read.timestamp) / 1000.0);
		}
	}

	RTMP_Log(RTMP_LOGDEBUG, "RTMP_Read returned: %d", nRead);

	if (bResume && nRead == -2) {
		RTMP_LogPrintf("Couldn't resume FLV file, try --skip %d\n\n", 1);
		return RD_FAILED;
	}

	if (nRead == -3)
		return RD_SUCCESS;

	if ((duration > 0 && *percent < 99.9) || RTMP_ctrlC || nRead < 0 || RTMP_IsTimedout(rtmp)) {
		return RD_INCOMPLETE;
	}

	return RD_SUCCESS;
}

#define DEF_BUFTIME	(10 * 60 * 60 * 1000)	/* 10 hours default */

int stress_main(const char * p_url)
{
	RTMP rtmp = { 0 };
	AVal fullUrl = { 0, 0 };
	uint32_t bufferTime = DEF_BUFTIME;
	int nStatus = 0;
	int dSeek = -1000;
	double percent = 0;
	RTMP_Init(&rtmp);

	fullUrl.av_val = strdup(p_url);
	fullUrl.av_len = (int)strlen(p_url);

	if (RTMP_SetupURL(&rtmp, fullUrl.av_val) == FALSE)
	{
		RTMP_Close(&rtmp);

		RTMP_Log(RTMP_LOGERROR, "Couldn't parse URL: %s", fullUrl.av_val);
		return RD_FAILED;
	}

	RTMP_Log(RTMP_LOGDEBUG, "Setting buffer time to: %dms", bufferTime);
	RTMP_SetBufferMS(&rtmp, bufferTime);

	do {
		if (!RTMP_Connect(&rtmp, NULL))
		{
			nStatus = RD_NO_CONNECT;
			break;
		}


		if (!RTMP_ConnectStream(&rtmp, dSeek))
		{
			nStatus = RD_FAILED;
			break;
		}

		nStatus = Rtmp_PlayStream(&rtmp, dSeek, 0, 0, 0, 1, 0, 0,0,bufferTime, &percent);

	}while(0);


	RTMP_Close(&rtmp);

	if (fullUrl.av_val) 
		free(fullUrl.av_val);

	return 0;
}

volatile LONG g_nThreadCount = 0;

static DWORD WINAPI StressThreadProc(LPVOID lpParameter)
{
	const char * pUrl = (const char *)lpParameter;
	int r;

	InterlockedIncrement(&g_nThreadCount);

	r = stress_main(pUrl);

	InterlockedDecrement(&g_nThreadCount);

	return r;
}

void StartStressTester(const char * pUrl,int nStream)
{
	int i;
	int64_t  last_totalSize = 0;
	DWORD    dwTimeStart,dwTimeCur, dwTimeStart0, dwTotalTime;
	double   dwBitrateKb, dwUseTime;
	char     buf[1024];

	fprintf(stderr,"Start Stream %d ....\n",nStream);

	for (i = 0; i < nStream; i ++) {
		HANDLE hThread;
		DWORD dwThreadID;

		hThread = CreateThread(NULL,0,StressThreadProc,(LPVOID)pUrl,CREATE_SUSPENDED,&dwThreadID);
		if (hThread) {
			ResumeThread(hThread);
			CloseHandle(hThread);
		}
		else {
			break;
		}

		Sleep(10);
	}

	fprintf(stderr,"Stream %d Running....\n",nStream);

	dwTimeStart0 = timeGetTime();
	dwTimeStart = dwTimeStart0;

	while(g_nThreadCount > 0) {
		int64_t llTotalSize;
		double dTotalSize;
		const char * sizeUnit = "B ", * bitrateUint = "K";

		Sleep(1000);

		llTotalSize = g_TotalSize;

		dTotalSize = llTotalSize * 1.0;
		
		dwTimeCur = timeGetTime();
		dwUseTime = dwTimeCur - dwTimeStart;

		dwBitrateKb = (llTotalSize - last_totalSize) * 8.0 / dwUseTime; //kbps

		dwTotalTime = dwTimeCur - dwTimeStart0;

		if (dTotalSize > 1048576*1024) {
			sizeUnit = "GB";
			dTotalSize /= (1048576*1024);
		}
		else if (dTotalSize > 1048576) {
			sizeUnit = "MB";
			dTotalSize /= 1048576;
		}
		
		if (dwBitrateKb > 2000.0) {
			dwBitrateKb /= 1000.0;
			bitrateUint = "M";
		}
				
		sprintf(buf, "\r\rClients: %4d, BR: %4.3f %sbps, Time: %6.2f sec, Total Size: %8.3f %s\t", g_nThreadCount , dwBitrateKb, bitrateUint, (double) (dwTotalTime) / 1000.0, dTotalSize, sizeUnit);

		printf(buf);

		last_totalSize = llTotalSize;
		dwTimeStart = dwTimeCur;
	}
}

#ifdef _DEBUG
uint32_t debugTS = 0;
int pnum = 0;

FILE *netstackdump = 0;
FILE *netstackdump_read = 0;
#endif

// starts sockets
int
	InitSockets()
{
#ifdef WIN32
	WORD version;
	WSADATA wsaData;

	version = MAKEWORD(1, 1);
	return (WSAStartup(version, &wsaData) == 0);
#else
	return TRUE;
#endif
}

inline void
	CleanupSockets()
{
#ifdef WIN32
	WSACleanup();
#endif
}


int g_Log_Level = RTMP_LOGINFO;

void MyRTMP_LogCallback(int level, const char *fmt, va_list va)
{
	if (level > g_Log_Level) {
		return;
	}
	else {
		char szBuf[1024];

		int r = vsprintf_s(szBuf,ARRAYSIZE(szBuf),fmt,va);
		if (r > 0) {
			printf(szBuf);printf("\n");
		}
	}
}

void Usage()
{
	printf("Usage:\n rtmpstress -c <num> -i <url>\n");
	printf("-c : specify the number of concurrent threads\n");
	printf("-i : specify the url of rtmp stream to test\n");
}

int main(int argc, char **argv)
{
	int nThreadCount = 1, i;
	const char * url = NULL;
	HANDLE handle_out;
	COORD size = {160, 25}; 

	handle_out = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleScreenBufferSize(handle_out, size);

	if (argc < 2) {
		Usage();
		
		return 1;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			nThreadCount = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-i") == 0) {
			url = argv[++i];
		}
	}

	if (!url) {
		Usage();

		return 1;
	}

	RTMP_LogSetCallback(MyRTMP_LogCallback);

	RTMP_LogPrintf("RTMPDump %s\n", RTMPDUMP_VERSION);
	RTMP_LogPrintf("(c) 2010 Andrej Stepanchuk, Howard Chu, The Flvstreamer Team; license: GPL\n");

	if (!InitSockets()) {
		RTMP_Log(RTMP_LOGERROR,
			"Couldn't load sockets support on your platform, exiting!");
		return RD_FAILED;
	}

	g_Log_Level = RTMP_LOGCRIT;
		
	StartStressTester(url, nThreadCount);
		
	CleanupSockets();

	return 0;
}




