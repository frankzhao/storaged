/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <mntent.h>

#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxblock.h"
#include "udisksmount.h"
#include "udisksmountmonitor.h"
#include "udiskslinuxdriveobject.h"
#include "udiskslinuxdrive.h"
#include "udiskslinuxfilesystem.h"
#include "udiskslinuxencrypted.h"
#include "udiskslinuxswapspace.h"
#include "udiskslinuxloop.h"
#include "udiskspersistentstore.h"
#include "udiskslinuxprovider.h"
#include "udisksfstabmonitor.h"
#include "udisksfstabentry.h"
#include "udiskscrypttabmonitor.h"
#include "udiskscrypttabentry.h"

/**
 * SECTION:udiskslinuxblockobject
 * @title: UDisksLinuxBlockObject
 * @short_description: Object representing a block device on Linux.
 *
 * Object corresponding to a block device on Linux.
 */

typedef struct _UDisksLinuxBlockObjectClass   UDisksLinuxBlockObjectClass;

/**
 * UDisksLinuxBlockObject:
 *
 * The #UDisksLinuxBlockObject structure contains only private data and
 * should only be accessed using the provided API.
 */
struct _UDisksLinuxBlockObject
{
  UDisksObjectSkeleton parent_instance;

  UDisksDaemon *daemon;
  UDisksMountMonitor *mount_monitor;

  GUdevDevice *device;

  /* interface */
  UDisksBlock *iface_block_device;
  UDisksFilesystem *iface_filesystem;
  UDisksSwapspace *iface_swapspace;
  UDisksEncrypted *iface_encrypted;
  UDisksLoop *iface_loop;
};

struct _UDisksLinuxBlockObjectClass
{
  UDisksObjectSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_DEVICE
};

G_DEFINE_TYPE (UDisksLinuxBlockObject, udisks_linux_block_object, UDISKS_TYPE_OBJECT_SKELETON);

static void on_mount_monitor_mount_added   (UDisksMountMonitor  *monitor,
                                            UDisksMount         *mount,
                                            gpointer             user_data);
static void on_mount_monitor_mount_removed (UDisksMountMonitor  *monitor,
                                            UDisksMount         *mount,
                                            gpointer             user_data);

static void
udisks_linux_block_object_finalize (GObject *_object)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);

  /* note: we don't hold a ref to block->daemon or block->mount_monitor */
  g_signal_handlers_disconnect_by_func (object->mount_monitor, on_mount_monitor_mount_added, object);
  g_signal_handlers_disconnect_by_func (object->mount_monitor, on_mount_monitor_mount_removed, object);

  g_object_unref (object->device);

  if (object->iface_block_device != NULL)
    g_object_unref (object->iface_block_device);
  if (object->iface_filesystem != NULL)
    g_object_unref (object->iface_filesystem);
  if (object->iface_swapspace != NULL)
    g_object_unref (object->iface_swapspace);
  if (object->iface_encrypted != NULL)
    g_object_unref (object->iface_encrypted);
  if (object->iface_loop != NULL)
    g_object_unref (object->iface_loop);

  if (G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->finalize (_object);
}

static void
udisks_linux_block_object_get_property (GObject    *_object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_value_set_object (value, udisks_linux_block_object_get_daemon (object));
      break;

    case PROP_DEVICE:
      g_value_set_object (value, udisks_linux_block_object_get_device (object));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (_object, prop_id, pspec);
      break;
    }
}

static void
udisks_linux_block_object_set_property (GObject      *_object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_assert (object->daemon == NULL);
      /* we don't take a reference to the daemon */
      object->daemon = g_value_get_object (value);
      break;

    case PROP_DEVICE:
      g_assert (object->device == NULL);
      object->device = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (_object, prop_id, pspec);
      break;
    }
}


static void
udisks_linux_block_object_init (UDisksLinuxBlockObject *object)
{
}

static void
udisks_linux_block_object_constructed (GObject *_object)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);
  GString *str;

  object->mount_monitor = udisks_daemon_get_mount_monitor (object->daemon);
  g_signal_connect (object->mount_monitor,
                    "mount-added",
                    G_CALLBACK (on_mount_monitor_mount_added),
                    object);
  g_signal_connect (object->mount_monitor,
                    "mount-removed",
                    G_CALLBACK (on_mount_monitor_mount_removed),
                    object);

  /* initial coldplug */
  udisks_linux_block_object_uevent (object, "add", NULL);

  /* compute the object path */
  str = g_string_new ("/org/freedesktop/UDisks2/block_devices/");
  udisks_safe_append_to_object_path (str, g_udev_device_get_name (object->device));
  g_dbus_object_skeleton_set_object_path (G_DBUS_OBJECT_SKELETON (object), str->str);
  g_string_free (str, TRUE);

  if (G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->constructed (_object);
}

static void
udisks_linux_block_object_class_init (UDisksLinuxBlockObjectClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_block_object_finalize;
  gobject_class->constructed  = udisks_linux_block_object_constructed;
  gobject_class->set_property = udisks_linux_block_object_set_property;
  gobject_class->get_property = udisks_linux_block_object_get_property;

  /**
   * UDisksLinuxBlockObject:daemon:
   *
   * The #UDisksDaemon the object is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon the object is for",
                                                        UDISKS_TYPE_DAEMON,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksLinuxBlockObject:device:
   *
   * The #GUdevDevice for the object. Connect to the #GObject::notify
   * signal to get notified whenever this is updated.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DEVICE,
                                   g_param_spec_object ("device",
                                                        "Device",
                                                        "The device for the object",
                                                        G_UDEV_TYPE_DEVICE,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

}

/**
 * udisks_linux_block_object_new:
 * @daemon: A #UDisksDaemon.
 * @device: The #GUdevDevice for the sysfs block device.
 *
 * Create a new block object.
 *
 * Returns: A #UDisksLinuxBlockObject object. Free with g_object_unref().
 */
UDisksLinuxBlockObject *
udisks_linux_block_object_new (UDisksDaemon  *daemon,
                               GUdevDevice   *device)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return UDISKS_LINUX_BLOCK_OBJECT (g_object_new (UDISKS_TYPE_LINUX_BLOCK_OBJECT,
                                                  "daemon", daemon,
                                                  "device", device,
                                                  NULL));
}

/**
 * udisks_linux_block_object_get_daemon:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the daemon used by @object.
 *
 * Returns: A #UDisksDaemon. Do not free, the object is owned by @object.
 */
UDisksDaemon *
udisks_linux_block_object_get_daemon (UDisksLinuxBlockObject *object)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);
  return object->daemon;
}

/**
 * udisks_linux_block_object_get_device:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the current #GUdevDevice for @object. Connect to
 * #GObject::notify to track changes to the #UDisksLinuxBlockObject:device
 * property.
 *
 * Returns: A #GUdevDevice. Free with g_object_unref().
 */
GUdevDevice *
udisks_linux_block_object_get_device (UDisksLinuxBlockObject *object)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);
  return g_object_ref (object->device);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef gboolean (*HasInterfaceFunc)    (UDisksLinuxBlockObject     *object);
typedef void     (*ConnectInterfaceFunc) (UDisksLinuxBlockObject    *object);
typedef void     (*UpdateInterfaceFunc) (UDisksLinuxBlockObject     *object,
                                         const gchar    *uevent_action,
                                         GDBusInterface *interface);

static void
update_iface (UDisksLinuxBlockObject   *object,
              const gchar              *uevent_action,
              HasInterfaceFunc          has_func,
              ConnectInterfaceFunc      connect_func,
              UpdateInterfaceFunc       update_func,
              GType                     skeleton_type,
              gpointer                  _interface_pointer)
{
  gboolean has;
  gboolean add;
  GDBusInterface **interface_pointer = _interface_pointer;

  g_return_if_fail (object != NULL);
  g_return_if_fail (has_func != NULL);
  g_return_if_fail (update_func != NULL);
  g_return_if_fail (g_type_is_a (skeleton_type, G_TYPE_OBJECT));
  g_return_if_fail (g_type_is_a (skeleton_type, G_TYPE_DBUS_INTERFACE));
  g_return_if_fail (interface_pointer != NULL);
  g_return_if_fail (*interface_pointer == NULL || G_IS_DBUS_INTERFACE (*interface_pointer));

  add = FALSE;
  has = has_func (object);
  if (*interface_pointer == NULL)
    {
      if (has)
        {
          *interface_pointer = g_object_new (skeleton_type, NULL);
          if (connect_func != NULL)
            connect_func (object);
          add = TRUE;
        }
    }
  else
    {
      if (!has)
        {
          g_dbus_object_skeleton_remove_interface (G_DBUS_OBJECT_SKELETON (object),
                                                   G_DBUS_INTERFACE_SKELETON (*interface_pointer));
          g_object_unref (*interface_pointer);
          *interface_pointer = NULL;
        }
    }

  if (*interface_pointer != NULL)
    {
      update_func (object, uevent_action, G_DBUS_INTERFACE (*interface_pointer));
      if (add)
        g_dbus_object_skeleton_add_interface (G_DBUS_OBJECT_SKELETON (object),
                                              G_DBUS_INTERFACE_SKELETON (*interface_pointer));
    }
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks.Block */

static gboolean
block_device_check (UDisksLinuxBlockObject *object)
{
  return TRUE;
}

static void
block_device_connect (UDisksLinuxBlockObject *object)
{
}

static void
block_device_update (UDisksLinuxBlockObject *object,
                     const gchar            *uevent_action,
                     GDBusInterface         *_iface)
{
  udisks_linux_block_update (UDISKS_LINUX_BLOCK (_iface), object);
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks.Filesystem */

static gboolean
filesystem_check (UDisksLinuxBlockObject *object)
{
  gboolean ret;
  UDisksMountType mount_type;

  ret = FALSE;
  if (g_strcmp0 (udisks_block_get_id_usage (object->iface_block_device), "filesystem") == 0 ||
      (udisks_mount_monitor_is_dev_in_use (object->mount_monitor,
                                           g_udev_device_get_device_number (object->device),
                                           &mount_type) &&
       mount_type == UDISKS_MOUNT_TYPE_FILESYSTEM))
    ret = TRUE;

  return ret;
}


static void
filesystem_connect (UDisksLinuxBlockObject *object)
{
}

static void
filesystem_update (UDisksLinuxBlockObject  *object,
                   const gchar             *uevent_action,
                   GDBusInterface          *_iface)
{
  udisks_linux_filesystem_update (UDISKS_LINUX_FILESYSTEM (_iface), object);
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks.Swapspace */

static gboolean
swapspace_check (UDisksLinuxBlockObject *object)
{
  gboolean ret;
  UDisksMountType mount_type;

  ret = FALSE;
  if ((g_strcmp0 (udisks_block_get_id_usage (object->iface_block_device), "other") == 0 &&
       g_strcmp0 (udisks_block_get_id_type (object->iface_block_device), "swap") == 0)
      || (udisks_mount_monitor_is_dev_in_use (object->mount_monitor,
                                              g_udev_device_get_device_number (object->device),
                                              &mount_type)
          && mount_type == UDISKS_MOUNT_TYPE_SWAP))
    ret = TRUE;

  return ret;
}

static void
swapspace_connect (UDisksLinuxBlockObject *object)
{
}

static void
swapspace_update (UDisksLinuxBlockObject  *object,
                  const gchar             *uevent_action,
                  GDBusInterface          *_iface)
{
  udisks_linux_swapspace_update (UDISKS_LINUX_SWAPSPACE (_iface), object);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
encrypted_check (UDisksLinuxBlockObject *object)
{
  gboolean ret;

  ret = FALSE;
  if (g_strcmp0 (udisks_block_get_id_usage (object->iface_block_device), "crypto") == 0 &&
      g_strcmp0 (udisks_block_get_id_type (object->iface_block_device), "crypto_LUKS") == 0)
    ret = TRUE;

  return ret;
}

static void
encrypted_connect (UDisksLinuxBlockObject *object)
{
}

static void
encrypted_update (UDisksLinuxBlockObject  *object,
                  const gchar             *uevent_action,
                  GDBusInterface          *_iface)
{
  udisks_linux_encrypted_update (UDISKS_LINUX_ENCRYPTED (_iface), object);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
loop_check (UDisksLinuxBlockObject *object)
{
  gboolean ret;

  ret = FALSE;
  if (g_str_has_prefix (g_udev_device_get_name (object->device), "loop"))
    ret = TRUE;

  return ret;
}

static void
loop_connect (UDisksLinuxBlockObject *object)
{
}

static void
loop_update (UDisksLinuxBlockObject  *object,
             const gchar             *uevent_action,
             GDBusInterface          *_iface)
{
  udisks_linux_loop_update (UDISKS_LINUX_LOOP (_iface), object);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_object_uevent:
 * @object: A #UDisksLinuxBlockObject.
 * @action: Uevent action or %NULL
 * @device: A new #GUdevDevice device object or %NULL if the device hasn't changed.
 *
 * Updates all information on interfaces on @object.
 */
void
udisks_linux_block_object_uevent (UDisksLinuxBlockObject *object,
                                  const gchar            *action,
                                  GUdevDevice            *device)
{
  g_return_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object));
  g_return_if_fail (device == NULL || G_UDEV_IS_DEVICE (device));

  if (device != NULL)
    {
      g_object_unref (object->device);
      object->device = g_object_ref (device);
      g_object_notify (G_OBJECT (object), "device");
    }

  update_iface (object, action, block_device_check, block_device_connect, block_device_update,
                UDISKS_TYPE_LINUX_BLOCK, &object->iface_block_device);
  update_iface (object, action, filesystem_check, filesystem_connect, filesystem_update,
                UDISKS_TYPE_LINUX_FILESYSTEM, &object->iface_filesystem);
  update_iface (object, action, swapspace_check, swapspace_connect, swapspace_update,
                UDISKS_TYPE_LINUX_SWAPSPACE, &object->iface_swapspace);
  update_iface (object, action, encrypted_check, encrypted_connect, encrypted_update,
                UDISKS_TYPE_LINUX_ENCRYPTED, &object->iface_encrypted);
  update_iface (object, action, loop_check, loop_connect, loop_update,
                UDISKS_TYPE_LINUX_LOOP, &object->iface_loop);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_mount_monitor_mount_added (UDisksMountMonitor  *monitor,
                              UDisksMount         *mount,
                              gpointer             user_data)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (user_data);
  if (udisks_mount_get_dev (mount) == g_udev_device_get_device_number (object->device))
    udisks_linux_block_object_uevent (object, NULL, NULL);
}

static void
on_mount_monitor_mount_removed (UDisksMountMonitor  *monitor,
                                UDisksMount         *mount,
                                gpointer             user_data)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (user_data);
  if (udisks_mount_get_dev (mount) == g_udev_device_get_device_number (object->device))
    udisks_linux_block_object_uevent (object, NULL, NULL);
}

/* ---------------------------------------------------------------------------------------------------- */