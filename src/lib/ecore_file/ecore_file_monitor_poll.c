/*
 * vim:ts=8:sw=3:sts=8:noexpandtab:cino=>5n-3f0^-2{2
 */
#include "ecore_file_private.h"

#ifdef HAVE_POLL

/*
 * TODO:
 * - Implement recursive as an option!
 * - Keep whole path or just name of file? (Memory or CPU...)
 * - Remove requests without files?
 * - Change poll time
 */

typedef struct _Ecore_File             Ecore_File;

struct _Ecore_File
{
   char            *name;
   int              mtime;
   Ecore_File_Type  type;
};

#define ECORE_FILE_INTERVAL_MIN  1.0
#define ECORE_FILE_INTERVAL_STEP 0.5
#define ECORE_FILE_INTERVAL_MAX  5.0

static double       _interval = ECORE_FILE_INTERVAL_MIN;
static Ecore_Timer *_timer = NULL;
static Evas_List   *_monitors = NULL;
static int          _lock = 0;

static int                         _ecore_file_monitor_handler(void *data);
static void                        _ecore_file_monitor_check(Ecore_File_Monitor *em);
static int                         _ecore_file_monitor_checking(Ecore_File_Monitor *em, char *name);

int
ecore_file_monitor_init(void)
{
   return 1;
}

int
ecore_file_monitor_shutdown(void)
{
   Evas_List *l;
   for (l = _monitors; l;)
     {
	Ecore_File_Monitor *em;

	em = l->data;
	l = l->next;
	ecore_file_monitor_del(em);
     }
   evas_list_free(_monitors);
   if (_timer)
     {
       	ecore_timer_del(_timer);
	_timer = NULL;
     }
   return 1;
}

Ecore_File_Monitor *
ecore_file_monitor_add(const char *path,
		       void (*func) (void *data, Ecore_File_Monitor *em,
				     Ecore_File_Type type,
				     Ecore_File_Event event,
				     const char *path),
		       void *data)
{
   Ecore_File_Monitor *em;
   int len;

   if (!path) return NULL;
   if (!func) return NULL;

   em = calloc(1, sizeof(Ecore_File_Monitor));
   if (!em) return NULL;

   if (!_timer)
     _timer = ecore_timer_add(_interval, _ecore_file_monitor_handler, NULL);
   else
     ecore_timer_interval_set(_timer, ECORE_FILE_INTERVAL_MIN);

   em->path = strdup(path);
   len = strlen(em->path);
   if (em->path[len - 1] == '/')
     em->path[len - 1] = 0;

   em->func = func;
   em->data = data;

   em->mtime = ecore_file_mod_time(em->path);
   if (ecore_file_exists(em->path))
     {
	em->type = ecore_file_is_dir(em->path) ?
		   ECORE_FILE_TYPE_DIRECTORY :
		   ECORE_FILE_TYPE_FILE;

	em->func(em->data, em, em->type, ECORE_FILE_EVENT_EXISTS, em->path);
	if (em->type == ECORE_FILE_TYPE_DIRECTORY)
	  {
	     /* Check for subdirs */
	     Evas_List *files, *l;

	     files = ecore_file_ls(em->path);
	     for (l = files; l; l = l->next)
	       {
		  Ecore_File *f;
		  char *file;
		  char buf[PATH_MAX];

		  file = l->data;
		  f = calloc(1, sizeof(Ecore_File));
		  if (!f)
		    {
		       free(file);
		       continue;
		    }

		  snprintf(buf, sizeof(buf), "%s/%s", em->path, file);
		  f->name = file;
		  f->mtime = ecore_file_mod_time(buf);
		  f->type = ecore_file_is_dir(buf) ?
			    ECORE_FILE_TYPE_DIRECTORY :
			    ECORE_FILE_TYPE_FILE;
		  em->func(em->data, em, f->type, ECORE_FILE_EVENT_EXISTS, buf);
		  em->files = evas_list_append(em->files, f);
	       }
	     evas_list_free(files);
	  }
     }
   else
     {
	em->type = ECORE_FILE_TYPE_NONE;
	em->func(em->data, em, em->type, ECORE_FILE_EVENT_DELETED, em->path);
     }

   _monitors = evas_list_append(_monitors, em);

   return em;
}

void
ecore_file_monitor_del(Ecore_File_Monitor *em)
{
   Evas_List *l;

   if (_lock)
     {
	em->deleted = 1;
	return;
     }

   /* Remove files */
   for (l = em->files; l; l = l->next)
     {
	Ecore_File *f;
	f = l->data;
	free(f->name);
	free(f);
     }
   evas_list_free(em->files);

   _monitors = evas_list_remove(_monitors, em);
   free(em->path);
   free(em);

   if ((!_monitors) && (_timer))
     {
	ecore_timer_del(_timer);
	_timer = NULL;
     }
   else
     ecore_timer_interval_set(_timer, ECORE_FILE_INTERVAL_MIN);
}

static int
_ecore_file_monitor_handler(void *data)
{
   Evas_List *monitor;

   _interval += ECORE_FILE_INTERVAL_STEP;
   _lock = 1;
   for (monitor = _monitors; monitor;)
     {
	Ecore_File_Monitor *em;

	em = monitor->data;
	monitor = monitor->next;
	_ecore_file_monitor_check(em);
     }
   _lock = 0;
   if (_interval > ECORE_FILE_INTERVAL_MAX)
     _interval = ECORE_FILE_INTERVAL_MAX;
   ecore_timer_interval_set(_timer, _interval);

   for (monitor = _monitors; monitor;)
     {
	Ecore_File_Monitor *em;

	em = monitor->data;
	monitor = monitor->next;
	if (em->deleted)
	  ecore_file_monitor_del(em);
     }
   return 1;
}

static void
_ecore_file_monitor_check(Ecore_File_Monitor *em)
{
   int mtime;

   mtime = ecore_file_mod_time(em->path);
   switch (em->type)
     {
      case ECORE_FILE_TYPE_FILE:
	 if (mtime < em->mtime)
	   {
	      em->func(em->data, em, em->type, ECORE_FILE_EVENT_DELETED, em->path);
	      em->type = ECORE_FILE_TYPE_NONE;
	      _interval = ECORE_FILE_INTERVAL_MIN;
	   }
	 else if (mtime > em->mtime)
	   {
	      em->func(em->data, em, em->type, ECORE_FILE_EVENT_CHANGED, em->path);
	      _interval = ECORE_FILE_INTERVAL_MIN;
	   }
	 break;
      case ECORE_FILE_TYPE_DIRECTORY:
	 if (mtime < em->mtime)
	   {
	      /* Deleted */
	      Evas_List *l;

	      /* Notify all files deleted */
	      for (l = em->files; l;)
		{
		   Ecore_File *f;
		   char buf[PATH_MAX];

		   f = l->data;
		   l = l->next;
		   snprintf(buf, sizeof(buf), "%s/%s", em->path, f->name);
		   em->func(em->data, em, f->type, ECORE_FILE_EVENT_DELETED, buf);
		   free(f->name);
		   free(f);
		}
	      em->files = evas_list_free(em->files);
	      em->func(em->data, em, em->type, ECORE_FILE_EVENT_DELETED, em->path);
	      em->type = ECORE_FILE_TYPE_NONE;
	      _interval = ECORE_FILE_INTERVAL_MIN;
	   }
	 else
	   {
	      Evas_List *l;

	      /* Check for changed files */
	      for (l = em->files; l;)
		{
		   Ecore_File *f;
		   char buf[PATH_MAX];
		   int mtime;

		   f = l->data;
		   l = l->next;
		   snprintf(buf, sizeof(buf), "%s/%s", em->path, f->name);
		   mtime = ecore_file_mod_time(buf);
		   if (mtime < f->mtime)
		     {
			em->func(em->data, em, f->type, ECORE_FILE_EVENT_DELETED, buf);
			em->files = evas_list_remove(em->files, f);
			free(f->name);
			free(f);
			_interval = ECORE_FILE_INTERVAL_MIN;
		     }
		   else if (mtime > f->mtime)
		     {
			em->func(em->data, em, f->type, ECORE_FILE_EVENT_CHANGED, buf);
			_interval = ECORE_FILE_INTERVAL_MIN;
		     }
		   f->mtime = mtime;
		}

	      /* Check for new files */
	      if (em->mtime < mtime)
		{
		   Evas_List *files;

		   /* Files have been added or removed */
		   files = ecore_file_ls(em->path);
		   for (l = files; l; l = l->next)
		     {
			Ecore_File *f;
			char *file;
			char buf[PATH_MAX];

			file = l->data;
			if (_ecore_file_monitor_checking(em, file))
			  {
			     free(file);
			     continue;
			  }

			snprintf(buf, sizeof(buf), "%s/%s", em->path, file);
			f = calloc(1, sizeof(Ecore_File));
			if (!f)
			  {
			     free(file);
			     continue;
			  }

			f->name = file;
			f->mtime = ecore_file_mod_time(buf);
			f->type = ecore_file_is_dir(buf) ?
				  ECORE_FILE_TYPE_DIRECTORY :
				  ECORE_FILE_TYPE_FILE;
			em->func(em->data, em, f->type, ECORE_FILE_EVENT_CREATED, buf);
			em->files = evas_list_append(em->files, f);
		     }
		   em->func(em->data, em, em->type, ECORE_FILE_EVENT_CHANGED, em->path);
		   _interval = ECORE_FILE_INTERVAL_MIN;
		}
	   }
	 break;
      case ECORE_FILE_TYPE_NONE:
	 if (mtime > em->mtime)
	   {
	      /* Something has been created! */
	      em->type = ecore_file_is_dir(em->path) ?
			 ECORE_FILE_TYPE_DIRECTORY :
			 ECORE_FILE_TYPE_FILE;

	      em->func(em->data, em, em->type, ECORE_FILE_EVENT_CREATED, em->path);
	      if (em->type == ECORE_FILE_TYPE_DIRECTORY)
		{
		   /* Check for subdirs */
		   Evas_List *files, *l;

		   em->func(em->data, em, em->type, ECORE_FILE_EVENT_CREATED, em->path);
		   files = ecore_file_ls(em->path);
		   for (l = files; l; l = l->next)
		     {
			Ecore_File *f;
			char *file;
			char buf[PATH_MAX];

			file = l->data;
			snprintf(buf, sizeof(buf), "%s/%s", em->path, file);
			f = calloc(1, sizeof(Ecore_File));
			if (!f)
			  {
			     free(file);
			     continue;
			  }

			f->name = file;
			f->mtime = ecore_file_mod_time(buf);
			f->type = ecore_file_is_dir(buf) ?
			   ECORE_FILE_TYPE_DIRECTORY :
			   ECORE_FILE_TYPE_FILE;
			em->func(em->data, em, f->type, ECORE_FILE_EVENT_CREATED, buf);
			em->files = evas_list_append(em->files, f);
		     }
		   evas_list_free(files);
		}
	      _interval = ECORE_FILE_INTERVAL_MIN;
	   }
	 break;
     }
   em->mtime = mtime;
}

static int
_ecore_file_monitor_checking(Ecore_File_Monitor *em, char *name)
{
   Evas_List *l;

   for (l = em->files; l; l = l->next)
     {
	Ecore_File *f;

	f = l->data;
	if (!strcmp(f->name, name))
	  return 1;
     }

   return 0;
}
#endif
