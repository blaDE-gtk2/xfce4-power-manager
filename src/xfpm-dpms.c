/*
 *
 * * Copyright (C) 2008-2011 Ali <aliov@xfce.org>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk/gdk.h>

#include <libxfce4util/libxfce4util.h>

#include "xfpm-common.h"

#include "xfpm-dpms.h"
#include "xfpm-power.h"
#include "xfpm-xfconf.h"
#include "xfpm-config.h"
#include "xfpm-debug.h"

#ifdef HAVE_DPMS

static void xfpm_dpms_finalize   (GObject *object);

#define XFPM_DPMS_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE((o), XFPM_TYPE_DPMS, XfpmDpmsPrivate))

struct XfpmDpmsPrivate
{
    XfpmXfconf      *conf;
    XfpmPower       *power;
    
    gboolean         dpms_capable;
    
    gboolean         on_battery;
    
    gulong	     switch_off_timeout_id;
    gulong	     switch_on_timeout_id;
};

G_DEFINE_TYPE (XfpmDpms, xfpm_dpms, G_TYPE_OBJECT)

static void
xfpm_dpms_set_timeouts (XfpmDpms *dpms, guint16 standby, guint16 suspend, guint off)
{
    CARD16 x_standby = 0 , x_suspend = 0, x_off = 0;
    
    DPMSGetTimeouts (GDK_DISPLAY(), &x_standby, &x_suspend, &x_off);
    
    if ( standby != x_standby || suspend != x_suspend || off != x_off )
    {
	XFPM_DEBUG ("Settings dpms: standby=%d suspend=%d off=%d\n", standby, suspend, off);
	DPMSSetTimeouts (GDK_DISPLAY(), standby,
					suspend,
					off );
    }
}

/*
 * Disable DPMS
 */
static void
xfpm_dpms_disable (XfpmDpms *dpms)
{
    BOOL state;
    CARD16 power_level;
    
    if (!DPMSInfo (GDK_DISPLAY(), &power_level, &state) )
	g_warning ("Cannot get DPMSInfo");
	
    if ( state )
	DPMSDisable (GDK_DISPLAY());
}

/*
 * Enable DPMS
 */
static void
xfpm_dpms_enable (XfpmDpms *dpms)
{
    BOOL state;
    CARD16 power_level;
    
    if (!DPMSInfo (GDK_DISPLAY(), &power_level, &state) )
	g_warning ("Cannot get DPMSInfo");
	
    if ( !state )
	DPMSEnable (GDK_DISPLAY());
}

static void
xfpm_dpms_get_enabled (XfpmDpms *dpms, gboolean *dpms_enabled)
{
    g_object_get (G_OBJECT (dpms->priv->conf),
		  DPMS_ENABLED_CFG, dpms_enabled,
		  NULL);
}

static void
xfpm_dpms_get_sleep_mode (XfpmDpms *dpms, gboolean *ret_standby_mode)
{
    gchar *sleep_mode;
    
    g_object_get (G_OBJECT (dpms->priv->conf),
		  DPMS_SLEEP_MODE, &sleep_mode,
		  NULL);
    
    if ( !g_strcmp0 (sleep_mode, "standby"))
	*ret_standby_mode = TRUE;
    else
	*ret_standby_mode = FALSE;
	
    g_free (sleep_mode);
}

static void
xfpm_dpms_get_configuration_timeouts (XfpmDpms *dpms, guint16 *ret_sleep, guint16 *ret_off )
{
    guint sleep_time, off_time;
    
    g_object_get (G_OBJECT (dpms->priv->conf),
		  dpms->priv->on_battery ? ON_BATT_DPMS_SLEEP : ON_AC_DPMS_SLEEP, &sleep_time,
		  dpms->priv->on_battery ? ON_BATT_DPMS_OFF : ON_AC_DPMS_OFF, &off_time,
		  NULL);
		  
    *ret_sleep = sleep_time * 60;
    *ret_off =  off_time * 60;
}

void
xfpm_dpms_refresh (XfpmDpms *dpms)
{
    gboolean enabled;
    guint16 off_timeout;
    guint16 sleep_timeout;
    gboolean sleep_mode;
    gboolean presentation_mode;
    
    presentation_mode = (xfpm_power_get_mode (dpms->priv->power) == XFPM_POWER_MODE_PRESENTATION);
    
    if (presentation_mode)
    {
	xfpm_dpms_disable (dpms);
	return;
    }
    
    xfpm_dpms_get_enabled (dpms, &enabled);
    
    if ( !enabled )
    {
	xfpm_dpms_disable (dpms);
	return;
    }

    xfpm_dpms_enable (dpms);
    xfpm_dpms_get_configuration_timeouts (dpms, &sleep_timeout, &off_timeout);
    xfpm_dpms_get_sleep_mode (dpms, &sleep_mode);

    if (sleep_mode == TRUE )
    {
	xfpm_dpms_set_timeouts	   (dpms, 
				    sleep_timeout,
				    0,
				    off_timeout);
    }
    else
    {
	xfpm_dpms_set_timeouts     (dpms, 
				    0,
				    sleep_timeout,
				    off_timeout );
    }
}

static void
xfpm_dpms_settings_changed_cb (GObject *obj, GParamSpec *spec, XfpmDpms *dpms)
{
    if ( g_str_has_prefix (spec->name, "dpms"))
    {
	XFPM_DEBUG ("Configuration changed");
	xfpm_dpms_refresh (dpms);
    }
}

static void
xfpm_dpms_on_battery_changed_cb (XfpmPower *power, gboolean on_battery, XfpmDpms *dpms)
{
    dpms->priv->on_battery = on_battery;
    xfpm_dpms_refresh (dpms);
}

static void
xfpm_dpms_class_init(XfpmDpmsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    
    object_class->finalize = xfpm_dpms_finalize;

    g_type_class_add_private(klass,sizeof(XfpmDpmsPrivate));
}

/*
 * Check if the display is DPMS capabale if not do nothing.
 */
static void
xfpm_dpms_init(XfpmDpms *dpms)
{
    dpms->priv = XFPM_DPMS_GET_PRIVATE(dpms);
    
    dpms->priv->dpms_capable = DPMSCapable (GDK_DISPLAY());
    dpms->priv->switch_off_timeout_id = 0;
    dpms->priv->switch_on_timeout_id = 0;

    if ( dpms->priv->dpms_capable )
    {
	dpms->priv->power = xfpm_power_get ();
	dpms->priv->conf = xfpm_xfconf_new  ();
    
	g_signal_connect (dpms->priv->power, "on-battery-changed",
			  G_CALLBACK(xfpm_dpms_on_battery_changed_cb), dpms);
			  
	g_signal_connect (dpms->priv->conf, "notify",
			  G_CALLBACK (xfpm_dpms_settings_changed_cb), dpms);
			  
	dpms->priv->on_battery = xfpm_power_get_on_battery (dpms->priv->power);
	
	xfpm_dpms_refresh (dpms);
    }
    else
    {
	g_warning ("Monitor is not DPMS capable");
    }
}

static void
xfpm_dpms_finalize(GObject *object)
{
    XfpmDpms *dpms;

    dpms = XFPM_DPMS (object);
    
    g_object_unref (dpms->priv->conf);
    g_object_unref (dpms->priv->power);

    G_OBJECT_CLASS(xfpm_dpms_parent_class)->finalize(object);
}

XfpmDpms *
xfpm_dpms_new (void)
{
    static gpointer xfpm_dpms_object = NULL;
    
    if ( G_LIKELY (xfpm_dpms_object != NULL ) )
    {
	g_object_ref (xfpm_dpms_object);
    }
    else
    {
	xfpm_dpms_object = g_object_new (XFPM_TYPE_DPMS, NULL);
	g_object_add_weak_pointer (xfpm_dpms_object, &xfpm_dpms_object);
    }
    
    return XFPM_DPMS (xfpm_dpms_object);
}

gboolean xfpm_dpms_capable (XfpmDpms *dpms)
{
    g_return_val_if_fail (XFPM_IS_DPMS(dpms), FALSE);
    
    return dpms->priv->dpms_capable;
}

void xfpm_dpms_force_level (XfpmDpms *dpms, CARD16 level)
{
    CARD16 current_level;
    BOOL current_state;
    
    XFPM_DEBUG ("start");
    
    if ( !dpms->priv->dpms_capable )
	goto out;
    
    if ( G_UNLIKELY (!DPMSInfo (GDK_DISPLAY (), &current_level, &current_state)) )
    {
	g_warning ("Cannot get DPMSInfo");
	goto out;
    }

    if ( !current_state )
    {
	XFPM_DEBUG ("DPMS is disabled");
	goto out;
    }

    if ( current_level != level )
    {
	XFPM_DEBUG ("Forcing DPMS mode %d", level);
	
	if ( !DPMSForceLevel (GDK_DISPLAY (), level ) )
	{
	    g_warning ("Cannot set Force DPMS level %d", level);
	    goto out;
	}
	if ( level == DPMSModeOn )
	    XResetScreenSaver (GDK_DISPLAY ());
	XSync (GDK_DISPLAY (), FALSE);
    }
    else
    {
	XFPM_DEBUG ("No need to change DPMS mode, current_level=%d requested_level=%d", current_level, level);
    }
    
    out:
	;
}

#endif /* HAVE_DPMS */
