/*
 * QEMU I/O channel over a host's read or write callback
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * An embedding host (LightTouch's save states) takes the machine's state stream straight into its own writer, or
 * feeds it back from its own reader, without a file, pipe or socket in between: QEMU's file channel has no blocking
 * mode on Windows, and the host compresses the stream as it arrives. The channel is blocking and one-way.
 */

#ifndef QIO_CHANNEL_CALLBACK_H
#define QIO_CHANNEL_CALLBACK_H

#include "io/channel.h"
#include "qom/object.h"

#define TYPE_QIO_CHANNEL_CALLBACK "qio-channel-callback"
OBJECT_DECLARE_SIMPLE_TYPE(QIOChannelCallback, QIO_CHANNEL_CALLBACK)

/* Writes all of len bytes; 0, or a negative errno. */
typedef int (*QIOChannelCallbackWrite)(void *opaque, const void *buf, size_t len);
/* Reads up to len bytes; how many (0 at the end), or a negative errno. */
typedef ssize_t (*QIOChannelCallbackRead)(void *opaque, void *buf, size_t len);

struct QIOChannelCallback {
    QIOChannel parent;
    QIOChannelCallbackWrite write;
    QIOChannelCallbackRead read;
    void *opaque;
    uint64_t bytes;     /* moved so far */
};

QIOChannelCallback *qio_channel_callback_new_writer(QIOChannelCallbackWrite write, void *opaque);
QIOChannelCallback *qio_channel_callback_new_reader(QIOChannelCallbackRead read, void *opaque);

#endif /* QIO_CHANNEL_CALLBACK_H */
