#include "fitz.h"
#include "mupdf.h"

#define TILE

typedef struct pdf_material_s pdf_material;
typedef struct pdf_gstate_s pdf_gstate;
typedef struct pdf_csi_s pdf_csi;

enum
{
	PDF_FILL,
	PDF_STROKE,
};

enum
{
	PDF_MAT_NONE,
	PDF_MAT_COLOR,
	PDF_MAT_PATTERN,
	PDF_MAT_SHADE,
};

struct pdf_material_s
{
	int kind;
	fz_colorspace *colorspace;
	pdf_pattern *pattern;
	fz_shade *shade;
	float alpha;
	float v[32];
};

struct pdf_gstate_s
{
	fz_matrix ctm;
	int clip_depth;

	/* path stroking */
	fz_stroke_state stroke_state;

	/* materials */
	pdf_material stroke;
	pdf_material fill;

	/* text state */
	float char_space;
	float word_space;
	float scale;
	float leading;
	pdf_font_desc *font;
	float size;
	int render;
	float rise;

	/* transparency */
	int blendmode;
	pdf_xobject *softmask;
	fz_matrix softmask_ctm;
	float softmask_bc[FZ_MAX_COLORS];
	int luminosity;
};

struct pdf_csi_s
{
	fz_device *dev;
	pdf_xref *xref;

	/* usage mode for optional content groups */
	char *event; /* "View", "Print", "Export" */

	/* interpreter stack */
	fz_obj *obj;
	char name[256];
	unsigned char string[256];
	int string_len;
	float stack[32];
	int top;

	int xbalance;
	int in_text;
	int in_hidden_ocg;

	/* path object state */
	fz_path *path;
	int clip;
	int clip_even_odd;

	/* text object state */
	fz_text *text;
	fz_matrix tlm;
	fz_matrix tm;
	int text_mode;
	int accumulate;

	/* graphics state */
	fz_matrix top_ctm;
	/* SumatraPDF: make gstate growable for rendering tikz-qtree documents */
	pdf_gstate *gstate;
	int gcap;
	int gtop;
};

static fz_error pdf_run_buffer(pdf_csi *csi, fz_obj *rdb, fz_buffer *contents);
static fz_error pdf_run_xobject(pdf_csi *csi, fz_obj *resources, pdf_xobject *xobj, fz_matrix transform);
static void pdf_show_pattern(pdf_csi *csi, pdf_pattern *pat, fz_rect area, int what);

static int
ocg_intents_include(fz_context *ctx, pdf_ocg_descriptor *desc, char *name)
{
	int i, len;

	if (strcmp(name, "All") == 0)
			return 1;

	/* In the absence of a specified intent, it's 'View' */
	if (desc->intent == NULL)
		return (strcmp(name, "View") == 0);

	if (fz_is_name(ctx, desc->intent))
	{
		char *intent = fz_to_name(ctx, desc->intent);
		if (strcmp(intent, "All") == 0)
			return 1;
		return (strcmp(intent, name) == 0);
	}
	if (!fz_is_array(ctx, desc->intent))
		return 0;

	len = fz_array_len(ctx, desc->intent);
	for (i=0; i < len; i++)
	{
		char *intent = fz_to_name(ctx, fz_array_get(ctx, desc->intent, i));
		if (strcmp(intent, "All") == 0)
			return 1;
		if (strcmp(intent, name) == 0)
			return 1;
	}
	return 0;
}

static int
pdf_is_hidden_ocg(fz_obj *ocg, pdf_csi *csi, fz_obj *rdb)
{
	char event_state[16];
	fz_obj *obj, *obj2;
	char *type;
	pdf_ocg_descriptor *desc = csi->xref->ocg;
	fz_context *ctx = csi->xref->ctx;

	/* If no ocg descriptor, everything is visible */
	if (desc == NULL)
		return 0;

	/* If we've been handed a name, look it up in the properties. */
	if (fz_is_name(ctx, ocg))
	{
		ocg = fz_dict_gets(ctx, fz_dict_gets(ctx, rdb, "Properties"), fz_to_name(ctx, ocg));
	}
	/* If we haven't been given an ocg at all, then we're visible */
	if (ocg == NULL)
		return 0;

	fz_strlcpy(event_state, csi->event, sizeof event_state);
	fz_strlcat(event_state, "State", sizeof event_state);

	type = fz_to_name(ctx, fz_dict_gets(ctx, ocg, "Type"));

	if (strcmp(type, "OCG") == 0)
	{
		/* An Optional Content Group */
		int num = fz_to_num(ocg);
		int gen = fz_to_gen(ocg);
		int len = desc->len;
		int i;

		for (i = 0; i < len; i++)
		{
			if (desc->ocgs[i].num == num && desc->ocgs[i].gen == gen)
			{
				if (desc->ocgs[i].state == 0)
					return 1; /* If off, hidden */
				break;
			}
}

		/* Check Intents; if our intent is not part of the set given
		 * by the current config, we should ignore it. */
		obj = fz_dict_gets(ctx, ocg, "Intent");
		if (fz_is_name(ctx, obj))
{
			/* If it doesn't match, it's hidden */
			if (ocg_intents_include(ctx, desc, fz_to_name(ctx, obj)) == 0)
				return 1;
		}
		else if (fz_is_array(ctx, obj))
		{
			int match = 0;
			len = fz_array_len(ctx, obj);
			for (i=0; i<len; i++) {
				match |= ocg_intents_include(ctx, desc, fz_to_name(ctx, fz_array_get(ctx, obj, i)));
				if (match)
					break;
			}
			/* If we don't match any, it's hidden */
			if (match == 0)
				return 1;
		}
		else
		{
			/* If it doesn't match, it's hidden */
			if (ocg_intents_include(ctx, desc, "View") == 0)
				return 1;
		}

		/* FIXME: Currently we do a very simple check whereby we look
		 * at the Usage object (an Optional Content Usage Dictionary)
		 * and check to see if the corresponding 'event' key is on
		 * or off.
		 *
		 * Really we should only look at Usage dictionaries that
		 * correspond to entries in the AS list in the OCG config.
		 * Given that we don't handle Zoom or User, or Language
		 * dicts, this is not really a problem. */
		obj = fz_dict_gets(ctx, ocg, "Usage");
		if (!fz_is_dict(ctx, obj))
			return 0;
		/* FIXME: Should look at Zoom (and return hidden if out of
		 * max/min range) */
		/* FIXME: Could provide hooks to the caller to check if
		 * User is appropriate - if not return hidden. */
		obj2 = fz_dict_gets(ctx, obj, csi->event);
		if (strcmp(fz_to_name(ctx, fz_dict_gets(ctx, obj2, event_state)), "OFF") == 0)
		{
			return 1;
		}
		return 0;
	}
	else if (strcmp(type, "OCMD") == 0)
	{
		/* An Optional Content Membership Dictionary */
		char *name;
		int combine, on;

		obj = fz_dict_gets(ctx, ocg, "VE");
		if (fz_is_array(ctx, obj)) {
			/* FIXME: Calculate visibility from array */
			return 0;
		}
		name = fz_to_name(ctx, fz_dict_gets(ctx, ocg, "P"));
		/* Set combine; Bit 0 set => AND, Bit 1 set => true means
		 * Off, otherwise true means On */
		if (strcmp(name, "AllOn") == 0)
		{
			combine = 1;
		}
		else if (strcmp(name, "AnyOff") == 0)
		{
			combine = 2;
		}
		else if (strcmp(name, "AllOff") == 0)
		{
			combine = 3;
		}
		else	/* Assume it's the default (AnyOn) */
		{
			combine = 0;
		}

		obj = fz_dict_gets(ctx, ocg, "OCGs");
		on = combine & 1;
		if (fz_is_array(ctx, obj)) {
			int i, len;
			len = fz_array_len(ctx, obj);
			for (i = 0; i < len; i++)
			{
				int hidden;
				hidden = pdf_is_hidden_ocg(fz_array_get(ctx, obj, i), csi, rdb);
				if ((combine & 1) == 0)
					hidden = !hidden;
				if (combine & 2)
					on &= hidden;
				else
					on |= hidden;
			}
		}
		else
		{
			on = pdf_is_hidden_ocg(obj, csi, rdb);
			if ((combine & 1) == 0)
				on = !on;
		}
		return !on;
	}
	/* No idea what sort of object this is - be visible */
	return 0;
}

/*
 * Emit graphics calls to device.
 */

static void
pdf_begin_group(pdf_csi *csi, fz_rect bbox)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_error error;

	if (gstate->softmask)
	{
		pdf_xobject *softmask = gstate->softmask;
		fz_rect bbox = fz_transform_rect(gstate->softmask_ctm, softmask->bbox);
		fz_matrix save_ctm = gstate->ctm;

		gstate->softmask = NULL;
		gstate->ctm = gstate->softmask_ctm;

		fz_begin_mask(csi->dev, bbox, gstate->luminosity,
			softmask->colorspace, gstate->softmask_bc);
		error = pdf_run_xobject(csi, NULL, softmask, fz_identity);
		if (error)
			fz_error_handle(csi->dev->ctx, error, "cannot run softmask");
		fz_end_mask(csi->dev);

		gstate->softmask = softmask;
		gstate->ctm = save_ctm;
	}

	if (gstate->blendmode)
		fz_begin_group(csi->dev, bbox, 1, 0, gstate->blendmode, 1);
}

static void
pdf_end_group(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;

	if (gstate->blendmode)
		fz_end_group(csi->dev);

	if (gstate->softmask)
		fz_pop_clip(csi->dev);
}

static void
pdf_show_shade(pdf_csi *csi, fz_shade *shd)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_rect bbox;

	if (csi->in_hidden_ocg > 0)
		return;

	bbox = fz_bound_shade(shd, gstate->ctm);

	pdf_begin_group(csi, bbox);

	fz_fill_shade(csi->dev, shd, gstate->ctm, gstate->fill.alpha);

	pdf_end_group(csi);
}

static void
pdf_show_image(pdf_csi *csi, fz_pixmap *image)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_rect bbox;

	if (csi->in_hidden_ocg > 0)
		return;

	bbox = fz_transform_rect(gstate->ctm, fz_unit_rect);

	if (image->mask)
	{
		/* apply blend group even though we skip the softmask */
		if (gstate->blendmode)
			fz_begin_group(csi->dev, bbox, 0, 0, gstate->blendmode, 1);
		fz_clip_image_mask(csi->dev, image->mask, &bbox, gstate->ctm);
	}
	else
		pdf_begin_group(csi, bbox);

	if (!image->colorspace)
	{

		switch (gstate->fill.kind)
		{
		case PDF_MAT_NONE:
			break;
		case PDF_MAT_COLOR:
			fz_fill_image_mask(csi->dev, image, gstate->ctm,
				gstate->fill.colorspace, gstate->fill.v, gstate->fill.alpha);
			break;
		case PDF_MAT_PATTERN:
			if (gstate->fill.pattern)
			{
				fz_clip_image_mask(csi->dev, image, &bbox, gstate->ctm);
				pdf_show_pattern(csi, gstate->fill.pattern, bbox, PDF_FILL);
				fz_pop_clip(csi->dev);
			}
			break;
		case PDF_MAT_SHADE:
			if (gstate->fill.shade)
			{
				fz_clip_image_mask(csi->dev, image, &bbox, gstate->ctm);
				fz_fill_shade(csi->dev, gstate->fill.shade, gstate->ctm, gstate->fill.alpha);
				fz_pop_clip(csi->dev);
			}
			break;
		}
	}
	else
	{
		fz_fill_image(csi->dev, image, gstate->ctm, gstate->fill.alpha);
	}

	if (image->mask)
	{
		fz_pop_clip(csi->dev);
		if (gstate->blendmode)
			fz_end_group(csi->dev);
	}
	else
		pdf_end_group(csi);
}

static void
pdf_show_path(pdf_csi *csi, int doclose, int dofill, int dostroke, int even_odd)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_path *path;
	fz_rect bbox;
	fz_context *ctx = csi->dev->ctx;

	path = csi->path;
	csi->path = fz_new_path(ctx);

	if (doclose)
		fz_closepath(path);

	if (dostroke)
		bbox = fz_bound_path(path, &gstate->stroke_state, gstate->ctm);
	else
		bbox = fz_bound_path(path, NULL, gstate->ctm);

	if (csi->clip)
	{
		gstate->clip_depth++;
		fz_clip_path(csi->dev, path, NULL, csi->clip_even_odd, gstate->ctm);
		csi->clip = 0;
	}

	if (csi->in_hidden_ocg > 0)
		dostroke = dofill = 0;

	if (dofill || dostroke)
		pdf_begin_group(csi, bbox);

	if (dofill)
	{
		switch (gstate->fill.kind)
		{
		case PDF_MAT_NONE:
			break;
		case PDF_MAT_COLOR:
			// cf. http://code.google.com/p/sumatrapdf/issues/detail?id=966
			if (6 <= path->len && path->len <= 7 && path->items[0].k == FZ_MOVETO && path->items[3].k == FZ_LINETO)
			{
				fz_stroke_state state = { 0 };
				state.linewidth = 0.1f / fz_matrix_expansion(gstate->ctm);
				fz_stroke_path(csi->dev, path, &state, gstate->ctm,
					gstate->fill.colorspace, gstate->fill.v, gstate->fill.alpha);
				break;
			}
			fz_fill_path(csi->dev, path, even_odd, gstate->ctm,
				gstate->fill.colorspace, gstate->fill.v, gstate->fill.alpha);
			break;
		case PDF_MAT_PATTERN:
			if (gstate->fill.pattern)
			{
				fz_clip_path(csi->dev, path, NULL, even_odd, gstate->ctm);
				pdf_show_pattern(csi, gstate->fill.pattern, bbox, PDF_FILL);
				fz_pop_clip(csi->dev);
			}
			break;
		case PDF_MAT_SHADE:
			if (gstate->fill.shade)
			{
				fz_clip_path(csi->dev, path, NULL, even_odd, gstate->ctm);
				fz_fill_shade(csi->dev, gstate->fill.shade, csi->top_ctm, gstate->fill.alpha);
				fz_pop_clip(csi->dev);
			}
			break;
		}
	}

	if (dostroke)
	{
		switch (gstate->stroke.kind)
		{
		case PDF_MAT_NONE:
			break;
		case PDF_MAT_COLOR:
			fz_stroke_path(csi->dev, path, &gstate->stroke_state, gstate->ctm,
				gstate->stroke.colorspace, gstate->stroke.v, gstate->stroke.alpha);
			break;
		case PDF_MAT_PATTERN:
			if (gstate->stroke.pattern)
			{
				fz_clip_stroke_path(csi->dev, path, &bbox, &gstate->stroke_state, gstate->ctm);
				pdf_show_pattern(csi, gstate->stroke.pattern, bbox, PDF_STROKE);
				fz_pop_clip(csi->dev);
			}
			break;
		case PDF_MAT_SHADE:
			if (gstate->stroke.shade)
			{
				fz_clip_stroke_path(csi->dev, path, &bbox, &gstate->stroke_state, gstate->ctm);
				fz_fill_shade(csi->dev, gstate->stroke.shade, csi->top_ctm, gstate->stroke.alpha);
				fz_pop_clip(csi->dev);
			}
			break;
		}
	}

	if (dofill || dostroke)
		pdf_end_group(csi);

	fz_free_path(path);
}

/*
 * Assemble and emit text
 */

static void
pdf_flush_text(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_text *text;
	int dofill = 0;
	int dostroke = 0;
	int doclip = 0;
	int doinvisible = 0;
	fz_rect bbox;
	fz_context *ctx = csi->dev->ctx;

	if (!csi->text)
		return;
	text = csi->text;
	csi->text = NULL;

	dofill = dostroke = doclip = doinvisible = 0;
	switch (csi->text_mode)
	{
	case 0: dofill = 1; break;
	case 1: dostroke = 1; break;
	case 2: dofill = dostroke = 1; break;
	case 3: doinvisible = 1; break;
	case 4: dofill = doclip = 1; break;
	case 5: dostroke = doclip = 1; break;
	case 6: dofill = dostroke = doclip = 1; break;
	case 7: doclip = 1; break;
	}

	if (csi->in_hidden_ocg > 0)
		dostroke = dofill = 0;

	bbox = fz_bound_text(text, gstate->ctm);

	pdf_begin_group(csi, bbox);

	if (doinvisible)
		fz_ignore_text(csi->dev, text, gstate->ctm);

	if (doclip)
	{
		if (csi->accumulate < 2)
			gstate->clip_depth++;
		fz_clip_text(csi->dev, text, gstate->ctm, csi->accumulate);
		csi->accumulate = 2;
	}

	if (dofill)
	{
		switch (gstate->fill.kind)
		{
		case PDF_MAT_NONE:
			break;
		case PDF_MAT_COLOR:
			fz_fill_text(csi->dev, text, gstate->ctm,
				gstate->fill.colorspace, gstate->fill.v, gstate->fill.alpha);
			break;
		case PDF_MAT_PATTERN:
			if (gstate->fill.pattern)
			{
				fz_clip_text(csi->dev, text, gstate->ctm, 0);
				pdf_show_pattern(csi, gstate->fill.pattern, bbox, PDF_FILL);
				fz_pop_clip(csi->dev);
			}
			break;
		case PDF_MAT_SHADE:
			if (gstate->fill.shade)
			{
				fz_clip_text(csi->dev, text, gstate->ctm, 0);
				fz_fill_shade(csi->dev, gstate->fill.shade, csi->top_ctm, gstate->fill.alpha);
				fz_pop_clip(csi->dev);
			}
			break;
		}
	}

	if (dostroke)
	{
		switch (gstate->stroke.kind)
		{
		case PDF_MAT_NONE:
			break;
		case PDF_MAT_COLOR:
			fz_stroke_text(csi->dev, text, &gstate->stroke_state, gstate->ctm,
				gstate->stroke.colorspace, gstate->stroke.v, gstate->stroke.alpha);
			break;
		case PDF_MAT_PATTERN:
			if (gstate->stroke.pattern)
			{
				fz_clip_stroke_text(csi->dev, text, &gstate->stroke_state, gstate->ctm);
				pdf_show_pattern(csi, gstate->stroke.pattern, bbox, PDF_STROKE);
				fz_pop_clip(csi->dev);
			}
			break;
		case PDF_MAT_SHADE:
			if (gstate->stroke.shade)
			{
				fz_clip_stroke_text(csi->dev, text, &gstate->stroke_state, gstate->ctm);
				fz_fill_shade(csi->dev, gstate->stroke.shade, csi->top_ctm, gstate->stroke.alpha);
				fz_pop_clip(csi->dev);
			}
			break;
		}
	}

	pdf_end_group(csi);

	fz_free_text(ctx, text);
}

static void
pdf_show_char(pdf_csi *csi, int cid)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	pdf_font_desc *fontdesc = gstate->font;
	fz_matrix tsm, trm;
	float w0, w1, tx, ty;
	pdf_hmtx h;
	pdf_vmtx v;
	int gid;
	int ucsbuf[8];
	int ucslen;
	int i;
	fz_context *ctx = csi->dev->ctx;

	tsm.a = gstate->size * gstate->scale;
	tsm.b = 0;
	tsm.c = 0;
	tsm.d = gstate->size;
	tsm.e = 0;
	tsm.f = gstate->rise;

	ucslen = 0;
	if (fontdesc->to_unicode)
		ucslen = pdf_lookup_cmap_full(fontdesc->to_unicode, cid, ucsbuf);
	if (ucslen == 0 && cid < fontdesc->cid_to_ucs_len)
	{
		ucsbuf[0] = fontdesc->cid_to_ucs[cid];
		ucslen = 1;
	}
	if (ucslen == 0 || (ucslen == 1 && ucsbuf[0] == 0))
	{
		ucsbuf[0] = '?';
		ucslen = 1;
	}

	gid = pdf_font_cid_to_gid(fontdesc, cid);

	/* cf. http://code.google.com/p/sumatrapdf/issues/detail?id=1149 */
	if (fontdesc->wmode == 1 && fontdesc->font->ft_face)
		gid = pdf_ft_get_vgid(ctx, fontdesc, gid);

	if (fontdesc->wmode == 1)
	{
		v = pdf_get_vmtx(fontdesc, cid);
		tsm.e -= v.x * gstate->size * 0.001f;
		tsm.f -= v.y * gstate->size * 0.001f;
	}

	trm = fz_concat(tsm, csi->tm);

	/* flush buffered text if face or matrix or rendermode has changed */
	if (!csi->text ||
		fontdesc->font != csi->text->font ||
		fontdesc->wmode != csi->text->wmode ||
		fabsf(trm.a - csi->text->trm.a) > FLT_EPSILON ||
		fabsf(trm.b - csi->text->trm.b) > FLT_EPSILON ||
		fabsf(trm.c - csi->text->trm.c) > FLT_EPSILON ||
		fabsf(trm.d - csi->text->trm.d) > FLT_EPSILON ||
		gstate->render != csi->text_mode)
	{
		pdf_flush_text(csi);

		csi->text = fz_new_text(ctx, fontdesc->font, trm, fontdesc->wmode);
		csi->text->trm.e = 0;
		csi->text->trm.f = 0;
		csi->text_mode = gstate->render;
	}

	/* add glyph to textobject */
	fz_add_text(ctx, csi->text, gid, ucsbuf[0], trm.e, trm.f);

	/* add filler glyphs for one-to-many unicode mapping */
	for (i = 1; i < ucslen; i++)
		fz_add_text(ctx, csi->text, -1, ucsbuf[i], trm.e, trm.f);

	if (fontdesc->wmode == 0)
	{
		h = pdf_get_hmtx(fontdesc, cid);
		w0 = h.w * 0.001f;
		tx = (w0 * gstate->size + gstate->char_space) * gstate->scale;
		csi->tm = fz_concat(fz_translate(tx, 0), csi->tm);
	}

	if (fontdesc->wmode == 1)
	{
		w1 = v.w * 0.001f;
		ty = w1 * gstate->size + gstate->char_space;
		csi->tm = fz_concat(fz_translate(0, ty), csi->tm);
	}
}

static void
pdf_show_space(pdf_csi *csi, float tadj)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	pdf_font_desc *fontdesc = gstate->font;

	if (!fontdesc)
	{
		fz_warn(csi->dev->ctx, "cannot draw text since font and size not set");
		return;
	}

	if (fontdesc->wmode == 0)
		csi->tm = fz_concat(fz_translate(tadj * gstate->scale, 0), csi->tm);
	else
		csi->tm = fz_concat(fz_translate(0, tadj), csi->tm);
}

static void
pdf_show_string(pdf_csi *csi, unsigned char *buf, int len)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	pdf_font_desc *fontdesc = gstate->font;
	unsigned char *end = buf + len;
	int cpt, cid;
	fz_context *ctx = csi->dev->ctx;

	if (!fontdesc)
	{
		fz_warn(ctx, "cannot draw text since font and size not set");
		return;
	}

	while (buf < end)
	{
		buf = pdf_decode_cmap(fontdesc->encoding, buf, &cpt);
		cid = pdf_lookup_cmap(fontdesc->encoding, cpt);
		if (cid >= 0)
			pdf_show_char(csi, cid);
		else
			fz_warn(ctx, "cannot encode character with code point %#x", cpt);
		if (cpt == 32)
			pdf_show_space(csi, gstate->word_space);
	}
}

static void
pdf_show_text(pdf_csi *csi, fz_obj *text)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	int i;
	fz_context *ctx = csi->dev->ctx;

	if (fz_is_array(ctx, text))
	{
		for (i = 0; i < fz_array_len(ctx, text); i++)
		{
			fz_obj *item = fz_array_get(ctx, text, i);
			if (fz_is_string(ctx, item))
				pdf_show_string(csi, (unsigned char *)fz_to_str_buf(ctx, item), fz_to_str_len(ctx, item));
			else
				pdf_show_space(csi, - fz_to_real(ctx, item) * gstate->size * 0.001f);
		}
	}
	else if (fz_is_string(ctx, text))
	{
		pdf_show_string(csi, (unsigned char *)fz_to_str_buf(ctx, text), fz_to_str_len(ctx, text));
	}
}

/*
 * Interpreter and graphics state stack.
 */

static void
pdf_init_gstate(pdf_gstate *gs, fz_matrix ctm)
{
	gs->ctm = ctm;
	gs->clip_depth = 0;

	gs->stroke_state.start_cap = 0;
	gs->stroke_state.dash_cap = 0;
	gs->stroke_state.end_cap = 0;
	gs->stroke_state.linejoin = 0;
	gs->stroke_state.linewidth = 1;
	gs->stroke_state.miterlimit = 10;
	gs->stroke_state.dash_phase = 0;
	gs->stroke_state.dash_len = 0;
	memset(gs->stroke_state.dash_list, 0, sizeof(gs->stroke_state.dash_list));

	gs->stroke.kind = PDF_MAT_COLOR;
	gs->stroke.colorspace = fz_keep_colorspace(fz_device_gray);
	gs->stroke.v[0] = 0;
	gs->stroke.pattern = NULL;
	gs->stroke.shade = NULL;
	gs->stroke.alpha = 1;

	gs->fill.kind = PDF_MAT_COLOR;
	gs->fill.colorspace = fz_keep_colorspace(fz_device_gray);
	gs->fill.v[0] = 0;
	gs->fill.pattern = NULL;
	gs->fill.shade = NULL;
	gs->fill.alpha = 1;

	gs->char_space = 0;
	gs->word_space = 0;
	gs->scale = 1;
	gs->leading = 0;
	gs->font = NULL;
	gs->size = -1;
	gs->render = 0;
	gs->rise = 0;

	gs->blendmode = 0;
	gs->softmask = NULL;
	gs->softmask_ctm = fz_identity;
	gs->luminosity = 0;
}

static pdf_csi *
pdf_new_csi(pdf_xref *xref, fz_device *dev, fz_matrix ctm, char *event)
{
	pdf_csi *csi;

	csi = fz_malloc(dev->ctx, sizeof(pdf_csi));
	csi->xref = xref;
	csi->dev = dev;
	csi->event = event;

	csi->top = 0;
	csi->obj = NULL;
	csi->name[0] = 0;
	csi->string_len = 0;
	memset(csi->stack, 0, sizeof csi->stack);

	csi->xbalance = 0;
	csi->in_text = 0;
	csi->in_hidden_ocg = 0;

	csi->path = fz_new_path(dev->ctx);
	csi->clip = 0;
	csi->clip_even_odd = 0;

	csi->text = NULL;
	csi->tlm = fz_identity;
	csi->tm = fz_identity;
	csi->text_mode = 0;
	csi->accumulate = 1;

	/* SumatraPDF: make gstate growable for rendering tikz-qtree documents */
	csi->gcap = 32;
	csi->gstate = fz_calloc(dev->ctx, csi->gcap, sizeof(pdf_gstate));

	csi->top_ctm = ctm;
	pdf_init_gstate(&csi->gstate[0], ctm);
	csi->gtop = 0;

	return csi;
}

static void
pdf_clear_stack(pdf_csi *csi)
{
	int i;

	if (csi->obj)
		fz_drop_obj(csi->dev->ctx, csi->obj);
	csi->obj = NULL;

	csi->name[0] = 0;
	csi->string_len = 0;
	for (i = 0; i < csi->top; i++)
		csi->stack[i] = 0;

	csi->top = 0;
}

static pdf_material *
pdf_keep_material(pdf_material *mat)
{
	if (mat->colorspace)
		fz_keep_colorspace(mat->colorspace);
	if (mat->pattern)
		pdf_keep_pattern(mat->pattern);
	if (mat->shade)
		fz_keep_shade(mat->shade);
	return mat;
}

static pdf_material *
pdf_drop_material(fz_context *ctx, pdf_material *mat)
{
	if (mat->colorspace)
		fz_drop_colorspace(ctx, mat->colorspace);
	if (mat->pattern)
		pdf_drop_pattern(ctx, mat->pattern);
	if (mat->shade)
		fz_drop_shade(ctx, mat->shade);
	return mat;
}

static void
pdf_gsave(pdf_csi *csi)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;

	/* SumatraPDF: make gstate growable for rendering tikz-qtree documents */
	if (csi->gtop == csi->gcap - 1)
	{
		fz_warn(csi->dev->ctx, "gstate overflow in content stream");
		csi->gcap *= 2;
		csi->gstate = fz_realloc(csi->xref->ctx, csi->gstate, csi->gcap * sizeof(pdf_gstate));
		gs = csi->gstate + csi->gtop;
	}

	memcpy(&csi->gstate[csi->gtop + 1], &csi->gstate[csi->gtop], sizeof(pdf_gstate));

	csi->gtop ++;

	pdf_keep_material(&gs->stroke);
	pdf_keep_material(&gs->fill);
	if (gs->font)
		pdf_keep_font(gs->font);
	if (gs->softmask)
		pdf_keep_xobject(gs->softmask);
}

static void
pdf_grestore(pdf_csi *csi)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	int clip_depth = gs->clip_depth;
	fz_context *ctx = csi->dev->ctx;

	if (csi->gtop == 0)
	{
		fz_warn(csi->xref->ctx, "gstate underflow in content stream");
		return;
	}

	pdf_drop_material(ctx, &gs->stroke);
	pdf_drop_material(ctx, &gs->fill);
	if (gs->font)
		pdf_drop_font(ctx, gs->font);
	if (gs->softmask)
		pdf_drop_xobject(ctx, gs->softmask);

	csi->gtop --;

	gs = csi->gstate + csi->gtop;
	while (clip_depth > gs->clip_depth)
	{
		fz_pop_clip(csi->dev);
		clip_depth--;
	}
}

static void
pdf_free_csi(pdf_csi *csi)
{
	fz_context *ctx = csi->dev->ctx;

	while (csi->gtop)
		pdf_grestore(csi);

	pdf_drop_material(ctx, &csi->gstate[0].fill);
	pdf_drop_material(ctx, &csi->gstate[0].stroke);
	if (csi->gstate[0].font)
		pdf_drop_font(ctx, csi->gstate[0].font);
	if (csi->gstate[0].softmask)
		pdf_drop_xobject(ctx, csi->gstate[0].softmask);

	while (csi->gstate[0].clip_depth--)
		fz_pop_clip(csi->dev);

	if (csi->path) fz_free_path(csi->path);
	if (csi->text) fz_free_text(ctx, csi->text);

	pdf_clear_stack(csi);
	/* SumatraPDF: make gstate growable for rendering tikz-qtree documents */
	fz_free(ctx, csi->gstate);

	fz_free(ctx, csi);
}

/*
 * Material state
 */

static void
pdf_set_colorspace(pdf_csi *csi, int what, fz_colorspace *colorspace)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	pdf_material *mat;

	pdf_flush_text(csi);

	mat = what == PDF_FILL ? &gs->fill : &gs->stroke;

	fz_drop_colorspace(csi->dev->ctx, mat->colorspace);

	mat->kind = PDF_MAT_COLOR;
	mat->colorspace = fz_keep_colorspace(colorspace);

	mat->v[0] = 0;
	mat->v[1] = 0;
	mat->v[2] = 0;
	mat->v[3] = 1;
}

static void
pdf_set_color(pdf_csi *csi, int what, float *v)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	pdf_material *mat;
	int i;

	pdf_flush_text(csi);

	mat = what == PDF_FILL ? &gs->fill : &gs->stroke;

	switch (mat->kind)
	{
	case PDF_MAT_PATTERN:
	case PDF_MAT_COLOR:
		if (!strcmp(mat->colorspace->name, "Lab"))
		{
			mat->v[0] = v[0] / 100;
			mat->v[1] = (v[1] + 100) / 200;
			mat->v[2] = (v[2] + 100) / 200;
		}
		for (i = 0; i < mat->colorspace->n; i++)
			mat->v[i] = v[i];
		break;
	default:
		fz_warn(csi->dev->ctx, "color incompatible with material");
	}
}

static void
pdf_set_shade(pdf_csi *csi, int what, fz_shade *shade)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	pdf_material *mat;

	pdf_flush_text(csi);

	mat = what == PDF_FILL ? &gs->fill : &gs->stroke;

	if (mat->shade)
		fz_drop_shade(csi->dev->ctx, mat->shade);

	mat->kind = PDF_MAT_SHADE;
	mat->shade = fz_keep_shade(shade);
}

static void
pdf_set_pattern(pdf_csi *csi, int what, pdf_pattern *pat, float *v)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	pdf_material *mat;

	pdf_flush_text(csi);

	mat = what == PDF_FILL ? &gs->fill : &gs->stroke;

	if (mat->pattern)
		pdf_drop_pattern(csi->dev->ctx, mat->pattern);

	mat->kind = PDF_MAT_PATTERN;
	if (pat)
		mat->pattern = pdf_keep_pattern(pat);
	else
		mat->pattern = NULL;

	if (v)
		pdf_set_color(csi, what, v);
}

static void
pdf_unset_pattern(pdf_csi *csi, int what)
{
	pdf_gstate *gs = csi->gstate + csi->gtop;
	pdf_material *mat;
	mat = what == PDF_FILL ? &gs->fill : &gs->stroke;
	if (mat->kind == PDF_MAT_PATTERN)
	{
		if (mat->pattern)
			pdf_drop_pattern(csi->dev->ctx, mat->pattern);
		mat->pattern = NULL;
		mat->kind = PDF_MAT_COLOR;
	}
}

/*
 * Patterns, XObjects and ExtGState
 */

static void
pdf_show_pattern(pdf_csi *csi, pdf_pattern *pat, fz_rect area, int what)
{
	pdf_gstate *gstate;
	fz_matrix ptm, invptm;
	fz_matrix oldtopctm;
	fz_error error;
	int x0, y0, x1, y1;
	int oldtop;
	fz_context *ctx = csi->dev->ctx;

	pdf_gsave(csi);
	gstate = csi->gstate + csi->gtop;

	if (pat->ismask)
	{
		pdf_unset_pattern(csi, PDF_FILL);
		pdf_unset_pattern(csi, PDF_STROKE);
		if (what == PDF_FILL)
		{
			pdf_drop_material(ctx, &gstate->stroke);
			pdf_keep_material(&gstate->fill);
			gstate->stroke = gstate->fill;
		}
		if (what == PDF_STROKE)
		{
			pdf_drop_material(ctx, &gstate->fill);
			pdf_keep_material(&gstate->stroke);
			gstate->fill = gstate->stroke;
		}
	}
	else
	{
		// TODO: unset only the current fill/stroke or both?
		pdf_unset_pattern(csi, what);
	}

	/* don't apply softmasks to objects in the pattern as well */
	if (gstate->softmask)
	{
		pdf_drop_xobject(ctx, gstate->softmask);
		gstate->softmask = NULL;
	}

	ptm = fz_concat(pat->matrix, csi->top_ctm);
	invptm = fz_invert_matrix(ptm);

	/* patterns are painted using the ctm in effect at the beginning of the content stream */
	/* get bbox of shape in pattern space for stamping */
	area = fz_transform_rect(invptm, area);

	/* When calculating the number of tiles required, we adjust by a small
	 * amount to allow for rounding errors. By choosing this amount to be
	 * smaller than 1/256, we guarantee we won't cause problems that will
	 * be visible even under our most extreme antialiasing. */
	x0 = floorf(area.x0 / pat->xstep + 0.001);
	y0 = floorf(area.y0 / pat->ystep + 0.001);
	x1 = ceilf(area.x1 / pat->xstep - 0.001);
	y1 = ceilf(area.y1 / pat->ystep - 0.001);

	oldtopctm = csi->top_ctm;
	oldtop = csi->gtop;

#ifdef TILE
	if ((x1 - x0) * (y1 - y0) > 1)
#else
	if (0)
#endif
	{
		fz_begin_tile(csi->dev, area, pat->bbox, pat->xstep, pat->ystep, ptm);
		gstate->ctm = ptm;
		csi->top_ctm = gstate->ctm;
		pdf_gsave(csi);
		error = pdf_run_buffer(csi, pat->resources, pat->contents);
		if (error)
			fz_error_handle(ctx, error, "cannot render pattern tile");
		pdf_grestore(csi);
		while (oldtop < csi->gtop)
			pdf_grestore(csi);
		fz_end_tile(csi->dev);
	}
	else
	{
		int x, y;
		for (y = y0; y < y1; y++)
		{
			for (x = x0; x < x1; x++)
			{
				gstate->ctm = fz_concat(fz_translate(x * pat->xstep, y * pat->ystep), ptm);
				csi->top_ctm = gstate->ctm;
				pdf_gsave(csi);
				error = pdf_run_buffer(csi, pat->resources, pat->contents);
				pdf_grestore(csi);
				while (oldtop < csi->gtop)
					pdf_grestore(csi);
				if (error)
				{
					fz_error_handle(ctx, error, "cannot render pattern tile");
					goto cleanup;
				}
			}
		}
	}
cleanup:

	csi->top_ctm = oldtopctm;

	pdf_grestore(csi);
}

static fz_error
pdf_run_xobject(pdf_csi *csi, fz_obj *resources, pdf_xobject *xobj, fz_matrix transform)
{
	fz_error error;
	pdf_gstate *gstate;
	fz_matrix oldtopctm;
	int oldtop;
	int popmask;
	fz_context *ctx = csi->dev->ctx;

	/* SumatraPDF: prevent a potentially infinite recursion */
	if (csi->gtop >= 96)
		return fz_error_make(ctx, "aborting potentially infinite recursion (csi->gtop == %d)", csi->gtop);

	pdf_gsave(csi);

	gstate = csi->gstate + csi->gtop;
	oldtop = csi->gtop;
	popmask = 0;

	/* apply xobject's transform matrix */
	transform = fz_concat(xobj->matrix, transform);
	gstate->ctm = fz_concat(transform, gstate->ctm);

	/* apply soft mask, create transparency group and reset state */
	if (xobj->transparency)
	{
		if (gstate->softmask)
		{
			pdf_xobject *softmask = gstate->softmask;
			fz_rect bbox = fz_transform_rect(gstate->ctm, xobj->bbox);

			gstate->softmask = NULL;
			popmask = 1;

			fz_begin_mask(csi->dev, bbox, gstate->luminosity,
				softmask->colorspace, gstate->softmask_bc);
			error = pdf_run_xobject(csi, resources, softmask, fz_identity);
			if (error)
				return fz_error_note(ctx, error, "cannot run softmask");
			fz_end_mask(csi->dev);

			pdf_drop_xobject(ctx, softmask);
		}

		fz_begin_group(csi->dev,
			fz_transform_rect(gstate->ctm, xobj->bbox),
			xobj->isolated, xobj->knockout, gstate->blendmode, gstate->fill.alpha);

		gstate->blendmode = 0;
		gstate->stroke.alpha = 1;
		gstate->fill.alpha = 1;
	}

	/* clip to the bounds */

	fz_moveto(csi->path, xobj->bbox.x0, xobj->bbox.y0);
	fz_lineto(csi->path, xobj->bbox.x1, xobj->bbox.y0);
	fz_lineto(csi->path, xobj->bbox.x1, xobj->bbox.y1);
	fz_lineto(csi->path, xobj->bbox.x0, xobj->bbox.y1);
	fz_closepath(csi->path);
	csi->clip = 1;
	pdf_show_path(csi, 0, 0, 0, 0);

	/* run contents */

	oldtopctm = csi->top_ctm;
	csi->top_ctm = gstate->ctm;

	if (xobj->resources)
		resources = xobj->resources;

	error = pdf_run_buffer(csi, resources, xobj->contents);
	if (error)
		fz_error_note(ctx, error, "cannot interpret XObject stream");

	csi->top_ctm = oldtopctm;

	while (oldtop < csi->gtop)
		pdf_grestore(csi);

	pdf_grestore(csi);

	/* wrap up transparency stacks */

	if (xobj->transparency)
	{
		fz_end_group(csi->dev);
		if (popmask)
			fz_pop_clip(csi->dev);
	}

	return fz_okay;
}

static fz_error
pdf_run_extgstate(pdf_csi *csi, fz_obj *rdb, fz_obj *extgstate)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_colorspace *colorspace;
	int i, k;
	fz_context *ctx = csi->xref->ctx;

	pdf_flush_text(csi);

	for (i = 0; i < fz_dict_len(ctx, extgstate); i++)
	{
		fz_obj *key = fz_dict_get_key(ctx, extgstate, i);
		fz_obj *val = fz_dict_get_val(ctx, extgstate, i);
		char *s = fz_to_name(ctx, key);

		if (!strcmp(s, "Font"))
		{
			if (fz_is_array(ctx, val) && fz_array_len(ctx, val) == 2)
			{
				fz_error error;
				fz_obj *font = fz_array_get(ctx, val, 0);

				if (gstate->font)
				{
					pdf_drop_font(ctx, gstate->font);
					gstate->font = NULL;
				}

				error = pdf_load_font(&gstate->font, csi->xref, rdb, font);
				if (error)
					return fz_error_note(ctx, error, "cannot load font (%d %d R)", fz_to_num(font), fz_to_gen(font));
				if (!gstate->font)
					return fz_error_make(ctx, "cannot find font in store");
				gstate->size = fz_to_real(ctx, fz_array_get(ctx, val, 1));
			}
			else
				return fz_error_make(ctx, "malformed /Font dictionary");
		}

		else if (!strcmp(s, "LC"))
		{
			gstate->stroke_state.start_cap = fz_to_int(ctx, val);
			gstate->stroke_state.dash_cap = fz_to_int(ctx, val);
			gstate->stroke_state.end_cap = fz_to_int(ctx, val);
		}
		else if (!strcmp(s, "LW"))
			gstate->stroke_state.linewidth = fz_to_real(ctx, val);
		else if (!strcmp(s, "LJ"))
			gstate->stroke_state.linejoin = fz_to_int(ctx, val);
		else if (!strcmp(s, "ML"))
			gstate->stroke_state.miterlimit = fz_to_real(ctx, val);

		else if (!strcmp(s, "D"))
		{
			if (fz_is_array(ctx, val) && fz_array_len(ctx, val) == 2)
			{
				fz_obj *dashes = fz_array_get(ctx, val, 0);
				gstate->stroke_state.dash_len = MAX(fz_array_len(ctx, dashes), 32);
				for (k = 0; k < gstate->stroke_state.dash_len; k++)
					gstate->stroke_state.dash_list[k] = fz_to_real(ctx, fz_array_get(ctx, dashes, k));
				gstate->stroke_state.dash_phase = fz_to_real(ctx, fz_array_get(ctx, val, 1));
			}
			else
				return fz_error_make(ctx, "malformed /D");
		}

		else if (!strcmp(s, "CA"))
			gstate->stroke.alpha = fz_to_real(ctx, val);

		else if (!strcmp(s, "ca"))
			gstate->fill.alpha = fz_to_real(ctx, val);

		else if (!strcmp(s, "BM"))
		{
			if (fz_is_array(ctx, val))
				val = fz_array_get(ctx, val, 0);
			gstate->blendmode = fz_find_blendmode(fz_to_name(ctx, val));
		}

		else if (!strcmp(s, "SMask"))
		{
			if (fz_is_dict(ctx, val))
			{
				fz_error error;
				pdf_xobject *xobj;
				fz_obj *group, *luminosity, *bc;

				if (gstate->softmask)
				{
					pdf_drop_xobject(ctx, gstate->softmask);
					gstate->softmask = NULL;
				}

				group = fz_dict_gets(ctx, val, "G");
				if (!group)
					return fz_error_make(ctx, "cannot load softmask xobject (%d %d R)", fz_to_num(val), fz_to_gen(val));
				error = pdf_load_xobject(&xobj, csi->xref, group);
				if (error)
					return fz_error_note(ctx, error, "cannot load xobject (%d %d R)", fz_to_num(val), fz_to_gen(val));

				colorspace = xobj->colorspace;
				if (!colorspace)
					colorspace = fz_device_gray;

				gstate->softmask_ctm = fz_concat(xobj->matrix, gstate->ctm);
				gstate->softmask = xobj;
				for (k = 0; k < colorspace->n; k++)
					gstate->softmask_bc[k] = 0;

				bc = fz_dict_gets(ctx, val, "BC");
				if (fz_is_array(ctx, bc))
				{
					for (k = 0; k < colorspace->n; k++)
						gstate->softmask_bc[k] = fz_to_real(ctx, fz_array_get(ctx, bc, k));
				}

				luminosity = fz_dict_gets(ctx, val, "S");
				if (fz_is_name(ctx, luminosity) && !strcmp(fz_to_name(ctx, luminosity), "Luminosity"))
					gstate->luminosity = 1;
				else
					gstate->luminosity = 0;
			}
			else if (fz_is_name(ctx, val) && !strcmp(fz_to_name(ctx, val), "None"))
			{
				if (gstate->softmask)
				{
					pdf_drop_xobject(ctx, gstate->softmask);
					gstate->softmask = NULL;
				}
			}
		}

		else if (!strcmp(s, "TR"))
		{
			if (fz_is_name(ctx, val) && strcmp(fz_to_name(ctx, val), "Identity"))
				fz_warn(ctx, "ignoring transfer function");
		}
	}

	return fz_okay;
}

/*
 * Operators
 */

static void pdf_run_BDC(pdf_csi *csi, fz_obj *rdb)
{
	fz_obj *ocg;
	fz_context *ctx = csi->dev->ctx;

	/* If we are already in a hidden OCG, then we'll still be hidden -
	 * just increment the depth so we pop back to visibility when we've
	 * seen enough EDCs. */
	if (csi->in_hidden_ocg > 0)
	{
			csi->in_hidden_ocg++;
		return;
	}

	ocg = fz_dict_gets(ctx, fz_dict_gets(ctx, rdb, "Properties"), csi->name);
	if (ocg == NULL)
	{
		/* No Properties array, or name not found in the properties
		 * means visible. */
		return;
	}
	if (strcmp(fz_to_name(ctx, fz_dict_gets(ctx, ocg, "Type")), "OCG") != 0)
	{
		/* Wrong type of property */
		return;
	}
	if (pdf_is_hidden_ocg(ocg, csi, rdb))
		csi->in_hidden_ocg++;
}

static fz_error pdf_run_BI(pdf_csi *csi, fz_obj *rdb, fz_stream *file)
{
	int ch;
	fz_error error;
	char *buf = csi->xref->scratch;
	int buflen = sizeof(csi->xref->scratch);
	fz_pixmap *img;
	fz_obj *obj;
	fz_context *ctx = csi->dev->ctx;

	error = pdf_parse_dict(&obj, csi->xref, file, buf, buflen);
	if (error)
		return fz_error_note(ctx, error, "cannot parse inline image dictionary");

	/* read whitespace after ID keyword */
	ch = fz_read_byte(file);
	if (ch == '\r')
		if (fz_peek_byte(file) == '\n')
			fz_read_byte(file);

	error = pdf_load_inline_image(&img, csi->xref, rdb, obj, file);
	fz_drop_obj(ctx, obj);
	if (error)
		return fz_error_note(ctx, error, "cannot load inline image");

	pdf_show_image(csi, img);

	fz_drop_pixmap(ctx, img);

	/* find EI */
	ch = fz_read_byte(file);
	while (ch != 'E' && ch != EOF)
		ch = fz_read_byte(file);
	ch = fz_read_byte(file);
	if (ch != 'I')
		return fz_error_note(ctx, error, "syntax error after inline image");

	return fz_okay;
}

static void pdf_run_B(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 1, 1, 0);
}

static void pdf_run_BMC(pdf_csi *csi)
{
	/* If we are already in a hidden OCG, then we'll still be hidden -
	 * just increment the depth so we pop back to visibility when we've
	 * seen enough EDCs. */
	if (csi->in_hidden_ocg > 0)
	{
		csi->in_hidden_ocg++;
	}
}

static void pdf_run_BT(pdf_csi *csi)
{
	csi->in_text = 1;
	csi->tm = fz_identity;
	csi->tlm = fz_identity;
}

static void pdf_run_BX(pdf_csi *csi)
{
	csi->xbalance ++;
}

static void pdf_run_Bstar(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 1, 1, 1);
}

static fz_error pdf_run_cs_imp(pdf_csi *csi, fz_obj *rdb, int what)
{
	fz_colorspace *colorspace;
	fz_obj *obj, *dict;
	fz_error error;
	fz_context *ctx = csi->dev->ctx;

	if (!strcmp(csi->name, "Pattern"))
	{
		pdf_set_pattern(csi, what, NULL, NULL);
	}
	else
	{
		if (!strcmp(csi->name, "DeviceGray"))
			colorspace = fz_keep_colorspace(fz_device_gray);
		else if (!strcmp(csi->name, "DeviceRGB"))
			colorspace = fz_keep_colorspace(fz_device_rgb);
		else if (!strcmp(csi->name, "DeviceCMYK"))
			colorspace = fz_keep_colorspace(fz_device_cmyk);
		else
		{
			dict = fz_dict_gets(ctx, rdb, "ColorSpace");
			if (!dict)
				return fz_error_make(ctx, "cannot find ColorSpace dictionary");
			obj = fz_dict_gets(ctx, dict, csi->name);
			if (!obj)
				return fz_error_make(ctx, "cannot find colorspace resource '%s'", csi->name);
			error = pdf_load_colorspace(&colorspace, csi->xref, obj);
			if (error)
				return fz_error_note(ctx, error, "cannot load colorspace (%d 0 R)", fz_to_num(obj));
		}

		pdf_set_colorspace(csi, what, colorspace);

		fz_drop_colorspace(ctx, colorspace);
	}
	return fz_okay;
}

static void pdf_run_CS(pdf_csi *csi, fz_obj *rdb)
{
	fz_error error;
	error = pdf_run_cs_imp(csi, rdb, PDF_STROKE);
	if (error)
		fz_error_handle(csi->dev->ctx, error, "cannot set colorspace");
}

static void pdf_run_cs(pdf_csi *csi, fz_obj *rdb)
{
	fz_error error;
	error = pdf_run_cs_imp(csi, rdb, PDF_FILL);
	if (error)
		fz_error_handle(csi->dev->ctx, error, "cannot set colorspace");
}

static void pdf_run_DP(pdf_csi *csi)
{
}

static fz_error pdf_run_Do(pdf_csi *csi, fz_obj *rdb)
{
	fz_obj *dict;
	fz_obj *obj;
	fz_obj *subtype;
	fz_error error;
	fz_context *ctx = csi->dev->ctx;

	dict = fz_dict_gets(ctx, rdb, "XObject");
	if (!dict)
		return fz_error_make(ctx, "cannot find XObject dictionary when looking for: '%s'", csi->name);

	obj = fz_dict_gets(ctx, dict, csi->name);
	if (!obj)
		return fz_error_make(ctx, "cannot find xobject resource: '%s'", csi->name);

	subtype = fz_dict_gets(ctx, obj, "Subtype");
	if (!fz_is_name(ctx, subtype))
		return fz_error_make(ctx, "no XObject subtype specified");

	if (pdf_is_hidden_ocg(fz_dict_gets(ctx, obj, "OC"), csi, rdb))
		return fz_okay;

	if (!strcmp(fz_to_name(ctx, subtype), "Form") && fz_dict_gets(ctx, obj, "Subtype2"))
		subtype = fz_dict_gets(ctx, obj, "Subtype2");

	if (!strcmp(fz_to_name(ctx, subtype), "Form"))
	{
		pdf_xobject *xobj;

		error = pdf_load_xobject(&xobj, csi->xref, obj);
		if (error)
			return fz_error_note(ctx, error, "cannot load xobject (%d %d R)", fz_to_num(obj), fz_to_gen(obj));

		/* Inherit parent resources, in case this one was empty XXX check where it's loaded */
		if (!xobj->resources)
			xobj->resources = fz_keep_obj(rdb);

		error = pdf_run_xobject(csi, xobj->resources, xobj, fz_identity);
		if (error)
		{
			/* SumatraPDF: fix memory leak */
			error = fz_error_note(ctx, error, "cannot draw xobject (%d %d R)", fz_to_num(obj), fz_to_gen(obj));
			pdf_drop_xobject(ctx, xobj);
			return error;
		}

		pdf_drop_xobject(ctx, xobj);
	}

	else if (!strcmp(fz_to_name(ctx, subtype), "Image"))
	{
		if ((csi->dev->hints & FZ_IGNORE_IMAGE) == 0)
		{
			fz_pixmap *img;
			error = pdf_load_image(&img, csi->xref, obj);
			if (error)
				return fz_error_note(ctx, error, "cannot load image (%d %d R)", fz_to_num(obj), fz_to_gen(obj));
			pdf_show_image(csi, img);
			fz_drop_pixmap(ctx, img);
		}
	}

	else if (!strcmp(fz_to_name(ctx, subtype), "PS"))
	{
		fz_warn(ctx, "ignoring XObject with subtype PS");
	}

	else
	{
		return fz_error_make(ctx, "unknown XObject subtype: '%s'", fz_to_name(ctx, subtype));
	}

	return fz_okay;
}

static void pdf_run_EMC(pdf_csi *csi)
{
	if (csi->in_hidden_ocg > 0)
		csi->in_hidden_ocg--;
}

static void pdf_run_ET(pdf_csi *csi)
{
	pdf_flush_text(csi);
	csi->accumulate = 1;
	csi->in_text = 0;
}

static void pdf_run_EX(pdf_csi *csi)
{
	csi->xbalance --;
}

static void pdf_run_F(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 1, 0, 0);
}

static void pdf_run_G(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_STROKE, fz_device_gray);
	pdf_set_color(csi, PDF_STROKE, csi->stack);
}

static void pdf_run_J(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->stroke_state.start_cap = csi->stack[0];
	gstate->stroke_state.dash_cap = csi->stack[0];
	gstate->stroke_state.end_cap = csi->stack[0];
}

static void pdf_run_K(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_STROKE, fz_device_cmyk);
	pdf_set_color(csi, PDF_STROKE, csi->stack);
}

static void pdf_run_M(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->stroke_state.miterlimit = csi->stack[0];
}

static void pdf_run_MP(pdf_csi *csi)
{
}

static void pdf_run_Q(pdf_csi *csi)
{
	pdf_grestore(csi);
}

static void pdf_run_RG(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_STROKE, fz_device_rgb);
	pdf_set_color(csi, PDF_STROKE, csi->stack);
}

static void pdf_run_S(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 0, 1, 0);
}

static fz_error pdf_run_SC_imp(pdf_csi *csi, fz_obj *rdb, int what, pdf_material *mat)
{
	fz_error error;
	fz_obj *patterntype;
	fz_obj *dict;
	fz_obj *obj;
	int kind;
	fz_context *ctx = csi->dev->ctx;

	kind = mat->kind;
	if (csi->name[0])
		kind = PDF_MAT_PATTERN;

	switch (kind)
	{
	case PDF_MAT_NONE:
		return fz_error_make(ctx, "cannot set color in mask objects");

	case PDF_MAT_COLOR:
		pdf_set_color(csi, what, csi->stack);
		break;

	case PDF_MAT_PATTERN:
		dict = fz_dict_gets(ctx, rdb, "Pattern");
		if (!dict)
			return fz_error_make(ctx, "cannot find Pattern dictionary");

		obj = fz_dict_gets(ctx, dict, csi->name);
		if (!obj)
			return fz_error_make(ctx, "cannot find pattern resource '%s'", csi->name);

		patterntype = fz_dict_gets(ctx, obj, "PatternType");

		if (fz_to_int(ctx, patterntype) == 1)
		{
			pdf_pattern *pat;
			error = pdf_load_pattern(&pat, csi->xref, obj);
			if (error)
				return fz_error_note(ctx, error, "cannot load pattern (%d 0 R)", fz_to_num(obj));
			pdf_set_pattern(csi, what, pat, csi->top > 0 ? csi->stack : NULL);
			pdf_drop_pattern(ctx, pat);
		}
		else if (fz_to_int(ctx, patterntype) == 2)
		{
			fz_shade *shd;
			error = pdf_load_shading(&shd, csi->xref, obj);
			if (error)
				return fz_error_note(ctx, error, "cannot load shading (%d 0 R)", fz_to_num(obj));
			pdf_set_shade(csi, what, shd);
			fz_drop_shade(ctx, shd);
		}
		else
		{
			return fz_error_make(ctx, "unknown pattern type: %d", fz_to_int(ctx, patterntype));
		}
		break;

	case PDF_MAT_SHADE:
		return fz_error_make(ctx, "cannot set color in shade objects");
	}

	return fz_okay;
}

static void pdf_run_SC(pdf_csi *csi, fz_obj *rdb)
{
	fz_error error;
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	error = pdf_run_SC_imp(csi, rdb, PDF_STROKE, &gstate->stroke);
	if (error)
		fz_error_handle(csi->dev->ctx, error, "cannot set color and colorspace");
}

static void pdf_run_sc(pdf_csi *csi, fz_obj *rdb)
{
	fz_error error;
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	error = pdf_run_SC_imp(csi, rdb, PDF_FILL, &gstate->fill);
	if (error)
		fz_error_handle(csi->dev->ctx, error, "cannot set color and colorspace");
}

static void pdf_run_Tc(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->char_space = csi->stack[0];
}

static void pdf_run_Tw(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->word_space = csi->stack[0];
}

static void pdf_run_Tz(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	float a = csi->stack[0] / 100;
	pdf_flush_text(csi);
	gstate->scale = a;
}

static void pdf_run_TL(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->leading = csi->stack[0];
}

static fz_error pdf_run_Tf(pdf_csi *csi, fz_obj *rdb)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_error error;
	fz_obj *dict;
	fz_obj *obj;
	fz_context *ctx = csi->dev->ctx;

	gstate->size = csi->stack[0];
	if (gstate->font)
		pdf_drop_font(ctx, gstate->font);
	gstate->font = NULL;

	dict = fz_dict_gets(ctx, rdb, "Font");
	if (!dict)
		return fz_error_make(ctx, "cannot find Font dictionary");

	obj = fz_dict_gets(ctx, dict, csi->name);
	if (!obj)
		return fz_error_make(ctx, "cannot find font resource: '%s'", csi->name);

	error = pdf_load_font(&gstate->font, csi->xref, rdb, obj);
	if (error)
		return fz_error_note(ctx, error, "cannot load font (%d 0 R)", fz_to_num(obj));

	return fz_okay;
}

static void pdf_run_Tr(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->render = csi->stack[0];
}

static void pdf_run_Ts(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->rise = csi->stack[0];
}

static void pdf_run_Td(pdf_csi *csi)
{
	fz_matrix m = fz_translate(csi->stack[0], csi->stack[1]);
	csi->tlm = fz_concat(m, csi->tlm);
	csi->tm = csi->tlm;
}

static void pdf_run_TD(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_matrix m;

	gstate->leading = -csi->stack[1];
	m = fz_translate(csi->stack[0], csi->stack[1]);
	csi->tlm = fz_concat(m, csi->tlm);
	csi->tm = csi->tlm;
}

static void pdf_run_Tm(pdf_csi *csi)
{
	csi->tm.a = csi->stack[0];
	csi->tm.b = csi->stack[1];
	csi->tm.c = csi->stack[2];
	csi->tm.d = csi->stack[3];
	csi->tm.e = csi->stack[4];
	csi->tm.f = csi->stack[5];
	csi->tlm = csi->tm;
}

static void pdf_run_Tstar(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_matrix m = fz_translate(0, -gstate->leading);
	csi->tlm = fz_concat(m, csi->tlm);
	csi->tm = csi->tlm;
}

static void pdf_run_Tj(pdf_csi *csi)
{
	if (csi->string_len)
		pdf_show_string(csi, csi->string, csi->string_len);
	else
		pdf_show_text(csi, csi->obj);
}

static void pdf_run_TJ(pdf_csi *csi)
{
	if (csi->string_len)
		pdf_show_string(csi, csi->string, csi->string_len);
	else
		pdf_show_text(csi, csi->obj);
}

static void pdf_run_W(pdf_csi *csi)
{
	csi->clip = 1;
	csi->clip_even_odd = 0;
}

static void pdf_run_Wstar(pdf_csi *csi)
{
	csi->clip = 1;
	csi->clip_even_odd = 1;
}

static void pdf_run_b(pdf_csi *csi)
{
	pdf_show_path(csi, 1, 1, 1, 0);
}

static void pdf_run_bstar(pdf_csi *csi)
{
	pdf_show_path(csi, 1, 1, 1, 1);
}

static void pdf_run_c(pdf_csi *csi)
{
	float a, b, c, d, e, f;
	a = csi->stack[0];
	b = csi->stack[1];
	c = csi->stack[2];
	d = csi->stack[3];
	e = csi->stack[4];
	f = csi->stack[5];
	fz_curveto(csi->path, a, b, c, d, e, f);
}

static void pdf_run_cm(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_matrix m;

	m.a = csi->stack[0];
	m.b = csi->stack[1];
	m.c = csi->stack[2];
	m.d = csi->stack[3];
	m.e = csi->stack[4];
	m.f = csi->stack[5];

	gstate->ctm = fz_concat(m, gstate->ctm);
}

static void pdf_run_d(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	fz_obj *array;
	int i;
	fz_context *ctx = csi->dev->ctx;

	array = csi->obj;
	gstate->stroke_state.dash_len = MIN(fz_array_len(ctx, array), nelem(gstate->stroke_state.dash_list));
	for (i = 0; i < gstate->stroke_state.dash_len; i++)
		gstate->stroke_state.dash_list[i] = fz_to_real(ctx, fz_array_get(ctx, array, i));
	gstate->stroke_state.dash_phase = csi->stack[0];
}

static void pdf_run_d0(pdf_csi *csi)
{
	csi->dev->flags |= FZ_CHARPROC_COLOR;
}

static void pdf_run_d1(pdf_csi *csi)
{
	csi->dev->flags |= FZ_CHARPROC_MASK;
}

static void pdf_run_f(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 1, 0, 0);
}

static void pdf_run_fstar(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 1, 0, 1);
}

static void pdf_run_g(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_FILL, fz_device_gray);
	pdf_set_color(csi, PDF_FILL, csi->stack);
}

static fz_error pdf_run_gs(pdf_csi *csi, fz_obj *rdb)
{
	fz_error error;
	fz_obj *dict;
	fz_obj *obj;
	fz_context *ctx = csi->dev->ctx;

	dict = fz_dict_gets(ctx, rdb, "ExtGState");
	if (!dict)
		return fz_error_make(ctx, "cannot find ExtGState dictionary");

	obj = fz_dict_gets(ctx, dict, csi->name);
	if (!obj)
		return fz_error_make(ctx, "cannot find extgstate resource '%s'", csi->name);

	error = pdf_run_extgstate(csi, rdb, obj);
	if (error)
		return fz_error_note(ctx, error, "cannot set ExtGState (%d 0 R)", fz_to_num(obj));
	return fz_okay;
}

static void pdf_run_h(pdf_csi *csi)
{
	fz_closepath(csi->path);
}

static void pdf_run_i(pdf_csi *csi)
{
}

static void pdf_run_j(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	gstate->stroke_state.linejoin = csi->stack[0];
}

static void pdf_run_k(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_FILL, fz_device_cmyk);
	pdf_set_color(csi, PDF_FILL, csi->stack);
}

static void pdf_run_l(pdf_csi *csi)
{
	float a, b;
	a = csi->stack[0];
	b = csi->stack[1];
	fz_lineto(csi->path, a, b);
}

static void pdf_run_m(pdf_csi *csi)
{
	float a, b;
	a = csi->stack[0];
	b = csi->stack[1];
	fz_moveto(csi->path, a, b);
}

static void pdf_run_n(pdf_csi *csi)
{
	pdf_show_path(csi, 0, 0, 0, 0);
}

static void pdf_run_q(pdf_csi *csi)
{
	pdf_gsave(csi);
}

static void pdf_run_re(pdf_csi *csi)
{
	float x, y, w, h;
	fz_context *ctx = csi->dev->ctx;

	x = csi->stack[0];
	y = csi->stack[1];
	w = csi->stack[2];
	h = csi->stack[3];

	fz_moveto(csi->path, x, y);
	fz_lineto(csi->path, x + w, y);
	fz_lineto(csi->path, x + w, y + h);
	fz_lineto(csi->path, x, y + h);
	fz_closepath(csi->path);
}

static void pdf_run_rg(pdf_csi *csi)
{
	pdf_set_colorspace(csi, PDF_FILL, fz_device_rgb);
	pdf_set_color(csi, PDF_FILL, csi->stack);
}

static void pdf_run_ri(pdf_csi *csi)
{
}

static void pdf_run(pdf_csi *csi)
{
	pdf_show_path(csi, 1, 0, 1, 0);
}

static fz_error pdf_run_sh(pdf_csi *csi, fz_obj *rdb)
{
	fz_obj *dict;
	fz_obj *obj;
	fz_shade *shd;
	fz_error error;
	fz_context *ctx = csi->dev->ctx;

	dict = fz_dict_gets(ctx, rdb, "Shading");
	if (!dict)
		return fz_error_make(ctx, "cannot find shading dictionary");

	obj = fz_dict_gets(ctx, dict, csi->name);
	if (!obj)
		return fz_error_make(ctx, "cannot find shading resource: '%s'", csi->name);

	if ((csi->dev->hints & FZ_IGNORE_SHADE) == 0)
	{
		error = pdf_load_shading(&shd, csi->xref, obj);
		if (error)
			return fz_error_note(ctx, error, "cannot load shading (%d %d R)", fz_to_num(obj), fz_to_gen(obj));
		pdf_show_shade(csi, shd);
		fz_drop_shade(ctx, shd);
	}
	return fz_okay;
}

static void pdf_run_v(pdf_csi *csi)
{
	float a, b, c, d;
	a = csi->stack[0];
	b = csi->stack[1];
	c = csi->stack[2];
	d = csi->stack[3];
	fz_curvetov(csi->path, a, b, c, d);
}

static void pdf_run_w(pdf_csi *csi)
{
	pdf_gstate *gstate = csi->gstate + csi->gtop;
	pdf_flush_text(csi); /* linewidth affects stroked text rendering mode */
	gstate->stroke_state.linewidth = csi->stack[0];
}

static void pdf_run_y(pdf_csi *csi)
{
	float a, b, c, d;
	a = csi->stack[0];
	b = csi->stack[1];
	c = csi->stack[2];
	d = csi->stack[3];
	fz_curvetoy(csi->path, a, b, c, d);
}

static void pdf_run_squote(pdf_csi *csi)
{
	fz_matrix m;
	pdf_gstate *gstate = csi->gstate + csi->gtop;

	m = fz_translate(0, -gstate->leading);
	csi->tlm = fz_concat(m, csi->tlm);
	csi->tm = csi->tlm;

	if (csi->string_len)
		pdf_show_string(csi, csi->string, csi->string_len);
	else
		pdf_show_text(csi, csi->obj);
}

static void pdf_run_dquote(pdf_csi *csi)
{
	fz_matrix m;
	pdf_gstate *gstate = csi->gstate + csi->gtop;

	gstate->word_space = csi->stack[0];
	gstate->char_space = csi->stack[1];

	m = fz_translate(0, -gstate->leading);
	csi->tlm = fz_concat(m, csi->tlm);
	csi->tm = csi->tlm;

	if (csi->string_len)
		pdf_show_string(csi, csi->string, csi->string_len);
	else
		pdf_show_text(csi, csi->obj);
}

#define A(a) (a)
#define B(a,b) (a | b << 8)
#define C(a,b,c) (a | b << 8 | c << 16)

static fz_error
pdf_run_keyword(pdf_csi *csi, fz_obj *rdb, fz_stream *file, char *buf)
{
	fz_error error;
	int key;

	key = buf[0];
	if (buf[1])
	{
		key |= buf[1] << 8;
		if (buf[2])
		{
			key |= buf[2] << 16;
			if (buf[3])
				key = 0;
		}
	}

	switch (key)
	{
	case A('"'): pdf_run_dquote(csi); break;
	case A('\''): pdf_run_squote(csi); break;
	case A('B'): pdf_run_B(csi); break;
	case B('B','*'): pdf_run_Bstar(csi); break;
	case C('B','D','C'): pdf_run_BDC(csi, rdb); break;
	case B('B','I'):
		error = pdf_run_BI(csi, rdb, file);
		if (error)
			return fz_error_note(csi->dev->ctx, error, "cannot draw inline image");
		break;
	case C('B','M','C'): pdf_run_BMC(csi); break;
	case B('B','T'): pdf_run_BT(csi); break;
	case B('B','X'): pdf_run_BX(csi); break;
	case B('C','S'): pdf_run_CS(csi, rdb); break;
	case B('D','P'): pdf_run_DP(csi); break;
	case B('D','o'):
		error = pdf_run_Do(csi, rdb);
		if (error)
			fz_error_handle(csi->dev->ctx, error, "cannot draw xobject/image");
		break;
	case C('E','M','C'): pdf_run_EMC(csi); break;
	case B('E','T'): pdf_run_ET(csi); break;
	case B('E','X'): pdf_run_EX(csi); break;
	case A('F'): pdf_run_F(csi); break;
	case A('G'): pdf_run_G(csi); break;
	case A('J'): pdf_run_J(csi); break;
	case A('K'): pdf_run_K(csi); break;
	case A('M'): pdf_run_M(csi); break;
	case B('M','P'): pdf_run_MP(csi); break;
	case A('Q'): pdf_run_Q(csi); break;
	case B('R','G'): pdf_run_RG(csi); break;
	case A('S'): pdf_run_S(csi); break;
	case B('S','C'): pdf_run_SC(csi, rdb); break;
	case C('S','C','N'): pdf_run_SC(csi, rdb); break;
	case B('T','*'): pdf_run_Tstar(csi); break;
	case B('T','D'): pdf_run_TD(csi); break;
	case B('T','J'): pdf_run_TJ(csi); break;
	case B('T','L'): pdf_run_TL(csi); break;
	case B('T','c'): pdf_run_Tc(csi); break;
	case B('T','d'): pdf_run_Td(csi); break;
	case B('T','f'):
		error = pdf_run_Tf(csi, rdb);
		if (error)
			fz_error_handle(csi->dev->ctx, error, "cannot set font");
		break;
	case B('T','j'): pdf_run_Tj(csi); break;
	case B('T','m'): pdf_run_Tm(csi); break;
	case B('T','r'): pdf_run_Tr(csi); break;
	case B('T','s'): pdf_run_Ts(csi); break;
	case B('T','w'): pdf_run_Tw(csi); break;
	case B('T','z'): pdf_run_Tz(csi); break;
	case A('W'): pdf_run_W(csi); break;
	case B('W','*'): pdf_run_Wstar(csi); break;
	case A('b'): pdf_run_b(csi); break;
	case B('b','*'): pdf_run_bstar(csi); break;
	case A('c'): pdf_run_c(csi); break;
	case B('c','m'): pdf_run_cm(csi); break;
	case B('c','s'): pdf_run_cs(csi, rdb); break;
	case A('d'): pdf_run_d(csi); break;
	case B('d','0'): pdf_run_d0(csi); break;
	case B('d','1'): pdf_run_d1(csi); break;
	case A('f'): pdf_run_f(csi); break;
	case B('f','*'): pdf_run_fstar(csi); break;
	case A('g'): pdf_run_g(csi); break;
	case B('g','s'):
		error = pdf_run_gs(csi, rdb);
		if (error)
			fz_error_handle(csi->dev->ctx, error, "cannot set graphics state");
		break;
	case A('h'): pdf_run_h(csi); break;
	case A('i'): pdf_run_i(csi); break;
	case A('j'): pdf_run_j(csi); break;
	case A('k'): pdf_run_k(csi); break;
	case A('l'): pdf_run_l(csi); break;
	case A('m'): pdf_run_m(csi); break;
	case A('n'): pdf_run_n(csi); break;
	case A('q'): pdf_run_q(csi); break;
	case B('r','e'): pdf_run_re(csi); break;
	case B('r','g'): pdf_run_rg(csi); break;
	case B('r','i'): pdf_run_ri(csi); break;
	case A('s'): pdf_run(csi); break;
	case B('s','c'): pdf_run_sc(csi, rdb); break;
	case C('s','c','n'): pdf_run_sc(csi, rdb); break;
	case B('s','h'):
		error = pdf_run_sh(csi, rdb);
		if (error)
			fz_error_handle(csi->dev->ctx, error, "cannot draw shading");
		break;
	case A('v'): pdf_run_v(csi); break;
	case A('w'): pdf_run_w(csi); break;
	case A('y'): pdf_run_y(csi); break;
	default:
		if (!csi->xbalance)
			fz_warn(csi->dev->ctx, "unknown keyword: '%s'", buf);
		break;
	}

	return fz_okay;
}

static fz_error
pdf_run_stream(pdf_csi *csi, fz_obj *rdb, fz_stream *file, char *buf, int buflen)
{
	fz_error error;
	int tok, len, in_array;
	fz_context *ctx = csi->dev->ctx;

	/* make sure we have a clean slate if we come here from flush_text */
	pdf_clear_stack(csi);
	in_array = 0;

	while (1)
	{
		if (csi->top == nelem(csi->stack) - 1)
			return fz_error_make(ctx, "stack overflow");

		error = pdf_lex(&tok, file, buf, buflen, &len);
		if (error)
			return fz_error_note(ctx, error, "lexical error in content stream");

		if (in_array)
		{
			if (tok == PDF_TOK_CLOSE_ARRAY)
			{
				in_array = 0;
			}
			else if (tok == PDF_TOK_INT || tok == PDF_TOK_REAL)
			{
				pdf_gstate *gstate = csi->gstate + csi->gtop;
				pdf_show_space(csi, -fz_atof(buf) * gstate->size * 0.001f);
			}
			else if (tok == PDF_TOK_STRING)
			{
				pdf_show_string(csi, (unsigned char *)buf, len);
			}
			else if (tok == PDF_TOK_KEYWORD)
			{
				if (!strcmp(buf, "Tw") || !strcmp(buf, "Tc"))
					fz_warn(ctx, "ignoring keyword '%s' inside array", buf);
				else
					return fz_error_make(ctx, "syntax error in array");
			}
			else if (tok == PDF_TOK_EOF)
				return fz_okay;
			else
				return fz_error_make(ctx, "syntax error in array");
		}

		else switch (tok)
		{
		case PDF_TOK_ENDSTREAM:
		case PDF_TOK_EOF:
			return fz_okay;

		case PDF_TOK_OPEN_ARRAY:
			if (!csi->in_text)
			{
				error = pdf_parse_array(&csi->obj, csi->xref, file, buf, buflen);
				if (error)
					return fz_error_note(ctx, error, "cannot parse array");
			}
			else
			{
				in_array = 1;
			}
			break;

		case PDF_TOK_OPEN_DICT:
			error = pdf_parse_dict(&csi->obj, csi->xref, file, buf, buflen);
			if (error)
				return fz_error_note(ctx, error, "cannot parse dictionary");
			break;

		case PDF_TOK_NAME:
			fz_strlcpy(csi->name, buf, sizeof(csi->name));
			break;

		case PDF_TOK_INT:
			csi->stack[csi->top] = atoi(buf);
			csi->top ++;
			break;

		case PDF_TOK_REAL:
			csi->stack[csi->top] = fz_atof(buf);
			csi->top ++;
			break;

		case PDF_TOK_STRING:
			if (len <= sizeof(csi->string))
			{
				memcpy(csi->string, buf, len);
				csi->string_len = len;
			}
			else
			{
				csi->obj = fz_new_string(ctx, buf, len);
			}
			break;

		case PDF_TOK_KEYWORD:
			error = pdf_run_keyword(csi, rdb, file, buf);
			if (error)
				return fz_error_note(ctx, error, "cannot run keyword");
			pdf_clear_stack(csi);
			break;

		default:
			return fz_error_make(ctx, "syntax error in content stream");
		}
	}
}

/*
 * Entry points
 */

static fz_error
pdf_run_buffer(pdf_csi *csi, fz_obj *rdb, fz_buffer *contents)
{
	fz_context *ctx = csi->dev->ctx;

	/* SumatraPDF: be slightly more defensive */
	if (contents)
	{
		fz_error error;
		int len = sizeof csi->xref->scratch;
		char *buf = fz_malloc(ctx, len); /* we must be re-entrant for type3 fonts */
		fz_stream *file = fz_open_buffer(ctx, contents);
		int save_in_text = csi->in_text;
		csi->in_text = 0;
		error = pdf_run_stream(csi, rdb, file, buf, len);
		csi->in_text = save_in_text;
		fz_close(file);
		fz_free(ctx, buf);
		if (error)
			/* cf. http://bugs.ghostscript.com/show_bug.cgi?id=692260 */
			fz_error_handle(ctx, error, "couldn't parse the whole content stream, rendering anyway");
		return fz_okay;
		/* SumatraPDF: be slightly more defensive */
	}
	return fz_error_make(ctx, "cannot run NULL content stream");
}

fz_error
pdf_run_page_with_usage(pdf_xref *xref, pdf_page *page, fz_device *dev, fz_matrix ctm, char *event)
{
	pdf_csi *csi;
	fz_error error;
	pdf_annot *annot;
	int flags;
	fz_context *ctx = dev->ctx;

	if (page->transparency)
		fz_begin_group(dev, fz_transform_rect(ctm, page->mediabox), 1, 0, 0, 1);

	csi = pdf_new_csi(xref, dev, ctm, event);
	error = pdf_run_buffer(csi, page->resources, page->contents);
	pdf_free_csi(csi);
	if (error)
		return fz_error_note(ctx, error, "cannot parse page content stream");

	for (annot = page->annots; annot; annot = annot->next)
	{
		flags = fz_to_int(ctx, fz_dict_gets(ctx, annot->obj, "F"));

		/* TODO: NoZoom and NoRotate */
		if (flags & (1 << 0)) /* Invisible */
			continue;
		if (flags & (1 << 1)) /* Hidden */
			continue;
		/* SumatraPDF: don't print annotations unless explicitly asked to */
		if (!(flags & (1 << 2)) /* Print */ && !strcmp(event, "Print"))
			continue;
		/* SumatraPDF: only consider the NoView flag for the View target */
		if ((flags & (1 << 5)) /* NoView */ && !strcmp(event, "View"))
			continue;

		csi = pdf_new_csi(xref, dev, ctm, event);
		if (!pdf_is_hidden_ocg(fz_dict_gets(ctx, annot->obj, "OC"), csi, page->resources))
		{
		error = pdf_run_xobject(csi, page->resources, annot->ap, annot->matrix);
		}
		pdf_free_csi(csi);
		if (error)
			return fz_error_note(ctx, error, "cannot parse annotation appearance stream");
	}

	if (page->transparency)
		fz_end_group(dev);

	return fz_okay;
}

fz_error
pdf_run_page(pdf_xref *xref, pdf_page *page, fz_device *dev, fz_matrix ctm)
{
	return pdf_run_page_with_usage(xref, page, dev, ctm, "View");
}

fz_error
pdf_run_glyph(pdf_xref *xref, fz_obj *resources, fz_buffer *contents, fz_device *dev, fz_matrix ctm)
{
	pdf_csi *csi = pdf_new_csi(xref, dev, ctm, "View");
	fz_error error = pdf_run_buffer(csi, resources, contents);
	pdf_free_csi(csi);
	if (error)
		return fz_error_note(dev->ctx, error, "cannot parse glyph content stream");
	return fz_okay;
}
