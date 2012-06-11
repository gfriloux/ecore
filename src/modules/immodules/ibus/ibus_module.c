#include <ibus.h>
#include "ibus_imcontext.h"
#include <Ecore_IMF.h>
#include <Ecore.h>
#include <stdio.h>

#define IBUS_LOCALDIR ""
static const Ecore_IMF_Context_Info ibus_im_info = {
    "ibus",
    "IBus (Intelligent Input Bus)",
    "*",
    NULL,
    0
};

static Ecore_IMF_Context_Class ibus_imf_class = {
    ibus_im_context_add,                    /* add */
    ibus_im_context_del,                    /* del */
    ibus_im_context_client_window_set,      /* client_window_set */
    ibus_im_context_client_canvas_set,      /* client_canvas_set */
    NULL,                                   /* input_panel_show */
    NULL,                                   /* input_panel_hide */
    ibus_im_context_preedit_string_get,     /* get_preedit_string */
    ibus_im_context_focus_in,               /* focus_in */
    ibus_im_context_focus_out,              /* focus_out */
    ibus_im_context_reset,                  /* reset */
    NULL,                                   /* cursor_position_set */
    ibus_im_context_use_preedit_set,        /* use_preedit_set */
    NULL,                                   /* input_mode_set */
    ibus_im_context_filter_event,           /* filter_event */
    ibus_im_context_preedit_string_with_attributes_get,  /* preedit_string_with_attribute_get */
    NULL,                                   /* prediction_allow_set */
    NULL,                                   /* autocapital_type_set */
    NULL,                                   /* control panel show */
    NULL,                                   /* control panel hide */
    NULL,                                   /* input_panel_layout_set */
    NULL,                                   /* ibus_im_context_input_panel_layout_get, */
    NULL,                                   /* ibus_im_context_input_panel_language_set, */
    NULL,                                   /* ibus_im_context_input_panel_language_get, */
    ibus_im_context_cursor_location_set,    /* cursor_location_set */
    NULL,                                   /* input_panel_imdata_set */
    NULL,                                   /* input_panel_imdata_get */
    NULL,                                   /* input_panel_return_key_type_set */
    NULL,                                   /* input_panel_return_key_disabled_set */
    NULL                                    /* input_panel_caps_lock_mode_set */
};

static Ecore_IMF_Context *im_module_create (void);
static Ecore_IMF_Context *im_module_exit (void);

static Eina_Bool
im_module_init(void)
{
    ecore_main_loop_glib_integrate();
    g_type_init();
    ecore_imf_module_register(&ibus_im_info, im_module_create, im_module_exit);

    return EINA_TRUE;
}

static void im_module_shutdown(void)
{
}

static Ecore_IMF_Context *
im_module_exit(void)
{
    return NULL;
}

static Ecore_IMF_Context *
im_module_create()
{
    Ecore_IMF_Context *ctx = NULL;
    IBusIMContext *ctxd = NULL;

    ctxd = ibus_im_context_new();
    if (!ctxd)
      {
         return NULL;
      }

    ctx = ecore_imf_context_new(&ibus_imf_class);
    if (!ctx)
      {
         free(ctxd);
         return NULL;
      }

    ecore_imf_context_data_set(ctx, ctxd);

    return ctx;
}

EINA_MODULE_INIT(im_module_init);
EINA_MODULE_SHUTDOWN(im_module_shutdown);

