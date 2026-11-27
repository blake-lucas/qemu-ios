/*
 * QEMU I/O channel over a host's read or write callback: see include/io/channel-callback.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "io/channel-callback.h"
#include "qapi/error.h"
#include "qemu/module.h"

QIOChannelCallback *qio_channel_callback_new_writer(QIOChannelCallbackWrite write, void *opaque)
{
    QIOChannelCallback *ioc = QIO_CHANNEL_CALLBACK(object_new(TYPE_QIO_CHANNEL_CALLBACK));

    ioc->write = write;
    ioc->opaque = opaque;
    return ioc;
}

QIOChannelCallback *qio_channel_callback_new_reader(QIOChannelCallbackRead read, void *opaque)
{
    QIOChannelCallback *ioc = QIO_CHANNEL_CALLBACK(object_new(TYPE_QIO_CHANNEL_CALLBACK));

    ioc->read = read;
    ioc->opaque = opaque;
    return ioc;
}

static ssize_t qio_channel_callback_readv(QIOChannel *ioc, const struct iovec *iov, size_t niov, int **fds,
                                          size_t *nfds, int flags, Error **errp)
{
    QIOChannelCallback *c = QIO_CHANNEL_CALLBACK(ioc);
    ssize_t total = 0;

    if (!c->read) {
        error_setg(errp, "this channel only writes");
        return -1;
    }
    for (size_t i = 0; i < niov; i++) {
        size_t done = 0;

        while (done < iov[i].iov_len) {
            ssize_t n = c->read(c->opaque, (uint8_t *)iov[i].iov_base + done, iov[i].iov_len - done);

            if (n < 0) {
                error_setg_errno(errp, (int)-n, "reading the state stream");
                return -1;
            }
            if (n == 0) {
                c->bytes += total;
                return total;       /* the end: what was read so far */
            }
            done += n;
            total += n;
        }
    }
    c->bytes += total;
    return total;
}

static ssize_t qio_channel_callback_writev(QIOChannel *ioc, const struct iovec *iov, size_t niov, int *fds,
                                           size_t nfds, int flags, Error **errp)
{
    QIOChannelCallback *c = QIO_CHANNEL_CALLBACK(ioc);
    ssize_t total = 0;

    if (!c->write) {
        error_setg(errp, "this channel only reads");
        return -1;
    }
    for (size_t i = 0; i < niov; i++) {
        int result = iov[i].iov_len ? c->write(c->opaque, iov[i].iov_base, iov[i].iov_len) : 0;

        if (result < 0) {
            error_setg_errno(errp, -result, "writing the state stream");
            return -1;
        }
        total += iov[i].iov_len;
    }
    c->bytes += total;
    return total;
}

static int qio_channel_callback_set_blocking(QIOChannel *ioc, bool enabled, Error **errp)
{
    return 0;   /* always blocking: the callbacks return when done */
}

static int qio_channel_callback_close(QIOChannel *ioc, Error **errp)
{
    QIOChannelCallback *c = QIO_CHANNEL_CALLBACK(ioc);

    c->read = NULL;
    c->write = NULL;
    return 0;
}

/* Always ready: nothing here waits on an event loop. */
typedef struct {
    GSource parent;
    GIOCondition condition;
} QIOChannelCallbackSource;

static gboolean qio_channel_callback_source_prepare(GSource *source, gint *timeout)
{
    *timeout = -1;
    return (G_IO_IN | G_IO_OUT) & ((QIOChannelCallbackSource *)source)->condition;
}

static gboolean qio_channel_callback_source_check(GSource *source)
{
    return (G_IO_IN | G_IO_OUT) & ((QIOChannelCallbackSource *)source)->condition;
}

static gboolean qio_channel_callback_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    return ((QIOChannelFunc)callback)(NULL, (G_IO_IN | G_IO_OUT) & ((QIOChannelCallbackSource *)source)->condition,
                                      user_data);
}

static GSourceFuncs qio_channel_callback_source_funcs = {
    qio_channel_callback_source_prepare,
    qio_channel_callback_source_check,
    qio_channel_callback_source_dispatch,
    NULL,
};

static GSource *qio_channel_callback_create_watch(QIOChannel *ioc, GIOCondition condition)
{
    GSource *source = g_source_new(&qio_channel_callback_source_funcs, sizeof(QIOChannelCallbackSource));

    ((QIOChannelCallbackSource *)source)->condition = condition;
    return source;
}

static void qio_channel_callback_class_init(ObjectClass *klass, const void *class_data G_GNUC_UNUSED)
{
    QIOChannelClass *ioc_klass = QIO_CHANNEL_CLASS(klass);

    ioc_klass->io_writev = qio_channel_callback_writev;
    ioc_klass->io_readv = qio_channel_callback_readv;
    ioc_klass->io_set_blocking = qio_channel_callback_set_blocking;
    ioc_klass->io_close = qio_channel_callback_close;
    ioc_klass->io_create_watch = qio_channel_callback_create_watch;
}

static const TypeInfo qio_channel_callback_info = {
    .parent = TYPE_QIO_CHANNEL,
    .name = TYPE_QIO_CHANNEL_CALLBACK,
    .instance_size = sizeof(QIOChannelCallback),
    .class_init = qio_channel_callback_class_init,
};

static void qio_channel_callback_register_types(void)
{
    type_register_static(&qio_channel_callback_info);
}

type_init(qio_channel_callback_register_types);
