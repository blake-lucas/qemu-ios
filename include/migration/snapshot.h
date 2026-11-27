/*
 * QEMU snapshots
 *
 * Copyright (c) 2004-2008 Fabrice Bellard
 * Copyright (c) 2009-2015 Red Hat Inc
 *
 * Authors:
 *  Juan Quintela <quintela@redhat.com>
 *
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_SNAPSHOT_H
#define QEMU_MIGRATION_SNAPSHOT_H

#include "qapi/qapi-builtin-types.h"
#include "io/channel.h"
#include "qapi/qapi-types-run-state.h"

/**
 * save_snapshot: Save an internal snapshot.
 * @name: name of internal snapshot
 * @overwrite: replace existing snapshot with @name
 * @vmstate: blockdev node name to store VM state in
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool save_snapshot(const char *name, bool overwrite,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   Error **errp);

/**
 * load_snapshot: Load an internal snapshot.
 * @name: name of internal snapshot
 * @vmstate: blockdev node name to load VM state from
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool load_snapshot(const char *name,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   Error **errp);

/**
 * delete_snapshot: Delete a snapshot.
 * @name: path to snapshot
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool delete_snapshot(const char *name,
                    bool has_devices, strList *devices,
                    Error **errp);

/**
 * load_snapshot_resume: Restore runstate after loading snapshot.
 * @state: state to restore
 */
void load_snapshot_resume(RunState state);

/**
 * savevm_to_channel: Save the whole machine (RAM and every device) to @ioc.
 * loadvm_from_channel: Reset the machine and load it from @ioc.
 *
 * The state stream of an internal snapshot, with no block device: an embedding host (LightTouch's save states) keeps
 * it, and keeps the guest's storage in step itself. The caller holds the BQL with the VM stopped. A machine whose
 * migration is blocked refuses to save, with the blocker's reason.
 */
int savevm_to_channel(QIOChannel *ioc, Error **errp);
int loadvm_from_channel(QIOChannel *ioc, Error **errp);

#endif
