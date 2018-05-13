//--------------------------------------------//
// DTVProxy                                   //
// License: Public Domain (www.unlicense.org) //
//--------------------------------------------//

//Global settings
#define MOZREPL_HOST   "127.0.0.1"
#define MOZREPL_PORT   4242
#define PROXY_PORT     4243
#define PROXY_URL      "http://127.0.0.1:4243"
#define STREAMING_HOST "abc-streaming.akamaized.net"

//Settings to use mobile app requests on low quality streams
//#define APP_COOKIE_VIDEOID "<COPY videoid COOKIE FROM APP REQUEST>"
//#define APP_USER_AGENT "<COPY User-Agent HEADER FROM APP REQUEST>"
//#define APP_ADDITIONAL_HEADER "c.setRequestHeader('X-Drm-wSL', '1');" //uncomment if videoid is from Android app

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define _HAS_EXCEPTIONS 0
#if _DEBUG
#define DTVPROXY_USE_WIN32SYSTRAY
#endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <assert.h>

#define STS_NET_IMPLEMENTATION
#define STS_NET_NO_PACKETS
#define STS_NET_NO_ERRORSTRINGS
#include "sts_net.inl"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4702) //unreachable code
#include <xtree>
#pragma warning(pop)
#endif
#include <map>
#include <vector>
#include <string>

#ifdef _WIN32
struct sMutex { sMutex() : h(CreateMutexA(0,0,0)) {} ~sMutex() { CloseHandle(h); } __inline void Lock() { WaitForSingleObject(h,INFINITE); } __inline void Unlock() { ReleaseMutex(h); } private:HANDLE h;sMutex(const sMutex&);sMutex& operator=(const sMutex&);};
struct sLockCount { sLockCount() : h(CreateEventA(0,0,1,0)) {} ~sLockCount() { CloseHandle(h); } __inline void Increment() { InterlockedIncrement(&c); } __inline void Decrement() { InterlockedDecrement(&c); SetEvent(h); } void WaitForZero() { while (c) WaitForSingleObject(h,INFINITE); } private:HANDLE h;volatile LONG c;sLockCount& operator=(const sLockCount&);};
#else
#error MUTEX TODO
#endif

typedef unsigned int ticks_t;
static ticks_t GetTicksSecond() { return (ticks_t)time(0); }
static int GetSecondsSince(ticks_t t) { return (int)(GetTicksSecond()-t); }

using namespace std;
struct sFileBlock
{
	INT64 RangeStart, RangeEnd;
	vector<unsigned char> Data;
};
static INT64 WorkFileRequestIDCounter = 1;
struct sWorkFile
{
	sWorkFile(INT64 Offset) : RequestID(WorkFileRequestIDCounter++), OffsetRequested(Offset), OffsetServed(Offset), Size(0), TotalDownloadedBytes(0), ActiveDownloads(0), LastServe(GetTicksSecond()) { }
	INT64 RequestID, OffsetRequested, OffsetServed, Size, TotalDownloadedBytes, ActiveDownloads; ticks_t LastServe;
	vector<sFileBlock*> BufferedBlocks;
};
struct sWorkRequest
{
	sWorkRequest(sts_net_socket_t Client, const char* Request) : Client(Client), Request(Request) { }
	sts_net_socket_t Client; string Request;
};
static map<string, sWorkFile*> WorkFiles;
static vector<sWorkRequest*> WorkRequests;
static sMutex WorkFilesMutex;
static sMutex WorkRequestsMutex;
static sLockCount FileBlockLockCount;

static const INT64 DownloadBlockSize       =  2*1024*1024;
static const INT64 DownloadAheadMax        = 64*1024*1024;
static const INT64 DownloadKeepBufferedMax = 16*1024*1024;
static const int TotalNumDownloadWorkers   = 5;
static const int TotalNumServerWorkers     = 2;
static const int ClearBufferAfterSeconds   = 60*5;

static sts_net_interfaceinfo_t g_AddressTable[20];
static int g_AddressCount;

#if defined(_MSC_VER)
typedef signed __int64 INT64;
extern "C" int __cdecl _fseeki64(FILE *, __int64, int);
extern "C" __int64 __cdecl _ftelli64(FILE *);
extern "C" int __cdecl _setmode(int, int);
#define ftello64 _ftelli64
#define fseeko64 _fseeki64
#define PRLLD "%I64d"
#define PRLLU "%I64u"
#else
typedef signed long long INT64;
#define PRLLD "%lld"
#define PRLLU "%llu"
#endif

#ifdef DTVPROXY_USE_WIN32SYSTRAY
#include <shellapi.h>
static bool g_Active = true;
static void Tray_FlipActive();
static void Tray_ResetAll();

#ifndef _DEBUG
#pragma comment(linker, "/subsystem:windows")
#endif

static DWORD CALLBACK SystrayThread(LPVOID) 
{
	struct Wnd
	{
		enum { IDM_NONE, IDM_ACTIVE, IDM_RESET, IDM_EXIT };

		static void OpenMenu(HWND hWnd, bool AtCursor)
		{
			char buf[256];
			static POINT lpClickPoint;
			if (AtCursor) GetCursorPos(&lpClickPoint);
			HMENU hPopMenu = CreatePopupMenu();
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING|MF_GRAYED,IDM_NONE, "DTV Proxy");
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_SEPARATOR,IDM_NONE,NULL);
			for (int i = 0; i < g_AddressCount; i++)
			{
				sprintf(buf, (PROXY_PORT == 80 ? "@ http://%s/" : "@ http://%s:%d/"), g_AddressTable[i].address, PROXY_PORT);
				InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING|MF_GRAYED,IDM_NONE, buf);
			}
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_SEPARATOR,IDM_NONE,NULL);
			WorkFilesMutex.Lock();
			for (map<string, sWorkFile*>::iterator it = WorkFiles.begin(); it != WorkFiles.end(); ++it)
			{
				sprintf(buf, "%s - Buffered: %d MB - Active Downloads: %d", it->first.c_str(), (int)(it->second->BufferedBlocks.size() * DownloadBlockSize / 1024 / 1024), (int)it->second->ActiveDownloads);
				InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING|MF_GRAYED,IDM_NONE, buf);
			}
			WorkFilesMutex.Unlock();
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_SEPARATOR,IDM_NONE,NULL);
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING|(g_Active ? MF_CHECKED : MF_UNCHECKED),IDM_ACTIVE,"Is Active");
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING,IDM_RESET,"Reset");
			InsertMenuA(hPopMenu,0xFFFFFFFF,MF_STRING,IDM_EXIT,"Exit");
			SetForegroundWindow(hWnd); //cause the popup to be focused
			TrackPopupMenu(hPopMenu,TPM_LEFTALIGN|TPM_LEFTBUTTON|TPM_BOTTOMALIGN, lpClickPoint.x, lpClickPoint.y,0,hWnd,NULL);
		}

		static LRESULT CALLBACK Proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
		{
			static UINT s_WM_TASKBARRESTART;
			if (Msg == WM_COMMAND && wParam == IDM_EXIT) { NOTIFYICONDATAA i; ZeroMemory(&i, sizeof(i)); i.cbSize = sizeof(i); i.hWnd = hWnd; Shell_NotifyIconA(NIM_DELETE, &i); ExitProcess(EXIT_SUCCESS); }
			if (Msg == WM_COMMAND && wParam == IDM_ACTIVE) { Tray_FlipActive(); }
			if (Msg == WM_COMMAND && wParam == IDM_RESET) { Tray_ResetAll(); }
			if (Msg == WM_USER && (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP)) OpenMenu(hWnd, true); //systray rightclick
			if (Msg == WM_CREATE || Msg == s_WM_TASKBARRESTART)
			{
				if (Msg == WM_CREATE) s_WM_TASKBARRESTART = RegisterWindowMessageA("TaskbarCreated");
				NOTIFYICONDATAA nid;
				ZeroMemory(&nid, sizeof(nid));
				nid.cbSize = sizeof(nid); 
				nid.hWnd = hWnd;
				nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
				nid.hIcon = LoadIconA(GetModuleHandleA(NULL), "ICN");
				nid.uCallbackMessage = WM_USER; 
				strcpy(nid.szTip, "DTV Proxy");
				Shell_NotifyIconA(NIM_ADD, &nid);
			}
			return DefWindowProcA(hWnd, Msg, wParam, lParam);
		}
	};

	WNDCLASSA c;
	ZeroMemory(&c, sizeof(c));
	c.lpfnWndProc = Wnd::Proc;
	c.hInstance = GetModuleHandleA(NULL);
	c.lpszClassName = "DTVPROXY";
	HWND hwnd = (RegisterClassA(&c) ? CreateWindowA(c.lpszClassName, 0, 0, 0, 0, 0, 0, 0, 0, c.hInstance, 0) : 0);
	if (!hwnd) return 1;

	MSG Msg;
	while (GetMessageA(&Msg, NULL, 0, 0) > 0) { TranslateMessage(&Msg); DispatchMessageA(&Msg); }
	return 0;
}

static void LogMsgBox(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	char* logbuf = (char*)malloc(1+vsnprintf(NULL, 0, format, ap));
	vsprintf(logbuf, format, ap);
	va_end(ap);
	MessageBoxA(0, logbuf, "DTVPROXY", MB_ICONSTOP);
	free(logbuf);
}

#ifdef _DEBUG
#define LogText(...) LogFile(stdout, __VA_ARGS__)
#else
#pragma comment(linker, "/subsystem:windows")
#define LogText(format, ...) (void)0
#endif
#define LogError(format, ...) LogMsgBox(format, __VA_ARGS__)
#else //DTVPROXY_USE_WIN32SYSTRAY
#define LogText(...) LogFile(stdout, __VA_ARGS__)
#define LogError(...) fprintf(stderr, __VA_ARGS__),fprintf(stderr, "\n\n")
#endif //DTVPROXY_USE_WIN32SYSTRAY

#if !defined(DTVPROXY_USE_WIN32SYSTRAY) || defined(_DEBUG)
static void LogFile(FILE* f, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	char* logbuf = (char*)malloc(1+vsnprintf(NULL, 0, format, ap));
	va_start(ap, format);
	vsprintf(logbuf, format, ap);
	va_end(ap);
	time_t RAWTime = time(0);
	tm* LOCALTime = localtime(&RAWTime);
	fprintf(f, "[%02d:%02d:%02d] %s\n", LOCALTime->tm_hour, LOCALTime->tm_min, LOCALTime->tm_sec, logbuf);
	free(logbuf);
}
#endif

static void ShowHelp()
{
	LogError("DTVPROXY - Command Line Arguments" "\n\n"
		"-b <ip-address>   Specify the ip address of which interface to listen on (defaults to all interfaces)" "\n\n"
		"-h                Show this help");
}

static void sleep_ms(unsigned int ms)
{
	#ifdef _WIN32
	Sleep(ms);
	#else
	timespec req, rem;
	req.tv_sec = ms / 1000;
	req.tv_nsec = (ms % 1000) * 1000000ULL;
	while (nanosleep(&req, &rem)) req = rem;
	#endif
}

static void DownloadWorker(size_t WID)
{
	(void)WID;
	LogText("[D%02d] Worker starting up", WID);
	string WorkFileName = string();
	INT64 WorkFileRequestID = 0, WorkFileRangeStart = 0, WorkFileRangeEnd = 0;
	sts_net_socket_t client;
	for (;;)
	{
		sleep_ms(33);
		WorkFileName.clear();
		WorkFilesMutex.Lock();
		{
			map<string, sWorkFile*>::iterator itSelected = WorkFiles.end();
			for (map<string, sWorkFile*>::iterator it = WorkFiles.begin(); it != WorkFiles.end(); ++it)
			{
				if (it->second->Size && it->second->OffsetRequested >= it->second->Size) continue;
				if (it->second->OffsetRequested > it->second->OffsetServed + DownloadAheadMax) continue;
				if (it->second->TotalDownloadedBytes == 0 && it->second->ActiveDownloads > 1) continue; //have only max 2 workers at beginning of download
				if (itSelected == WorkFiles.end() || it->second->BufferedBlocks.size() < itSelected->second->BufferedBlocks.size()) itSelected = it;
			}
			if (itSelected == WorkFiles.end()) { WorkFilesMutex.Unlock(); continue; }
			WorkFileName = itSelected->first;
			WorkFileRequestID = itSelected->second->RequestID;
			WorkFileRangeStart = itSelected->second->OffsetRequested;
			WorkFileRangeEnd = WorkFileRangeStart + DownloadBlockSize;
			itSelected->second->OffsetRequested = WorkFileRangeEnd;
			itSelected->second->ActiveDownloads++;
		}
		WorkFilesMutex.Unlock();
		if (WorkFileName.empty()) continue;

		INT64 TotalSize = 0;
		sFileBlock* FileBlock = new sFileBlock;
		FileBlock->Data.resize(DownloadBlockSize);
		FileBlock->RangeStart = WorkFileRangeStart;

		for (;;)
		{
			LogText("[D%02d]       Requesting http://" STREAMING_HOST "/%s - Range: " PRLLD "-" PRLLD " ...", WID,  WorkFileName.c_str(), WorkFileRangeStart, WorkFileRangeEnd-1);
			sts_net_connect(&client, STREAMING_HOST, 80);
			char ReqBuf[1024];
			size_t HeaderLen = sprintf(ReqBuf, "GET /%s HTTP/1.1" "\r\n"
				"Host: " STREAMING_HOST "\r\n"
				"Range: bytes="PRLLD"-"PRLLD "\r\n"
				"Connection: close" "\r\n"
				"\r\n", WorkFileName.c_str(), WorkFileRangeStart, WorkFileRangeEnd-1);
			sts_net_send(&client, ReqBuf, (int)HeaderLen);

			char ResponseHeader[2048];
			INT64 ResumeOffset = WorkFileRangeStart - FileBlock->RangeStart, BlockReceived = 0, ContentLength = 0;
			if (sts_net_check_socket(&client, 30.f) == 1)
			{
				int Read = sts_net_recv(&client, ResponseHeader, (int)(sizeof(ResponseHeader)-1));
				ResponseHeader[Read > 0 ? Read : 0] = '\0';

				INT64 RangeFirst = WorkFileRangeStart, RangeLast = WorkFileRangeEnd - 1;
				char* HeaderEnd = strstr(ResponseHeader, "\r\n\r\n");
				if (HeaderEnd)
				{
					char* DataStart = HeaderEnd + 4;
					BlockReceived = ResponseHeader + Read - DataStart;
					memcpy(&FileBlock->Data[(size_t)ResumeOffset], DataStart, (size_t)BlockReceived);
				}
				for (char* p = ResponseHeader; HeaderEnd && p != HeaderEnd;) *(p++) |= 0x20; //make lower case 
				const char* HdrRangeBytes    = strstr(ResponseHeader, "content-range: bytes ");
				const char* HdrContentLength = strstr(ResponseHeader, "content-length: "     );
				if (HdrRangeBytes)    sscanf(HdrRangeBytes   +(sizeof("content-range: bytes ")-1), PRLLD"-"PRLLD"/"PRLLD, &RangeFirst, &RangeLast, &TotalSize);
				if (HdrContentLength) sscanf(HdrContentLength+(sizeof("content-length: "     )-1), PRLLD, &ContentLength);
				if (TotalSize && RangeLast && !ContentLength) { ContentLength = RangeLast + 1 - RangeFirst; }
				if (ContentLength && !TotalSize) TotalSize = ContentLength;
			}

			for (INT64 Read = 0, Expect = (ContentLength ? ContentLength : DownloadBlockSize - ResumeOffset); BlockReceived < Expect; BlockReceived += Read)
			{
				if (sts_net_check_socket(&client, 30.f) < 1) break;
				Read = sts_net_recv(&client, &FileBlock->Data[(size_t)(ResumeOffset + BlockReceived)], (int)(Expect - BlockReceived));
				if (Read <= 0) break;
			}
			sts_net_close_socket(&client);
			LogText("[D%02d]       Received block of %s - Bytes: "PRLLD" - IsSuccess: %d", WID, WorkFileName.c_str(), BlockReceived, (ContentLength == BlockReceived));

			WorkFileRangeStart += BlockReceived;
			if (!ContentLength) ContentLength = BlockReceived;
			if (ContentLength != BlockReceived) continue; //DOWNLOAD ERROR
			if (ResumeOffset + BlockReceived != (INT64)FileBlock->Data.size()) { FileBlock->Data.resize((size_t)(ResumeOffset + BlockReceived)); FileBlock->Data.shrink_to_fit(); }
			FileBlock->RangeEnd = WorkFileRangeStart;
			assert(WorkFileRangeStart == FileBlock->RangeStart + (INT64)FileBlock->Data.size());
			break;
		}

		WorkFilesMutex.Lock();
		map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.find(WorkFileName);
		if (itWorkFile != WorkFiles.end() && itWorkFile->second->RequestID == WorkFileRequestID)
		{
			itWorkFile->second->ActiveDownloads--;
			itWorkFile->second->TotalDownloadedBytes += FileBlock->Data.size();
			itWorkFile->second->BufferedBlocks.push_back(FileBlock);
			if (!itWorkFile->second->Size) itWorkFile->second->Size = TotalSize;
			LogText("[D%02d]       Add buffered block to %s (Total buffer count: %d - size: %d MB)", WID, WorkFileName.c_str(), (int)(itWorkFile->second->BufferedBlocks.size()), (int)(itWorkFile->second->BufferedBlocks.size() * DownloadBlockSize / 1024 / 1024));
		}
		else delete FileBlock;
		WorkFilesMutex.Unlock();
	}
}

static bool ProcessRequest(size_t WID, sts_net_socket_t client, char* Request)
{
	(void)WID;
	char *ReqPath = strchr(Request, ' ')+2, *ReqPathEnd = strchr(ReqPath, ' '), *ReqPathQuery = strchr(ReqPath, '?');

	size_t ReqMethodLen = (ReqPath - Request - 2);
	if (ReqMethodLen != 3) //if not GET
	{
		static const char EmptyResponse[] = "HTTP/1.1 200 OK" "\r\n"
			"Connection: close" "\r\n"
			"Content-Type: video/mp4" "\r\n"
			"Access-Control-Max-Age: 10800" "\r\n"
			"Access-Control-Allow-Headers: accept, origin, content-type, range" "\r\n"
			"Access-Control-Allow-Methods: GET,POST,OPTIONS" "\r\n"
			"Access-Control-Allow-Origin: *" "\r\n"
			"Content-Length: 0" "\r\n"
			"\r\n";
		sts_net_send(&client, EmptyResponse, (int)(sizeof(EmptyResponse)-1));
		return true;
	}

	if (ReqPathEnd <= ReqPath) { LogText("[S%02d]  Invalid Request '%s'", WID, Request); return false; }
	*ReqPathEnd = '\0';
	if (ReqPathQuery) *ReqPathQuery = '\0';
	if (strstr(ReqPath, "..")) { LogText("[S%02d]  Requested file '%s' contains ..", WID, ReqPath); return false; }

	INT64 TotalSize = 0, RangeFirst = 0, RangeLast = -1;
	char* RangeBytes = strstr(ReqPathEnd+1, "Range: bytes=");
	if (RangeBytes)
	{
		sscanf(RangeBytes+(sizeof("Range: bytes=")-1), PRLLD"-"PRLLD, &RangeFirst, &RangeLast);
		if (RangeFirst < 0) RangeFirst = 0;
		if (RangeLast < RangeFirst) RangeLast = RangeFirst;
	}

	LogText("[S%02d]  Serving %s byte range "PRLLD" - "PRLLD, WID, ReqPath, RangeFirst, RangeLast);

	#ifdef DTVPROXY_SERVE_LOCAL_FILES
	FILE* ReqFile = fopen(ReqPath, "rb");
	if (ReqFile)
	{
		fseeko64(ReqFile, 0, SEEK_END);
		TotalSize = ftello64(ReqFile);
		fseeko64(ReqFile, 0, SEEK_SET);
	}
	else
	#endif
	{
		string WorkFileName = ReqPath;

		WorkFilesMutex.Lock();
		{
			map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.find(WorkFileName);
			if (itWorkFile != WorkFiles.end())
			{
				bool IsValid = (RangeFirst >= itWorkFile->second->OffsetServed && RangeFirst < itWorkFile->second->OffsetServed + DownloadAheadMax);
				if (!IsValid)
					for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
						if (RangeFirst >= (*itFileBlock)->RangeStart && RangeFirst < (*itFileBlock)->RangeEnd) { IsValid = true; break; }
				if (IsValid)
				{
					TotalSize = itWorkFile->second->Size;
					itWorkFile->second->LastServe = GetTicksSecond();
					const INT64 DeleteOlderThan = RangeFirst - DownloadKeepBufferedMax;
					bool NeedCleanup = false;
					for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
						if ((*itFileBlock)->RangeEnd <= DeleteOlderThan) { NeedCleanup = true; break; }
					if (NeedCleanup)
					{
						FileBlockLockCount.WaitForZero();
						for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end();)
							if ((*itFileBlock)->RangeEnd > DeleteOlderThan) ++itFileBlock;
							else { delete *itFileBlock; itFileBlock = itWorkFile->second->BufferedBlocks.erase(itFileBlock); }
					}
				}
				else
				{
					FileBlockLockCount.WaitForZero();
					for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
						delete *itFileBlock;
					delete itWorkFile->second;
					WorkFiles.erase(itWorkFile);
					itWorkFile = WorkFiles.end(); 
				}
			}
			if (itWorkFile == WorkFiles.end()) WorkFiles[WorkFileName] = new sWorkFile(RangeFirst - (RangeFirst % DownloadBlockSize));
		}
		WorkFilesMutex.Unlock();

		if (!TotalSize) { LogText("[S%02d]    Waiting for initial packet with total size ...", WID); }

		while (!TotalSize)
		{
			sleep_ms(33);
			WorkFilesMutex.Lock();
			map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.find(WorkFileName);
			if (itWorkFile == WorkFiles.end()) { WorkFilesMutex.Unlock(); return false; }
			TotalSize = itWorkFile->second->Size;
			itWorkFile->second->LastServe = GetTicksSecond();
			WorkFilesMutex.Unlock();
			if (TotalSize) { LogText("[S%02d]    Received initial packet with total size "PRLLD, WID, TotalSize); }
		}
	}

	if (RangeLast < 0) RangeLast = TotalSize - 1;

	char ReqBuf[1024];
	size_t HeaderLen = 0;
	if (!RangeBytes)
	{
		HeaderLen = sprintf(ReqBuf, "HTTP/1.1 200 OK" "\r\n"
			"Connection: close" "\r\n"
			"Content-Type: video/mp4" "\r\n"
			"Access-Control-Max-Age: 10800" "\r\n"
			"Access-Control-Allow-Headers: accept, origin, content-type, range" "\r\n"
			"Access-Control-Allow-Methods: GET,POST,OPTIONS" "\r\n"
			"Access-Control-Allow-Origin: *" "\r\n"
			"Content-Length: "PRLLD "\r\n"
			"\r\n", RangeLast+1-RangeFirst);
	}
	else
	{
		if (RangeLast > TotalSize-1) RangeLast = TotalSize-1;
		if (RangeFirst > RangeLast) RangeFirst = RangeLast;
		HeaderLen = sprintf(ReqBuf, "HTTP/1.1 206 Partial Content" "\r\n"
			"Connection: close" "\r\n"
			"Content-Type: video/mp4" "\r\n"
			"Access-Control-Max-Age: 10800" "\r\n"
			"Access-Control-Allow-Headers: accept, origin, content-type, range" "\r\n"
			"Access-Control-Allow-Methods: GET,POST,OPTIONS" "\r\n"
			"Access-Control-Allow-Origin: *" "\r\n"
			"Content-Length: "PRLLD "\r\n"
			"Content-Range: bytes "PRLLD"-"PRLLD"/"PRLLD "\r\n"
			"\r\n", RangeLast+1-RangeFirst, RangeFirst, RangeLast, TotalSize);
	}

	sts_net_send(&client, ReqBuf, (int)HeaderLen);

	bool SendSuccess = true;
	#ifdef DTVPROXY_SERVE_LOCAL_FILES
	if (ReqFile)
	{
		if (RangeFirst) fseeko64(ReqFile, RangeFirst, SEEK_SET);
		for (INT64 ReqRead, ReadLeft = RangeLast - RangeFirst + 1; ReadLeft; ReadLeft -= ReqRead)
		{
			ReqRead = fread(ReqBuf, 1, (size_t)(ReadLeft > (INT64)sizeof(ReqBuf) ? (INT64)sizeof(ReqBuf) : ReadLeft), ReqFile);
			if (sts_net_send(&client, ReqBuf, (int)ReqRead) < 0) { SendSuccess = false; break; }
		}
		fclose(ReqFile);
	}
	else
	#endif
	{
		bool LoggedWait = false;
		string WorkFileName = ReqPath;

		for (INT64 ReadNext = RangeFirst, ReadLeft = RangeLast - RangeFirst + 1; ReadLeft; )
		{
			sleep_ms(33);

			sFileBlock* FileBlock = NULL;
			WorkFilesMutex.Lock();
			{
				map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.find(WorkFileName);
				if (itWorkFile == WorkFiles.end()) { WorkFilesMutex.Unlock(); SendSuccess = false; break; }

				for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
					if (ReadNext >= (*itFileBlock)->RangeStart && ReadNext < (*itFileBlock)->RangeEnd) { FileBlock = *itFileBlock; FileBlockLockCount.Increment(); break; }
			}
			WorkFilesMutex.Unlock();

			if (!FileBlock)
			{
				if (!LoggedWait) { LogText("[S%02d]    Waiting for %s byte range "PRLLD" - "PRLLD" to arrive ...", WID, ReqPath, RangeFirst, RangeLast); LoggedWait = true; }
				continue;
			}
			if (LoggedWait) { LogText("[S%02d]    Received %s byte range "PRLLD" - "PRLLD" from download thread - serving", WID, ReqPath, RangeFirst, RangeLast); LoggedWait = false; }

			INT64 FileBlockOffset = ReadNext - FileBlock->RangeStart, FileBlockUse = (ReadLeft > (INT64)FileBlock->Data.size() - FileBlockOffset ? (INT64)FileBlock->Data.size() - FileBlockOffset : ReadLeft);
			//LogText("[S%02d]    Sending data %s byte range "PRLLD" - "PRLLD, WID, ReqPath, ReadNext, ReadNext + FileBlockUse - 1);
			if (sts_net_send(&client, &FileBlock->Data[(size_t)(FileBlockOffset)], (int)FileBlockUse) < 0) SendSuccess = false;
			FileBlockLockCount.Decrement();
			if (!SendSuccess) break;
			ReadNext += FileBlockUse;
			ReadLeft -= FileBlockUse;

			WorkFilesMutex.Lock();
			{
				map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.find(WorkFileName);
				if (itWorkFile != WorkFiles.end()) { itWorkFile->second->OffsetServed = ReadNext; itWorkFile->second->LastServe = GetTicksSecond(); }
				else { SendSuccess = false; ReadLeft = 0; }
			}
			WorkFilesMutex.Unlock();
		}
	}
	return SendSuccess;
}

static void ServerWorker(size_t WID)
{
	LogText("[S%02d] Server worker starting up", WID);
	sWorkRequest* WorkRequest;
	for (unsigned int loop = 0;; loop++)
	{
		sleep_ms(33);

		if (WID == 1 && !(loop % 303)) //about every 10 seconds
		{
			WorkFilesMutex.Lock();
			for (map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.begin(); itWorkFile != WorkFiles.end();)
			{
				if (GetSecondsSince(itWorkFile->second->LastServe) < ClearBufferAfterSeconds) { ++itWorkFile; continue; }
				LogText("[S%02d] Purging %d buffers for %s", WID, itWorkFile->second->BufferedBlocks.size(), itWorkFile->first.c_str());
				FileBlockLockCount.WaitForZero();
				for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
					delete *itFileBlock;
				delete itWorkFile->second;
				itWorkFile = WorkFiles.erase(itWorkFile);
			}
			WorkFilesMutex.Unlock();
		}

		WorkRequest = NULL;
		WorkRequestsMutex.Lock();
		if (WorkRequests.size()) { WorkRequest = WorkRequests[0]; WorkRequests.erase(WorkRequests.begin()); }
		WorkRequestsMutex.Unlock();
		if (!WorkRequest) continue;

		//LogText("    %.*s", WorkRequest->Request.size(), &WorkRequest->Request[0]);
		ProcessRequest(WID, WorkRequest->Client, &WorkRequest->Request[0]);
		//LogText("    Disconnecting client");
		sts_net_close_socket(&WorkRequest->Client);
		delete WorkRequest;
	}
};

static const char MozRepl_Reset[] =
	//"try { const Cc = Components.classes; const Ci = Components.interfaces; const Cr = Components.results; const Cu = Components.utils; } catch (e) {}" //for non firefox
	"var obss = Cc['@mozilla.org/observer-service;1'].getService(Ci.nsIObserverService);"
	"try{while(1)obss.removeObserver(obss.enumerateObservers('http-on-examine-response').getNext(), 'http-on-examine-response');}catch(e){}"
	"try{while(1)obss.removeObserver(obss.enumerateObservers('http-on-examine-cached-response').getNext(), 'http-on-examine-cached-response');}catch(e){}"
	"try{while(1)obss.removeObserver(obss.enumerateObservers('http-on-examine-merged-response').getNext(), 'http-on-examine-merged-response');}catch(e){}";

static const char MozRepl_ResetB[] =
	"try{content.window.document.body.style.background  = 'red';}catch(e){}"
	"return 7;";

static const char MozRepl_Initialize[] =
	"var myobs ="
	"{"
		"observe:function(subject, topic, data)"
		"{"
			"subject.QueryInterface(Ci.nsIHttpChannel);"
			     "if (subject.URI.host == 'pc.video.dmkt-sp.jp')" "{ if (subject.name.search('/vapi/request')<0 && subject.name.search(/\\/resume\\?rid=/)<0) return; }"
			"else if (subject.URI.host == '" STREAMING_HOST "')"  "{ if (subject.name.search(/\\.mpd/)<0 || subject.name.search('/trailer/')>0) return; }"
			"else if (subject.URI.host == 'stlog.d.dmkt-sp.jp')"  "{ subject.cancel(Cr.NS_BINDING_ABORTED); return; }"
			"else return;"

			"subject.QueryInterface(Ci.nsITraceableChannel);"
			"var l = { OL: null, data: '',"
				"onStartRequest: function(request, context) { try { this.OL.onStartRequest(request, context); } catch (e) { alert('E-START:'+e); } },"
				"onDataAvailable: function(request, context, is, offset, count){try{"
						"var bis = Cc['@mozilla.org/binaryinputstream;1'].createInstance(Ci.nsIBinaryInputStream);"
						"bis.setInputStream(is);"
						"this.data += bis.readBytes(count);"
					"}catch(e){alert('E-DATA:'+e);}},"
				"onStopRequest: function(request, context, statuscode){try{"
					//"alert('STOP: ' + request.name);"
					"var buf = this.data, sis = Cc['@mozilla.org/io/string-input-stream;1'].createInstance(Ci.nsIStringInputStream);"
					//"alert('STOP - URI [' + subject.URI.asciiSpec + ']\\n\\n' + buf.substr(0,1000));" //show unfiltered response in browser
					"if (request.name.search(/\\/resume\\?rid=/)>0)"
					"{"
						"buf = buf.replace(/play_flag=0/, 'play_flag=1');"
						"appbuf = 1;"
					"}"
					"else if (request.name.search('vapi/request')<0 && buf.search('bandwidth=')<0)"
					"{"
						"alert('MPD file request returned \"Not found\" response :-(\\n\\n'+buf);"
						"if (content.window && content.window.document && content.window.document.body) content.window.document.body.style.background  = 'red';" //error
					"}"
					"else if (request.name.search('vapi/request')<0)"
					"{"
						"var id = 1, maxbw = 0, rereq = 0, pc_content_id = (subject.URI.asciiSpec.match(/(\\d+)\\.mpd/)||[])[1];"
						"for (var re = /video[^>]+bandwidth=[^0-9]?([0-9]+)/g, a; a = re.exec(buf);) if (a[1]>maxbw) maxbw = a[1]|0;" //find max video bandwidth number
						#if defined(APP_USER_AGENT) && defined(APP_COOKIE_VIDEOID)
						"if (maxbw < 2500000 && content.window.DTepid && !content.window.DTbqd)" //if the offered max bandwidth is low, lets retry with APP request to see if we get a better stream
						"{"
							"content.window.DTbqd = 1;" //bad qual done
							"alert('EpisodeId: '+content.window.DTepid+', ContentId: '+pc_content_id+', Max Width: '+(buf.match(/ maxWidth=\"(\\d+)/)||[])[1]+', Max Height: '+(buf.match(/ maxHeight=\"(\\d+)/)||[])[1]+', Max Bandwidth: '+maxbw+'\\nQuality is low! Retry with App request to see if we get a higher quality');"
							"var c, os, ios = Cc['@mozilla.org/network/io-service;1'].getService(Ci.nsIIOService), bis = Cc['@mozilla.org/binaryinputstream;1'].createInstance(Ci.nsIBinaryInputStream);"

							"sis.data = '{\"params\":{\"episodeId\":'+content.window.DTepid+'},\"id\":\"1\",\"jsonrpc\":\"2.0\",\"method\":\"streamingContentInfo\"}';"
							"c = ios.newChannelFromURI(ios.newURI('https://app.video.dmkt-sp.jp/vapi/request'));"
							"c.QueryInterface(Ci.nsIHttpChannel).QueryInterface(Ci.nsIUploadChannel);"
							"c.setRequestHeader('User-Agent', '" APP_USER_AGENT "',0);"
							"c.setRequestHeader('Cookie', 'videoid=" APP_COOKIE_VIDEOID "',0);"
							#ifdef APP_ADDITIONAL_HEADER
							APP_ADDITIONAL_HEADER
							#endif
							"c.setUploadStream(sis, 'application/json; charset=utf-8', -1);"
							"c.requestMethod = 'POST';"
							"bis.setInputStream(os = c.open());"
							"var app_vapi = bis.readBytes(os.available());"
							"var app_content_id = app_vapi.match(/contentId\":\"(\\d+)/);"
							"if (!app_content_id)"
							"{"
								"alert('APP VAPI RESULTED IN ERROR! POST: ' + sis.data + ' - RESULT: ' + app_vapi);"
							"}"
							"else if (pc_content_id != app_content_id[1])"
							"{"
								"app_content_id = app_content_id[1];"
								"alert('PC contentId: '+pc_content_id+'\\nAPP contentId: '+app_content_id+'\\n\\nFrom contentUrl: https://" STREAMING_HOST "/g/mpd/d/main/video/'+pc_content_id.substr(0,4)+'/'+pc_content_id+'/'+pc_content_id+'.mpd\\nTo   contentUrl: https://" STREAMING_HOST "/g/mpd/d/main/video/'+app_content_id.substr(0,4)+'/'+app_content_id+'/'+app_content_id+'.mpd\\n');"
								"c = ios.newChannelFromURI(ios.newURI('https://" STREAMING_HOST "/g/mpd/d/main/video/'+app_content_id.substr(0,4)+'/'+app_content_id+'/'+app_content_id+'.mpd'));"
								"c.QueryInterface(Ci.nsIHttpChannel);"
								"bis.setInputStream(os = c.open());"
								"content.window.DTappbuf = bis.readBytes(os.available());"
								//"alert('New MPD buf: ' + content.window.DTappbuf.substr(0,1000));"
							"}"
							"else alert('APP and PC are served the same content ID, playing with bad quality :-(');"
						"}"
						"if (content.window.DTappbuf)"
						"{"
							"buf = content.window.DTappbuf;"
						"}"
						"else"
						#endif
						"{"
							"buf = buf"
								".replace(/minBufferTime=\"PT1.500S\"/, 'minBufferTime=\"PT30.000S\"')" //increase allowed buffer time
								".replace(new RegExp('<Representation[^>]+video[^>]+bandwidth=(?![^0-9]?'+maxbw+')[\\\\s\\\\S]*?</Representation>\\\\s*','g'),'')" //remove all streams except the highest bandwidth one
								".replace(/(Representation id=[^0-9]?)([0-9]+)/g,function(x,y){return y+id++;})" //re-number streams due to removing of low quality streams
								".replace(/<BaseURL>/g, '<BaseURL>' + '" PROXY_URL "' + subject.URI.path.replace(/\\/[^\\/]*$/, '') + '/');" //redirect streaming to localhost server (this program)
						"}"
						//"alert(oldbuf + '\\n\\n----------------------------------------------------------------\\n\\n' + buf);" //show full response
						//"alert('Playing Width: '+(buf.match(/ width=\"(\\d+)/)||[])[1]+', Height: '+(buf.match(/ height=\"(\\d+)/)||[])[1]+', Bandwidth: '+(buf.match(/ bandwidth=\"(\\d+)/)||[])[1]);"
						"if (content.window && content.window.document && content.window.document.body && !content.window.DTreport)"
						"{"
							"content.window.DTreport = 1;"
							"content.window.document.body.style.background  = 'green';" //show that we are active
							"content.window.document.body.append('Playing Width: '+(buf.match(/ width=\"(\\d+)/)||[])[1]+', Height: '+(buf.match(/ height=\"(\\d+)/)||[])[1]+', Bandwidth: '+(buf.match(/ bandwidth=\"(\\d+)/)||[])[1]);"
						"}"
					"}"
					#if defined(APP_USER_AGENT) && defined(APP_COOKIE_VIDEOID)
					"else if ((buf.search('errorCode')>0 && buf.search('forceSd=on')>0))" //if we were asked to force SD then request streams with APP vapi request
					"{"
						"var pc_vapi = buf, c, os, ios = Cc['@mozilla.org/network/io-service;1'].getService(Ci.nsIIOService), bis = Cc['@mozilla.org/binaryinputstream;1'].createInstance(Ci.nsIBinaryInputStream);"

						"sis.data = this.OLpost.replace(/\"}}$/, ',forceSd:on\"}}');"
						"c = ios.newChannelFromURI(ios.newURI('https://pc.video.dmkt-sp.jp/vapi/request'));"
						"c.QueryInterface(Ci.nsIHttpChannel).QueryInterface(Ci.nsIUploadChannel);"
						"c.setRequestHeader('Cookie', this.OLcookie,0);"
						"c.setUploadStream(sis, 'application/json; charset=utf-8', -1);"
						"c.requestMethod = 'POST';"
						"bis.setInputStream(os = c.open());"
						"pc_vapi = bis.readBytes(os.available());"

						"sis.data = '{\"params\":{\"episodeId\":' + this.OLpost.match(/episodeId:(\\d+)/)[1] + '},\"id\":\"1\",\"jsonrpc\":\"2.0\",\"method\":\"streamingContentInfo\"}';"
						"c = ios.newChannelFromURI(ios.newURI('https://app.video.dmkt-sp.jp/vapi/request'));"
						"c.QueryInterface(Ci.nsIHttpChannel).QueryInterface(Ci.nsIUploadChannel);"
						"c.setRequestHeader('User-Agent', '" APP_USER_AGENT "',0);"
						"c.setRequestHeader('Cookie', 'videoid=" APP_COOKIE_VIDEOID "',0);"
						#ifdef APP_ADDITIONAL_HEADER
						APP_ADDITIONAL_HEADER
						#endif
						"c.setUploadStream(sis, 'application/json; charset=utf-8', -1);"
						"c.requestMethod = 'POST';"
						"bis.setInputStream(os = c.open());"
						"var app_vapi = bis.readBytes(os.available());"
						//"alert('APP VAPI POST: ' + sis.data + ' - RESULT: ' + app_vapi);"

						"var pc_content_id = pc_vapi.match(/contentId\":\"(\\d+)/)[1];"
						"var app_content_id = app_vapi.match(/contentId\":\"(\\d+)/)[1];"
						"if (pc_content_id != app_content_id)"
						"{"
							"alert('PC contentId: '+pc_content_id+'\\nAPP contentId: '+app_content_id+'\\n\\nFrom contentUrl: https://" STREAMING_HOST "/g/mpd/d/main/video/'+pc_content_id.substr(0,4)+'/'+pc_content_id+'/'+pc_content_id+'.mpd\\nTo   contentUrl: https://" STREAMING_HOST "/g/mpd/d/main/video/'+app_content_id.substr(0,4)+'/'+app_content_id+'/'+app_content_id+'.mpd\\n');"
							"buf = pc_vapi"
								".replace(/contentUrl\":\"[^\"]+/, 'contentUrl\":\"https://" STREAMING_HOST "/g/mpd/d/main/video/'+app_content_id.substr(0,4)+'/'+app_content_id+'/'+app_content_id+'.mpd')"
								".replace(/,.resumeInfoUrl.:.[^\"]+./, '');"
						"}"
						//"alert('pc_vapi: ' + pc_vapi + '\\n\\n\\napp_vapi: ' + app_vapi + '\\n\\n\\npc_content_id: ' + pc_content_id + '\\napp_content_id: ' + app_content_id + '\\n\\n\\nbuf: ' + buf);"
					"}"
					#endif

					"sis.data = buf;"
					"this.OL.onDataAvailable(request, context, sis, 0, buf.length);"
					"this.OL.onStopRequest(request, context, statuscode);"
				"}catch(e){alert('E-STOP:'+e);}},"
				"QueryInterface: function (IID) { if (IID.equals(Ci.nsIStreamListener) || IID.equals(Ci.nsISupports)) return this; throw Components.results.NS_NOINTERFACE; }"
			"};"
			#if defined(APP_USER_AGENT) && defined(APP_COOKIE_VIDEOID)
			"if (subject.name.search('video\\\\.dmkt-sp\\\\.jp/vapi/request')>0)"
			"{"
				"subject.QueryInterface(Ci.nsIUploadChannel);"
				"subject.uploadStream.QueryInterface(Ci.nsISeekableStream).seek(Ci.nsISeekableStream.NS_SEEK_SET, 0);"
				"var bis = Cc['@mozilla.org/binaryinputstream;1'].createInstance(Ci.nsIBinaryInputStream);"
				"bis.setInputStream(subject.uploadStream);"
				"l.OLpost = bis.readBytes(bis.available());"
				"content.window.DTepid = l.OLpost.match(/episodeId:(\\d+)/)[1];"
				"if (l.OLpost.search(',forceSd:on')>0) return;"
				"l.OLcookie = subject.getRequestHeader('Cookie');"
				//"alert('OLREQ URI [' + subject.URI.asciiSpec + ']\\n\\nOLcookie: ' + l.OLcookie + '\\n\\nOLpost: ' + l.OLpost);"
			"}"
			#endif
			"l.OL = subject.setNewListener(l);"
		"},"
		"QueryInterface : function (IID) { if (IID.equals(Ci.nsIObserver) || IID.equals(Ci.nsISupports)) return this; throw Components.results.NS_NOINTERFACE; }"
	"};"
	"var res = 0;"
	"res |= (obss.addObserver(myobs, 'http-on-examine-response', false)?0:1);"
	"res |= (obss.addObserver(myobs, 'http-on-examine-cached-response', false)?0:2);"
	"res |= (obss.addObserver(myobs, 'http-on-examine-merged-response', false)?0:4);"
	"setTimeout(function(){res == 7 ? content.window.document.body.style.background  = 'yellow' : alert(res == 7 ? 'Successfully initialized dtv proxy observers' : 'Error while initializing dtv proxy observer!!');},50);"
	"return res;";

static bool InitMozRepl(const char* CmdA, const char* CmdB)
{
	sts_net_socket_t mozrepl;
	if (sts_net_connect(&mozrepl, MOZREPL_HOST, MOZREPL_PORT) < 0)
	{
		LogError("Could not connect to MozRepl on %s:%d - Aborting", MOZREPL_HOST, MOZREPL_PORT);
		return false;
	}

	for (string MOZREPL_MyId;;)
	{
		char MOZREPL_Response[1024], *MOZREPL_ResponseId = NULL, *MOZREPL_Start = NULL, *MOZREPL_End = NULL;
		for (int i = 0; i < sizeof(MOZREPL_Response) - 1; )
		{
			if (sts_net_check_socket(&mozrepl, 5.f) != 1)
			{
				LogError("No data end received from MozRepl - Aborting\nReceived so far:\n%.*s", i, MOZREPL_Response);
				sts_net_close_socket(&mozrepl);
				return false;
			}
			i += sts_net_recv(&mozrepl, MOZREPL_Response + i, sizeof(MOZREPL_Response) - 1 - i);
			if (i<4 || MOZREPL_Response[i-2] != '>') continue;
			for (char* p = &MOZREPL_Response[i-3]; p > MOZREPL_Response; p--) { if (p[-1] == '\n') { MOZREPL_ResponseId = p; break; } }
			if (!MOZREPL_ResponseId) continue;
			MOZREPL_Response[i-2] = '\0';
			for (char* p = &MOZREPL_Response[0]; p < MOZREPL_ResponseId; p++)  { if (p[0]  != ' ' && p[0]  != '.' && p[0]  != '>' && p[0]  != '\n' && p[0]  != '\r')  { MOZREPL_Start = p; break; } }
			for (char* p = &MOZREPL_ResponseId[-1]; p > MOZREPL_Response; p--) { if (p[-1] != ' ' && p[-1] != '.' && p[-1] != '>' && p[-1] != '\n' && p[-1] != '\r') { MOZREPL_End = p; break; } }
			if (MOZREPL_ResponseId && (MOZREPL_MyId.empty() || MOZREPL_MyId == string(MOZREPL_ResponseId))) break;
		}
		if (MOZREPL_MyId.empty())
		{
			MOZREPL_MyId = MOZREPL_ResponseId;
			string MOZREPL_Execute = MOZREPL_MyId + ".print( (function() {" + CmdA + CmdB + "})() );";
			sts_net_send(&mozrepl, MOZREPL_Execute.c_str(), (int)MOZREPL_Execute.length());
			continue;
		}
		sts_net_close_socket(&mozrepl);
		int result = atoi(MOZREPL_Start);
		if (result == 7) return true;
		LogError("Error during MozRepl execution, result is not 7 - Aborting");
		return false;
	}
}

int main(int argc, char *argv[])
{
	const char *BindAddress = NULL;
	for (int i = 1; i < argc; i++)
	{
		if (argv[i][0] != '-' && argv[i][0] != '/') continue;
		if      (argv[i][1] == 'b' && i < argc - 1) BindAddress = argv[++i];
		else if (argv[i][1] == 'h' || argv[i][1] == '?') { ShowHelp(); return 0; }
	}

	if (sts_net_init() < 0)
	{
		LogError("Could not initialize networking - Aborting");
		return EXIT_FAILURE;
	}

	if (!InitMozRepl(MozRepl_Reset, MozRepl_Initialize)) return EXIT_FAILURE;

	sts_net_socket_t server;
	if (sts_net_init() < 0 || sts_net_listen(&server, PROXY_PORT, BindAddress) < 0)
	{
		if (BindAddress) LogError("Webserver could not listen on %s:%d - Aborting", BindAddress, PROXY_PORT);
		else LogError("Webserver could not listen on port %d - Aborting", PROXY_PORT);
		return EXIT_FAILURE;
	}

	#ifdef DTVPROXY_USE_WIN32SYSTRAY
	CreateThread(NULL, 0, SystrayThread, NULL, 0, NULL);
	#endif

	if (BindAddress)
	{
		strcpy(g_AddressTable[0].address, BindAddress);
		g_AddressCount = 1;
	}
	else
	{
		g_AddressCount = sts_net_enumerate_interfaces(g_AddressTable, 20, 1, 0);
		if (g_AddressCount > 20) g_AddressCount = 20;
	}

	for (int i = 0; i < g_AddressCount; i++)
		LogText((PROXY_PORT == 80 ? "Serving on http://%s/" : "Serving on http://%s:%d/"), g_AddressTable[i].address, PROXY_PORT);

	for (int WID = 1; WID <= TotalNumDownloadWorkers; WID++)
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&DownloadWorker, (LPVOID)WID, 0, NULL);

	for (int WID = 1; WID <= TotalNumServerWorkers; WID++)
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&ServerWorker, (LPVOID)WID, 0, NULL);

	for (;;)
	{
		sts_net_socket_t client;
		if (sts_net_accept_socket(&server, &client) < 0) { LogError("Unknown error while accepting new connection"); continue; }

		char clientname[64] = {0,0};
		sts_net_gethostname(&client, clientname, sizeof(clientname), 1, NULL);
		//LogText("Client connected '%s'!", clientname);

		char Request[2048];
		int RequestSize = (sts_net_check_socket(&client, 1.f) ? sts_net_recv(&client, Request, sizeof(Request)-1) : 0);
		Request[RequestSize > 0 ? RequestSize : 0] = '\0'; //zero terminate for strstr
		if (RequestSize < 7 || !strstr(Request, "\r\n\r\n") || !strchr(Request, ' ') || strstr(Request, "/favicon"))
		{
			//ignore failed connections, illegal http requests, favicon requests
			sts_net_close_socket(&client);
		}
		else
		{
			WorkRequestsMutex.Lock();
			WorkRequests.push_back(new sWorkRequest(client, Request));
			WorkRequestsMutex.Unlock();
		}
	}
}

#ifdef DTVPROXY_USE_WIN32SYSTRAY
int WINAPI WinMain(HINSTANCE, HINSTANCE,LPSTR, int)
{
	return main(__argc, __argv);
}
static void Tray_FlipActive()
{
	g_Active ^= 1;
	(g_Active ? InitMozRepl(MozRepl_Reset, MozRepl_Initialize) : InitMozRepl(MozRepl_Reset, MozRepl_ResetB));
	Tray_ResetAll();
}
static void Tray_ResetAll()
{
	WorkFilesMutex.Lock();
	FileBlockLockCount.WaitForZero();
	for (map<string, sWorkFile*>::iterator itWorkFile = WorkFiles.begin(); itWorkFile != WorkFiles.end(); ++itWorkFile)
	{
		for (vector<sFileBlock*>::iterator itFileBlock = itWorkFile->second->BufferedBlocks.begin(); itFileBlock != itWorkFile->second->BufferedBlocks.end(); ++itFileBlock)
			delete *itFileBlock;
		delete itWorkFile->second;
	}
	WorkFiles.clear();
	WorkFilesMutex.Unlock();
	WorkRequestsMutex.Lock();
	for (vector<sWorkRequest*>::iterator itWorkRequest = WorkRequests.begin(); itWorkRequest != WorkRequests.end(); ++itWorkRequest)
		delete *itWorkRequest;
	WorkRequests.clear();
	WorkRequestsMutex.Unlock();
}
#endif
