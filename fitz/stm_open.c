#include "fitz.h"

fz_stream *
fz_new_stream(fz_context *ctx, void *state,
	int(*read)(fz_stream *stm, unsigned char *buf, int len),
	void(*close)(fz_stream *stm))
{
	fz_stream *stm;

	stm = fz_malloc(ctx, sizeof(fz_stream));

	stm->refs = 1;
	stm->error = 0;
	stm->eof = 0;
	stm->pos = 0;

	stm->bits = 0;
	stm->avail = 0;

	stm->bp = stm->buf;
	stm->rp = stm->bp;
	stm->wp = stm->bp;
	stm->ep = stm->buf + sizeof stm->buf;

	stm->state = state;
	stm->read = read;
	stm->close = close;
	stm->seek = NULL;
	stm->ctx = ctx;

	return stm;
}

fz_stream *
fz_keep_stream(fz_stream *stm)
{
	stm->refs ++;
	return stm;
}

void
fz_close(fz_stream *stm)
{
	stm->refs --;
	if (stm->refs == 0)
	{
		if (stm->close)
			stm->close(stm);
		fz_free(stm->ctx, stm);
	}
}

/* File stream */

static int read_file(fz_stream *stm, unsigned char *buf, int len)
{
	int n = read(*(int*)stm->state, buf, len);
	if (n < 0)
		return fz_error_make(stm->ctx, "read error: %s", strerror(errno));
	return n;
}

static void seek_file(fz_stream *stm, int offset, int whence)
{
	int n = lseek(*(int*)stm->state, offset, whence);
	if (n < 0)
		fz_warn(stm->ctx, "cannot lseek: %s", strerror(errno));
	stm->pos = n;
	stm->rp = stm->bp;
	stm->wp = stm->bp;
}

static void close_file(fz_stream *stm)
{
	int n = close(*(int*)stm->state);
	if (n < 0)
		fz_warn(stm->ctx, "close error: %s", strerror(errno));
	fz_free(stm->ctx, stm->state);
}

fz_stream *
fz_open_fd(fz_context *ctx, int fd)
{
	fz_stream *stm;
	int *state;

	state = fz_malloc(ctx, sizeof(int));
	*state = fd;

	stm = fz_new_stream(ctx, state, read_file, close_file);
	stm->seek = seek_file;

	return stm;
}

fz_stream *
fz_open_file(fz_context *ctx, const char *name)
{
	int fd = open(name, O_BINARY | O_RDONLY, 0);
	if (fd == -1)
		return NULL;
	return fz_open_fd(ctx, fd);
}

#ifdef _WIN32
fz_stream *
fz_open_file_w(fz_context *ctx, const wchar_t *name)
{
	int fd = _wopen(name, O_BINARY | O_RDONLY, 0);
	if (fd == -1)
		return NULL;
	return fz_open_fd(ctx, fd);
}
#endif

/* Memory stream */

static int read_buffer(fz_stream *stm, unsigned char *buf, int len)
{
	return 0;
}

static void seek_buffer(fz_stream *stm, int offset, int whence)
{
	if (whence == 0)
		stm->rp = stm->bp + offset;
	if (whence == 1)
		stm->rp += offset;
	if (whence == 2)
		stm->rp = stm->ep - offset;
	stm->rp = CLAMP(stm->rp, stm->bp, stm->ep);
	stm->wp = stm->ep;
}

static void close_buffer(fz_stream *stm)
{
	if (stm->state)
		fz_drop_buffer(stm->ctx, stm->state);
}

fz_stream *
fz_open_buffer(fz_context *ctx, fz_buffer *buf)
{
	fz_stream *stm;

	stm = fz_new_stream(ctx, fz_keep_buffer(buf), read_buffer, close_buffer);
	stm->seek = seek_buffer;

	stm->bp = buf->data;
	stm->rp = buf->data;
	stm->wp = buf->data + buf->len;
	stm->ep = buf->data + buf->len;

	stm->pos = buf->len;

	return stm;
}

fz_stream *
fz_open_memory(fz_context *ctx, unsigned char *data, int len)
{
	fz_stream *stm;

	stm = fz_new_stream(ctx, NULL, read_buffer, close_buffer);
	stm->seek = seek_buffer;

	stm->bp = data;
	stm->rp = data;
	stm->wp = data + len;
	stm->ep = data + len;

	stm->pos = len;

	return stm;
}
