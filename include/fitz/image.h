typedef struct fz_image_s fz_image;

/* loadtile will fill a pixmap with the pixel samples. non-premultiplied alpha. */

struct fz_image_s
{
	fz_error* (*loadtile)(fz_image*,fz_pixmap*);
	void (*drop)(fz_image*);
	fz_colorspace *cs;
	int w, h, n, a;
};

void fz_dropimage(fz_image *img);
