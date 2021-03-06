#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif
#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#elif !defined alloca
# ifdef __GNUC__
#  define alloca __builtin_alloca
# elif defined _AIX
#  define alloca __alloca
# elif defined _MSC_VER
#  include <malloc.h>
#  define alloca _alloca
# elif !defined HAVE_ALLOCA
#  ifdef  __cplusplus
extern "C"
#  endif
void *alloca (size_t);
# endif
#endif

#include "ecore_wl_private.h"

/* local function prototypes */
static void _ecore_wl_output_cb_geometry(void *data, struct wl_output *wl_output __UNUSED__, int x, int y, int w, int h, int subpixel __UNUSED__, const char *make __UNUSED__, const char *model __UNUSED__, int transform __UNUSED__);
static void _ecore_wl_output_cb_mode(void *data, struct wl_output *wl_output __UNUSED__, unsigned int flags, int w, int h, int refresh __UNUSED__);

/* wayland listeners */
static const struct wl_output_listener _ecore_wl_output_listener = 
{
   _ecore_wl_output_cb_geometry,
   _ecore_wl_output_cb_mode
};

/* @since 1.2 */
EAPI struct wl_list *
ecore_wl_outputs_get(void)
{
   return &(_ecore_wl_disp->outputs);
}

void 
_ecore_wl_output_add(Ecore_Wl_Display *ewd, unsigned int id)
{
   Ecore_Wl_Output *output;

   LOGFN(__FILE__, __LINE__, __FUNCTION__);

   if (!(output = malloc(sizeof(Ecore_Wl_Output)))) return;

   memset(output, 0, sizeof(Ecore_Wl_Output));

   output->display = ewd;

   output->output = 
     wl_registry_bind(ewd->wl.registry, id, &wl_output_interface, 1);

   wl_list_insert(ewd->outputs.prev, &output->link);
   wl_output_add_listener(output->output, &_ecore_wl_output_listener, output);
}

void 
_ecore_wl_output_del(Ecore_Wl_Output *output) 
{
   if (!output) return;
   if (output->destroy) (*output->destroy)(output, output->data);
   if (output->output) wl_output_destroy(output->output);
   wl_list_remove(&output->link);
   free(output);
}

/* local functions */
static void 
_ecore_wl_output_cb_geometry(void *data, struct wl_output *wl_output __UNUSED__, int x, int y, int w, int h, int subpixel __UNUSED__, const char *make __UNUSED__, const char *model __UNUSED__, int transform)
{
   Ecore_Wl_Output *output;

   LOGFN(__FILE__, __LINE__, __FUNCTION__);

   output = data;
   output->allocation.x = x;
   output->allocation.y = y;
   output->mw = w;
   output->mh = h;
   output->transform = transform;
}

static void 
_ecore_wl_output_cb_mode(void *data, struct wl_output *wl_output __UNUSED__, unsigned int flags, int w, int h, int refresh __UNUSED__)
{
   Ecore_Wl_Output *output;
   Ecore_Wl_Display *ewd;

   LOGFN(__FILE__, __LINE__, __FUNCTION__);

   output = data;
   ewd = output->display;
   if (flags & WL_OUTPUT_MODE_CURRENT)
     {
        output->allocation.w = w;
        output->allocation.h = h;
        _ecore_wl_disp->output = output;
        if (ewd->output_configure) (*ewd->output_configure)(output, ewd->data);
     }
}
