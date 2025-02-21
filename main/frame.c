/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Frame and codec manipulation routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 413588 $")

#include "asterisk/_private.h"
#include "asterisk/lock.h"
#include "asterisk/frame.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/term.h"
#include "asterisk/utils.h"
#include "asterisk/threadstorage.h"
#include "asterisk/linkedlists.h"
#include "asterisk/translate.h"
#include "asterisk/dsp.h"
#include "asterisk/file.h"

#if !defined(LOW_MEMORY)
static void frame_cache_cleanup(void *data);

/*! \brief A per-thread cache of frame headers */
AST_THREADSTORAGE_CUSTOM(frame_cache, NULL, frame_cache_cleanup);

/*!
 * \brief Maximum ast_frame cache size
 *
 * In most cases where the frame header cache will be useful, the size
 * of the cache will stay very small.  However, it is not always the case that
 * the same thread that allocates the frame will be the one freeing them, so
 * sometimes a thread will never have any frames in its cache, or the cache
 * will never be pulled from.  For the latter case, we limit the maximum size.
 */
#define FRAME_CACHE_MAX_SIZE	10

/*! \brief This is just so ast_frames, a list head struct for holding a list of
 *  ast_frame structures, is defined. */
AST_LIST_HEAD_NOLOCK(ast_frames, ast_frame);

struct ast_frame_cache {
	struct ast_frames list;
	size_t size;
};
#endif

#define SMOOTHER_SIZE 8000

enum frame_type {
	TYPE_HIGH,     /* 0x0 */
	TYPE_LOW,      /* 0x1 */
	TYPE_SILENCE,  /* 0x2 */
	TYPE_DONTSEND  /* 0x3 */
};

#define TYPE_MASK 0x3

struct ast_smoother {
	int size;
	struct ast_format format;
	int flags;
	float samplesperbyte;
	unsigned int opt_needs_swap:1;
	struct ast_frame f;
	struct timeval delivery;
	char data[SMOOTHER_SIZE];
	char framedata[SMOOTHER_SIZE + AST_FRIENDLY_OFFSET];
	struct ast_frame *opt;
	int len;
};

struct ast_frame ast_null_frame = { AST_FRAME_NULL, };

static int smoother_frame_feed(struct ast_smoother *s, struct ast_frame *f, int swap)
{
	if (s->flags & AST_SMOOTHER_FLAG_G729) {
		if (s->len % 10) {
			ast_log(LOG_NOTICE, "Dropping extra frame of G.729 since we already have a VAD frame at the end\n");
			return 0;
		}
	}
	if (swap) {
		ast_swapcopy_samples(s->data + s->len, f->data.ptr, f->samples);
	} else {
		memcpy(s->data + s->len, f->data.ptr, f->datalen);
	}
	/* If either side is empty, reset the delivery time */
	if (!s->len || ast_tvzero(f->delivery) || ast_tvzero(s->delivery)) {	/* XXX really ? */
		s->delivery = f->delivery;
	}
	s->len += f->datalen;

	return 0;
}

void ast_smoother_reset(struct ast_smoother *s, int bytes)
{
	memset(s, 0, sizeof(*s));
	s->size = bytes;
}

void ast_smoother_reconfigure(struct ast_smoother *s, int bytes)
{
	/* if there is no change, then nothing to do */
	if (s->size == bytes) {
		return;
	}
	/* set the new desired output size */
	s->size = bytes;
	/* if there is no 'optimized' frame in the smoother,
	 *   then there is nothing left to do
	 */
	if (!s->opt) {
		return;
	}
	/* there is an 'optimized' frame here at the old size,
	 * but it must now be put into the buffer so the data
	 * can be extracted at the new size
	 */
	smoother_frame_feed(s, s->opt, s->opt_needs_swap);
	s->opt = NULL;
}

struct ast_smoother *ast_smoother_new(int size)
{
	struct ast_smoother *s;
	if (size < 1)
		return NULL;
	if ((s = ast_malloc(sizeof(*s))))
		ast_smoother_reset(s, size);
	return s;
}

int ast_smoother_get_flags(struct ast_smoother *s)
{
	return s->flags;
}

void ast_smoother_set_flags(struct ast_smoother *s, int flags)
{
	s->flags = flags;
}

int ast_smoother_test_flag(struct ast_smoother *s, int flag)
{
	return (s->flags & flag);
}

int __ast_smoother_feed(struct ast_smoother *s, struct ast_frame *f, int swap)
{
	if (f->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING, "Huh?  Can't smooth a non-voice frame!\n");
		return -1;
	}
	if (!s->format.id) {
		ast_format_copy(&s->format, &f->subclass.format);
		s->samplesperbyte = (float)f->samples / (float)f->datalen;
	} else if (ast_format_cmp(&s->format, &f->subclass.format) == AST_FORMAT_CMP_NOT_EQUAL) {
		ast_log(LOG_WARNING, "Smoother was working on %s format frames, now trying to feed %s?\n",
			ast_getformatname(&s->format), ast_getformatname(&f->subclass.format));
		return -1;
	}
	if (s->len + f->datalen > SMOOTHER_SIZE) {
		ast_log(LOG_WARNING, "Out of smoother space\n");
		return -1;
	}
	if (((f->datalen == s->size) ||
	     ((f->datalen < 10) && (s->flags & AST_SMOOTHER_FLAG_G729))) &&
	    !s->opt &&
	    !s->len &&
	    (f->offset >= AST_MIN_OFFSET)) {
		/* Optimize by sending the frame we just got
		   on the next read, thus eliminating the douple
		   copy */
		if (swap)
			ast_swapcopy_samples(f->data.ptr, f->data.ptr, f->samples);
		s->opt = f;
		s->opt_needs_swap = swap ? 1 : 0;
		return 0;
	}

	return smoother_frame_feed(s, f, swap);
}

struct ast_frame *ast_smoother_read(struct ast_smoother *s)
{
	struct ast_frame *opt;
	int len;

	/* IF we have an optimization frame, send it */
	if (s->opt) {
		if (s->opt->offset < AST_FRIENDLY_OFFSET)
			ast_log(LOG_WARNING, "Returning a frame of inappropriate offset (%d).\n",
							s->opt->offset);
		opt = s->opt;
		s->opt = NULL;
		return opt;
	}

	/* Make sure we have enough data */
	if (s->len < s->size) {
		/* Or, if this is a G.729 frame with VAD on it, send it immediately anyway */
		if (!((s->flags & AST_SMOOTHER_FLAG_G729) && (s->len % 10)))
			return NULL;
	}
	len = s->size;
	if (len > s->len)
		len = s->len;
	/* Make frame */
	s->f.frametype = AST_FRAME_VOICE;
	ast_format_copy(&s->f.subclass.format, &s->format);
	s->f.data.ptr = s->framedata + AST_FRIENDLY_OFFSET;
	s->f.offset = AST_FRIENDLY_OFFSET;
	s->f.datalen = len;
	/* Samples will be improper given VAD, but with VAD the concept really doesn't even exist */
	s->f.samples = len * s->samplesperbyte;	/* XXX rounding */
	s->f.delivery = s->delivery;
	/* Fill Data */
	memcpy(s->f.data.ptr, s->data, len);
	s->len -= len;
	/* Move remaining data to the front if applicable */
	if (s->len) {
		/* In principle this should all be fine because if we are sending
		   G.729 VAD, the next timestamp will take over anyawy */
		memmove(s->data, s->data + len, s->len);
		if (!ast_tvzero(s->delivery)) {
			/* If we have delivery time, increment it, otherwise, leave it at 0 */
			s->delivery = ast_tvadd(s->delivery, ast_samp2tv(s->f.samples, ast_format_rate(&s->format)));
		}
	}
	/* Return frame */
	return &s->f;
}

void ast_smoother_free(struct ast_smoother *s)
{
	ast_free(s);
}

static struct ast_frame *ast_frame_header_new(void)
{
	struct ast_frame *f;

#if !defined(LOW_MEMORY)
	struct ast_frame_cache *frames;

	if ((frames = ast_threadstorage_get(&frame_cache, sizeof(*frames)))) {
		if ((f = AST_LIST_REMOVE_HEAD(&frames->list, frame_list))) {
			size_t mallocd_len = f->mallocd_hdr_len;
			memset(f, 0, sizeof(*f));
			f->mallocd_hdr_len = mallocd_len;
			f->mallocd = AST_MALLOCD_HDR;
			frames->size--;
			return f;
		}
	}
	if (!(f = ast_calloc_cache(1, sizeof(*f))))
		return NULL;
#else
	if (!(f = ast_calloc(1, sizeof(*f))))
		return NULL;
#endif

	f->mallocd_hdr_len = sizeof(*f);

	return f;
}

#if !defined(LOW_MEMORY)
static void frame_cache_cleanup(void *data)
{
	struct ast_frame_cache *frames = data;
	struct ast_frame *f;

	while ((f = AST_LIST_REMOVE_HEAD(&frames->list, frame_list)))
		ast_free(f);

	ast_free(frames);
}
#endif

static void __frame_free(struct ast_frame *fr, int cache)
{
	if (!fr->mallocd)
		return;

#if !defined(LOW_MEMORY)
	if (cache && fr->mallocd == AST_MALLOCD_HDR) {
		/* Cool, only the header is malloc'd, let's just cache those for now
		 * to keep things simple... */
		struct ast_frame_cache *frames;

		if ((frames = ast_threadstorage_get(&frame_cache, sizeof(*frames))) &&
		    (frames->size < FRAME_CACHE_MAX_SIZE)) {
			AST_LIST_INSERT_HEAD(&frames->list, fr, frame_list);
			frames->size++;
			return;
		}
	}
#endif

	if (fr->mallocd & AST_MALLOCD_DATA) {
		if (fr->data.ptr)
			ast_free(fr->data.ptr - fr->offset);
	}
	if (fr->mallocd & AST_MALLOCD_SRC) {
		if (fr->src)
			ast_free((void *) fr->src);
	}
	if (fr->mallocd & AST_MALLOCD_HDR) {
		ast_free(fr);
	}
}


void ast_frame_free(struct ast_frame *frame, int cache)
{
	struct ast_frame *next;

	for (next = AST_LIST_NEXT(frame, frame_list);
	     frame;
	     frame = next, next = frame ? AST_LIST_NEXT(frame, frame_list) : NULL) {
		__frame_free(frame, cache);
	}
}

void ast_frame_dtor(struct ast_frame *f)
{
	if (f) {
		ast_frfree(f);
	}
}

/*!
 * \brief 'isolates' a frame by duplicating non-malloc'ed components
 * (header, src, data).
 * On return all components are malloc'ed
 */
struct ast_frame *ast_frisolate(struct ast_frame *fr)
{
	struct ast_frame *out;
	void *newdata;

	/* if none of the existing frame is malloc'd, let ast_frdup() do it
	   since it is more efficient
	*/
	if (fr->mallocd == 0) {
		return ast_frdup(fr);
	}

	/* if everything is already malloc'd, we are done */
	if ((fr->mallocd & (AST_MALLOCD_HDR | AST_MALLOCD_SRC | AST_MALLOCD_DATA)) ==
	    (AST_MALLOCD_HDR | AST_MALLOCD_SRC | AST_MALLOCD_DATA)) {
		return fr;
	}

	if (!(fr->mallocd & AST_MALLOCD_HDR)) {
		/* Allocate a new header if needed */
		if (!(out = ast_frame_header_new())) {
			return NULL;
		}
		out->frametype = fr->frametype;
		ast_format_copy(&out->subclass.format, &fr->subclass.format);
		out->datalen = fr->datalen;
		out->samples = fr->samples;
		out->offset = fr->offset;
		/* Copy the timing data */
		ast_copy_flags(out, fr, AST_FLAGS_ALL);
		if (ast_test_flag(fr, AST_FRFLAG_HAS_TIMING_INFO)) {
			out->ts = fr->ts;
			out->len = fr->len;
			out->seqno = fr->seqno;
		}
	} else {
		out = fr;
	}

	if (!(fr->mallocd & AST_MALLOCD_SRC) && fr->src) {
		if (!(out->src = ast_strdup(fr->src))) {
			if (out != fr) {
				ast_free(out);
			}
			return NULL;
		}
	} else {
		out->src = fr->src;
		fr->src = NULL;
		fr->mallocd &= ~AST_MALLOCD_SRC;
	}

	if (!(fr->mallocd & AST_MALLOCD_DATA))  {
		if (!fr->datalen) {
			out->data.uint32 = fr->data.uint32;
			out->mallocd = AST_MALLOCD_HDR | AST_MALLOCD_SRC;
			return out;
		}
		if (!(newdata = ast_malloc(fr->datalen + AST_FRIENDLY_OFFSET))) {
			if (out->src != fr->src) {
				ast_free((void *) out->src);
			}
			if (out != fr) {
				ast_free(out);
			}
			return NULL;
		}
		newdata += AST_FRIENDLY_OFFSET;
		out->offset = AST_FRIENDLY_OFFSET;
		out->datalen = fr->datalen;
		memcpy(newdata, fr->data.ptr, fr->datalen);
		out->data.ptr = newdata;
	} else {
		out->data = fr->data;
		memset(&fr->data, 0, sizeof(fr->data));
		fr->mallocd &= ~AST_MALLOCD_DATA;
	}

	out->mallocd = AST_MALLOCD_HDR | AST_MALLOCD_SRC | AST_MALLOCD_DATA;

	return out;
}

struct ast_frame *ast_frdup(const struct ast_frame *f)
{
	struct ast_frame *out = NULL;
	int len, srclen = 0;
	void *buf = NULL;

#if !defined(LOW_MEMORY)
	struct ast_frame_cache *frames;
#endif

	/* Start with standard stuff */
	len = sizeof(*out) + AST_FRIENDLY_OFFSET + f->datalen;
	/* If we have a source, add space for it */
	/*
	 * XXX Watch out here - if we receive a src which is not terminated
	 * properly, we can be easily attacked. Should limit the size we deal with.
	 */
	if (f->src)
		srclen = strlen(f->src);
	if (srclen > 0)
		len += srclen + 1;

#if !defined(LOW_MEMORY)
	if ((frames = ast_threadstorage_get(&frame_cache, sizeof(*frames)))) {
		AST_LIST_TRAVERSE_SAFE_BEGIN(&frames->list, out, frame_list) {
			if (out->mallocd_hdr_len >= len) {
				size_t mallocd_len = out->mallocd_hdr_len;

				AST_LIST_REMOVE_CURRENT(frame_list);
				memset(out, 0, sizeof(*out));
				out->mallocd_hdr_len = mallocd_len;
				buf = out;
				frames->size--;
				break;
			}
		}
		AST_LIST_TRAVERSE_SAFE_END;
	}
#endif

	if (!buf) {
		if (!(buf = ast_calloc_cache(1, len)))
			return NULL;
		out = buf;
		out->mallocd_hdr_len = len;
	}

	out->frametype = f->frametype;
	ast_format_copy(&out->subclass.format, &f->subclass.format);
	out->datalen = f->datalen;
	out->samples = f->samples;
	out->delivery = f->delivery;
	/* Even though this new frame was allocated from the heap, we can't mark it
	 * with AST_MALLOCD_HDR, AST_MALLOCD_DATA and AST_MALLOCD_SRC, because that
	 * would cause ast_frfree() to attempt to individually free each of those
	 * under the assumption that they were separately allocated. Since this frame
	 * was allocated in a single allocation, we'll only mark it as if the header
	 * was heap-allocated; this will result in the entire frame being properly freed.
	 */
	out->mallocd = AST_MALLOCD_HDR;
	out->offset = AST_FRIENDLY_OFFSET;
	if (out->datalen) {
		out->data.ptr = buf + sizeof(*out) + AST_FRIENDLY_OFFSET;
		memcpy(out->data.ptr, f->data.ptr, out->datalen);
	} else {
		out->data.uint32 = f->data.uint32;
	}
	if (srclen > 0) {
		/* This may seem a little strange, but it's to avoid a gcc (4.2.4) compiler warning */
		char *src;
		out->src = buf + sizeof(*out) + AST_FRIENDLY_OFFSET + f->datalen;
		src = (char *) out->src;
		/* Must have space since we allocated for it */
		strcpy(src, f->src);
	}
	ast_copy_flags(out, f, AST_FLAGS_ALL);
	out->ts = f->ts;
	out->len = f->len;
	out->seqno = f->seqno;
	return out;
}

void ast_swapcopy_samples(void *dst, const void *src, int samples)
{
	int i;
	unsigned short *dst_s = dst;
	const unsigned short *src_s = src;

	for (i = 0; i < samples; i++)
		dst_s[i] = (src_s[i]<<8) | (src_s[i]>>8);
}

void ast_frame_subclass2str(struct ast_frame *f, char *subclass, size_t slen, char *moreinfo, size_t mlen)
{
	switch(f->frametype) {
	case AST_FRAME_DTMF_BEGIN:
		if (slen > 1) {
			subclass[0] = f->subclass.integer;
			subclass[1] = '\0';
		}
		break;
	case AST_FRAME_DTMF_END:
		if (slen > 1) {
			subclass[0] = f->subclass.integer;
			subclass[1] = '\0';
		}
		break;
	case AST_FRAME_CONTROL:
		switch (f->subclass.integer) {
		case AST_CONTROL_HANGUP:
			ast_copy_string(subclass, "Hangup", slen);
			break;
		case AST_CONTROL_RING:
			ast_copy_string(subclass, "Ring", slen);
			break;
		case AST_CONTROL_RINGING:
			ast_copy_string(subclass, "Ringing", slen);
			break;
		case AST_CONTROL_ANSWER:
			ast_copy_string(subclass, "Answer", slen);
			break;
		case AST_CONTROL_BUSY:
			ast_copy_string(subclass, "Busy", slen);
			break;
		case AST_CONTROL_TAKEOFFHOOK:
			ast_copy_string(subclass, "Take Off Hook", slen);
			break;
		case AST_CONTROL_OFFHOOK:
			ast_copy_string(subclass, "Line Off Hook", slen);
			break;
		case AST_CONTROL_CONGESTION:
			ast_copy_string(subclass, "Congestion", slen);
			break;
		case AST_CONTROL_FLASH:
			ast_copy_string(subclass, "Flash", slen);
			break;
		case AST_CONTROL_WINK:
			ast_copy_string(subclass, "Wink", slen);
			break;
		case AST_CONTROL_OPTION:
			ast_copy_string(subclass, "Option", slen);
			break;
		case AST_CONTROL_RADIO_KEY:
			ast_copy_string(subclass, "Key Radio", slen);
			break;
		case AST_CONTROL_RADIO_UNKEY:
			ast_copy_string(subclass, "Unkey Radio", slen);
			break;
		case AST_CONTROL_HOLD:
			ast_copy_string(subclass, "Hold", slen);
			break;
		case AST_CONTROL_UNHOLD:
			ast_copy_string(subclass, "Unhold", slen);
			break;
		case AST_CONTROL_T38_PARAMETERS: {
			char *message = "Unknown";
			if (f->datalen != sizeof(struct ast_control_t38_parameters)) {
				message = "Invalid";
			} else {
				struct ast_control_t38_parameters *parameters = f->data.ptr;
				enum ast_control_t38 state = parameters->request_response;
				if (state == AST_T38_REQUEST_NEGOTIATE)
					message = "Negotiation Requested";
				else if (state == AST_T38_REQUEST_TERMINATE)
					message = "Negotiation Request Terminated";
				else if (state == AST_T38_NEGOTIATED)
					message = "Negotiated";
				else if (state == AST_T38_TERMINATED)
					message = "Terminated";
				else if (state == AST_T38_REFUSED)
					message = "Refused";
			}
			snprintf(subclass, slen, "T38_Parameters/%s", message);
			break;
		}
		case -1:
			ast_copy_string(subclass, "Stop generators", slen);
			break;
		default:
			snprintf(subclass, slen, "Unknown control '%d'", f->subclass.integer);
		}
		break;
	case AST_FRAME_NULL:
		ast_copy_string(subclass, "N/A", slen);
		break;
	case AST_FRAME_IAX:
		/* Should never happen */
		snprintf(subclass, slen, "IAX Frametype %d", f->subclass.integer);
		break;
	case AST_FRAME_BRIDGE_ACTION:
		/* Should never happen */
		snprintf(subclass, slen, "Bridge Frametype %d", f->subclass.integer);
		break;
	case AST_FRAME_BRIDGE_ACTION_SYNC:
		/* Should never happen */
		snprintf(subclass, slen, "Synchronous Bridge Frametype %d", f->subclass.integer);
		break;
	case AST_FRAME_TEXT:
		ast_copy_string(subclass, "N/A", slen);
		if (moreinfo) {
			ast_copy_string(moreinfo, f->data.ptr, mlen);
		}
		break;
	case AST_FRAME_IMAGE:
		snprintf(subclass, slen, "Image format %s\n", ast_getformatname(&f->subclass.format));
		break;
	case AST_FRAME_HTML:
		switch (f->subclass.integer) {
		case AST_HTML_URL:
			ast_copy_string(subclass, "URL", slen);
			if (moreinfo) {
				ast_copy_string(moreinfo, f->data.ptr, mlen);
			}
			break;
		case AST_HTML_DATA:
			ast_copy_string(subclass, "Data", slen);
			break;
		case AST_HTML_BEGIN:
			ast_copy_string(subclass, "Begin", slen);
			break;
		case AST_HTML_END:
			ast_copy_string(subclass, "End", slen);
			break;
		case AST_HTML_LDCOMPLETE:
			ast_copy_string(subclass, "Load Complete", slen);
			break;
		case AST_HTML_NOSUPPORT:
			ast_copy_string(subclass, "No Support", slen);
			break;
		case AST_HTML_LINKURL:
			ast_copy_string(subclass, "Link URL", slen);
			if (moreinfo) {
				ast_copy_string(moreinfo, f->data.ptr, mlen);
			}
			break;
		case AST_HTML_UNLINK:
			ast_copy_string(subclass, "Unlink", slen);
			break;
		case AST_HTML_LINKREJECT:
			ast_copy_string(subclass, "Link Reject", slen);
			break;
		default:
			snprintf(subclass, slen, "Unknown HTML frame '%d'\n", f->subclass.integer);
			break;
		}
		break;
	case AST_FRAME_MODEM:
		switch (f->subclass.integer) {
		case AST_MODEM_T38:
			ast_copy_string(subclass, "T.38", slen);
			break;
		case AST_MODEM_V150:
			ast_copy_string(subclass, "V.150", slen);
			break;
		default:
			snprintf(subclass, slen, "Unknown MODEM frame '%d'\n", f->subclass.integer);
			break;
		}
		break;
	default:
		ast_copy_string(subclass, "Unknown Subclass", slen);
		break;
	}
}

void ast_frame_type2str(enum ast_frame_type frame_type, char *ftype, size_t len)
{
	switch (frame_type) {
	case AST_FRAME_DTMF_BEGIN:
		ast_copy_string(ftype, "DTMF Begin", len);
		break;
	case AST_FRAME_DTMF_END:
		ast_copy_string(ftype, "DTMF End", len);
		break;
	case AST_FRAME_CONTROL:
		ast_copy_string(ftype, "Control", len);
		break;
	case AST_FRAME_NULL:
		ast_copy_string(ftype, "Null Frame", len);
		break;
	case AST_FRAME_IAX:
		/* Should never happen */
		ast_copy_string(ftype, "IAX Specific", len);
		break;
	case AST_FRAME_BRIDGE_ACTION:
		/* Should never happen */
		ast_copy_string(ftype, "Bridge Specific", len);
		break;
	case AST_FRAME_BRIDGE_ACTION_SYNC:
		/* Should never happen */
		ast_copy_string(ftype, "Bridge Specific", len);
		break;
	case AST_FRAME_TEXT:
		ast_copy_string(ftype, "Text", len);
		break;
	case AST_FRAME_IMAGE:
		ast_copy_string(ftype, "Image", len);
		break;
	case AST_FRAME_HTML:
		ast_copy_string(ftype, "HTML", len);
		break;
	case AST_FRAME_MODEM:
		ast_copy_string(ftype, "Modem", len);
		break;
	case AST_FRAME_VOICE:
		ast_copy_string(ftype, "Voice", len);
		break;
	case AST_FRAME_VIDEO:
		ast_copy_string(ftype, "Video", len);
		break;
	default:
		snprintf(ftype, len, "Unknown Frametype '%u'", frame_type);
		break;
	}
}

/*! Dump a frame for debugging purposes */
void ast_frame_dump(const char *name, struct ast_frame *f, char *prefix)
{
	const char noname[] = "unknown";
	char ftype[40] = "Unknown Frametype";
	char cft[80];
	char subclass[40] = "Unknown Subclass";
	char csub[80];
	char moreinfo[40] = "";
	char cn[60];
	char cp[40];
	char cmn[40];

	if (!name) {
		name = noname;
	}

	if (!f) {
		ast_verb(-1, "%s [ %s (NULL) ] [%s]\n",
			term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			term_color(cft, "HANGUP", COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
			term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
		return;
	}
	/* XXX We should probably print one each of voice and video when the format changes XXX */
	if (f->frametype == AST_FRAME_VOICE) {
		return;
	}
	if (f->frametype == AST_FRAME_VIDEO) {
		return;
	}

	ast_frame_type2str(f->frametype, ftype, sizeof(ftype));
	ast_frame_subclass2str(f, subclass, sizeof(subclass), moreinfo, sizeof(moreinfo));

	if (!ast_strlen_zero(moreinfo))
		ast_verb(-1, "%s [ TYPE: %s (%u) SUBCLASS: %s (%d) '%s' ] [%s]\n",
			    term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			    term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
			    f->frametype,
			    term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
			    f->subclass.integer,
			    term_color(cmn, moreinfo, COLOR_BRGREEN, COLOR_BLACK, sizeof(cmn)),
			    term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
	else
		ast_verb(-1, "%s [ TYPE: %s (%u) SUBCLASS: %s (%d) ] [%s]\n",
			    term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			    term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
			    f->frametype,
			    term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
			    f->subclass.integer,
			    term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
}

int ast_parse_allow_disallow(struct ast_codec_pref *pref, struct ast_format_cap *cap, const char *list, int allowing)
{
	int errors = 0, framems = 0, all = 0, iter_allowing;
	char *parse = NULL, *this = NULL, *psize = NULL;
	struct ast_format format;

	parse = ast_strdupa(list);
	while ((this = strsep(&parse, ","))) {
		iter_allowing = allowing;
		framems = 0;
		if (*this == '!') {
			this++;
			iter_allowing = !allowing;
		}
		if ((psize = strrchr(this, ':'))) {
			*psize++ = '\0';
			ast_debug(1, "Packetization for codec: %s is %s\n", this, psize);
			framems = atoi(psize);
			if (framems < 0) {
				framems = 0;
				errors++;
				ast_log(LOG_WARNING, "Bad packetization value for codec %s\n", this);
			}
		}
		all = strcasecmp(this, "all") ? 0 : 1;

		if (!all && !ast_getformatbyname(this, &format)) {
			ast_log(LOG_WARNING, "Cannot %s unknown format '%s'\n", iter_allowing ? "allow" : "disallow", this);
			errors++;
			continue;
		}

		if (cap) {
			if (iter_allowing) {
				if (all) {
					ast_format_cap_add_all(cap);
				} else {
					ast_format_cap_add(cap, &format);
				}
			} else {
				if (all) {
					ast_format_cap_remove_all(cap);
				} else {
					ast_format_cap_remove(cap, &format);
				}
			}
		}

		if (pref) {
			if (!all) {
				if (iter_allowing) {
					ast_codec_pref_append(pref, &format);
					ast_codec_pref_setsize(pref, &format, framems);
				} else {
					ast_codec_pref_remove(pref, &format);
				}
			} else if (!iter_allowing) {
				memset(pref, 0, sizeof(*pref));
			} else {
				ast_codec_pref_append_all(pref);
			}
		}
	}
	return errors;
}

static int g723_len(unsigned char buf)
{
	enum frame_type type = buf & TYPE_MASK;

	switch(type) {
	case TYPE_DONTSEND:
		return 0;
		break;
	case TYPE_SILENCE:
		return 4;
		break;
	case TYPE_HIGH:
		return 24;
		break;
	case TYPE_LOW:
		return 20;
		break;
	default:
		ast_log(LOG_WARNING, "Badly encoded frame (%u)\n", type);
	}
	return -1;
}

static int g723_samples(unsigned char *buf, int maxlen)
{
	int pos = 0;
	int samples = 0;
	int res;
	while(pos < maxlen) {
		res = g723_len(buf[pos]);
		if (res <= 0)
			break;
		samples += 240;
		pos += res;
	}
	return samples;
}

static unsigned char get_n_bits_at(unsigned char *data, int n, int bit)
{
	int byte = bit / 8;       /* byte containing first bit */
	int rem = 8 - (bit % 8);  /* remaining bits in first byte */
	unsigned char ret = 0;

	if (n <= 0 || n > 8)
		return 0;

	if (rem < n) {
		ret = (data[byte] << (n - rem));
		ret |= (data[byte + 1] >> (8 - n + rem));
	} else {
		ret = (data[byte] >> (rem - n));
	}

	return (ret & (0xff >> (8 - n)));
}

static int speex_get_wb_sz_at(unsigned char *data, int len, int bit)
{
	static const int SpeexWBSubModeSz[] = {
		4, 36, 112, 192,
		352, 0, 0, 0 };
	int off = bit;
	unsigned char c;

	/* skip up to two wideband frames */
	if (((len * 8 - off) >= 5) &&
		get_n_bits_at(data, 1, off)) {
		c = get_n_bits_at(data, 3, off + 1);
		off += SpeexWBSubModeSz[c];

		if (((len * 8 - off) >= 5) &&
			get_n_bits_at(data, 1, off)) {
			c = get_n_bits_at(data, 3, off + 1);
			off += SpeexWBSubModeSz[c];

			if (((len * 8 - off) >= 5) &&
				get_n_bits_at(data, 1, off)) {
				ast_log(LOG_WARNING, "Encountered corrupt speex frame; too many wideband frames in a row.\n");
				return -1;
			}
		}

	}
	return off - bit;
}

static int speex_samples(unsigned char *data, int len)
{
	static const int SpeexSubModeSz[] = {
		5, 43, 119, 160,
		220, 300, 364, 492,
		79, 0, 0, 0,
		0, 0, 0, 0 };
	static const int SpeexInBandSz[] = {
		1, 1, 4, 4,
		4, 4, 4, 4,
		8, 8, 16, 16,
		32, 32, 64, 64 };
	int bit = 0;
	int cnt = 0;
	int off;
	unsigned char c;

	while ((len * 8 - bit) >= 5) {
		/* skip wideband frames */
		off = speex_get_wb_sz_at(data, len, bit);
		if (off < 0)  {
			ast_log(LOG_WARNING, "Had error while reading wideband frames for speex samples\n");
			break;
		}
		bit += off;

		if ((len * 8 - bit) < 5)
			break;

		/* get control bits */
		c = get_n_bits_at(data, 5, bit);
		bit += 5;

		if (c == 15) {
			/* terminator */
			break;
		} else if (c == 14) {
			/* in-band signal; next 4 bits contain signal id */
			c = get_n_bits_at(data, 4, bit);
			bit += 4;
			bit += SpeexInBandSz[c];
		} else if (c == 13) {
			/* user in-band; next 4 bits contain msg len */
			c = get_n_bits_at(data, 4, bit);
			bit += 4;
			/* after which it's 5-bit signal id + c bytes of data */
			bit += 5 + c * 8;
		} else if (c > 8) {
			/* unknown */
			ast_log(LOG_WARNING, "Unknown speex control frame %d\n", c);
			break;
		} else {
			/* skip number bits for submode (less the 5 control bits) */
			bit += SpeexSubModeSz[c] - 5;
			cnt += 160; /* new frame */
		}
	}
	return cnt;
}

int ast_codec_get_samples(struct ast_frame *f)
{
	int samples = 0;

	switch (f->subclass.format.id) {
	case AST_FORMAT_SPEEX:
		samples = speex_samples(f->data.ptr, f->datalen);
		break;
	case AST_FORMAT_SPEEX16:
		samples = 2 * speex_samples(f->data.ptr, f->datalen);
		break;
	case AST_FORMAT_SPEEX32:
		samples = 4 * speex_samples(f->data.ptr, f->datalen);
		break;
	case AST_FORMAT_G723_1:
		samples = g723_samples(f->data.ptr, f->datalen);
		break;
	case AST_FORMAT_ILBC:
		samples = 240 * (f->datalen / 50);
		break;
	case AST_FORMAT_GSM:
		samples = 160 * (f->datalen / 33);
		break;
	case AST_FORMAT_G729A:
		samples = f->datalen * 8;
		break;
	case AST_FORMAT_SLINEAR:
	case AST_FORMAT_SLINEAR16:
		samples = f->datalen / 2;
		break;
	case AST_FORMAT_LPC10:
		/* assumes that the RTP packet contains one LPC10 frame */
		samples = 22 * 8;
		samples += (((char *)(f->data.ptr))[7] & 0x1) * 8;
		break;
	case AST_FORMAT_ULAW:
	case AST_FORMAT_ALAW:
	case AST_FORMAT_TESTLAW:
		samples = f->datalen;
		break;
	case AST_FORMAT_G722:
	case AST_FORMAT_ADPCM:
	case AST_FORMAT_G726:
	case AST_FORMAT_G726_AAL2:
		samples = f->datalen * 2;
		break;
	case AST_FORMAT_SIREN7:
		/* 16,000 samples per second at 32kbps is 4,000 bytes per second */
		samples = f->datalen * (16000 / 4000);
		break;
	case AST_FORMAT_SIREN14:
		/* 32,000 samples per second at 48kbps is 6,000 bytes per second */
		samples = (int) f->datalen * ((float) 32000 / 6000);
		break;
	case AST_FORMAT_G719:
		/* 48,000 samples per second at 64kbps is 8,000 bytes per second */
		samples = (int) f->datalen * ((float) 48000 / 8000);
		break;
	case AST_FORMAT_SILK:
		if (!(ast_format_isset(&f->subclass.format,
			SILK_ATTR_KEY_SAMP_RATE,
			SILK_ATTR_VAL_SAMP_24KHZ,
			AST_FORMAT_ATTR_END))) {
			return 480;
		} else if (!(ast_format_isset(&f->subclass.format,
			SILK_ATTR_KEY_SAMP_RATE,
			SILK_ATTR_VAL_SAMP_16KHZ,
			AST_FORMAT_ATTR_END))) {
			return 320;
		} else if (!(ast_format_isset(&f->subclass.format,
			SILK_ATTR_KEY_SAMP_RATE,
			SILK_ATTR_VAL_SAMP_12KHZ,
			AST_FORMAT_ATTR_END))) {
			return 240;
		} else {
			return 160;
		}
	case AST_FORMAT_CELT:
		/* TODO This assumes 20ms delivery right now, which is incorrect */
		samples = ast_format_rate(&f->subclass.format) / 50;
		break;
	case AST_FORMAT_OPUS:
		/* TODO This assumes 20ms delivery right now, which is incorrect */
		samples = 960;
		break;
	default:
		ast_log(LOG_WARNING, "Unable to calculate samples for format %s\n", ast_getformatname(&f->subclass.format));
	}
	return samples;
}

int ast_codec_get_len(struct ast_format *format, int samples)
{
	int len = 0;

	/* XXX Still need speex, and lpc10 XXX */
	switch(format->id) {
	case AST_FORMAT_G723_1:
		len = (samples / 240) * 20;
		break;
	case AST_FORMAT_ILBC:
		len = (samples / 240) * 50;
		break;
	case AST_FORMAT_GSM:
		len = (samples / 160) * 33;
		break;
	case AST_FORMAT_G729A:
		len = samples / 8;
		break;
	case AST_FORMAT_SLINEAR:
	case AST_FORMAT_SLINEAR16:
		len = samples * 2;
		break;
	case AST_FORMAT_ULAW:
	case AST_FORMAT_ALAW:
	case AST_FORMAT_TESTLAW:
		len = samples;
		break;
	case AST_FORMAT_G722:
	case AST_FORMAT_ADPCM:
	case AST_FORMAT_G726:
	case AST_FORMAT_G726_AAL2:
		len = samples / 2;
		break;
	case AST_FORMAT_SIREN7:
		/* 16,000 samples per second at 32kbps is 4,000 bytes per second */
		len = samples / (16000 / 4000);
		break;
	case AST_FORMAT_SIREN14:
		/* 32,000 samples per second at 48kbps is 6,000 bytes per second */
		len = (int) samples / ((float) 32000 / 6000);
		break;
	case AST_FORMAT_G719:
		/* 48,000 samples per second at 64kbps is 8,000 bytes per second */
		len = (int) samples / ((float) 48000 / 8000);
		break;
	default:
		ast_log(LOG_WARNING, "Unable to calculate sample length for format %s\n", ast_getformatname(format));
	}

	return len;
}

int ast_frame_adjust_volume(struct ast_frame *f, int adjustment)
{
	int count;
	short *fdata = f->data.ptr;
	short adjust_value = abs(adjustment);

	if ((f->frametype != AST_FRAME_VOICE) || !(ast_format_is_slinear(&f->subclass.format))) {
		return -1;
	}

	if (!adjustment) {
		return 0;
	}

	for (count = 0; count < f->samples; count++) {
		if (adjustment > 0) {
			ast_slinear_saturated_multiply(&fdata[count], &adjust_value);
		} else if (adjustment < 0) {
			ast_slinear_saturated_divide(&fdata[count], &adjust_value);
		}
	}

	return 0;
}

int ast_frame_slinear_sum(struct ast_frame *f1, struct ast_frame *f2)
{
	int count;
	short *data1, *data2;

	if ((f1->frametype != AST_FRAME_VOICE) || (f1->subclass.format.id != AST_FORMAT_SLINEAR))
		return -1;

	if ((f2->frametype != AST_FRAME_VOICE) || (f2->subclass.format.id != AST_FORMAT_SLINEAR))
		return -1;

	if (f1->samples != f2->samples)
		return -1;

	for (count = 0, data1 = f1->data.ptr, data2 = f2->data.ptr;
	     count < f1->samples;
	     count++, data1++, data2++)
		ast_slinear_saturated_add(data1, data2);

	return 0;
}

int ast_frame_clear(struct ast_frame *frame)
{
	struct ast_frame *next;

	for (next = AST_LIST_NEXT(frame, frame_list);
		 frame;
		 frame = next, next = frame ? AST_LIST_NEXT(frame, frame_list) : NULL) {
		memset(frame->data.ptr, 0, frame->datalen);
	}
	return 0;
}
