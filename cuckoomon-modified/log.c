/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2014 Cuckoo Sandbox Developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "ntapi.h"
#include "hooking.h"
#include "misc.h"
#include "utf8.h"
#include "log.h"
#include "bson.h"
#include "pipe.h"
#include "config.h"

// the size of the logging buffer
#define BUFFERSIZE 16 * 1024 * 1024
#define BUFFER_LOG_MAX 256
#define LARGE_BUFFER_LOG_MAX 64 * 1024
#define BUFFER_REGVAL_MAX 512

static CRITICAL_SECTION g_mutex;
static CRITICAL_SECTION g_writing_log_buffer_mutex;
static SOCKET g_sock;
static unsigned int g_starttick;

static char *g_buffer;
static volatile int g_idx;

// current to-be-logged API call
static bson g_bson[1];
static char g_istr[4];

static char logtbl_explained[256] = {0};

#define LOG_ID_PROCESS 0
#define LOG_ID_THREAD 1
#define LOG_ID_ANOMALY 2
#define LOG_ID_ANOMALY_EXTRA 3

int g_log_index = 10;  // index must start after the special IDs (see defines)

//
// Log API
//

static HANDLE g_log_thread_handle;
static HANDLE g_logwatcher_thread_handle;
static HANDLE g_log_flush;

// snprintf can end up acquiring the process' heap lock which will be unsafe in the context of a hooked
// NtAllocate/FreeVirtualMemory
static void num_to_string(char *buf, unsigned int buflen, unsigned int num)
{
	unsigned int dec = 1000000000;
	unsigned int i = 0;

	if (!buflen)
		return;

	while (dec) {
		if (!i && ((num / dec) || dec == 1))
			buf[i++] = '0' + (num / dec);
		else if (i)
			buf[i++] = '0' + (num / dec);
		if (i == buflen - 1)
			break;
		num = num % dec;
		dec /= 10;
	}
	buf[i] = '\0';
}

extern int process_shutting_down;

static DWORD WINAPI _log_thread(LPVOID param)
{
	hook_disable();

	while (1) {
		WaitForSingleObject(g_log_flush, 500);
		EnterCriticalSection(&g_writing_log_buffer_mutex);
		while (g_idx > 0) {
			int written = -1;

			if (g_sock == DEBUG_SOCKET) {
				char filename[64];
				char pid[8];
				strcpy(filename, "c:\\debug");
				num_to_string(pid, sizeof(pid), GetCurrentProcessId());
				strcat(filename, pid);
				strcat(filename, ".log");
				// will happen when we're in debug mode
				FILE *f = fopen(filename, "ab");
				if (f) {
					written = (int)fwrite(g_buffer, 1, g_idx, f);
					fclose(f);
				}
				else {
					// some non-admin debug case
					written = g_idx;
				}
			}
			else if (g_sock == INVALID_SOCKET) {
				g_idx = 0;
				continue;
			}
			else {
				written = send(g_sock, g_buffer, g_idx, 0);
			}

			if (written < 0)
				continue;

			// if this call didn't write the entire buffer, then we have to move
			// around some stuff in the buffer
			if (written < g_idx) {
				memmove(g_buffer, g_buffer + written, g_idx - written);
			}

			// subtract the amount of written bytes from the index
			g_idx -= written;
		}
		LeaveCriticalSection(&g_writing_log_buffer_mutex);
	}
}

static DWORD WINAPI _logwatcher_thread(LPVOID param)
{
	hook_disable();

	while (WaitForSingleObject(g_log_thread_handle, 1000) == WAIT_TIMEOUT);

	if (is_shutting_down() == 0) {
		pipe("CRITICAL:Logging thread was terminated!");
	}
	return 0;
}

extern BOOLEAN g_dll_main_complete;

void log_flush()
{
	/* The logging thread we create in DllMain won't actually start until after DllMain
	completes, so we need to ensure we don't wait here on the logging thread as it will
	result in a deadlock.
	There's thus an implicit assumption here that we won't log more than BUFFERSIZE before
	DllMain completes, otherwise we'll lose logs.
	*/
	if (g_dll_main_complete) {
		SetEvent(g_log_flush);
		while (g_idx && (g_sock != INVALID_SOCKET || !process_shutting_down)) raw_sleep(50);
	}
}

static void log_raw_direct(const char *buf, size_t length) {
	size_t copiedlen = 0;
	size_t copylen;

	while (copiedlen != length) {
		EnterCriticalSection(&g_writing_log_buffer_mutex);
		copylen = min(length - copiedlen, (size_t)(BUFFERSIZE - g_idx));
		memcpy(&g_buffer[g_idx], &buf[copiedlen], copylen);
		g_idx += (int)copylen;
		copiedlen += copylen;
		LeaveCriticalSection(&g_writing_log_buffer_mutex);
		if (copiedlen != length)
			log_flush();
	}
}

void debug_message(const char *msg) {
    bson b[1];
    bson_init( b );
    bson_append_string( b, "type", "debug" );
    bson_append_string( b, "msg", msg );
    bson_finish( b );
    log_raw_direct(bson_data( b ), bson_size( b ));
    bson_destroy( b );
    log_flush();
}

/*
static void log_int8(char value)
{
    bson_append_int( g_bson, g_istr, value );
}

static void log_int16(short value)
{
    bson_append_int( g_bson, g_istr, value );
}
*/

static int bson_append_ptr(bson *b, const char *name, ULONG_PTR ptr)
{
	if (sizeof(ULONG_PTR) == 8)
		return bson_append_long(b, name, ptr);
	else
		return bson_append_int(b, name, (int)ptr);
}

static void log_int32(int value)
{
    bson_append_int( g_bson, g_istr, value );
}

static void log_int64(int64_t value)
{
	bson_append_long(g_bson, g_istr, value);
}

static void log_ptr(void *value)
{
	if (sizeof(ULONG_PTR) == 8)
		log_int64((int64_t)value);
	else
		log_int32((int)value);
}

static void log_string(const char *str, int length)
{
    if (str == NULL) {
        bson_append_string_n( g_bson, g_istr, "", 0 );
        return;
    }
    int ret;
    char * utf8s = utf8_string(str, length);
    int utf8len = * (int *) utf8s;
    ret = bson_append_binary( g_bson, g_istr, BSON_BIN_BINARY, utf8s+4, utf8len );
    if (ret == BSON_ERROR) {
		bson_append_string_n(g_bson, g_istr, "", 0);
	}
    free(utf8s);
}

static void log_wstring(const wchar_t *str, int length)
{
    if (str == NULL) {
        bson_append_string_n( g_bson, g_istr, "", 0 );
        return;
    }
    int ret;
    char * utf8s = utf8_wstring(str, length);
    int utf8len = * (int *) utf8s;
    ret = bson_append_binary( g_bson, g_istr, BSON_BIN_BINARY, utf8s+4, utf8len );
    if (ret == BSON_ERROR) {
		bson_append_string_n(g_bson, g_istr, "", 0);
	}
    free(utf8s);
}

static void log_argv(int argc, const char ** argv) {
    bson_append_start_array( g_bson, g_istr );

    for (int i=0; i<argc; i++) {
		num_to_string(g_istr, 4, i);
        log_string(argv[i], -1);
    }
    bson_append_finish_array( g_bson );
}

static void log_wargv(int argc, const wchar_t ** argv) {
    bson_append_start_array( g_bson, g_istr );

    for (int i=0; i<argc; i++) {
		num_to_string(g_istr, 4, i);
		log_wstring(argv[i], -1);
    }

    bson_append_finish_array( g_bson );
}

static void log_buffer(const char *buf, size_t length) {
    size_t trunclength = min(length, BUFFER_LOG_MAX);

    if (buf == NULL) {
        trunclength = 0;
    }

    bson_append_binary( g_bson, g_istr, BSON_BIN_BINARY, buf, trunclength );
}

static void log_large_buffer(const char *buf, size_t length) {
	size_t trunclength = min(length, LARGE_BUFFER_LOG_MAX);

	if (buf == NULL) {
		trunclength = 0;
	}

	bson_append_binary(g_bson, g_istr, BSON_BIN_BINARY, buf, trunclength);
}

static lastlog_t lastlog;

void loq(int index, const char *category, const char *name,
    int is_success, ULONG_PTR return_value, const char *fmt, ...)
{
    va_list args;
    const char * fmtbak = fmt;
    int argnum = 2;
    int count = 1; char key = 0;
	unsigned int repeat_offset = 0;
	unsigned int compare_offset = 0;
	lasterror_t lasterror;

	if (index >= LOG_ID_ANOMALY && g_config.suspend_logging)
		return;

	get_lasterrors(&lasterror);

	EnterCriticalSection(&g_mutex);

	if(logtbl_explained[index] == 0) {
        logtbl_explained[index] = 1;
        const char * pname;
        bson b[1];

		va_start(args, fmt);

		bson_init( b );
        bson_append_int( b, "I", index );
        bson_append_string( b, "name", name );
        bson_append_string( b, "type", "info" );
        bson_append_string( b, "category", category );

        bson_append_start_array( b, "args" );
        bson_append_string( b, "0", "is_success" );
        bson_append_string( b, "1", "retval" );

        while (--count != 0 || *fmt != 0) {
            // we have to find the next format specifier
            if(count == 0) {
                // end of format
                if(*fmt == 0) break;

                // set the count, possibly with a repeated format specifier
                count = *fmt >= '2' && *fmt <= '9' ? *fmt++ - '0' : 1;

                // the next format specifier
                key = *fmt++;
            }

            pname = va_arg(args, const char *);
			num_to_string(g_istr, 4, argnum);
            argnum++;

            //on certain formats, we need to tell cuckoo about them for nicer display / matching
            if (key == 'p' || key == 'P' || key == 'h' || key == 'H') {
				const char *typestr;
				if (key == 'h' || key == 'H' || sizeof(ULONG_PTR) != 8)
					typestr = "h";
				else
					typestr = "p";

				bson_append_start_array( b, g_istr );
                bson_append_string( b, "0", pname );
                bson_append_string( b, "1", typestr );
                bson_append_finish_array( b );
            } else {
                bson_append_string( b, g_istr, pname );
            }

            //now ignore the values
            if(key == 's' || key == 'f') {
                (void) va_arg(args, const char *);
            }
            else if(key == 'S') {
                (void) va_arg(args, int);
                (void) va_arg(args, const char *);
            }
            else if(key == 'u' || key == 'F') {
                (void) va_arg(args, const wchar_t *);
            }
            else if(key == 'U') {
                (void) va_arg(args, int);
                (void) va_arg(args, const wchar_t *);
            }
			else if (key == 'e' || key == 'v') {
				(void)va_arg(args, HKEY);
				(void)va_arg(args, const char *);
			}
			else if (key == 'E' || key == 'V') {
				(void)va_arg(args, HKEY);
				(void)va_arg(args, const wchar_t *);
			}
			else if (key == 'k') {
				(void)va_arg(args, HKEY);
				(void)va_arg(args, const PUNICODE_STRING);
			}
			else if (key == 'b') {
                (void) va_arg(args, size_t);
                (void) va_arg(args, const char *);
            }
            else if(key == 'B') {
                (void) va_arg(args, size_t *);
                (void) va_arg(args, const char *);
            }
            else if(key == 'i' || key == 'h') {
                (void) va_arg(args, int);
            }
            else if(key == 'I' || key == 'H') {
                (void) va_arg(args, int *);
            }
			else if (key == 'l' || key == 'L') {
				(void)va_arg(args, ULONG_PTR);
			}
			else if (key == 'p' || key == 'P') {
				(void)va_arg(args, void *);
			}
			else if (key == 'o') {
                (void) va_arg(args, UNICODE_STRING *);
            }
            else if(key == 'O' || key == 'K') {
                (void) va_arg(args, OBJECT_ATTRIBUTES *);
            }
            else if(key == 'a') {
                (void) va_arg(args, int);
                (void) va_arg(args, const char **);
            }
            else if(key == 'A') {
                (void) va_arg(args, int);
                (void) va_arg(args, const wchar_t **);
            }
            else if(key == 'r' || key == 'R') {
                (void) va_arg(args, unsigned long);
                (void) va_arg(args, unsigned long);
                (void) va_arg(args, unsigned char *);
			}
			else {
				pipe("CRITICAL:Unknown format string character %c", key);
			}

        }
        bson_append_finish_array( b );
        bson_finish( b );
        log_raw_direct(bson_data( b ), bson_size( b ));
        bson_destroy( b );
        // log_flush();
		va_end(args);
	}

    fmt = fmtbak;
    va_start(args, fmt);
    count = 1; key = 0; argnum = 2;

    bson_init( g_bson );
    bson_append_int( g_bson, "I", index );
	hook_info_t *hookinfo = hook_info();
	bson_append_ptr(g_bson, "C", hookinfo->return_address);
	// return location of malware callsite
	bson_append_ptr(g_bson, "R", hookinfo->main_caller_retaddr);
	// return parent location of malware callsite
	bson_append_ptr(g_bson, "P", hookinfo->parent_caller_retaddr);
	bson_append_int(g_bson, "T", GetCurrentThreadId());
    bson_append_int(g_bson, "t", GetTickCount() - g_starttick );
	// number of times this log was repeated -- we'll modify this 
	bson_append_int(g_bson, "r", 0);

	compare_offset = (unsigned int )(g_bson->cur - bson_data(g_bson));
	// the repeated value is encoded immediately before the stream we want to compare
	repeat_offset = compare_offset - 4;

	bson_append_start_array(g_bson, "args");
    bson_append_int( g_bson, "0", is_success );
    bson_append_ptr( g_bson, "1", return_value );


    while (--count != 0 || *fmt != 0) {

        // we have to find the next format specifier
        if(count == 0) {
            // end of format
            if(*fmt == 0) break;

            // set the count, possibly with a repeated format specifier
            count = *fmt >= '2' && *fmt <= '9' ? *fmt++ - '0' : 1;

            // the next format specifier
            key = *fmt++;
        }
        // pop the key and omit it
        (void) va_arg(args, const char *);
		num_to_string(g_istr, 4, argnum);
		argnum++;

        // log the value
        if(key == 's') {
            const char *s = va_arg(args, const char *);
            if(s == NULL) s = "";
            log_string(s, -1);
        }
		else if (key == 'f') {
			const char *s = va_arg(args, const char *);
			char absolutepath[MAX_PATH];
			if (s == NULL) s = "";
			ensure_absolute_ascii_path(absolutepath, s);

			log_string(absolutepath, -1);
		}
        else if(key == 'S') {
            int len = va_arg(args, int);
            const char *s = va_arg(args, const char *);
            if(s == NULL) { s = ""; len = 0; }
            log_string(s, len);
        }
        else if(key == 'u') {
            const wchar_t *s = va_arg(args, const wchar_t *);
            if(s == NULL) s = L"";
            log_wstring(s, -1);
        }
		else if (key == 'F') {
			const wchar_t *s = va_arg(args, const wchar_t *);
			wchar_t *absolutepath = malloc(32768 * sizeof(wchar_t));
			if (s == NULL) s = L"";
			if (absolutepath) {
				ensure_absolute_unicode_path(absolutepath, s);
				log_wstring(absolutepath, -1);
				free(absolutepath);
			}
			else {
				log_wstring(L"", -1);
			}
		}
		else if (key == 'U') {
            int len = va_arg(args, int);
            const wchar_t *s = va_arg(args, const wchar_t *);
            if(s == NULL) { s = L""; len = 0; }
            log_wstring(s, len);
        }
        else if(key == 'b') {
            size_t len = va_arg(args, size_t);
            const char *s = va_arg(args, const char *);
            log_buffer(s, len);
        }
        else if(key == 'B') {
            size_t *len = va_arg(args, size_t *);
            const char *s = va_arg(args, const char *);
            log_buffer(s, len == NULL ? 0 : *len);
        }
		else if (key == 'c') {
			size_t len = va_arg(args, size_t);
			const char *s = va_arg(args, const char *);
			log_large_buffer(s, len);
		}
		else if (key == 'C') {
			size_t *len = va_arg(args, size_t *);
			const char *s = va_arg(args, const char *);
			log_large_buffer(s, len == NULL ? 0 : *len);
		}
		else if (key == 'i' || key == 'h') {
			int value = va_arg(args, int);
            log_int32(value);
        }
        else if(key == 'I' || key == 'H') {
            int *ptr = va_arg(args, int *);
            log_int32(ptr != NULL ? *ptr : 0);
        }
		else if (key == 'l' || key == 'p') {
			void *value = va_arg(args, void *);
			log_ptr(value);
		}
		else if (key == 'L' || key == 'P') {
			void **ptr = va_arg(args, void **);
			log_ptr(ptr != NULL ? *ptr : NULL);
		}
		else if (key == 'e') {
			HKEY reg = va_arg(args, HKEY);
			const char *s = va_arg(args, const char *);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_full_key_pathA(reg, s, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'E') {
			HKEY reg = va_arg(args, HKEY);
			const wchar_t *s = va_arg(args, const wchar_t *);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_full_key_pathW(reg, s, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'K') {
			OBJECT_ATTRIBUTES *obj = va_arg(args, OBJECT_ATTRIBUTES *);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_key_path(obj, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'k') {
			HKEY reg = va_arg(args, HKEY);
			const PUNICODE_STRING s = va_arg(args, const PUNICODE_STRING);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_full_keyvalue_pathUS(reg, s, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'v') {
			HKEY reg = va_arg(args, HKEY);
			const char *s = va_arg(args, const char *);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_full_keyvalue_pathA(reg, s, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'V') {
			HKEY reg = va_arg(args, HKEY);
			const wchar_t *s = va_arg(args, const wchar_t *);
			unsigned int allocsize = sizeof(KEY_NAME_INFORMATION) + MAX_KEY_BUFLEN;
			PKEY_NAME_INFORMATION keybuf = malloc(allocsize);

			log_wstring(get_full_keyvalue_pathW(reg, s, keybuf, allocsize), -1);
			free(keybuf);
		}
		else if (key == 'o') {
            UNICODE_STRING *str = va_arg(args, UNICODE_STRING *);
            if(str == NULL) {
                log_string("", 0);
            }
            else {
                log_wstring(str->Buffer, str->Length / sizeof(wchar_t));
            }
        }
        else if(key == 'O') {
            OBJECT_ATTRIBUTES *obj = va_arg(args, OBJECT_ATTRIBUTES *);
            if(obj == NULL) {
                log_string("", 0);
            }
			else {
				wchar_t path[MAX_PATH_PLUS_TOLERANCE];
				wchar_t *absolutepath = malloc(32768 * sizeof(wchar_t));
				if (absolutepath) {
					path_from_object_attributes(obj, path, MAX_PATH_PLUS_TOLERANCE);

					ensure_absolute_unicode_path(absolutepath, path);
					log_wstring(absolutepath, -1);
					free(absolutepath);
				}
				else {
					log_wstring(L"", -1);
				}
            }
        }
        else if(key == 'a') {
            int argc = va_arg(args, int);
            const char **argv = va_arg(args, const char **);
            log_argv(argc, argv);
        }
        else if(key == 'A') {
            int argc = va_arg(args, int);
            const wchar_t **argv = va_arg(args, const wchar_t **);
            log_wargv(argc, argv);
        }
        else if(key == 'r' || key == 'R') {
            unsigned long type = va_arg(args, unsigned long);
            unsigned long size = va_arg(args, unsigned long);
            unsigned char *data = va_arg(args, unsigned char *);

			if (size > BUFFER_REGVAL_MAX)
				size = BUFFER_REGVAL_MAX;
			
			// bson_append_start_object( g_bson, g_istr );
            // bson_append_int( g_bson, "type", type );

            // strncpy(g_istr, "val", 4);
            if(type == REG_NONE) {
                log_string("", 0);
            }
            else if(type == REG_DWORD || type == REG_DWORD_LITTLE_ENDIAN) {
                unsigned int value = *(unsigned int *) data;
                log_int32(value);
            }
            else if(type == REG_DWORD_BIG_ENDIAN) {
                unsigned int value = *(unsigned int *) data;
                log_int32(htonl(value));
            }
            else if(type == REG_EXPAND_SZ || type == REG_SZ) {

                if(data == NULL) {
                    bson_append_binary(g_bson, g_istr, BSON_BIN_BINARY,
                        (const char *) data, 0);
                }
                // ascii strings
                else if(key == 'r') {
					if (size >= 1 && data[size - 1] == '\0')
						log_string(data, size - 1);
					else
						log_string(data, size);
                    //bson_append_binary(g_bson, g_istr, BSON_BIN_BINARY,
                    //    (const char *) data, size);
                }
                // unicode strings
                else {
					const wchar_t *wdata = (const wchar_t *)data;
					if (size >= 2 && wdata[(size / sizeof(wchar_t)) - 1] == L'\0')
						log_wstring(wdata, (size / sizeof(wchar_t)) - 1);
					else
						log_wstring(wdata, size / sizeof(wchar_t));
                    //bson_append_binary(g_bson, g_istr, BSON_BIN_BINARY,
                    //    (const char *) data, size);
                }
            } else {
                bson_append_binary(g_bson, g_istr, BSON_BIN_BINARY,
                    (const char *) data, 0);
            }

            // bson_append_finish_object( g_bson );
        }
    }

    va_end(args);

    bson_append_finish_array( g_bson );
    bson_finish( g_bson );

	if (lastlog.buf) {
		unsigned int our_len = bson_size(g_bson) - compare_offset;
		if (lastlog.compare_len == our_len && !memcmp(lastlog.compare_ptr, bson_data(g_bson) + compare_offset, our_len)) {
			// we're about to log a duplicate of the last log message, just increment the previous log's repeated count
			(*lastlog.repeated_ptr)++;
		}
		else {
			log_raw_direct(lastlog.buf, lastlog.len);
			free(lastlog.buf);
			lastlog.buf = NULL;
		}
	}
	if (lastlog.buf == NULL) {
		lastlog.len = bson_size(g_bson);
		lastlog.buf = malloc(lastlog.len);
		memcpy(lastlog.buf, bson_data(g_bson), lastlog.len);
		lastlog.compare_len = lastlog.len - compare_offset;
		lastlog.compare_ptr = lastlog.buf + compare_offset;
		lastlog.repeated_ptr = (int *)(lastlog.buf + repeat_offset);
	}

    bson_destroy( g_bson );
    LeaveCriticalSection(&g_mutex);

	//log_flush();

	set_lasterrors(&lasterror);
}

void announce_netlog()
{
    char protoname[32];
    strcpy(protoname, "BSON\n");
    //sprintf(protoname+5, "logs/%lu.bson\n", GetCurrentProcessId());
    log_raw_direct(protoname, strlen(protoname));
}

void log_new_process()
{
    g_starttick = GetTickCount();

    FILETIME st;
    GetSystemTimeAsFileTime(&st);

    loq(LOG_ID_PROCESS, "__notification__", "__process__", 1, 0, "llllu",
        "TimeLow", st.dwLowDateTime,
        "TimeHigh", st.dwHighDateTime,
        "ProcessIdentifier", GetCurrentProcessId(),
        "ParentProcessIdentifier", parent_process_id(),
        "ModulePath", our_process_path);
}

void log_new_thread()
{
    loq(LOG_ID_THREAD, "__notification__", "__thread__", 1, 0, "l",
        "ProcessIdentifier", GetCurrentProcessId());
}

void log_anomaly(const char *subcategory, int success,
    const char *funcname, const char *msg)
{
    loq(LOG_ID_ANOMALY, "__notification__", "__anomaly__", success, 0, "lsss",
        "ThreadIdentifier", GetCurrentThreadId(),
        "Subcategory", subcategory,
        "FunctionName", funcname,
        "Message", msg);
}

void log_hook_modification(const char *funcname, const char *origbytes, const char *newbytes, unsigned int len)
{
	char msg1[128] = { 0 };
	char msg2[128] = { 0 };
	char *p;
	unsigned int i;

	for (i = 0; i < len && i < 124/3; i++) {
		p = &msg1[i * 3];
		sprintf(p, "%02X ", (unsigned char)origbytes[i]);
	}
	for (i = 0; i < len && i < 124 / 3; i++) {
		p = &msg2[i * 3];
		sprintf(p, "%02X ", (unsigned char)newbytes[i]);
	}

	loq(LOG_ID_ANOMALY_EXTRA, "__notification__", "__anomaly__", 1, 0, "lsssss",
		"ThreadIdentifier", GetCurrentThreadId(),
		"Subcategory", "unhook",
		"FunctionName", funcname,
		"UnhookType", "modification",
		"OriginalBytes", msg1,
		"NewBytes", msg2);
}

void log_hook_removal(const char *funcname)
{
	loq(LOG_ID_ANOMALY, "__notification__", "__anomaly__", 1, 0, "lsss",
		"ThreadIdentifier", GetCurrentThreadId(),
		"Subcategory", "unhook",
		"FunctionName", funcname,
		"UnhookType", "removal");
}

void log_hook_restoration(const char *funcname)
{
	loq(LOG_ID_ANOMALY, "__notification__", "__anomaly__", 1, 0, "lsss",
		"ThreadIdentifier", GetCurrentThreadId(),
		"Subcategory", "unhook",
		"FunctionName", funcname,
		"UnhookType", "restored");
}


void log_init(unsigned int ip, unsigned short port, int debug)
{
	g_buffer = calloc(1, BUFFERSIZE);

	InitializeCriticalSection(&g_mutex);
	InitializeCriticalSection(&g_writing_log_buffer_mutex);

	g_log_flush = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(debug != 0) {
        g_sock = DEBUG_SOCKET;
    }
    else {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        struct sockaddr_in addr = {
            .sin_family         = AF_INET,
            .sin_addr.s_addr    = ip,
            .sin_port           = htons(port),
        };

		if (connect(g_sock, (struct sockaddr *) &addr, sizeof(addr))) {
			closesocket(g_sock);
			g_sock = DEBUG_SOCKET;
		}
    }

	g_log_thread_handle =
		CreateThread(NULL, 0, &_log_thread, NULL, 0, NULL);

	g_logwatcher_thread_handle =
		CreateThread(NULL, 0, &_logwatcher_thread, NULL, 0, NULL);

	if (g_log_thread_handle == NULL || g_logwatcher_thread_handle == NULL) {
		pipe("CRITICAL:Error initializing logging threads!");
		return;
	}

	announce_netlog();
    log_new_process();
    log_new_thread();
    // flushing here so host can create files / keep timestamps
    log_flush();
}

void log_free()
{
	// racy: fix me later
	if (lastlog.buf != NULL) {
		log_raw_direct(lastlog.buf, lastlog.len);
		free(lastlog.buf);
		lastlog.buf = NULL;
	}
    log_flush();
	if (g_sock != INVALID_SOCKET && g_sock != DEBUG_SOCKET) {
        closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		WSACleanup();
    }
}
