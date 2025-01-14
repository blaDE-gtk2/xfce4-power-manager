/*
 * * Copyright (C) 2009-2011 Ali <aliov@xfce.org>
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
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <upower.h>
#include <gdk/gdkx.h>

#include <libbladeutil/libbladeutil.h>
#include <libbladeui/libbladeui.h>

#include "blpm-power.h"
#include "blpm-dbus.h"
#include "blpm-dpms.h"
#include "blpm-battery.h"
#include "blpm-blconf.h"
#include "blpm-notify.h"
#include "blpm-errors.h"
#include "blpm-console-kit.h"
#include "blpm-inhibit.h"
#include "blpm-polkit.h"
#include "blpm-network-manager.h"
#include "blpm-icons.h"
#include "blpm-common.h"
#include "blpm-power-common.h"
#include "blpm-config.h"
#include "blpm-debug.h"
#include "blpm-enum-types.h"
#include "egg-idletime.h"
#include "blpm-systemd.h"
#include "blpm-suspend.h"
#include "blpm-brightness.h"

static void blpm_power_finalize     (GObject *object);

static void blpm_power_get_property (GObject *object,
				     guint prop_id,
				     GValue *value,
				     GParamSpec *pspec);

static void blpm_power_set_property (GObject *object,
				     guint prop_id,
				     const GValue *value,
				     GParamSpec *pspec);

static void blpm_power_change_presentation_mode (XfpmPower *power,
						 gboolean presentation_mode);

static void blpm_update_blank_time (XfpmPower *power);

static void blpm_power_dbus_class_init (XfpmPowerClass * klass);
static void blpm_power_dbus_init (XfpmPower *power);


#define XFPM_POWER_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), XFPM_TYPE_POWER, XfpmPowerPrivate))

struct XfpmPowerPrivate
{
    DBusGConnection *bus;

    UpClient        *upower;

    GHashTable      *hash;


    XfpmSystemd     *systemd;
    XfpmConsoleKit  *console;
    XfpmInhibit	    *inhibit;
    XfpmBlconf      *conf;

    XfpmBatteryCharge overall_state;
    gboolean         critical_action_done;

    XfpmDpms        *dpms;
    gboolean         presentation_mode;
    gint             on_ac_blank;
    gint             on_battery_blank;
    EggIdletime     *idletime;

    gboolean	     inhibited;

    XfpmNotify	    *notify;
#ifdef ENABLE_POLKIT
    XfpmPolkit 	    *polkit;
#endif
    gboolean	     auth_suspend;
    gboolean	     auth_hibernate;

    /* Properties */
    gboolean	     on_low_battery;
    gboolean	     lid_is_present;
    gboolean         lid_is_closed;
    gboolean	     on_battery;
    gchar           *daemon_version;
    gboolean	     can_suspend;
    gboolean         can_hibernate;

    /**
     * Warning dialog to use when notification daemon
     * doesn't support actions.
     **/
    GtkWidget 	    *dialog;
};

enum
{
    PROP_0,
    PROP_ON_LOW_BATTERY,
    PROP_ON_BATTERY,
    PROP_AUTH_SUSPEND,
    PROP_AUTH_HIBERNATE,
    PROP_CAN_SUSPEND,
    PROP_CAN_HIBERNATE,
    PROP_HAS_LID,
    PROP_PRESENTATION_MODE,
    PROP_ON_AC_BLANK,
    PROP_ON_BATTERY_BLANK,
    N_PROPERTIES
};

enum
{
    ON_BATTERY_CHANGED,
    LOW_BATTERY_CHANGED,
    LID_CHANGED,
    WAKING_UP,
    SLEEPING,
    ASK_SHUTDOWN,
    SHUTDOWN,
    LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };


G_DEFINE_TYPE (XfpmPower, blpm_power, G_TYPE_OBJECT)


#if UP_CHECK_VERSION(0, 99, 0)
/* This checks if consolekit returns TRUE for either suspend or
 * hibernate showing support. This means that ConsoleKit2 is running
 * (and the system is capable of those actions).
 */
static gboolean
check_for_consolekit2 (XfpmPower *power)
{
    XfpmConsoleKit *console;
    gboolean can_suspend, can_hibernate;

    g_return_val_if_fail (XFPM_IS_POWER (power), FALSE);

    if (power->priv->console == NULL)
	return FALSE;

    console = power->priv->console;

    g_object_get (G_OBJECT (console),
		  "can-suspend", &can_suspend,
		  NULL);
    g_object_get (G_OBJECT (console),
		  "can-hibernate", &can_hibernate,
		  NULL);

    /* ConsoleKit2 supports suspend and hibernate */
    if (can_suspend || can_hibernate)
    {
	return TRUE;
    }

    return FALSE;
}
#endif

#ifdef ENABLE_POLKIT
static void
blpm_power_check_polkit_auth (XfpmPower *power)
{
    const char *suspend = NULL, *hibernate = NULL;
    if (LOGIND_RUNNING())
    {
	XFPM_DEBUG ("using logind suspend backend");
	suspend   = POLKIT_AUTH_SUSPEND_LOGIND;
	hibernate = POLKIT_AUTH_HIBERNATE_LOGIND;
    }
    else
    {
#if !UP_CHECK_VERSION(0, 99, 0)
	XFPM_DEBUG ("using upower suspend backend");
	suspend   = POLKIT_AUTH_SUSPEND_UPOWER;
	hibernate = POLKIT_AUTH_HIBERNATE_UPOWER;
#else
	if (power->priv->console != NULL)
	{
	    /* ConsoleKit2 supports suspend and hibernate */
	    if (check_for_consolekit2 (power))
	    {
		XFPM_DEBUG ("using consolekit2 suspend backend");
		suspend   = POLKIT_AUTH_SUSPEND_CONSOLEKIT2;
		hibernate = POLKIT_AUTH_HIBERNATE_CONSOLEKIT2;
	    }
	    else
	    {
		XFPM_DEBUG ("using blpm internal suspend backend");
		suspend   = POLKIT_AUTH_SUSPEND_XFPM;
		hibernate = POLKIT_AUTH_HIBERNATE_XFPM;
	    }
	}
#endif
    }
    power->priv->auth_suspend = blpm_polkit_check_auth (power->priv->polkit,
							suspend);

    power->priv->auth_hibernate = blpm_polkit_check_auth (power->priv->polkit,
							  hibernate);
}
#endif

static void
blpm_power_check_power (XfpmPower *power, gboolean on_battery)
{
	if (on_battery != power->priv->on_battery )
	{
	    GList *list;
	    guint len, i;
	    g_signal_emit (G_OBJECT (power), signals [ON_BATTERY_CHANGED], 0, on_battery);

        blpm_dpms_set_on_battery (power->priv->dpms, on_battery);

	    power->priv->on_battery = on_battery;
	    list = g_hash_table_get_values (power->priv->hash);
	    len = g_list_length (list);
	    for ( i = 0; i < len; i++)
	    {
		g_object_set (G_OBJECT (g_list_nth_data (list, i)),
			      "ac-online", !on_battery,
			      NULL);
        blpm_update_blank_time (power);
	    }
	}
}

static void
blpm_power_check_lid (XfpmPower *power, gboolean present, gboolean closed)
{
    power->priv->lid_is_present = present;

    if (power->priv->lid_is_present)
    {
	if (closed != power->priv->lid_is_closed )
	{
	    power->priv->lid_is_closed = closed;
	    g_signal_emit (G_OBJECT (power), signals [LID_CHANGED], 0, power->priv->lid_is_closed);
	}
    }
}

/*
 * Get the properties on org.freedesktop.DeviceKit.Power
 *
 * DaemonVersion      's'
 * CanSuspend'        'b'
 * CanHibernate'      'b'
 * OnBattery'         'b'
 * OnLowBattery'      'b'
 * LidIsClosed'       'b'
 * LidIsPresent'      'b'
 */
static void
blpm_power_get_properties (XfpmPower *power)
{
    gboolean on_battery;
    gboolean lid_is_closed;
    gboolean lid_is_present;

#if !UP_CHECK_VERSION(0, 99, 0)
    power->priv->can_suspend = up_client_get_can_suspend(power->priv->upower);
    power->priv->can_hibernate = up_client_get_can_hibernate(power->priv->upower);
#else
    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-suspend", &power->priv->can_suspend,
                      NULL);
	g_object_get (G_OBJECT (power->priv->systemd),
                      "can-hibernate", &power->priv->can_hibernate,
                      NULL);
    }
    else
    {
	if (check_for_consolekit2 (power))
	{
	    g_object_get (G_OBJECT (power->priv->console),
			  "can-suspend", &power->priv->can_suspend,
			  NULL);
	    g_object_get (G_OBJECT (power->priv->console),
			  "can-hibernate", &power->priv->can_hibernate,
			  NULL);
	}
	else
	{
	    power->priv->can_suspend   = blpm_suspend_can_suspend ();
	    power->priv->can_hibernate = blpm_suspend_can_hibernate ();
	}
    }
#endif
    g_object_get (power->priv->upower,
                  "on-battery", &on_battery,
                  "lid-is-closed", &lid_is_closed,
                  "lid-is-present", &lid_is_present,
                  NULL);
    blpm_power_check_lid (power, lid_is_present, lid_is_closed);
    blpm_power_check_power (power, on_battery);
}

static void
blpm_power_report_error (XfpmPower *power, const gchar *error, const gchar *icon_name)
{
    GtkStatusIcon *battery = NULL;
    guint i, len;
    GList *list;

    list = g_hash_table_get_values (power->priv->hash);
    len = g_list_length (list);

    for ( i = 0; i < len; i++)
    {
	UpDeviceKind type;
	battery = g_list_nth_data (list, i);
	type = blpm_battery_get_device_type (XFPM_BATTERY (battery));
	if ( type == UP_DEVICE_KIND_BATTERY ||
	     type == UP_DEVICE_KIND_UPS )
	     break;
    }

    blpm_notify_show_notification (power->priv->notify,
				   _("Power Manager"),
				   error,
				   icon_name,
				   10000,
				   FALSE,
				   XFPM_NOTIFY_CRITICAL,
				   battery);

}

static void
blpm_power_sleep (XfpmPower *power, const gchar *sleep_time, gboolean force)
{
    GError *error = NULL;
    gboolean lock_screen;
#ifdef WITH_NETWORK_MANAGER
    gboolean network_manager_sleep;
#endif
    XfpmBrightness *brightness;
    gint32 brightness_level;

    if ( power->priv->inhibited && force == FALSE)
    {
	gboolean ret;

	ret = xfce_dialog_confirm (NULL,
				   GTK_STOCK_OK, _("_Hibernate"),
				   _("An application is currently disabling the automatic sleep. "
				     "Doing this action now may damage the working state of this application."),
				   _("Are you sure you want to hibernate the system?"));

	if ( !ret )
	    return;
    }

    g_signal_emit (G_OBJECT (power), signals [SLEEPING], 0);
    /* Get the current brightness level so we can use it after we suspend */
    brightness = blpm_brightness_new();
    blpm_brightness_setup (brightness);
    blpm_brightness_get_level (brightness, &brightness_level);

#ifdef WITH_NETWORK_MANAGER
    g_object_get (G_OBJECT (power->priv->conf),
                  NETWORK_MANAGER_SLEEP, &network_manager_sleep,
                  NULL);

    if ( network_manager_sleep )
    {
        blpm_network_manager_sleep (TRUE);
    }
#endif

    g_object_get (G_OBJECT (power->priv->conf),
		  LOCK_SCREEN_ON_SLEEP, &lock_screen,
		  NULL);

    if ( lock_screen )
    {
#ifdef WITH_NETWORK_MANAGER
        if ( network_manager_sleep )
        {
	    /* 2 seconds, to give network manager time to sleep */
            g_usleep (2000000);
	}
#endif
        if (!blpm_lock_screen ())
        {
	    gboolean ret;

	    ret = xfce_dialog_confirm (NULL,
				       GTK_STOCK_OK, _("Continue"),
			               _("None of the screen lock tools ran "
				         "successfully, the screen will not "
					 "be locked."),
				       _("Do you still want to continue to "
				         "suspend the system?"));

	    if ( !ret )
		return;
        }
    }

    /* This is fun, here's the order of operations:
     * - if the Logind is running then use it
     * - if UPower < 0.99.0 then use it (don't make changes on the user unless forced)
     * - if ConsoleKit2 is running then use it
     * - if everything else fails use our built-in fallback
     */
    if ( LOGIND_RUNNING () )
    {
	blpm_systemd_sleep (power->priv->systemd, sleep_time, &error);
    }
    else
    {
#if !UP_CHECK_VERSION(0, 99, 0)
	if (!g_strcmp0 (sleep_time, "Hibernate"))
	{
	    up_client_hibernate_sync(power->priv->upower, NULL, &error);
	}
	else
	{
	    up_client_suspend_sync(power->priv->upower, NULL, &error);
	}
#else
	if (!g_strcmp0 (sleep_time, "Hibernate"))
        {
	    if (check_for_consolekit2 (power))
	    {
		blpm_console_kit_hibernate (power->priv->console, &error);
	    }
	    else
	    {
                blpm_suspend_try_action (XFPM_HIBERNATE);
	    }
        }
        else
        {
	    if (check_for_consolekit2 (power))
	    {
		blpm_console_kit_suspend (power->priv->console, &error);
	    }
	    else
	    {
                blpm_suspend_try_action (XFPM_SUSPEND);
	    }
        }
#endif
    }

    if ( error )
    {
	if ( g_error_matches (error, DBUS_GERROR, DBUS_GERROR_NO_REPLY) )
	{
	    XFPM_DEBUG ("D-Bus time out, but should be harmless");
	}
	else
	{
	    blpm_power_report_error (power, error->message, "dialog-error");
	    g_error_free (error);
	}
    }

    g_signal_emit (G_OBJECT (power), signals [WAKING_UP], 0);
    /* Check/update any changes while we slept */
    blpm_power_get_properties (power);
    /* Restore the brightness level from before we suspended */
    blpm_brightness_set_level (brightness, brightness_level);

#ifdef WITH_NETWORK_MANAGER
    if ( network_manager_sleep )
    {
        blpm_network_manager_sleep (FALSE);
    }
#endif
}

static void
blpm_power_hibernate_clicked (XfpmPower *power)
{
    gtk_widget_destroy (power->priv->dialog );
    power->priv->dialog = NULL;
    blpm_power_sleep (power, "Hibernate", TRUE);
}

static void
blpm_power_suspend_clicked (XfpmPower *power)
{
    gtk_widget_destroy (power->priv->dialog );
    power->priv->dialog = NULL;
    blpm_power_sleep (power, "Suspend", TRUE);
}

static void
blpm_power_shutdown_clicked (XfpmPower *power)
{
    gtk_widget_destroy (power->priv->dialog );
    power->priv->dialog = NULL;
    g_signal_emit (G_OBJECT (power), signals [SHUTDOWN], 0);
}

static XfpmBatteryCharge
blpm_power_get_current_charge_state (XfpmPower *power)
{
    GList *list;
    guint len, i;
    gboolean power_supply;
    XfpmBatteryCharge max_charge_status = XFPM_BATTERY_CHARGE_UNKNOWN;

    list = g_hash_table_get_values (power->priv->hash);
    len = g_list_length (list);

    for ( i = 0; i < len; i++ )
    {
	XfpmBatteryCharge battery_charge;
	UpDeviceKind type;

	g_object_get (G_OBJECT (g_list_nth_data (list, i)),
                  "charge-status", &battery_charge,
                  "device-type", &type,
                  "ac-online", &power_supply,
                  NULL);
	if ( type != UP_DEVICE_KIND_BATTERY &&
	     type != UP_DEVICE_KIND_UPS &&
	     power_supply != TRUE)
	    continue;

	max_charge_status = MAX (max_charge_status, battery_charge);
    }

    return max_charge_status;
}

static void
blpm_power_notify_action_callback (NotifyNotification *n, gchar *action, XfpmPower *power)
{
    if ( !g_strcmp0 (action, "Shutdown") )
	g_signal_emit (G_OBJECT (power), signals [SHUTDOWN], 0);
    else
	blpm_power_sleep (power, action, TRUE);
}

static void
blpm_power_add_actions_to_notification (XfpmPower *power, NotifyNotification *n)
{
    gboolean can_shutdown;


    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-shutdown", &can_shutdown,
                      NULL);
    }
    else
    {
        g_object_get (G_OBJECT (power->priv->console),
                      "can-shutdown", &can_shutdown,
                      NULL);
    }

    if (  power->priv->can_hibernate && power->priv->auth_hibernate )
    {
        blpm_notify_add_action_to_notification(
			       power->priv->notify,
			       n,
                               "Hibernate",
                               _("Hibernate the system"),
                               (NotifyActionCallback)blpm_power_notify_action_callback,
                               power);
    }

    if (  power->priv->can_suspend && power->priv->auth_suspend )
    {
        blpm_notify_add_action_to_notification(
			       power->priv->notify,
			       n,
                               "Suspend",
                               _("Suspend the system"),
                               (NotifyActionCallback)blpm_power_notify_action_callback,
                               power);
    }

    if (can_shutdown )
	blpm_notify_add_action_to_notification(
				   power->priv->notify,
				   n,
				   "Shutdown",
				   _("Shutdown the system"),
				   (NotifyActionCallback)blpm_power_notify_action_callback,
				   power);
}

static void
blpm_power_show_critical_action_notification (XfpmPower *power, XfpmBattery *battery)
{
    const gchar *message;
    NotifyNotification *n;

    message = _("System is running on low power. "\
               "Save your work to avoid losing data");

    n =
	blpm_notify_new_notification (power->priv->notify,
				      _("Power Manager"),
				      message,
				      gtk_status_icon_get_icon_name (GTK_STATUS_ICON (battery)),
				      20000,
				      XFPM_NOTIFY_CRITICAL,
				      GTK_STATUS_ICON (battery));

    blpm_power_add_actions_to_notification (power, n);
    blpm_notify_critical (power->priv->notify, n);

}

static void
blpm_power_close_critical_dialog (XfpmPower *power)
{
    gtk_widget_destroy (power->priv->dialog);
    power->priv->dialog = NULL;
}

static void
blpm_power_show_critical_action_gtk (XfpmPower *power)
{
    GtkWidget *dialog;
    GtkWidget *content_area;
    GtkWidget *cancel;
    const gchar *message;
    gboolean can_shutdown;

    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-shutdown", &can_shutdown,
                      NULL);
    }
    else
    {
        g_object_get (G_OBJECT (power->priv->console),
                      "can-shutdown", &can_shutdown,
                      NULL);
    }

    message = _("System is running on low power. "\
               "Save your work to avoid losing data");

    dialog = gtk_dialog_new_with_buttons (_("Power Manager"), NULL, GTK_DIALOG_MODAL,
                                          NULL);

    gtk_dialog_set_default_response (GTK_DIALOG (dialog),
                                     GTK_RESPONSE_CANCEL);

    content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

    gtk_box_pack_start (GTK_BOX (content_area), gtk_label_new (message),
		        TRUE, TRUE, 8);

    if ( power->priv->can_hibernate && power->priv->auth_hibernate )
    {
	GtkWidget *hibernate;
	hibernate = gtk_button_new_with_label (_("Hibernate"));
	gtk_dialog_add_action_widget (GTK_DIALOG (dialog), hibernate, GTK_RESPONSE_NONE);

	g_signal_connect_swapped (hibernate, "clicked",
			          G_CALLBACK (blpm_power_hibernate_clicked), power);
    }

    if ( power->priv->can_suspend && power->priv->auth_suspend )
    {
	GtkWidget *suspend;

	suspend = gtk_button_new_with_label (_("Suspend"));
	gtk_dialog_add_action_widget (GTK_DIALOG (dialog), suspend, GTK_RESPONSE_NONE);

	g_signal_connect_swapped (suspend, "clicked",
			          G_CALLBACK (blpm_power_suspend_clicked), power);
    }

    if ( can_shutdown )
    {
	GtkWidget *shutdown;

	shutdown = gtk_button_new_with_label (_("Shutdown"));
	gtk_dialog_add_action_widget (GTK_DIALOG (dialog), shutdown, GTK_RESPONSE_NONE);

	g_signal_connect_swapped (shutdown, "clicked",
			          G_CALLBACK (blpm_power_shutdown_clicked), power);
    }

    cancel = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
    gtk_dialog_add_action_widget (GTK_DIALOG (dialog), cancel, GTK_RESPONSE_NONE);

    g_signal_connect_swapped (cancel, "clicked",
			      G_CALLBACK (blpm_power_close_critical_dialog), power);

    g_signal_connect_swapped (dialog, "destroy",
			      G_CALLBACK (blpm_power_close_critical_dialog), power);
    if ( power->priv->dialog )
    {
	gtk_widget_destroy (power->priv->dialog);
	power->priv->dialog = NULL;

    }
    power->priv->dialog = dialog;
    gtk_widget_show_all (dialog);
}

static void
blpm_power_show_critical_action (XfpmPower *power, XfpmBattery *battery)
{
    gboolean supports_actions;

    g_object_get (G_OBJECT (power->priv->notify),
		  "actions", &supports_actions,
		  NULL);

    if ( supports_actions )
	blpm_power_show_critical_action_notification (power, battery);
    else
	blpm_power_show_critical_action_gtk (power);
}

static void
blpm_power_process_critical_action (XfpmPower *power, XfpmShutdownRequest req)
{
    if ( req == XFPM_ASK )
	g_signal_emit (G_OBJECT (power), signals [ASK_SHUTDOWN], 0);
    else if ( req == XFPM_DO_SUSPEND )
	blpm_power_sleep (power, "Suspend", TRUE);
    else if ( req == XFPM_DO_HIBERNATE )
	blpm_power_sleep (power, "Hibernate", TRUE);
    else if ( req == XFPM_DO_SHUTDOWN )
	g_signal_emit (G_OBJECT (power), signals [SHUTDOWN], 0);
}

static void
blpm_power_system_on_critical_power (XfpmPower *power, XfpmBattery *battery)
{
    XfpmShutdownRequest critical_action;

    g_object_get (G_OBJECT (power->priv->conf),
	          CRITICAL_BATT_ACTION_CFG, &critical_action,
		  NULL);

    XFPM_DEBUG ("System is running on low power");
    XFPM_DEBUG_ENUM (critical_action, XFPM_TYPE_SHUTDOWN_REQUEST, "Critical battery action");

    if ( critical_action == XFPM_DO_NOTHING )
    {
	blpm_power_show_critical_action (power, battery);
    }
    else
    {
	if (power->priv->critical_action_done == FALSE)
	{
	    power->priv->critical_action_done = TRUE;
	    blpm_power_process_critical_action (power, critical_action);
	}
	else
	{
	    blpm_power_show_critical_action (power, battery);
	}
    }
}

static void
blpm_power_battery_charge_changed_cb (XfpmBattery *battery, XfpmPower *power)
{
    gboolean notify;
    XfpmBatteryCharge battery_charge;
    XfpmBatteryCharge current_charge;

    battery_charge = blpm_battery_get_charge (battery);
    current_charge = blpm_power_get_current_charge_state (power);

    XFPM_DEBUG_ENUM (current_charge, XFPM_TYPE_BATTERY_CHARGE, "Current system charge status");

    if (current_charge == power->priv->overall_state)
	return;

    if (current_charge >= XFPM_BATTERY_CHARGE_LOW)
	power->priv->critical_action_done = FALSE;

    power->priv->overall_state = current_charge;

    if ( current_charge == XFPM_BATTERY_CHARGE_CRITICAL && power->priv->on_battery)
    {
	blpm_power_system_on_critical_power (power, battery);

	power->priv->on_low_battery = TRUE;
	g_signal_emit (G_OBJECT (power), signals [LOW_BATTERY_CHANGED], 0, power->priv->on_low_battery);
	return;
    }

    if ( power->priv->on_low_battery )
    {
	power->priv->on_low_battery = FALSE;
	g_signal_emit (G_OBJECT (power), signals [LOW_BATTERY_CHANGED], 0, power->priv->on_low_battery);
    }

    g_object_get (G_OBJECT (power->priv->conf),
		  GENERAL_NOTIFICATION_CFG, &notify,
		  NULL);

    if ( power->priv->on_battery )
    {
	if ( current_charge == XFPM_BATTERY_CHARGE_LOW )
	{
	    if ( notify )
		blpm_notify_show_notification (power->priv->notify,
					       _("Power Manager"),
					       _("System is running on low power"),
					       gtk_status_icon_get_icon_name (GTK_STATUS_ICON (battery)),
					       10000,
					       FALSE,
					       XFPM_NOTIFY_NORMAL,
					       GTK_STATUS_ICON (battery));

	}
	else if ( battery_charge == XFPM_BATTERY_CHARGE_LOW )
	{
	    if ( notify )
	    {
		gchar *msg;
		gchar *time_str;

		const gchar *battery_name = blpm_battery_get_battery_name (battery);

		time_str = blpm_battery_get_time_left (battery);

		msg = g_strdup_printf (_("Your %s charge level is low\nEstimated time left %s"), battery_name, time_str);


		blpm_notify_show_notification (power->priv->notify,
					       _("Power Manager"),
					       msg,
					       gtk_status_icon_get_icon_name (GTK_STATUS_ICON (battery)),
					       10000,
					       FALSE,
					       XFPM_NOTIFY_NORMAL,
					       GTK_STATUS_ICON (battery));
		g_free (msg);
		g_free (time_str);
	    }
	}
    }

    /*Current charge is okay now, then close the dialog*/
    if ( power->priv->dialog )
    {
	gtk_widget_destroy (power->priv->dialog);
	power->priv->dialog = NULL;
    }
}

static void
blpm_power_add_device (UpDevice *device, XfpmPower *power)
{
    guint device_type = UP_DEVICE_KIND_UNKNOWN;
    const gchar *object_path = up_device_get_object_path(device);

    /* hack, this depends on XFPM_DEVICE_TYPE_* being in sync with UP_DEVICE_KIND_* */
    g_object_get (device,
		  "kind", &device_type,
		  NULL);

    XFPM_DEBUG ("'%s' device added", up_device_kind_to_string(device_type));

    if ( device_type == UP_DEVICE_KIND_BATTERY	||
	 device_type == UP_DEVICE_KIND_UPS	||
	 device_type == UP_DEVICE_KIND_MOUSE	||
	 device_type == UP_DEVICE_KIND_KEYBOARD	||
	 device_type == UP_DEVICE_KIND_PHONE)
    {
	GtkStatusIcon *battery;
	XFPM_DEBUG( "Battery device type '%s' detected at: %s",
		     up_device_kind_to_string(device_type), object_path);
	battery = blpm_battery_new ();
	gtk_status_icon_set_visible (battery, FALSE);
	blpm_battery_monitor_device (XFPM_BATTERY (battery),
				     object_path,
				     device_type);
	g_hash_table_insert (power->priv->hash, g_strdup (object_path), battery);

	g_signal_connect (battery, "battery-charge-changed",
			  G_CALLBACK (blpm_power_battery_charge_changed_cb), power);

    }
}

static void
blpm_power_get_power_devices (XfpmPower *power)
{
#if !UP_CHECK_VERSION(0, 99, 0)
    /* the device-add callback is called for each device */
    up_client_enumerate_devices_sync(power->priv->upower, NULL, NULL);
#else
    GPtrArray *array = NULL;
    guint i;

    array = up_client_get_devices(power->priv->upower);

    if ( array )
    {
	for ( i = 0; i < array->len; i++)
	{
	    UpDevice *device = g_ptr_array_index (array, i);
	    const gchar *object_path = up_device_get_object_path(device);
	    XFPM_DEBUG ("Power device detected at : %s", object_path);
	    blpm_power_add_device (device, power);
	}
	g_ptr_array_free (array, TRUE);
    }
#endif
}

static void
blpm_power_remove_device (XfpmPower *power, const gchar *object_path)
{
    g_hash_table_remove (power->priv->hash, object_path);
}

static void
blpm_power_inhibit_changed_cb (XfpmInhibit *inhibit, gboolean is_inhibit, XfpmPower *power)
{
    power->priv->inhibited = is_inhibit;
}

static void
blpm_power_changed_cb (UpClient *upower,
#if UP_CHECK_VERSION(0, 99, 0)
		       GParamSpec *pspec,
#endif
		       XfpmPower *power)
{
    blpm_power_get_properties (power);
}

static void
blpm_power_device_added_cb (UpClient *upower, UpDevice *device, XfpmPower *power)
{
    blpm_power_add_device (device, power);
}

#if UP_CHECK_VERSION(0, 99, 0)
static void
blpm_power_device_removed_cb (UpClient *upower, const gchar *object_path, XfpmPower *power)
{
    blpm_power_remove_device (power, object_path);
}
#else
static void
blpm_power_device_removed_cb (UpClient *upower, UpDevice *device, XfpmPower *power)
{
    const gchar *object_path = up_device_get_object_path(device);
    blpm_power_remove_device (power, object_path);
}
#endif

#ifdef ENABLE_POLKIT
static void
blpm_power_polkit_auth_changed_cb (XfpmPower *power)
{
    XFPM_DEBUG ("Auth configuration changed");
    blpm_power_check_polkit_auth (power);
}
#endif

static void
blpm_power_class_init (XfpmPowerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = blpm_power_finalize;

    object_class->get_property = blpm_power_get_property;
    object_class->set_property = blpm_power_set_property;

    signals [ON_BATTERY_CHANGED] =
        g_signal_new ("on-battery-changed",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, on_battery_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOOLEAN,
                      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals [LOW_BATTERY_CHANGED] =
        g_signal_new ("low-battery-changed",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, low_battery_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOOLEAN,
                      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals [LID_CHANGED] =
        g_signal_new ("lid-changed",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, lid_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOOLEAN,
                      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals [WAKING_UP] =
        g_signal_new ("waking-up",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, waking_up),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0, G_TYPE_NONE);

    signals [SLEEPING] =
        g_signal_new ("sleeping",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, sleeping),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0, G_TYPE_NONE);

    signals [ASK_SHUTDOWN] =
        g_signal_new ("ask-shutdown",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, ask_shutdown),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0, G_TYPE_NONE);

    signals [SHUTDOWN] =
        g_signal_new ("shutdown",
                      XFPM_TYPE_POWER,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmPowerClass, shutdown),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0, G_TYPE_NONE);

#define XFPM_PARAM_FLAGS  (  G_PARAM_READWRITE \
                           | G_PARAM_CONSTRUCT \
                           | G_PARAM_STATIC_NAME \
                           | G_PARAM_STATIC_NICK \
                           | G_PARAM_STATIC_BLURB)

    g_object_class_install_property (object_class,
                                     PROP_ON_BATTERY,
                                     g_param_spec_boolean ("on-battery",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_ON_LOW_BATTERY,
                                     g_param_spec_boolean ("on-low-battery",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTH_SUSPEND,
                                     g_param_spec_boolean ("auth-suspend",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTH_HIBERNATE,
                                     g_param_spec_boolean ("auth-hibernate",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_CAN_HIBERNATE,
                                     g_param_spec_boolean ("can-hibernate",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_CAN_SUSPEND,
                                     g_param_spec_boolean ("can-suspend",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_HAS_LID,
                                     g_param_spec_boolean ("has-lid",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_PRESENTATION_MODE,
                                     g_param_spec_boolean (PRESENTATION_MODE,
                                                          NULL, NULL,
                                                          FALSE,
                                                          XFPM_PARAM_FLAGS));

    g_object_class_install_property (object_class,
                                     PROP_ON_AC_BLANK,
                                     g_param_spec_int  (ON_AC_BLANK,
                                                        NULL, NULL,
                                                        0,
                                                        G_MAXINT16,
                                                        10,
                                                        XFPM_PARAM_FLAGS));

    g_object_class_install_property (object_class,
                                     PROP_ON_BATTERY_BLANK,
                                     g_param_spec_int  (ON_BATTERY_BLANK,
                                                        NULL, NULL,
                                                        0,
                                                        G_MAXINT16,
                                                        10,
                                                        XFPM_PARAM_FLAGS));
#undef XFPM_PARAM_FLAGS

    g_type_class_add_private (klass, sizeof (XfpmPowerPrivate));

    blpm_power_dbus_class_init (klass);
}

static void
blpm_power_init (XfpmPower *power)
{
    GError *error = NULL;

    power->priv = XFPM_POWER_GET_PRIVATE (power);

    power->priv->hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
    power->priv->lid_is_present  = FALSE;
    power->priv->lid_is_closed   = FALSE;
    power->priv->on_battery      = FALSE;
    power->priv->on_low_battery  = FALSE;
    power->priv->daemon_version  = NULL;
    power->priv->can_suspend     = FALSE;
    power->priv->can_hibernate   = FALSE;
    power->priv->auth_hibernate  = TRUE;
    power->priv->auth_suspend    = TRUE;
    power->priv->dialog          = NULL;
    power->priv->overall_state   = XFPM_BATTERY_CHARGE_OK;
    power->priv->critical_action_done = FALSE;

    power->priv->dpms                 = blpm_dpms_new ();

    power->priv->presentation_mode    = FALSE;
    power->priv->on_ac_blank          = 15;
    power->priv->on_battery_blank     = 10;

    power->priv->inhibit = blpm_inhibit_new ();
    power->priv->notify  = blpm_notify_new ();
    power->priv->conf    = blpm_blconf_new ();
    power->priv->upower  = up_client_new ();

    power->priv->systemd = NULL;
    power->priv->console = NULL;
    if ( LOGIND_RUNNING () )
        power->priv->systemd = blpm_systemd_new ();
    else
	power->priv->console = blpm_console_kit_new ();

#ifdef ENABLE_POLKIT
    power->priv->polkit  = blpm_polkit_get ();
    g_signal_connect_swapped (power->priv->polkit, "auth-changed",
			      G_CALLBACK (blpm_power_polkit_auth_changed_cb), power);
#endif

    g_signal_connect (power->priv->inhibit, "has-inhibit-changed",
		      G_CALLBACK (blpm_power_inhibit_changed_cb), power);

    power->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);

    if ( error )
    {
	g_critical ("Unable to connect to the system bus : %s", error->message);
	g_error_free (error);
	goto out;
    }

    g_signal_connect (power->priv->upower, "device-added", G_CALLBACK (blpm_power_device_added_cb), power);
    g_signal_connect (power->priv->upower, "device-removed", G_CALLBACK (blpm_power_device_removed_cb), power);
#if UP_CHECK_VERSION(0, 99, 0)
    g_signal_connect (power->priv->upower, "notify", G_CALLBACK (blpm_power_changed_cb), power);
#else
    g_signal_connect (power->priv->upower, "changed", G_CALLBACK (blpm_power_changed_cb), power);
#endif
    blpm_power_get_power_devices (power);
    blpm_power_get_properties (power);
#ifdef ENABLE_POLKIT
    blpm_power_check_polkit_auth (power);
#endif

out:
    blpm_power_dbus_init (power);

    /*
     * Emit org.freedesktop.PowerManagement session signals on startup
     */
    g_signal_emit (G_OBJECT (power), signals [ON_BATTERY_CHANGED], 0, power->priv->on_battery);
}

static void blpm_power_get_property (GObject *object,
				   guint prop_id,
				   GValue *value,
				   GParamSpec *pspec)
{
    XfpmPower *power;
    power = XFPM_POWER (object);

    switch (prop_id)
    {
    case PROP_ON_BATTERY:
        g_value_set_boolean (value, power->priv->on_battery);
        break;
    case PROP_AUTH_HIBERNATE:
        g_value_set_boolean (value, power->priv->auth_hibernate);
        break;
    case PROP_AUTH_SUSPEND:
        g_value_set_boolean (value, power->priv->auth_suspend);
        break;
    case PROP_CAN_SUSPEND:
        g_value_set_boolean (value, power->priv->can_suspend);
        break;
    case PROP_CAN_HIBERNATE:
        g_value_set_boolean (value, power->priv->can_hibernate);
        break;
    case PROP_HAS_LID:
        g_value_set_boolean (value, power->priv->lid_is_present);
        break;
    case PROP_PRESENTATION_MODE:
        g_value_set_boolean (value, power->priv->presentation_mode);
        break;
    case PROP_ON_AC_BLANK:
        g_value_set_int (value, power->priv->on_ac_blank);
        break;
    case PROP_ON_BATTERY_BLANK:
        g_value_set_int (value, power->priv->on_battery_blank);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void blpm_power_set_property (GObject *object,
				     guint prop_id,
				     const GValue *value,
				     GParamSpec *pspec)
{
    XfpmPower *power = XFPM_POWER (object);
    gint on_ac_blank, on_battery_blank;

    switch (prop_id)
    {
    case PROP_PRESENTATION_MODE:
        blpm_power_change_presentation_mode (power, g_value_get_boolean (value));
        break;
    case PROP_ON_AC_BLANK:
        on_ac_blank = g_value_get_int (value);
        power->priv->on_ac_blank = on_ac_blank;
        blpm_update_blank_time (power);
        break;
    case PROP_ON_BATTERY_BLANK:
        on_battery_blank = g_value_get_int (value);
        power->priv->on_battery_blank = on_battery_blank;
        blpm_update_blank_time (power);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
blpm_power_finalize (GObject *object)
{
    XfpmPower *power;

    power = XFPM_POWER (object);

    g_free (power->priv->daemon_version);

    g_object_unref (power->priv->inhibit);
    g_object_unref (power->priv->notify);
    g_object_unref (power->priv->conf);

    if ( power->priv->systemd != NULL )
        g_object_unref (power->priv->systemd);
    if ( power->priv->console != NULL )
        g_object_unref (power->priv->console);

    dbus_g_connection_unref (power->priv->bus);

    g_hash_table_destroy (power->priv->hash);

#ifdef ENABLE_POLKIT
    g_object_unref (power->priv->polkit);
#endif

    g_object_unref(power->priv->dpms);

    G_OBJECT_CLASS (blpm_power_parent_class)->finalize (object);
}

static XfpmPower*
blpm_power_new (void)
{
    XfpmPower *power = XFPM_POWER(g_object_new (XFPM_TYPE_POWER, NULL));

    blconf_g_property_bind (blpm_blconf_get_channel(power->priv->conf),
                            PROPERTIES_PREFIX PRESENTATION_MODE, G_TYPE_BOOLEAN,
                            G_OBJECT(power), PRESENTATION_MODE);

    blconf_g_property_bind (blpm_blconf_get_channel(power->priv->conf),
                            PROPERTIES_PREFIX ON_BATTERY_BLANK, G_TYPE_INT,
                            G_OBJECT (power), ON_BATTERY_BLANK);

    blconf_g_property_bind (blpm_blconf_get_channel(power->priv->conf),
                            PROPERTIES_PREFIX ON_AC_BLANK, G_TYPE_INT,
                            G_OBJECT (power), ON_AC_BLANK);

    return power;
}

XfpmPower *
blpm_power_get (void)
{
    static gpointer blpm_power_object = NULL;

    if ( G_LIKELY (blpm_power_object != NULL ) )
    {
        g_object_ref (blpm_power_object);
    }
    else
    {
        blpm_power_object = blpm_power_new ();
        g_object_add_weak_pointer (blpm_power_object, &blpm_power_object);
    }

    return XFPM_POWER (blpm_power_object);
}

void blpm_power_suspend (XfpmPower *power, gboolean force)
{
    blpm_power_sleep (power, "Suspend", force);
}

void blpm_power_hibernate (XfpmPower *power, gboolean force)
{
    blpm_power_sleep (power, "Hibernate", force);
}

gboolean blpm_power_has_battery (XfpmPower *power)
{
    GtkStatusIcon *battery = NULL;
    guint i, len;
    GList *list;

    gboolean ret = FALSE;

    list = g_hash_table_get_values (power->priv->hash);
    len = g_list_length (list);

    for ( i = 0; i < len; i++)
    {
	UpDeviceKind type;
	battery = g_list_nth_data (list, i);
	type = blpm_battery_get_device_type (XFPM_BATTERY (battery));
	if ( type == UP_DEVICE_KIND_BATTERY ||
	     type == UP_DEVICE_KIND_UPS )
	{
	    ret = TRUE;
	    break;
	}
    }

    return ret;
}

static void
blpm_update_blank_time (XfpmPower *power)
{
    Display* display = gdk_x11_display_get_xdisplay(gdk_display_get_default ());
    guint screensaver_timeout;

    if (power->priv->on_battery)
        screensaver_timeout = power->priv->on_battery_blank;
    else
        screensaver_timeout = power->priv->on_ac_blank;

    /* Presentation mode disables blanking */
    if (power->priv->presentation_mode)
        screensaver_timeout = 0;

    XFPM_DEBUG ("Timeout: %d", screensaver_timeout);

    XSetScreenSaver(display, screensaver_timeout * 60, 0, DefaultBlanking, DefaultExposures);
}

static void
blpm_power_change_presentation_mode (XfpmPower *power, gboolean presentation_mode)
{
    /* no change, exit */
    if (power->priv->presentation_mode == presentation_mode)
        return;

    XFPM_DEBUG ("presentation mode %s, changing to %s",
                power->priv->presentation_mode ? "TRUE" : "FALSE",
                presentation_mode ? "TRUE" : "FALSE");

    power->priv->presentation_mode = presentation_mode;

    /* presentation mode inhibits dpms */
    blpm_dpms_inhibit (power->priv->dpms, presentation_mode);

    if (presentation_mode == FALSE)
    {
        EggIdletime *idletime;
        idletime = egg_idletime_new ();
        egg_idletime_alarm_reset_all (idletime);

        g_object_unref (idletime);
    }

    blpm_update_blank_time (power);
}

gboolean
blpm_power_is_in_presentation_mode (XfpmPower *power)
{
    g_return_val_if_fail (XFPM_IS_POWER (power), FALSE);

    return power->priv->presentation_mode;
}


/*
 *
 * DBus server implementation for org.freedesktop.PowerManagement
 *
 */
static gboolean blpm_power_dbus_shutdown (XfpmPower *power,
				        GError **error);

static gboolean blpm_power_dbus_reboot   (XfpmPower *power,
					GError **error);

static gboolean blpm_power_dbus_hibernate (XfpmPower * power,
					 GError **error);

static gboolean blpm_power_dbus_suspend (XfpmPower * power,
				       GError ** error);

static gboolean blpm_power_dbus_can_reboot (XfpmPower * power,
					  gboolean * OUT_can_reboot,
					  GError ** error);

static gboolean blpm_power_dbus_can_shutdown (XfpmPower * power,
					    gboolean * OUT_can_reboot,
					    GError ** error);

static gboolean blpm_power_dbus_can_hibernate (XfpmPower * power,
					     gboolean * OUT_can_hibernate,
					     GError ** error);

static gboolean blpm_power_dbus_can_suspend (XfpmPower * power,
					   gboolean * OUT_can_suspend,
					   GError ** error);

static gboolean blpm_power_dbus_get_on_battery (XfpmPower * power,
					      gboolean * OUT_on_battery,
					      GError ** error);

static gboolean blpm_power_dbus_get_low_battery (XfpmPower * power,
					       gboolean * OUT_low_battery,
					       GError ** error);

#include "org.freedesktop.PowerManagement.h"

static void
blpm_power_dbus_class_init (XfpmPowerClass * klass)
{
    dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass),
                                     &dbus_glib_blpm_power_object_info);
}

static void
blpm_power_dbus_init (XfpmPower *power)
{
    DBusGConnection *bus = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);

    TRACE ("entering");

    dbus_g_connection_register_g_object (bus,
                                         "/org/freedesktop/PowerManagement",
                                         G_OBJECT (power));
}

static gboolean blpm_power_dbus_shutdown (XfpmPower *power,
				        GError **error)
{
    gboolean can_reboot;

    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-shutdown", &can_reboot,
                      NULL);
    }
    else
    {
        g_object_get (G_OBJECT (power->priv->console),
                      "can-shutdown", &can_reboot,
                      NULL);
    }

    if ( !can_reboot)
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_PERMISSION_DENIED,
                    _("Permission denied"));
        return FALSE;
    }

    if ( LOGIND_RUNNING () )
        blpm_systemd_shutdown (power->priv->systemd, error);
    else
	blpm_console_kit_shutdown (power->priv->console, error);

    return TRUE;
}

static gboolean blpm_power_dbus_reboot   (XfpmPower *power,
					GError **error)
{
    gboolean can_reboot;

    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-restart", &can_reboot,
                      NULL);
    }
    else
    {
        g_object_get (G_OBJECT (power->priv->console),
                      "can-restart", &can_reboot,
                      NULL);
    }

    if ( !can_reboot)
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_PERMISSION_DENIED,
                    _("Permission denied"));
        return FALSE;
    }

   if ( LOGIND_RUNNING () )
        blpm_systemd_reboot (power->priv->systemd, error);
    else
	blpm_console_kit_reboot (power->priv->console, error);


    return TRUE;
}

static gboolean blpm_power_dbus_hibernate (XfpmPower * power,
					 GError **error)
{
    if ( !power->priv->auth_hibernate )
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_PERMISSION_DENIED,
                    _("Permission denied"));
        return FALSE;

    }

    if (!power->priv->can_hibernate )
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_NO_HARDWARE_SUPPORT,
                    _("Suspend not supported"));
        return FALSE;
    }

    blpm_power_sleep (power, "Hibernate", FALSE);

    return TRUE;
}

static gboolean blpm_power_dbus_suspend (XfpmPower * power,
				       GError ** error)
{
    if ( !power->priv->auth_suspend )
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_PERMISSION_DENIED,
                    _("Permission denied"));
        return FALSE;

    }

    if (!power->priv->can_suspend )
    {
	g_set_error (error, XFPM_ERROR, XFPM_ERROR_NO_HARDWARE_SUPPORT,
                    _("Suspend not supported"));
        return FALSE;
    }

    blpm_power_sleep (power, "Suspend", FALSE);

    return TRUE;
}

static gboolean blpm_power_dbus_can_reboot (XfpmPower * power,
					  gboolean * OUT_can_reboot,
					  GError ** error)
{

    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                      "can-reboot", OUT_can_reboot,
                      NULL);
    }
    else
    {
        g_object_get (G_OBJECT (power->priv->console),
                      "can-reboot", OUT_can_reboot,
		      NULL);
    }

    return TRUE;
}

static gboolean blpm_power_dbus_can_shutdown (XfpmPower * power,
					    gboolean * OUT_can_shutdown,
					    GError ** error)
{

    if ( LOGIND_RUNNING () )
    {
        g_object_get (G_OBJECT (power->priv->systemd),
                  "can-shutdown", OUT_can_shutdown,
                  NULL);
    }
    else
    {
	g_object_get (G_OBJECT (power->priv->console),
		      "can-shutdown", OUT_can_shutdown,
		      NULL);
    }

    return TRUE;
}

static gboolean blpm_power_dbus_can_hibernate (XfpmPower * power,
					     gboolean * OUT_can_hibernate,
					     GError ** error)
{
    *OUT_can_hibernate = power->priv->can_hibernate;
    return TRUE;
}

static gboolean blpm_power_dbus_can_suspend (XfpmPower * power,
					   gboolean * OUT_can_suspend,
					   GError ** error)
{
    *OUT_can_suspend = power->priv->can_suspend;

    return TRUE;
}

static gboolean blpm_power_dbus_get_on_battery (XfpmPower * power,
					      gboolean * OUT_on_battery,
					      GError ** error)
{
    *OUT_on_battery = power->priv->on_battery;

    return TRUE;
}

static gboolean blpm_power_dbus_get_low_battery (XfpmPower * power,
					       gboolean * OUT_low_battery,
					       GError ** error)
{
    *OUT_low_battery = power->priv->on_low_battery;

    return TRUE;
}
