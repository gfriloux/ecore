#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include <process.h>

#include "Ecore.h"
#include "ecore_private.h"

/*
 * TESTS
 *
 * [X] add event
 * [X] data event
 * [X] error event
 * [X] data event buffered
 * [X] del event
 * [ ] batch files
 * [X] exit code
 * [X] inherited env var
 */

/*
 * FIXME :
 *
 * [ ] child program with ecore main loop does not exit and nothing is sent
 * [ ] ecore_exe_send fails (race condition ? same problem as above ?)
 */

#define ECORE_EXE_WIN32_TIMEOUT 3000

typedef enum
{
   ECORE_EXE_WIN32_SIGINT,
   ECORE_EXE_WIN32_SIGQUIT,
   ECORE_EXE_WIN32_SIGTERM,
   ECORE_EXE_WIN32_SIGKILL
} Ecore_Exe_Win32_Signal;

static Ecore_Exe *exes = NULL;
static int run_pri = NORMAL_PRIORITY_CLASS;

struct _Ecore_Exe
{
   EINA_INLIST;
   ECORE_MAGIC;

   char *cmd;
   char *tag;

   HANDLE process;
   HANDLE process_thread;
   DWORD process_id;
   DWORD thread_id;
   void *data;
   Ecore_Win32_Handler *h_close;
   Ecore_Exe_Flags flags;
   Ecore_Exe_Win32_Signal sig;
   Ecore_Exe_Cb pre_free_cb;

   struct
   {
      HANDLE child_pipe;
      HANDLE child_pipe_x;
      HANDLE thread;
      void *data_buf;
      DWORD data_size;
   } pipe_read;
   struct
   {
      HANDLE child_pipe;
      HANDLE child_pipe_x;
      void *data_buf;
      int data_size;
   } pipe_write;
   struct
   {
      HANDLE child_pipe;
      HANDLE child_pipe_x;
      HANDLE thread;
      void *data_buf;
      DWORD data_size;
   } pipe_error;

   Eina_Bool close_threads : 1;
   Eina_Bool close_stdin : 1;
   Eina_Bool is_suspended : 1;
};

static void
_ecore_exe_event_add_free(void *data EINA_UNUSED,
                          void *ev)
{
   free(ev);
}

static void
_ecore_exe_event_del_free(void *data EINA_UNUSED,
                          void *ev)
{
   Ecore_Exe_Event_Del *e;

   e = (Ecore_Exe_Event_Del *)ev;
   if (e->exe)
     ecore_exe_free(e->exe);
   free(e);
}

static void
_ecore_exe_event_exe_data_free(void *data EINA_UNUSED,
                               void *ev)
{
   Ecore_Exe_Event_Data *e;

   e = (Ecore_Exe_Event_Data *)ev;
   ecore_exe_event_data_free(e);
}

static Eina_Bool
_ecore_exe_close_cb(void *data,
                    Ecore_Win32_Handler *wh EINA_UNUSED)
{
   Ecore_Exe_Event_Del *e;
   Ecore_Exe *exe;
   DWORD exit_code = 0;

   e = calloc(1, sizeof(Ecore_Exe_Event_Del));
   if (!e) return 0;

   exe = (Ecore_Exe *)data;

   /* FIXME : manage the STILL_ACTIVE returned error */
   if (!GetExitCodeProcess(exe->process, &exit_code))
     {
        char *msg;

        msg = evil_last_error_get();
        printf("%s\n", msg);
        free(msg);
     }

   e->exit_code = exit_code;
   e->exited = 1;
   e->pid = exe->process_id;
   e->exe = exe;

   ecore_event_add(ECORE_EXE_EVENT_DEL, e,
                   _ecore_exe_event_del_free, NULL);

   DBG("Exiting process %s with exit code %d\n", exe->cmd, e->exit_code);

   return 0;
}

static unsigned int __stdcall
_ecore_exe_pipe_read_thread_cb(void *data)
{
   char buf[64];
   Ecore_Exe *exe;
   Ecore_Exe_Event_Data *event_data;
   DWORD size;
   BOOL res;

   exe = (Ecore_Exe *)data;

   while (!exe->close_threads)
     {
        res = ReadFile(exe->pipe_read.child_pipe, buf, sizeof(buf)-1, &size, NULL);
        if (!res || (size == 0))
          continue;

        if (!exe->pipe_read.data_buf)
          {
             exe->pipe_read.data_buf = buf;
             exe->pipe_read.data_size = size;
          }
        else
          {
             exe->pipe_read.data_buf = realloc(exe->pipe_read.data_buf, exe->pipe_read.data_size + size);
             memcpy(exe->pipe_read.data_buf + exe->pipe_read.data_size, buf, size);
             exe->pipe_read.data_size += size;
          }

        event_data = ecore_exe_event_data_get(exe, ECORE_EXE_PIPE_READ);
        if (event_data)
          {
             ecore_event_add(ECORE_EXE_EVENT_DATA, event_data,
                             _ecore_exe_event_exe_data_free,
                             NULL);
          }
     }

   _endthreadex(0);

   return 0;
}

static unsigned int __stdcall
_ecore_exe_pipe_error_thread_cb(void *data)
{
   char buf[64];
   Ecore_Exe *exe;
   Ecore_Exe_Event_Data *event_data;
   char *current_buf = NULL;
   DWORD size;
   DWORD current_size = 0;
   BOOL res;

   exe = (Ecore_Exe *)data;

   while (!exe->close_threads)
     {
        if (!PeekNamedPipe(exe->pipe_error.child_pipe,
                           buf, sizeof(buf) - 1, &size, &current_size, NULL))
          continue;
        if (size == 0)
          continue;
        current_buf = (char *)malloc(current_size);
        if (!current_buf)
          continue;
        res = ReadFile(exe->pipe_error.child_pipe, current_buf, current_size, &size, NULL);
        if (!res || (size == 0))
          {
             free(current_buf);
             current_buf = NULL;
             continue;
          }
        if (current_size != size)
          {
             free(current_buf);
             current_buf = NULL;
             continue;
          }
        current_size = size;

        exe->pipe_error.data_buf = current_buf;
        exe->pipe_error.data_size = current_size;

        event_data = ecore_exe_event_data_get(exe, ECORE_EXE_PIPE_ERROR);
        if (event_data)
          {
             ecore_event_add(ECORE_EXE_EVENT_ERROR, event_data,
                             _ecore_exe_event_exe_data_free,
                             NULL);
          }

        current_buf = NULL;
        current_size = 0;
     }

   _endthreadex(0);

   return 0;
}

static void
_ecore_exe_threads_terminate(Ecore_Exe *exe)
{
   HANDLE threads[2] = { NULL, NULL };
   int i = 0;

   if (exe->pipe_read.thread)
     {
        threads[i] = exe->pipe_read.thread;
        i++;
     }
   if (exe->pipe_error.thread)
     {
        threads[i] = exe->pipe_error.thread;
        i++;
     }
   if (i > 0)
     {
        exe->close_threads = 1;
        WaitForMultipleObjects(i, threads, TRUE, INFINITE);
        if (exe->pipe_error.thread)
          {
             CloseHandle(exe->pipe_error.thread);
             exe->pipe_error.thread = NULL;
          }
        if (exe->pipe_read.thread)
          {
             CloseHandle(exe->pipe_read.thread);
             exe->pipe_read.thread = NULL;
          }
     }
}

static DWORD WINAPI
_ecore_exe_thread_procedure(LPVOID data EINA_UNUSED)
{
   GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
   GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
   return 1;
}

static BOOL CALLBACK
_ecore_exe_enum_windows_procedure(HWND window,
                                  LPARAM data)
{
   Ecore_Exe *exe;
   DWORD thread_id;

   exe = (Ecore_Exe *)data;
   thread_id = GetWindowThreadProcessId(window, NULL);

   if (thread_id == exe->thread_id)
     {
        /* Ctrl-C or Ctrl-Break */
        if (CreateRemoteThread(exe->process, NULL, 0,
                               (LPTHREAD_START_ROUTINE)_ecore_exe_thread_procedure, NULL,
                                0, NULL))
          {
             printf ("remote thread\n");
             return EINA_FALSE;
          }

        if ((exe->sig == ECORE_EXE_WIN32_SIGINT) ||
            (exe->sig == ECORE_EXE_WIN32_SIGQUIT))
          {
             printf ("int or quit\n");
             return EINA_FALSE;
          }

        /* WM_CLOSE message */
        PostMessage(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(exe->process, ECORE_EXE_WIN32_TIMEOUT) == WAIT_OBJECT_0)
          {
             printf ("CLOSE\n");
             return EINA_FALSE;
          }

        /* WM_QUIT message */
        PostMessage(window, WM_QUIT, 0, 0);
        if (WaitForSingleObject(exe->process, ECORE_EXE_WIN32_TIMEOUT) == WAIT_OBJECT_0)
          {
             printf ("QUIT\n");
             return EINA_FALSE;
          }

        /* Exit process */
        if (CreateRemoteThread(exe->process, NULL, 0,
                               (LPTHREAD_START_ROUTINE)ExitProcess, NULL,
                               0, NULL))
          {
             printf ("remote thread 2\n");
             return EINA_FALSE;
          }

        if (exe->sig == ECORE_EXE_WIN32_SIGTERM)
          {
             printf ("term\n");
             return EINA_FALSE;
          }

        TerminateProcess(exe->process, 0);

        return EINA_FALSE;
     }

   return EINA_TRUE;
}


void
_ecore_exe_init(void)
{
   ECORE_EXE_EVENT_ADD = ecore_event_type_new();
   ECORE_EXE_EVENT_DEL = ecore_event_type_new();
   ECORE_EXE_EVENT_DATA = ecore_event_type_new();
   ECORE_EXE_EVENT_ERROR = ecore_event_type_new();
}

void
_ecore_exe_shutdown(void)
{
   while (exes)
     ecore_exe_free(exes);
}

EAPI int ECORE_EXE_EVENT_ADD = 0;
EAPI int ECORE_EXE_EVENT_DEL = 0;
EAPI int ECORE_EXE_EVENT_DATA = 0;
EAPI int ECORE_EXE_EVENT_ERROR = 0;

EAPI void
ecore_exe_run_priority_set(int pri)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   switch (pri)
     {
      case ECORE_EXE_WIN32_PRIORITY_IDLE:
        run_pri = IDLE_PRIORITY_CLASS;
        break;

      case ECORE_EXE_WIN32_PRIORITY_BELOW_NORMAL:
        run_pri = BELOW_NORMAL_PRIORITY_CLASS;
        break;

      case ECORE_EXE_WIN32_PRIORITY_NORMAL:
        run_pri = NORMAL_PRIORITY_CLASS;
        break;

      case ECORE_EXE_WIN32_PRIORITY_ABOVE_NORMAL:
        run_pri = ABOVE_NORMAL_PRIORITY_CLASS;
        break;

      case ECORE_EXE_WIN32_PRIORITY_HIGH:
        run_pri = HIGH_PRIORITY_CLASS;
        break;

      case ECORE_EXE_WIN32_PRIORITY_REALTIME:
        run_pri = REALTIME_PRIORITY_CLASS;
        break;

      default:
        break;
     }
}

EAPI int
ecore_exe_run_priority_get(void)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(0);
   switch (run_pri)
     {
      case IDLE_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_IDLE;

      case BELOW_NORMAL_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_BELOW_NORMAL;

      case NORMAL_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_NORMAL;

      case ABOVE_NORMAL_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_ABOVE_NORMAL;

      case HIGH_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_HIGH;

      case REALTIME_PRIORITY_CLASS:
        return ECORE_EXE_WIN32_PRIORITY_REALTIME;

      /* default should not be reached */
      default:
        return ECORE_EXE_WIN32_PRIORITY_NORMAL;
     }
}

EAPI Ecore_Exe *
ecore_exe_run(const char *exe_cmd,
              const void *data)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   return ecore_exe_pipe_run(exe_cmd, 0, data);
}

EAPI Ecore_Exe *
ecore_exe_pipe_run(const char     *exe_cmd,
                   Ecore_Exe_Flags flags,
                   const void     *data)
{
   char *exe_cmd_buf;
   SECURITY_ATTRIBUTES sa;
   STARTUPINFO si;
   PROCESS_INFORMATION pi;
   Ecore_Exe *exe;
   Ecore_Exe_Event_Add *e;
   Eina_Bool use_sh = EINA_FALSE;
   const char *shell = NULL;

   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);

   DBG("Creating process %s", exe_cmd);

   if (!exe_cmd || !*exe_cmd)
     return NULL;

   exe = calloc(1, sizeof(Ecore_Exe));
   if (!exe)
     return NULL;

   if ((flags & ECORE_EXE_PIPE_AUTO) && (!(flags & ECORE_EXE_PIPE_ERROR))
       && (!(flags & ECORE_EXE_PIPE_READ)))
     /* We need something to auto pipe. */
     flags |= ECORE_EXE_PIPE_READ | ECORE_EXE_PIPE_ERROR;

   if ((flags & ECORE_EXE_USE_SH))
     use_sh = EINA_TRUE;
   else
     {
       char *ret;

       ret = strrstr(exe_cmd, ".bat");
       if (ret && (ret[4] == '\0'))
         use_sh = EINA_TRUE;
     }

   if (use_sh)
     {
        int len;

        shell = "cmd.exe";
        exe_cmd_buf = calloc(1, strlen(exe_cmd) + 4);
        if (!exe_cmd_buf) goto free_exe;

        strcpy(exe_cmd_buf, "/c ");
        strcpy(exe_cmd_buf + 3, exe_cmd);
     }
   else
     {
        exe_cmd_buf = strdup(exe_cmd);
        if (!exe_cmd_buf) goto free_exe;
     }

   exe->flags = flags;
   exe->cmd = exe_cmd_buf;
   if (!exe->cmd)
     goto free_exe;

   /* stdout, stderr and stdin pipes */

   sa.nLength = sizeof(SECURITY_ATTRIBUTES);
   sa.bInheritHandle = EINA_TRUE;
   sa.lpSecurityDescriptor = NULL;

   /* stdout pipe */
   if (exe->flags & ECORE_EXE_PIPE_READ)
     {
        if (!CreatePipe(&exe->pipe_read.child_pipe, &exe->pipe_read.child_pipe_x, &sa, 0))
          goto free_exe_cmd;
        if (!SetHandleInformation(exe->pipe_read.child_pipe, HANDLE_FLAG_INHERIT, 0))
          goto close_pipe_read;
        exe->pipe_read.thread = (HANDLE)_beginthreadex(NULL, 0,
                                                       _ecore_exe_pipe_read_thread_cb,
                                                       exe, 0, NULL);
        if (!exe->pipe_read.thread)
          goto close_pipe_read;
     }

   /* stderr pipe */

   if (exe->flags & ECORE_EXE_PIPE_ERROR)
     {
        if (!CreatePipe(&exe->pipe_error.child_pipe, &exe->pipe_error.child_pipe_x, &sa, 0))
          goto close_pipe_read;
        if (!SetHandleInformation(exe->pipe_error.child_pipe, HANDLE_FLAG_INHERIT, 0))
          goto close_pipe_error;
        exe->pipe_error.thread = (HANDLE)_beginthreadex(NULL, 0,
                                                        _ecore_exe_pipe_error_thread_cb,
                                                        exe, 0, NULL);
        if (!exe->pipe_error.thread)
          goto close_pipe_error;
     }

   /* stdin pipe */
   if (exe->flags & ECORE_EXE_PIPE_WRITE)
     {
        if (!CreatePipe(&exe->pipe_write.child_pipe, &exe->pipe_write.child_pipe_x, &sa, 0))
          goto close_pipe_read;
        if (!SetHandleInformation(exe->pipe_write.child_pipe_x, HANDLE_FLAG_INHERIT, 0))
          goto close_pipe_write;
     }

   /* create child process */

   ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

   ZeroMemory(&si, sizeof(STARTUPINFO));
   si.cb = sizeof(STARTUPINFO);
   si.hStdOutput = exe->pipe_read.child_pipe_x;
   si.hStdInput = exe->pipe_write.child_pipe;
   si.hStdError = exe->pipe_error.child_pipe_x;
   si.dwFlags |= STARTF_USESTDHANDLES;

   DBG("CreateProcess: shell:%s child:%s", use_sh ? "yes" : "no", exe->cmd);
   if (!CreateProcess(shell, exe->cmd, NULL, NULL, EINA_TRUE,
                      run_pri | CREATE_SUSPENDED, NULL, NULL, &si, &pi))
     {
        ERR("Failed to create process %s : %d", exe->cmd, GetLastError());
        goto close_pipe_write;
     }

   /* be sure that the child process is running */
   /* FIXME: This does not work if the child is an EFL-based app */
   /* if (WaitForInputIdle(pi.hProcess, INFINITE) == WAIT_FAILED) */
   /*   goto close_pipe_write; */

   ECORE_MAGIC_SET(exe, ECORE_MAGIC_EXE);
   exe->process = pi.hProcess;
   exe->process_thread = pi.hThread;
   exe->process_id = pi.dwProcessId;
   exe->thread_id = pi.dwThreadId;
   exe->data = (void *)data;

   exe->h_close = ecore_main_win32_handler_add(exe->process,
                                               _ecore_exe_close_cb, exe);
   if (!exe->h_close)
     goto close_process;

   if (ResumeThread(exe->process_thread) == ((DWORD)-1))
     {
        ERR("Could not resume process");
        goto delete_h_close;
     }

   exes = (Ecore_Exe *)eina_inlist_append(EINA_INLIST_GET(exes), EINA_INLIST_GET(exe));

   e = (Ecore_Exe_Event_Add *)calloc(1, sizeof(Ecore_Exe_Event_Add));
   if (!e) goto delete_h_close;

   e->exe = exe;
   ecore_event_add(ECORE_EXE_EVENT_ADD, e,
                   _ecore_exe_event_add_free, NULL);

   return exe;

delete_h_close:
   ecore_main_win32_handler_del(exe->h_close);
close_process:
   CloseHandle(exe->process_thread);
   CloseHandle(exe->process);
close_pipe_write:
   if (exe->pipe_write.child_pipe)
     CloseHandle(exe->pipe_write.child_pipe);
   if (exe->pipe_write.child_pipe_x)
     CloseHandle(exe->pipe_write.child_pipe_x);
 close_pipe_error:
   _ecore_exe_threads_terminate(exe);
   if (exe->pipe_error.child_pipe)
     CloseHandle(exe->pipe_error.child_pipe);
   if (exe->pipe_error.child_pipe_x)
     CloseHandle(exe->pipe_error.child_pipe_x);
 close_pipe_read:
   _ecore_exe_threads_terminate(exe);
   if (exe->pipe_read.child_pipe)
     CloseHandle(exe->pipe_read.child_pipe);
   if (exe->pipe_read.child_pipe_x)
     CloseHandle(exe->pipe_read.child_pipe_x);
 free_exe_cmd:
   free(exe->cmd);
free_exe:
   free(exe);
   return NULL;
}

EAPI void
ecore_exe_callback_pre_free_set(Ecore_Exe   *exe,
                                Ecore_Exe_Cb func)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE,
                         "ecore_exe_callback_pre_free_set");
        return;
     }
   exe->pre_free_cb = func;
}

EAPI Eina_Bool
ecore_exe_send(Ecore_Exe  *exe,
               const void *data,
               int         size)
{
   void *buf = NULL;
   DWORD num_exe;
   BOOL res;

   EINA_MAIN_LOOP_CHECK_RETURN_VAL(EINA_FALSE);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_send");
        return EINA_FALSE;
     }

   if (exe->close_stdin)
     {
        ERR("Ecore_Exe %p stdin is closed! Cannot send %d bytes from %p",
            exe, size, data);
        return EINA_FALSE;
     }

   buf = realloc(exe->pipe_write.data_buf, exe->pipe_write.data_size + size);
   if (!buf) return EINA_FALSE;

   exe->pipe_write.data_buf = buf;
   memcpy((char *)exe->pipe_write.data_buf + exe->pipe_write.data_size, data, size);
   exe->pipe_write.data_size += size;

   res = WriteFile(exe->pipe_write.child_pipe_x, buf, READBUFSIZ, &num_exe, NULL);
   printf(" ** res : %d\n", res);
   if (!res || num_exe == 0)
     {
        ERR("Ecore_Exe %p stdin is closed! Cannot send %d bytes from %p",
            exe, size, data);
        return EINA_FALSE;
     }

   return EINA_TRUE;
}

EAPI void
ecore_exe_close_stdin(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_close_stdin");
        return;
     }
   exe->close_stdin = 1;
}

/* Not used on Windows */
EAPI void
ecore_exe_auto_limits_set(Ecore_Exe *exe __UNUSED__,
                          int        start_bytes __UNUSED__,
                          int        end_bytes __UNUSED__,
                          int        start_lines __UNUSED__,
                          int        end_lines __UNUSED__)
{
}

EAPI Ecore_Exe_Event_Data *
ecore_exe_event_data_get(Ecore_Exe      *exe,
                         Ecore_Exe_Flags flags)
{
   Ecore_Exe_Event_Data *e = NULL;
   unsigned char *inbuf;
   DWORD inbuf_num;
   Eina_Bool is_buffered = EINA_FALSE;

   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_event_data_get");
        return NULL;
     }

   /* Sort out what sort of event we are. */
   if (flags & ECORE_EXE_PIPE_READ)
     {
        flags = ECORE_EXE_PIPE_READ;
        if (exe->flags & ECORE_EXE_PIPE_READ_LINE_BUFFERED)
          is_buffered = EINA_TRUE;
     }
   else
     {
        flags = ECORE_EXE_PIPE_ERROR;
        if (exe->flags & ECORE_EXE_PIPE_ERROR_LINE_BUFFERED)
          is_buffered = EINA_TRUE;
     }

/* Get the data. */
   if (flags & ECORE_EXE_PIPE_READ)
     {
        inbuf = exe->pipe_read.data_buf;
        inbuf_num = exe->pipe_read.data_size;
        exe->pipe_read.data_buf = NULL;
        exe->pipe_read.data_size = 0;
     }
   else
     {
        inbuf = exe->pipe_error.data_buf;
        inbuf_num = exe->pipe_error.data_size;
        exe->pipe_error.data_buf = NULL;
        exe->pipe_error.data_size = 0;
     }

   e = calloc(1, sizeof(Ecore_Exe_Event_Data));
   if (e)
     {
        e->exe = exe;
        e->data = inbuf;
        e->size = inbuf_num;

        if (is_buffered) /* Deal with line buffering. */
          {
             char *c;
             DWORD i;
             DWORD max = 0;
             DWORD count = 0;
             DWORD last = 0;

             c = (char *)inbuf;
             for (i = 0; i < inbuf_num; i++)
               {
                  if (inbuf[i] == '\n')
                    {
                       int end;

                       if (count >= max)
                         {
                            max += 10;
                            e->lines = realloc(e->lines, sizeof(Ecore_Exe_Event_Data_Line) * (max + 1));
                         }

                       if ((i >= 1) && (inbuf[i - 1] == '\r'))
                         end = i - 1;
                       else
                         end = i;
                       inbuf[end] = '\0';
                       e->lines[count].line = c;
                       e->lines[count].size = end - last;
                       last = i + 1;
                       c = (char *)&inbuf[last];
                       count++;
                    }
               }
             if (i > last) /* Partial line left over, save it for next time. */
               {
                  if (count != 0) e->size = last;
                  if (flags & ECORE_EXE_PIPE_READ)
                    {
                       exe->pipe_read.data_size = i - last;
                       exe->pipe_read.data_buf = malloc(exe->pipe_read.data_size);
                       memcpy(exe->pipe_read.data_buf, c, exe->pipe_read.data_size);
                    }
                  else
                    {
                       exe->pipe_error.data_size = i - last;
                       exe->pipe_error.data_buf = malloc(exe->pipe_error.data_size);
                       memcpy(exe->pipe_error.data_buf, c, exe->pipe_error.data_size);
                    }
               }
             if (count == 0) /* No lines to send, cancel the event. */
               {
                  _ecore_exe_event_exe_data_free(NULL, e);
                  e = NULL;
               }
             else /* NULL terminate the array, so that people know where the end is. */
               {
                  e->lines[count].line = NULL;
                 e->lines[count].size = 0;
               }
          }
     }

   return e;
}

EAPI void
ecore_exe_event_data_free(Ecore_Exe_Event_Data *e)
{
   if (!e) return;
   IF_FREE(e->lines);
   IF_FREE(e->data);
   free(e);
}

EAPI void *
ecore_exe_free(Ecore_Exe *exe)
{
   void *data;

   if (!exe) return NULL;
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_free");
        return NULL;
     }

   data = exe->data;

   if (exe->pre_free_cb)
     exe->pre_free_cb(data, exe);

   /* if (exe->h_close) */
   /*   ecore_main_win32_handler_del(exe->h_close); */
   CloseHandle(exe->process_thread);
   CloseHandle(exe->process);
   if (exe->pipe_write.child_pipe)
     CloseHandle(exe->pipe_write.child_pipe);
   if (exe->pipe_write.child_pipe_x)
     CloseHandle(exe->pipe_write.child_pipe_x);
   _ecore_exe_threads_terminate(exe);
   if (exe->pipe_error.child_pipe)
     CloseHandle(exe->pipe_error.child_pipe);
   if (exe->pipe_error.child_pipe_x)
     CloseHandle(exe->pipe_error.child_pipe_x);
   if (exe->pipe_read.child_pipe)
     CloseHandle(exe->pipe_read.child_pipe);
   if (exe->pipe_read.child_pipe_x)
     CloseHandle(exe->pipe_read.child_pipe_x);
   free(exe->cmd);

   exes = (Ecore_Exe *)eina_inlist_remove(EINA_INLIST_GET(exes), EINA_INLIST_GET(exe));
   IF_FREE(exe->tag);
   ECORE_MAGIC_SET(exe, ECORE_MAGIC_NONE);
   free(exe);

   return data;
}

EAPI pid_t
ecore_exe_pid_get(const Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(0);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_pid_get");
        return -1;
     }
   return exe->process_id;
}

EAPI void
ecore_exe_tag_set(Ecore_Exe  *exe,
                  const char *tag)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_tag_set");
        return;
     }
   IF_FREE(exe->tag);
   if (tag)
     exe->tag = strdup(tag);
   else
     exe->tag = NULL;
}

EAPI const char *
ecore_exe_tag_get(const Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_tag_get");
        return NULL;
     }
   return exe->tag;
}

EAPI const char *
ecore_exe_cmd_get(const Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_cmd_get");
        return NULL;
     }
   return exe->cmd;
}

EAPI void *
ecore_exe_data_get(const Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_data_get");
        return NULL;
     }
   return exe->data;
}

EAPI void *
ecore_exe_data_set(Ecore_Exe *exe,
                   void      *data)
{
   void *ret;

   EINA_MAIN_LOOP_CHECK_RETURN_VAL(NULL);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, __func__);
        return NULL;
     }
   ret = exe->data;
   exe->data = data;
   return ret;
}

EAPI Ecore_Exe_Flags
ecore_exe_flags_get(const Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN_VAL(0);
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_data_get");
        return 0;
     }
   return exe->flags;
}

EAPI void
ecore_exe_pause(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_pause");
        return;
     }

   if (exe->is_suspended)
     return;

   if (SuspendThread(exe->process_thread) != (DWORD)-1)
     exe->is_suspended = 1;
}

EAPI void
ecore_exe_continue(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_continue");
        return;
     }

   if (!exe->is_suspended)
     return;

   if (ResumeThread(exe->process_thread) != (DWORD)-1)
     exe->is_suspended = 0;
}

EAPI void
ecore_exe_interrupt(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_interrupt");
        return;
     }

   CloseHandle(exe->process_thread);
   exe->process_thread = NULL;
   CloseHandle(exe->process);
   exe->process = NULL;
   exe->sig = ECORE_EXE_WIN32_SIGINT;
   while (EnumWindows(_ecore_exe_enum_windows_procedure, (LPARAM)exe)) ;
}

EAPI void
ecore_exe_quit(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_quit");
        return;
     }

   CloseHandle(exe->process_thread);
   exe->process_thread = NULL;
   CloseHandle(exe->process);
   exe->process = NULL;
   exe->sig = ECORE_EXE_WIN32_SIGQUIT;
   while (EnumWindows(_ecore_exe_enum_windows_procedure, (LPARAM)exe)) ;
}

EAPI void
ecore_exe_terminate(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_terminate");
        return;
     }

/*    CloseHandle(exe->thread); */
   CloseHandle(exe->process);
   exe->process = NULL;
   exe->sig = ECORE_EXE_WIN32_SIGTERM;
   EnumWindows(_ecore_exe_enum_windows_procedure, (LPARAM)exe);
}

EAPI void
ecore_exe_kill(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_kill");
        return;
     }

   CloseHandle(exe->process_thread);
   exe->process_thread = NULL;
   CloseHandle(exe->process);
   exe->process = NULL;
   exe->sig = ECORE_EXE_WIN32_SIGKILL;
   EnumWindows(_ecore_exe_enum_windows_procedure, (LPARAM)exe);
}

EAPI void
ecore_exe_signal(Ecore_Exe *exe,
                 int        num __UNUSED__)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_signal");
        return;
     }

   /* does nothing */
}

EAPI void
ecore_exe_hup(Ecore_Exe *exe)
{
   EINA_MAIN_LOOP_CHECK_RETURN;
   if (!ECORE_MAGIC_CHECK(exe, ECORE_MAGIC_EXE))
     {
        ECORE_MAGIC_FAIL(exe, ECORE_MAGIC_EXE, "ecore_exe_hup");
        return;
     }

   /* does nothing */
}
