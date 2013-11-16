/*****************************************************************************
 * cad.c : CAD IPC interface plugin
 *****************************************************************************
 * Copyright (C) 2011 Birunthan Mohanathas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#define UNICODE
#define _UNICODE

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_strings.h>
#include <vlc_aout.h>
#include <vlc_url.h>

/*****************************************************************************
 * intf_sys_t: description and status of log interface
 *****************************************************************************/
struct intf_sys_t
{
	bool b_event_callback;
};

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Open(vlc_object_t* p_this);
static void Close(vlc_object_t* p_this);

static int ItemChange(vlc_object_t* p_this, const char* psz_var, vlc_value_t oldval, vlc_value_t newval, void* param);
static int StateChange(vlc_object_t* p_this, const char* psz_var, vlc_value_t oldval, vlc_value_t newval, void* param);
static unsigned __stdcall TrackChangeProc(void* data);
static unsigned __stdcall WindowThreadProc(void* data);
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/*****************************************************************************
 * Globals
 *****************************************************************************/
 
enum IPCMESSAGE
{
	IPC_PLAY = 100,
	IPC_PLAYPAUSE = 101,
	IPC_PAUSE = 102,
	IPC_STOP = 103,
	IPC_NEXT = 104,
	IPC_PREVIOUS = 105,
	IPC_VOLUME_CHANGED_NOTIFICATION = 108,
	IPC_SET_VOLUME = 108,
	IPC_GET_VOLUME = 109,
	IPC_GET_CURRENT_TRACK = 110,
	IPC_GET_DURATION = 113,
	IPC_SET_POSITION = 114,
	IPC_SET_CALLBACK_HWND = 120,
	IPC_GET_POSITION = 122,
	IPC_TRACK_CHANGED_NOTIFICATION = 123,
	IPC_SHOW_WINDOW = 124,
	IPC_GET_STATE = 125,
	IPC_STATE_CHANGED_NOTIFICATION = 126,
	IPC_SET_REPEAT = 128,
	IPC_SHUTDOWN_NOTIFICATION = 129,
	IPC_GET_REPEAT = 130,
	IPC_CLOSE = 131,
	IPC_GET_SHUFFLE = 140,
	IPC_SET_SHUFFLE = 141,
	IPC_RATING_CHANGED_NOTIFICATION = 639,
	IPC_SET_RATING = 639,
	IPC_REGISTER_NOTIFICATION = 700,
	IPC_CURRENT_TRACK_NOTIFICATION = 701,
	IPC_CURRENT_LYRICS_NOTIFICATION = 702,
	IPC_NEW_LYRICS_NOTIFICATION = 703,
	IPC_NEW_COVER_NOTIFICATION = 800,
	IPC_GET_LYRICS = 801
};

#define DATA_MAX_LENGTH 1024
#define NOWPLAYING_TIMER 1

static HWND g_CAD = NULL;
static HWND g_Window = NULL;
static vlc_object_t* g_VLC = NULL;

/*****************************************************************************
 * Module descriptor
 *****************************************************************************
 * This module allows Windows applications to query currently playing media
 * information using window messages.
 *****************************************************************************/

vlc_module_begin()
	set_category(CAT_INTERFACE)
	set_subcategory(SUBCAT_INTERFACE_CONTROL)
	set_shortname("CAD")
	set_description(N_("CAD Now-Playing"))

	set_capability("interface", 0)
	set_callbacks(Open, Close)
vlc_module_end()

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
static int Open(vlc_object_t* p_this)
{
	intf_thread_t* p_intf = (intf_thread_t*)p_this;
	playlist_t* p_playlist;

	p_intf->p_sys = malloc(sizeof(intf_sys_t));
	if (!p_intf->p_sys)
	{
		return VLC_ENOMEM;
	}

	p_playlist = pl_Get(p_intf);
	var_AddCallback(p_playlist, "item-current", ItemChange, p_intf);

	g_VLC = p_this;
	_beginthreadex(NULL, 0, WindowThreadProc, NULL, 0, NULL);

	return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: destroy interface stuff
 *****************************************************************************/
static void Close(vlc_object_t* p_this)
{
	intf_thread_t* p_intf = (intf_thread_t*)p_this;
	intf_sys_t* p_sys = p_intf->p_sys;

	playlist_t* p_playlist = pl_Get(p_this);
	var_DelCallback(p_playlist, "item-current", ItemChange, p_intf);

	input_thread_t* p_input = playlist_CurrentInput(p_playlist);
	if (p_input)
	{
		if (p_sys->b_event_callback)
		{
			var_DelCallback(p_input, "intf-event", StateChange, p_intf);
		}

		vlc_object_release(p_input);
	}
	
	if (g_CAD)
	{
		PostMessage(g_CAD, WM_USER, (WPARAM)NULL, (LPARAM)IPC_SHUTDOWN_NOTIFICATION);
	}

	if (g_Window)
	{
		DestroyWindow(g_Window);
		UnregisterClass(L"CD Art Display IPC Class", GetModuleHandle(NULL));
	}

	// Destroy structure
	free(p_intf->p_sys);
}

/*****************************************************************************
 * ItemChange: Playlist item change callback
 *****************************************************************************/
static int ItemChange(vlc_object_t* p_this, const char* psz_var, vlc_value_t oldval, vlc_value_t newval, void* param)
{
	VLC_UNUSED(p_this);
	VLC_UNUSED(psz_var);
	VLC_UNUSED(oldval);
	VLC_UNUSED(newval);

	if (g_CAD)
	{
		intf_thread_t* p_intf = (intf_thread_t*)param;
		intf_sys_t* p_sys = p_intf->p_sys;
		
		input_thread_t* p_input = playlist_CurrentInput(pl_Get(p_intf));
		if (!p_input || p_input->b_dead)
		{
			return VLC_SUCCESS;
		}

		input_item_t* p_item = input_GetItem(p_input);
		if (!p_item)
		{
			vlc_object_release(p_input);
			return VLC_SUCCESS;
		}

		// Delay the IPC_TRACK_CHANGED_NOTIFICATION notification by ~500ms for VLC to parse item
		_beginthreadex(NULL, 0, TrackChangeProc, NULL, 0, NULL);

		var_AddCallback(p_input, "intf-event", StateChange, p_intf);
		p_sys->b_event_callback = true;

		vlc_object_release(p_input);
	}

	return VLC_SUCCESS;
}

/*****************************************************************************
 * StateChange: Playing status change callback
 *****************************************************************************/
static int StateChange(vlc_object_t* p_this, const char* psz_var, vlc_value_t oldval, vlc_value_t newval, void* param)
{
	VLC_UNUSED(psz_var);
	VLC_UNUSED(oldval);
	VLC_UNUSED(newval);
	VLC_UNUSED(param);

	switch (newval.i_int)
	{
	case INPUT_EVENT_STATE:
		{
			input_thread_t* p_input = (input_thread_t*)p_this;
			int state;

			switch (var_GetInteger(p_input, "state"))
			{
			case PLAYING_S:
				state = 1;
				break;

			case PAUSE_S:
				state = 2;
				break;

			default:
				state = 0;
			}

			PostMessage(g_CAD, WM_USER, (WPARAM)state, (LPARAM)IPC_STATE_CHANGED_NOTIFICATION);
		}
		break;
	}

	return VLC_SUCCESS;
}

static unsigned __stdcall TrackChangeProc(void* data)
{
	VLC_UNUSED(data);

	Sleep(500);
	SendMessage(g_CAD, WM_USER, (WPARAM)NULL, (LPARAM)IPC_TRACK_CHANGED_NOTIFICATION);

	return 0;
}

static unsigned __stdcall WindowThreadProc(void* data)
{
	VLC_UNUSED(data);

	HMODULE hModule = GetModuleHandle(NULL);

	// Create windows class
	WNDCLASS wc;
	memset(&wc, 0, sizeof(WNDCLASS));
	wc.hInstance = hModule;
	wc.lpfnWndProc = WindowProc;
	wc.lpszClassName = L"CD Art Display IPC Class";
	RegisterClass(&wc);

	// Create window
	g_Window = CreateWindow(L"CD Art Display IPC Class",
							L"VLC",
							WS_DISABLED,
							CW_USEDEFAULT,
							CW_USEDEFAULT,
							CW_USEDEFAULT,
							CW_USEDEFAULT,
							NULL,
							NULL,
							hModule,
							NULL);

	#define MSGFLT_ALLOW 1
	typedef struct tagCHANGEFILTERSTRUCT
	{
		DWORD cbSize;
		DWORD ExtStatus;
	} CHANGEFILTERSTRUCT, *PCHANGEFILTERSTRUCT;

	typedef BOOL (WINAPI * FPCHANGEWINDOWMESSAGEFILTER)(UINT message, DWORD dwFlag);
	typedef BOOL (WINAPI * FPCHANGEWINDOWMESSAGEFILTEREX)(HWND hWnd, UINT message, DWORD dwFlag, PCHANGEFILTERSTRUCT pChangeFilterStruct);

	// Need to remove WM_USER from filtered messages
	HMODULE hUser32 = LoadLibrary(L"user32.dll");
	if (hUser32)
	{
		// Try ChangeWindowMessageFilterEx first (Win7+)
		FPCHANGEWINDOWMESSAGEFILTEREX ChangeWindowMessageFilterEx = (FPCHANGEWINDOWMESSAGEFILTEREX)GetProcAddress(hUser32, "ChangeWindowMessageFilterEx");
		if (ChangeWindowMessageFilterEx)
		{
			ChangeWindowMessageFilterEx(g_Window, WM_USER, MSGFLT_ALLOW, NULL);
		}
		else
		{
			// Try ChangeWindowMessageFilter (Vista)
			FPCHANGEWINDOWMESSAGEFILTER ChangeWindowMessageFilter = (FPCHANGEWINDOWMESSAGEFILTER)GetProcAddress(hUser32, "ChangeWindowMessageFilter");
			if (ChangeWindowMessageFilter)
			{
				ChangeWindowMessageFilter(WM_USER, MSGFLT_ALLOW);
			}
		}

		FreeLibrary(hUser32);
	}

	g_CAD = FindWindow(NULL, L"CD Art Display 1.x Class");
	if (g_CAD)
	{
		// Register with CAD
		WCHAR filename[MAX_PATH];
		GetModuleFileName(hModule, &filename, MAX_PATH);

		WCHAR buffer[DATA_MAX_LENGTH];
		_snwprintf(buffer, DATA_MAX_LENGTH, L"1\tCD Art Display IPC Class\tVLC\t%s\t", filename);

		COPYDATASTRUCT cds;
		cds.dwData = IPC_REGISTER_NOTIFICATION;
		cds.lpData = buffer;
		cds.cbData = (wcslen(buffer) + 1) * 2;

		SendMessage(g_CAD, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
	}

	// Standard message loop
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_USER)
	{
		switch (lParam)
		{
		case IPC_PLAY:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				playlist_Play(p_playlist);
				return 1;
			}

		case IPC_PLAYPAUSE:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (playlist_Status(p_playlist) == PLAYLIST_RUNNING)
				{
					playlist_Pause(p_playlist);
				}
				else
				{
					playlist_Play(p_playlist);
				}
				return 1;
			}

		case IPC_PAUSE:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				playlist_Pause(p_playlist);
				return 1;
			}

		case IPC_STOP:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				playlist_Stop(p_playlist);
				return 1;
			}
			
		case IPC_NEXT:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				playlist_Next(p_playlist);
				return 1;
			}

		case IPC_PREVIOUS:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				playlist_Prev(p_playlist);
				return 1;
			}

		case IPC_SET_VOLUME:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				aout_VolumeSet(p_playlist, ((wParam * (AOUT_VOLUME_MAX - AOUT_VOLUME_MIN)) / 400 + AOUT_VOLUME_MIN));
				return 1;
			}

		case IPC_GET_VOLUME:
			{
				audio_volume_t volume;
				aout_VolumeGet(g_VLC, &volume);
				++volume;
				volume *= 100;
				volume /= 255;
				return (int)volume;
			}

		case IPC_GET_DURATION:
			{
				input_thread_t* p_input =  __pl_CurrentInput(g_VLC);
				if (!p_input) return 0;

				input_item_t* p_item = input_GetItem(p_input);
				unsigned int duration = (unsigned int)(input_item_GetDuration(p_item) / 1000000);
				vlc_object_release(p_input);
				return duration;
			}

		case IPC_GET_POSITION:
			{
				input_thread_t* p_input = __pl_CurrentInput(g_VLC);
				if (!p_input) return 0;

				int pos = (int)(var_GetTime(p_input, "time") / CLOCK_FREQ);
				vlc_object_release(p_input);
				return pos;
			}

		case IPC_SET_POSITION:
			{
				input_thread_t* p_input = __pl_CurrentInput(g_VLC);
				if (!p_input) return 0;

				var_SetTime(p_input, "time", (int64_t)wParam * CLOCK_FREQ);
				vlc_object_release(p_input);
				return 0;
			}

		case IPC_GET_SHUFFLE:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (p_playlist)
				{
					return (int)var_GetBool(p_playlist, "random");
				}

				return 0;
			}

		case IPC_SET_SHUFFLE:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (p_playlist)
				{
					return (int)var_SetBool(p_playlist, "random", (bool)wParam);
				}

				return 1;
			}

		case IPC_GET_REPEAT:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (p_playlist)
				{
					return (int)var_GetBool(p_playlist, "repeat");
				}

				return 0;
			}

		case IPC_SET_REPEAT:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (p_playlist)
				{
					return (int)var_SetBool(p_playlist, "repeat", (bool)wParam);
				}

				return 1;
			}

		case IPC_SET_RATING:
			{
				// Send back 0
				PostMessage(g_CAD, WM_USER, (WPARAM)0, (LPARAM)IPC_RATING_CHANGED_NOTIFICATION);
				return 0;
			}

		case IPC_SET_CALLBACK_HWND:
			{
				g_CAD = (HWND)wParam;
				return 1;
			}

		case IPC_SHOW_WINDOW:
			{
				// TODO
				return 0;
			}

		case IPC_GET_STATE:
			{
				playlist_t* p_playlist = pl_Get(g_VLC);
				if (p_playlist)
				{
					switch (playlist_Status(p_playlist))
					{
					case PLAYLIST_RUNNING:
						return 1;

					case PLAYLIST_PAUSED:
						return 2;
					}
				}

				return 0;
			}

		case IPC_SHUTDOWN_NOTIFICATION:
			{
				g_CAD = NULL;
				return 1;
			}

		case IPC_CLOSE:
			{
				libvlc_Quit(g_VLC->p_libvlc);
				return 1;
			}

		case IPC_GET_CURRENT_TRACK:
			{
				KillTimer(hwnd, NOWPLAYING_TIMER);

				input_thread_t* p_input = __pl_CurrentInput(g_VLC);
				if (!p_input) return 0;

				input_item_t* p_item = input_GetItem(p_input);
				char* file = input_item_GetURI(p_item);
				decode_URI(file);

				if (strncmp(file, "file://", 7) == 0)
				{
					char* artist = input_item_GetArtist(p_item);
					char* album = input_item_GetAlbum(p_item);
					char* cover = input_item_GetArtworkURL(p_item);
					decode_URI(cover);
					unsigned int duration = (unsigned int)(input_item_GetDuration(p_item) / 1000000);
					
					char* title = input_item_GetTitleFbName(p_item);

					char cBuffer[DATA_MAX_LENGTH];
					_snprintf(cBuffer, DATA_MAX_LENGTH, "%s\t%s\t%s\t\t\t\t\t%u\t%s\t\t%s\t\t\t\t\t\t\t",
														title ? title : "",
														artist ? artist : "",
														album ? album : "",
														duration,
														file ? &file[8] : "",
														cover ? &cover[8] : "");  // skip the file://

					wchar_t wBuffer[DATA_MAX_LENGTH];
					MultiByteToWideChar(CP_UTF8, 0, cBuffer, -1, &wBuffer, DATA_MAX_LENGTH);

					COPYDATASTRUCT cds;
					cds.dwData = IPC_CURRENT_TRACK_NOTIFICATION;
					cds.lpData = (PVOID)&wBuffer;
					cds.cbData = (wcslen(wBuffer) + 1) * 2;
					SendMessage(g_CAD, WM_COPYDATA, (WPARAM)IPC_CURRENT_TRACK_NOTIFICATION, (LPARAM)&cds);

					free(title);
					free(artist);
					free(album);
					free(file);
					free(cover);
				}
				else
				{
					SetTimer(hwnd, NOWPLAYING_TIMER, 2500, NULL);
				}

				vlc_object_release(p_input);
				return 1;
			}
		}

		return 0;
	}
	else if (msg == WM_TIMER && wParam == NOWPLAYING_TIMER)
	{
		input_thread_t* p_input = __pl_CurrentInput(g_VLC);
		if (!p_input) return 0;

		input_item_t* p_item = input_GetItem(p_input);
		char* nowplaying = input_item_GetNowPlaying(p_item);

		if (nowplaying)
		{
			char* artist = NULL;
			char* title = nowplaying;
			char* pos = strstr(nowplaying, " - ");
			if (pos)
			{
				pos[0] = '\0';
				artist = title;
				title = pos + 3;	// Skip the " - "
			}

			char cBuffer[DATA_MAX_LENGTH];
			_snprintf(cBuffer, DATA_MAX_LENGTH, "%s\t%s\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t",
												title ? title : "",
												artist ? artist : "");  // skip the file://

			wchar_t wBuffer[DATA_MAX_LENGTH];
			MultiByteToWideChar(CP_UTF8, 0, cBuffer, -1, &wBuffer, DATA_MAX_LENGTH);

			COPYDATASTRUCT cds;
			cds.dwData = IPC_CURRENT_TRACK_NOTIFICATION;
			cds.lpData = (PVOID)&wBuffer;
			cds.cbData = (wcslen(wBuffer) + 1) * 2;
			SendMessage(g_CAD, WM_COPYDATA, (WPARAM)IPC_CURRENT_TRACK_NOTIFICATION, (LPARAM)&cds);

			free(nowplaying);
		}

		vlc_object_release(p_input);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}
