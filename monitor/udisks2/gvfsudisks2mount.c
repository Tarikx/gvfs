/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gvfsmountinfo.h>

#include <gudev/gudev.h>

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2utils.h"

#define BUSY_UNMOUNT_NUM_ATTEMPTS              5
#define BUSY_UNMOUNT_MS_DELAY_BETWEEN_ATTEMPTS 100

typedef struct _GVfsUDisks2MountClass GVfsUDisks2MountClass;
struct _GVfsUDisks2MountClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Mount
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */

  /* may be NULL */
  GVfsUDisks2Volume        *volume;  /* owned by volume monitor */

  /* may be NULL */
  GUnixMountEntry *mount_entry;

  /* the following members are set in update_mount() */
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *sort_key;
  gchar *uuid;
  gchar *device_file;
  gchar *mount_path;
  gboolean can_unmount;
  gchar *mount_entry_name;
  gchar *mount_entry_fs_type;

  gboolean is_burn_mount;

  GIcon *autorun_icon;
  gboolean searched_for_autorun;

  gchar *xdg_volume_info_name;
  GIcon *xdg_volume_info_icon;
  gboolean searched_for_xdg_volume_info;

  gchar *bdmv_volume_info_name;
  GIcon *bdmv_volume_info_icon;
  gboolean searched_for_bdmv_volume_info;
};

static gboolean update_mount (GVfsUDisks2Mount *mount);

static void gvfs_udisks2_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Mount, gvfs_udisks2_mount, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
                                               gvfs_udisks2_mount_mount_iface_init))

static void on_volume_changed (GVolume *volume, gpointer user_data);

static void
gvfs_udisks2_mount_finalize (GObject *object)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (object);

  if (mount->volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
    }

  if (mount->root != NULL)
    g_object_unref (mount->root);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);
  g_free (mount->name);
  g_free (mount->sort_key);
  g_free (mount->uuid);
  g_free (mount->device_file);
  g_free (mount->mount_path);

  g_free (mount->mount_entry_name);

  if (mount->autorun_icon != NULL)
    g_object_unref (mount->autorun_icon);

  g_free (mount->xdg_volume_info_name);
  if (mount->xdg_volume_info_icon != NULL)
    g_object_unref (mount->xdg_volume_info_icon);

  G_OBJECT_CLASS (gvfs_udisks2_mount_parent_class)->finalize (object);
}

static void
gvfs_udisks2_mount_class_init (GVfsUDisks2MountClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_mount_finalize;
}

static void
gvfs_udisks2_mount_init (GVfsUDisks2Mount *mount)
{
}

static void
emit_changed (GVfsUDisks2Mount *mount)
{
  g_signal_emit_by_name (mount, "changed");
  g_signal_emit_by_name (mount->monitor, "mount-changed", mount);
}

static void
got_autorun_info_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->autorun_icon = g_vfs_mount_info_query_autorun_info_finish (G_FILE (source_object), res, NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_xdg_volume_info_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->xdg_volume_info_icon = g_vfs_mount_info_query_xdg_volume_info_finish (G_FILE (source_object),
                                                                               res,
                                                                               &(mount->xdg_volume_info_name),
                                                                               NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_bdmv_volume_info_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->bdmv_volume_info_icon = g_vfs_mount_info_query_bdmv_volume_info_finish (G_FILE (source_object),
                                                                                 res,
                                                                                 &(mount->bdmv_volume_info_name),
                                                                                 NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static gboolean
update_mount (GVfsUDisks2Mount *mount)
{
  gboolean changed;
  gboolean old_can_unmount;
  gchar *old_name;
  GIcon *old_icon;

  /* save old values */
  old_can_unmount = mount->can_unmount;
  old_name = g_strdup (mount->name);
  old_icon = mount->icon != NULL ? g_object_ref (mount->icon) : NULL;

  /* reset */
  mount->can_unmount = FALSE;
  g_clear_object (&mount->icon);
  g_free (mount->name); mount->name = NULL;

  /* in with the new */
  if (mount->volume != NULL)
    {
      mount->can_unmount = TRUE;

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = g_volume_get_icon (G_VOLUME (mount->volume));

      /* name order of preference : bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_volume_get_name (G_VOLUME (mount->volume));
    }
  else
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        {
          mount->icon = gvfs_udisks2_utils_icon_from_fs_type (g_unix_mount_get_fs_type (mount->mount_entry));
        }

      g_free (mount->name);

      /* name order of preference: bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_strdup (mount->mount_entry_name);
    }

  /* compute whether something changed */
  changed = !((old_can_unmount == mount->can_unmount) &&
              (g_strcmp0 (old_name, mount->name) == 0) &&
              g_icon_equal (old_icon, mount->icon));

  /* free old values */
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_mount(), changed=%d", changed);*/

  /* search for BDMV */
  if (!mount->searched_for_bdmv_volume_info)
    {
      mount->searched_for_bdmv_volume_info = TRUE;
      g_vfs_mount_info_query_bdmv_volume_info (mount->root,
      					       NULL,
      					       got_bdmv_volume_info_cb,
      					       g_object_ref (mount));
    }

  /* search for .xdg-volume-info */
  if (!mount->searched_for_xdg_volume_info)
    {
      mount->searched_for_xdg_volume_info = TRUE;
      g_vfs_mount_info_query_xdg_volume_info (mount->root,
                                              NULL,
                                              got_xdg_volume_info_cb,
                                              g_object_ref (mount));
    }

  /* search for autorun.inf */
  if (!mount->searched_for_autorun)
    {
      mount->searched_for_autorun = TRUE;
      g_vfs_mount_info_query_autorun_info (mount->root,
                                           NULL,
                                           got_autorun_info_cb,
                                           g_object_ref (mount));
    }

  return changed;
}

static void
on_volume_changed (GVolume  *volume,
                   gpointer  user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  if (update_mount (mount))
    emit_changed (mount);
}

GVfsUDisks2Mount *
gvfs_udisks2_mount_new (GVfsUDisks2VolumeMonitor *monitor,
                        GUnixMountEntry          *mount_entry, /* takes ownership */
                        GVfsUDisks2Volume        *volume)
{
  GVfsUDisks2Mount *mount = NULL;

  /* Ignore internal mounts unless there's a volume */
  if (volume == NULL && (mount_entry != NULL && !g_unix_mount_guess_should_display (mount_entry)))
    goto out;

  mount = g_object_new (GVFS_TYPE_UDISKS2_MOUNT, NULL);
  mount->monitor = monitor;
  mount->sort_key = g_strdup_printf ("gvfs.time_detected_usec.%" G_GINT64_FORMAT, g_get_real_time ());

  if (mount_entry != NULL)
    {
      mount->mount_entry = mount_entry; /* takes ownership */
      mount->mount_entry_name = g_unix_mount_guess_name (mount_entry);
      mount->device_file = g_strdup (g_unix_mount_get_device_path (mount_entry));
      mount->mount_path = g_strdup (g_unix_mount_get_mount_path (mount_entry));
      mount->root = g_file_new_for_path (mount->mount_path);
    }
  else
    {
      /* burn:/// mount (the only mounts we support with mount_entry == NULL) */
      mount->device_file = NULL;
      mount->mount_path = NULL;
      mount->root = g_file_new_for_uri ("burn:///");
      mount->is_burn_mount = TRUE;
    }

  /* need to set the volume only when the mount is fully constructed */
  mount->volume = volume;
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_set_mount (volume, mount);
      /* this is for piggy backing on the name and icon of the associated volume */
      g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
    }

  update_mount (mount);

 out:

  return mount;
}

void
gvfs_udisks2_mount_unmounted (GVfsUDisks2Mount *mount)
{
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

void
gvfs_udisks2_mount_unset_volume (GVfsUDisks2Mount   *mount,
                                 GVfsUDisks2Volume  *volume)
{
  if (mount->volume == volume)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

void
gvfs_udisks2_mount_set_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  if (mount->volume != volume)
    {
      if (mount->volume != NULL)
        gvfs_udisks2_mount_unset_volume (mount, mount->volume);
      mount->volume = volume;
      if (mount->volume != NULL)
        {
          gvfs_udisks2_volume_set_mount (volume, mount);
          /* this is for piggy backing on the name and icon of the associated volume */
          g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
        }
      update_mount (mount);
      emit_changed (mount);
    }
}

static GFile *
gvfs_udisks2_mount_get_root (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->root != NULL ? g_object_ref (mount->root) : NULL;
}

static GIcon *
gvfs_udisks2_mount_get_icon (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->icon != NULL ? g_object_ref (mount->icon) : NULL;
}

static gchar *
gvfs_udisks2_mount_get_uuid (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->uuid);
}

static gchar *
gvfs_udisks2_mount_get_name (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->name);
}

gboolean
gvfs_udisks2_mount_has_uuid (GVfsUDisks2Mount *_mount,
                             const gchar      *uuid)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strcmp0 (mount->uuid, uuid) == 0;
}

const gchar *
gvfs_udisks2_mount_get_mount_path (GVfsUDisks2Mount *mount)
{
  return mount->mount_path;
}

GUnixMountEntry *
gvfs_udisks2_mount_get_mount_entry (GVfsUDisks2Mount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->mount_entry;
}

static GDrive *
gvfs_udisks2_mount_get_drive (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive = NULL;

  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));
  return drive;
}

static GVolume *
gvfs_udisks2_mount_get_volume_ (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GVolume *volume = NULL;

  if (mount->volume != NULL)
    volume = G_VOLUME (g_object_ref (mount->volume));
  return volume;
}

static gboolean
gvfs_udisks2_mount_can_unmount (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->can_unmount;
}

static gboolean
gvfs_udisks2_mount_can_eject (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive;
  gboolean can_eject;

  can_eject = FALSE;
  if (mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (mount->volume));
      if (drive != NULL)
        can_eject = g_drive_can_eject (drive);
    }

  return can_eject;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  volatile gint ref_count;
  GSimpleAsyncResult *simple;
  gboolean completed;

  GVfsUDisks2Mount *mount;

  UDisksEncrypted *encrypted;
  UDisksFilesystem *filesystem;

  GCancellable *cancellable;

  GMountOperation *mount_operation;
  GMountUnmountFlags flags;

  gulong mount_op_reply_handler_id;
  guint retry_unmount_timer_id;
} UnmountData;

static UnmountData *
unmount_data_ref (UnmountData *data)
{
  g_atomic_int_inc (&data->ref_count);
  return data;
}

static void
unmount_data_unref (UnmountData *data)
{
  if (g_atomic_int_dec_and_test (&data->ref_count))
    {
      g_object_unref (data->simple);

      if (data->mount_op_reply_handler_id > 0)
        {
          /* make the operation dialog go away */
          g_signal_emit_by_name (data->mount_operation, "aborted");
          g_signal_handler_disconnect (data->mount_operation, data->mount_op_reply_handler_id);
        }
      if (data->retry_unmount_timer_id > 0)
        {
          g_source_remove (data->retry_unmount_timer_id);
          data->retry_unmount_timer_id = 0;
        }

      g_clear_object (&data->mount);
      g_clear_object (&data->cancellable);
      g_clear_object (&data->mount_operation);
      g_clear_object (&data->encrypted);
      g_clear_object (&data->filesystem);
      g_free (data);
    }
}

static void unmount_do (UnmountData *data, gboolean force);

static gboolean
on_retry_timer_cb (gpointer user_data)
{
  UnmountData *data = user_data;

  if (data->retry_unmount_timer_id == 0)
    goto out;

  /* we're removing the timeout */
  data->retry_unmount_timer_id = 0;

  /* timeout expired => try again */
  unmount_do (data, FALSE);

 out:
  return FALSE; /* remove timeout */
}

static void
on_mount_op_reply (GMountOperation       *mount_operation,
                   GMountOperationResult result,
                   gpointer              user_data)
{
  UnmountData *data = user_data;
  gint choice;

  /* disconnect the signal handler */
  g_warn_if_fail (data->mount_op_reply_handler_id != 0);
  g_signal_handler_disconnect (data->mount_operation,
                               data->mount_op_reply_handler_id);
  data->mount_op_reply_handler_id = 0;

  choice = g_mount_operation_get_choice (mount_operation);

  if (result == G_MOUNT_OPERATION_ABORTED ||
      (result == G_MOUNT_OPERATION_HANDLED && choice == 1))
    {
      /* don't show an error dialog here */
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED_HANDLED,
                                       "GMountOperation aborted (user should never see this "
                                       "error since it is G_IO_ERROR_FAILED_HANDLED)");
      g_simple_async_result_complete_in_idle (data->simple);
      data->completed = TRUE;
      unmount_data_unref (data);
    }
  else if (result == G_MOUNT_OPERATION_HANDLED)
    {
      /* user chose force unmount => try again with force_unmount==TRUE */
      unmount_do (data, TRUE);
    }
  else
    {
      /* result == G_MOUNT_OPERATION_UNHANDLED => GMountOperation instance doesn't
       * support :show-processes signal
       */
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_BUSY,
                                       _("One or more programs are preventing the unmount operation."));
      g_simple_async_result_complete_in_idle (data->simple);
      data->completed = TRUE;
      unmount_data_unref (data);
    }
}

static void
lsof_command_cb (GObject       *source_object,
                 GAsyncResult  *res,
                 gpointer       user_data)
{
  UnmountData *data = user_data;
  GError *error;
  gint exit_status;
  GArray *processes;
  const gchar *choices[3] = {NULL, NULL, NULL};
  const gchar *message;
  gchar *standard_output = NULL;
  const gchar *p;

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));

  error = NULL;
  if (!gvfs_udisks2_utils_spawn_finish (res,
                                        &exit_status,
                                        &standard_output,
                                        NULL, /* gchar **out_standard_error */
                                        &error))
    {
      g_printerr ("Error launching lsof(1): %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }

  if (!(WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0))
    {
      g_printerr ("lsof(1) did not exit normally\n");
      goto out;
    }

  p = standard_output;
  while (TRUE)
    {
      GPid pid;
      gchar *endp;

      if (*p == '\0')
        break;

      pid = strtol (p, &endp, 10);
      if (pid == 0 && p == endp)
        break;

      g_array_append_val (processes, pid);

      p = endp;
    }

 out:
  if (!data->completed)
    {
      gboolean is_eject;

      is_eject = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (data->mount_operation), "x-udisks2-is-eject"));

      /* We want to emit the 'show-processes' signal even if launching
       * lsof(1) failed or if it didn't return any PIDs. This is because
       * it won't show e.g. root-owned processes operating on files
       * on the mount point.
       *
       * (unfortunately there's no way to convey that it failed)
       */
      if (data->mount_op_reply_handler_id == 0)
        {
          data->mount_op_reply_handler_id = g_signal_connect (data->mount_operation,
                                                              "reply",
                                                              G_CALLBACK (on_mount_op_reply),
                                                              data);
        }
      if (is_eject)
        {
          choices[0] = _("Eject Anyway");
        }
      else
        {
          choices[0] = _("Unmount Anyway");
        }
      choices[1] = _("Cancel");
      message = _("Volume is busy\n"
                  "One or more applications are keeping the volume busy.");
      g_signal_emit_by_name (data->mount_operation,
                             "show-processes",
                             message,
                             processes,
                             choices);
      /* set up a timer to try unmounting every two seconds - this will also
       * update the list of busy processes
       */
      if (data->retry_unmount_timer_id == 0)
        {
          data->retry_unmount_timer_id = g_timeout_add_seconds (2,
                                                                on_retry_timer_cb,
                                                                data);
        }
      g_array_free (processes, TRUE);
      g_free (standard_output);
    }
  unmount_data_unref (data); /* return ref */
}


static void
unmount_show_busy (UnmountData  *data,
                   const gchar  *mount_point)
{
  gchar *escaped_mount_point;
  escaped_mount_point = g_strescape (mount_point, NULL);
  gvfs_udisks2_utils_spawn (10, /* timeout in seconds */
                            data->cancellable,
                            lsof_command_cb,
                            unmount_data_ref (data),
                            "lsof -t \"%s\"",
                            escaped_mount_point);
  g_free (escaped_mount_point);
}

static void
lock_cb (GObject       *source_object,
         GAsyncResult  *res,
         gpointer       user_data)
{
  UDisksEncrypted *encrypted = UDISKS_ENCRYPTED (source_object);
  UnmountData *data = user_data;
  GError *error;

  error = NULL;
  if (!udisks_encrypted_call_lock_finish (encrypted,
                                          res,
                                          &error))
    g_simple_async_result_take_error (data->simple, error);
  g_simple_async_result_complete (data->simple);
  data->completed = TRUE;
  unmount_data_unref (data);
}

static void
unmount_cb (GObject       *source_object,
            GAsyncResult  *res,
            gpointer       user_data)
{
  UDisksFilesystem *filesystem = UDISKS_FILESYSTEM (source_object);
  UnmountData *data = user_data;
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_unmount_finish (filesystem,
                                              res,
                                              &error))
    {
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);

      /* if the user passed in a GMountOperation, then do the GMountOperation::show-processes dance ... */
      if (error->code == G_IO_ERROR_BUSY && data->mount_operation != NULL)
        {
          unmount_show_busy (data, udisks_filesystem_get_mount_points (filesystem)[0]);
          goto out;
        }
      g_simple_async_result_take_error (data->simple, error);
    }
  else
    {
      gvfs_udisks2_volume_monitor_update (data->mount->monitor);
      if (data->encrypted != NULL)
        {
          udisks_encrypted_call_lock (data->encrypted,
                                      g_variant_new ("a{sv}", NULL), /* options */
                                      data->cancellable,
                                      lock_cb,
                                      data);
          goto out;
        }
    }

  g_simple_async_result_complete (data->simple);
  data->completed = TRUE;
  unmount_data_unref (data);
 out:
  ;
}


/* ------------------------------ */

static void
umount_command_cb (GObject       *source_object,
                   GAsyncResult  *res,
                   gpointer       user_data)
{
  UnmountData *data = user_data;
  GError *error;
  gint exit_status;
  gchar *standard_error = NULL;

  error = NULL;
  if (!gvfs_udisks2_utils_spawn_finish (res,
                                        &exit_status,
                                        NULL, /* gchar **out_standard_output */
                                        &standard_error,
                                        &error))
    {
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete (data->simple);
      data->completed = TRUE;
      unmount_data_unref (data);
      goto out;
    }

  if (WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0)
    {
      gvfs_udisks2_volume_monitor_update (data->mount->monitor);
      g_simple_async_result_complete (data->simple);
      data->completed = TRUE;
      unmount_data_unref (data);
      goto out;
    }

  if (standard_error != NULL &&
      (strstr (standard_error, "device is busy") != NULL ||
       strstr (standard_error, "target is busy") != NULL))
    {
      unmount_show_busy (data, data->mount->mount_path);
      goto out;
    }

  g_simple_async_result_set_error (data->simple,
                                   G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   standard_error);
  g_simple_async_result_complete (data->simple);
  data->completed = TRUE;
  unmount_data_unref (data);

 out:
  g_free (standard_error);
}

static void
unmount_do (UnmountData *data,
            gboolean     force)
{
  GVariantBuilder builder;

  /* Use the umount(8) command if there is no block device / filesystem */
  if (data->filesystem == NULL)
    {
      gchar *escaped_mount_path;
      escaped_mount_path = g_strescape (data->mount->mount_path, NULL);
      gvfs_udisks2_utils_spawn (10, /* timeout in seconds */
                                data->cancellable,
                                umount_command_cb,
                                data,
                                "umount %s \"%s\"",
                                force ? "-l " : "",
                                escaped_mount_path);
      g_free (escaped_mount_path);
      goto out;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  if (force || data->flags & G_MOUNT_UNMOUNT_FORCE)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "force", g_variant_new_boolean (TRUE));
    }
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (data->filesystem), G_MAXINT);
  udisks_filesystem_call_unmount (data->filesystem,
                                  g_variant_builder_end (&builder),
                                  data->cancellable,
                                  unmount_cb,
                                  data);

 out:
  ;
}

static void
gvfs_udisks2_mount_unmount_with_operation (GMount              *_mount,
                                           GMountUnmountFlags   flags,
                                           GMountOperation     *mount_operation,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  UnmountData *data;
  UDisksBlock *block;

  /* first emit the ::mount-pre-unmount signal */
  g_signal_emit_by_name (mount->monitor, "mount-pre-unmount", mount);

  data = g_new0 (UnmountData, 1);
  data->ref_count = 1;
  data->simple = g_simple_async_result_new (G_OBJECT (mount),
                                            callback,
                                            user_data,
                                            gvfs_udisks2_mount_unmount_with_operation);
  data->mount = g_object_ref (mount);
  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;
  data->flags = flags;

  if (mount->is_burn_mount)
    {
      /* burn mounts are really never mounted so complete successfully immediately */
      g_simple_async_result_complete_in_idle (data->simple);
      data->completed = TRUE;
      unmount_data_unref (data);
      goto out;
    }

  block = NULL;
  if (mount->volume != NULL)
    block = gvfs_udisks2_volume_get_block (data->mount->volume);
  if (block != NULL)
    {
      GDBusObject *object;
      object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
      if (object == NULL)
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "No object for D-Bus interface");
          g_simple_async_result_complete (data->simple);
          data->completed = TRUE;
          unmount_data_unref (data);
          goto out;
        }
      data->filesystem = udisks_object_get_filesystem (UDISKS_OBJECT (object));
      if (data->filesystem == NULL)
        {
          UDisksBlock *cleartext_block;

          data->encrypted = udisks_object_get_encrypted (UDISKS_OBJECT (object));
          if (data->encrypted == NULL)
            {
              g_simple_async_result_set_error (data->simple,
                                               G_IO_ERROR,
                                               G_IO_ERROR_FAILED,
                                               "No filesystem or encrypted interface on D-Bus object");
              g_simple_async_result_complete (data->simple);
              data->completed = TRUE;
              unmount_data_unref (data);
              goto out;
            }

          cleartext_block = udisks_client_get_cleartext_block (gvfs_udisks2_volume_monitor_get_udisks_client (mount->monitor),
                                                               block);
          if (cleartext_block != NULL)
            {
              data->filesystem = udisks_object_get_filesystem (UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (cleartext_block))));
              g_object_unref (cleartext_block);
              if (data->filesystem == NULL)
                {
                  g_simple_async_result_set_error (data->simple,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_FAILED,
                                                   "No filesystem interface on D-Bus object for cleartext device");
                  g_simple_async_result_complete (data->simple);
                  data->completed = TRUE;
                  unmount_data_unref (data);
                  goto out;
                }
            }
        }
      g_assert (data->filesystem != NULL);
    }
  unmount_do (data, FALSE /* force */);

 out:
  ;
}

static gboolean
gvfs_udisks2_mount_unmount_with_operation_finish (GMount        *mount,
                                                  GAsyncResult  *result,
                                                  GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_unmount (GMount              *mount,
                            GMountUnmountFlags   flags,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  gvfs_udisks2_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_mount_unmount_finish (GMount        *mount,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  return gvfs_udisks2_mount_unmount_with_operation_finish (mount, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject       *source_object,
                        GAsyncResult  *res,
                        gpointer       user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static void
gvfs_udisks2_mount_eject_with_operation (GMount              *_mount,
                                         GMountUnmountFlags   flags,
                                         GMountOperation     *mount_operation,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (mount);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject_with_operation (drive, flags, mount_operation, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
  else
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_error (G_OBJECT (mount),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                _("Operation not supported by backend"));
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }
}

static gboolean
gvfs_udisks2_mount_eject_with_operation_finish (GMount        *_mount,
                                                GAsyncResult  *result,
                                                GError       **error)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  gboolean ret = TRUE;
  GDrive *drive;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      ret = g_drive_eject_with_operation_finish (drive, result, error);
      g_object_unref (drive);
    }
  else
    {
      g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
      ret = FALSE;
    }
  return ret;
}

static void
gvfs_udisks2_mount_eject (GMount              *mount,
                          GMountUnmountFlags   flags,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  gvfs_udisks2_mount_eject_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_mount_eject_finish (GMount        *mount,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  return gvfs_udisks2_mount_eject_with_operation_finish (mount, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar **
gvfs_udisks2_mount_guess_content_type_sync (GMount        *_mount,
                                            gboolean       force_rescan,
                                            GCancellable  *cancellable,
                                            GError       **error)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  gchar **x_content_types;
  GPtrArray *p;
  gchar **ret;
  guint n;

  p = g_ptr_array_new ();

  /* doesn't make sense to probe blank discs - look at the disc type instead */
  if (mount->is_burn_mount)
    {
      GDrive *drive;
      drive = gvfs_udisks2_mount_get_drive (_mount);
      if (drive != NULL)
        {
          UDisksDrive *udisks_drive = gvfs_udisks2_drive_get_udisks_drive (GVFS_UDISKS2_DRIVE (drive));;
          const gchar *media = udisks_drive_get_media (udisks_drive);
          if (media != NULL)
            {
              if (g_str_has_prefix (media, "optical_dvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-dvd"));
              else if (g_str_has_prefix (media, "optical_hddvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-hddvd"));
              else if (g_str_has_prefix (media, "optical_bd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-bd"));
              else
                g_ptr_array_add (p, g_strdup ("x-content/blank-cd")); /* assume CD */
            }
          g_object_unref (drive);
        }
    }
  else
    {
      /* sniff content type */
      x_content_types = g_content_type_guess_for_tree (mount->root);
      if (x_content_types != NULL)
        {
          for (n = 0; x_content_types[n] != NULL; n++)
            g_ptr_array_add (p, g_strdup (x_content_types[n]));
          g_strfreev (x_content_types);
        }
    }

  /* Check if its bootable */
  if (mount->device_file != NULL)
    {
      GUdevDevice *gudev_device;
      gudev_device = g_udev_client_query_by_device_file (gvfs_udisks2_volume_monitor_get_gudev_client (mount->monitor),
                                                         mount->device_file);
      if (gudev_device != NULL)
        {
          if (g_udev_device_get_property_as_boolean (gudev_device, "OSINFO_BOOTABLE"))
            g_ptr_array_add (p, g_strdup ("x-content/bootable-media"));
          g_object_unref (gudev_device);
        }
    }

  if (p->len == 0)
    {
      ret = NULL;
      g_ptr_array_free (p, TRUE);
    }
  else
    {
      g_ptr_array_add (p, NULL);
      ret = (char **) g_ptr_array_free (p, FALSE);
    }
  return ret;
}

/* since we're an out-of-process volume monitor we'll just do this sync */
static void
gvfs_udisks2_mount_guess_content_type (GMount              *mount,
                                       gboolean             force_rescan,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  GSimpleAsyncResult *simple;
  simple = g_simple_async_result_new (G_OBJECT (mount),
                                      callback,
                                      user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static gchar **
gvfs_udisks2_mount_guess_content_type_finish (GMount        *mount,
                                              GAsyncResult  *result,
                                              GError       **error)
{
  return gvfs_udisks2_mount_guess_content_type_sync (mount, FALSE, NULL, error);
}
/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
gvfs_udisks2_mount_get_sort_key (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = gvfs_udisks2_mount_get_root;
  iface->get_name = gvfs_udisks2_mount_get_name;
  iface->get_icon = gvfs_udisks2_mount_get_icon;
  iface->get_uuid = gvfs_udisks2_mount_get_uuid;
  iface->get_drive = gvfs_udisks2_mount_get_drive;
  iface->get_volume = gvfs_udisks2_mount_get_volume_;
  iface->can_unmount = gvfs_udisks2_mount_can_unmount;
  iface->can_eject = gvfs_udisks2_mount_can_eject;
  iface->unmount = gvfs_udisks2_mount_unmount;
  iface->unmount_finish = gvfs_udisks2_mount_unmount_finish;
  iface->unmount_with_operation = gvfs_udisks2_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = gvfs_udisks2_mount_unmount_with_operation_finish;
  iface->eject = gvfs_udisks2_mount_eject;
  iface->eject_finish = gvfs_udisks2_mount_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_mount_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_mount_eject_with_operation_finish;
  iface->guess_content_type = gvfs_udisks2_mount_guess_content_type;
  iface->guess_content_type_finish = gvfs_udisks2_mount_guess_content_type_finish;
  iface->guess_content_type_sync = gvfs_udisks2_mount_guess_content_type_sync;
  iface->get_sort_key = gvfs_udisks2_mount_get_sort_key;
}

gboolean
gvfs_udisks2_mount_has_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  return mount->volume == volume;
}

GVfsUDisks2Volume *
gvfs_udisks2_mount_get_volume (GVfsUDisks2Mount *mount)
{
  return mount->volume;
}
