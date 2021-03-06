#include "fitz.h"
#include "mupdf.h"

/* Load or synthesize ToUnicode map for fonts */

fz_error
pdf_load_to_unicode(pdf_font_desc *font, pdf_xref *xref,
	char **strings, char *collection, fz_obj *cmapstm)
{
	fz_error error = fz_okay;
	pdf_cmap *cmap;
	int cid;
	int ucsbuf[8];
	int ucslen;
	int i;
	fz_context *ctx = xref->ctx;

	if (pdf_is_stream(xref, fz_to_num(cmapstm), fz_to_gen(cmapstm)))
	{
		error = pdf_load_embedded_cmap(&cmap, xref, cmapstm);
		if (error)
			return fz_error_note(ctx, error, "cannot load embedded cmap (%d %d R)", fz_to_num(cmapstm), fz_to_gen(cmapstm));

		font->to_unicode = pdf_new_cmap(ctx);

		for (i = 0; i < (strings ? 256 : 65536); i++)
		{
			cid = pdf_lookup_cmap(font->encoding, i);
			if (cid >= 0)
			{
				ucslen = pdf_lookup_cmap_full(cmap, i, ucsbuf);
				if (ucslen == 1)
					pdf_map_range_to_range(ctx, font->to_unicode, cid, cid, ucsbuf[0]);
				if (ucslen > 1)
					pdf_map_one_to_many(ctx, font->to_unicode, cid, ucsbuf, ucslen);
			}
		}

		pdf_sort_cmap(ctx, font->to_unicode);

		pdf_drop_cmap(ctx, cmap);
	}

	else if (collection)
	{
		error = fz_okay;

		if (!strcmp(collection, "Adobe-CNS1"))
			error = pdf_load_system_cmap(ctx, &font->to_unicode, "Adobe-CNS1-UCS2");
		else if (!strcmp(collection, "Adobe-GB1"))
			error = pdf_load_system_cmap(ctx, &font->to_unicode, "Adobe-GB1-UCS2");
		else if (!strcmp(collection, "Adobe-Japan1"))
			error = pdf_load_system_cmap(ctx, &font->to_unicode, "Adobe-Japan1-UCS2");
		else if (!strcmp(collection, "Adobe-Korea1"))
			error = pdf_load_system_cmap(ctx, &font->to_unicode, "Adobe-Korea1-UCS2");
		/* SumatraPDF: load an identity cmap (until a ToUnicode is synthesized below) */
		else if (!strcmp(collection, "Adobe-Identity") && !(font->flags & PDF_FD_SYMBOLIC))
			font->to_unicode = pdf_new_identity_cmap(ctx, font->wmode, 2);

		if (error)
			return fz_error_note(ctx, error, "cannot load ToUnicode system cmap %s-UCS2", collection);
	}

	if (strings)
	{
		/* TODO one-to-many mappings */

		font->cid_to_ucs_len = 256;
		font->cid_to_ucs = fz_calloc(ctx, 256, sizeof(unsigned short));

		for (i = 0; i < 256; i++)
		{
			if (strings[i])
				font->cid_to_ucs[i] = pdf_lookup_agl(strings[i]);
			else
				font->cid_to_ucs[i] = '?';
		}
	}

	if (!font->to_unicode && !font->cid_to_ucs)
	{
		/* TODO: synthesize a ToUnicode if it's a freetype font with
		 * cmap and/or post tables or if it has glyph names. */
	}

	return fz_okay;
}
