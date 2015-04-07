/*
 * image viewer, for framebuffer devices
 *
 *   (c) 1998-2004 Gerd Hoffmann <kraxel@bytesex.org>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <getopt.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <ctype.h>
#include <locale.h>
#include <wchar.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include <jpeglib.h>

#include <libexif/exif-data.h>
//network settings
#include <arpa/inet.h>    //close
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>


#ifdef HAVE_LIBLIRC
# include "lirc/lirc_client.h"
# include "lirc.h"
#endif

#include "readers.h"
#include "dither.h"
#include "fbtools.h"
#include "fb-gui.h"
#include "filter.h"
#include "desktop.h"
#include "list.h"
#include "fbiconfig.h"

#include "jpeg/transupp.h"		/* Support routines for jpegtran */
#include "jpegtools.h"

#define TRUE            1
#define FALSE           0
#undef  MAX
#define MAX(x,y)        ((x)>(y)?(x):(y))
#undef  MIN
#define MIN(x,y)        ((x)<(y)?(x):(y))
#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(x[0]))

#define KEY_EOF        -1       /* ^D */
#define KEY_ESC        -2
#define KEY_SPACE      -3
#define KEY_Q          -4
#define KEY_PGUP       -5
#define KEY_PGDN       -6
#define KEY_TIMEOUT    -7
#define KEY_TAGFILE    -8
#define KEY_PLUS       -9
#define KEY_MINUS     -10
#define KEY_VERBOSE   -11
#define KEY_ASCALE    -12
#define KEY_DESC      -13
#define KEY_B         -14
#define KEY_R         -15 /* replay key */
#define KEY_C         -16 /* blank key image */

/* with arg */
#define KEY_GOTO     -100
#define KEY_SCALE    -101
#define KEY_DELAY    -102

/* edit */
#define KEY_DELETE   -200
#define KEY_ROT_CW   -201
#define KEY_ROT_CCW  -202

#define DEFAULT_DEVICE  "/dev/fb0"

#define PORT 8888

/* ---------------------------------------------------------------------- */

/* lirc fd */
int lirc = -1;
/*master socket  */
int master_socket;
int client_socket[30] , max_clients = 30;
struct sockaddr_in address;
int addrlen;

/* variables for read_image */
int32_t         lut_red[256], lut_green[256], lut_blue[256];
int             dither = FALSE, pcd_res = 3;
int             v_steps = 50;
int             h_steps = 50;
int             textreading = 0, redraw = 0, statusline = 1;
int             fitwidth;

/* file list */
struct flist {
    /* file list */
    int               nr;
    int               tag;
    char              *name;
    struct list_head  list;

    /* image cache */
    int               seen;
    int               top;
    int               left;
    int               text_steps;
    float             scale;
    struct ida_image  *fimg;
    struct ida_image  *simg;
    struct list_head  lru;
    int               timeout;
};
static LIST_HEAD(flist);
static LIST_HEAD(flru);
static int           fcount;
static struct flist  *fcurrent; /* main current image used for looping */
static struct flist  *fmain_list;
static struct ida_image *img;

/* accounting */
static int img_cnt, min_cnt = 2, max_cnt = 16;
static int img_mem, max_mem_mb;

/* framebuffer */
char                       *fbdev = NULL;
char                       *fbmode  = NULL;
int                        fd, switch_last, debug;

unsigned short red[256],  green[256],  blue[256];
struct fb_cmap cmap  = { 0, 256, red,  green,  blue };

static float fbgamma = 1;

/* Command line options. */
int autodown;
int autoup;
int comments;
int transparency = 40;
int timeout;
int backup;
int preserve;
int read_ahead;
int editable;
int blend_msecs;
int perfmon = 0;

/* font handling */
static char *fontname = NULL;
static FT_Face face;




/*global blank picture */
struct flist *blank;




/* ---------------------------------------------------------------------- */
/* fwd declarations                                                       */

static struct ida_image *flist_img_get(struct flist *f);
static void *flist_malloc(size_t size);
static void flist_img_load(struct flist *f, int prefetch);

/* ---------------------------------------------------------------------- */

static void
version(void)
{
    fprintf(stderr,
	    "fbi version " VERSION ", compiled on %s\n"
	    "(c) 1999-2006 Gerd Hoffmann <kraxel@bytesex.org> [SUSE Labs]\n",
	    __DATE__ );
}

static void
usage(char *name)
{
    char           *h;

    if (NULL != (h = strrchr(name, '/')))
	name = h+1;
    fprintf(stderr,
	    "\n"
	    "This program displays images using the Linux framebuffer device.\n"
	    "Supported formats: PhotoCD, jpeg, ppm, gif, tiff, xwd, bmp, png.\n"
	    "It tries to use ImageMagick's convert for unknown file formats.\n"
	    "\n"
	    "usage: %s [ options ] file1 file2 ... fileN\n"
	    "\n",
	    name);

    cfg_help_cmdline(stderr,fbi_cmd,4,20,0);
    cfg_help_cmdline(stderr,fbi_cfg,4,20,40);

    fprintf(stderr,
	    "\n"
	    "Large images can be scrolled using the cursor keys.  Zoom in/out\n"
	    "works with '+' and '-'.  Use ESC or 'q' to quit.  Space and PgDn\n"
	    "show the next, PgUp shows the previous image. Jumping to a image\n"
	    "works with <number>g.  Return acts like Space but additionally\n"
	    "prints the filename of the currently displayed image to stdout.\n"
	    "\n");
}

/* ---------------------------------------------------------------------- */

static int flist_add(char *filename)
{
    struct flist *f;

    f = malloc(sizeof(*f));
    memset(f,0,sizeof(*f));
    f->name = strdup(filename);
    list_add_tail(&f->list,&flist);
    INIT_LIST_HEAD(&f->lru);
    return 0;
}

static int blank_add(char *filename)
{
    blank = malloc(sizeof(*blank));
    memset(blank,0,sizeof(*blank));
    blank->name = strdup(filename);
    return 0;
}

static struct flist* get_blank(void)
{
    return blank;
}


static int flist_timeout_add(char *filename, int timeout)
{
    struct flist *f;

    f = malloc(sizeof(*f));
    memset(f,0,sizeof(*f));
    f->name = strdup(filename);
    f->timeout = timeout;
    list_add_tail(&f->list,&flist);
    INIT_LIST_HEAD(&f->lru);
    return 0;
}

static int flist_add_list(char *listfile)
{
    char filename[256];
    FILE *list;

    list = fopen(listfile,"r");
    if (NULL == list) {
	fprintf(stderr,"open %s: %s\n",listfile,strerror(errno));
	return -1;
    }
    while (NULL != fgets(filename,sizeof(filename)-1,list)) {
	size_t off = strcspn(filename,"\r\n");
	if (off)
	    filename[off] = 0;
	flist_add(filename);
    }
    fclose(list);
    return 0;
}

static int flist_del(struct flist *f)
{
    list_del(&f->list);
    free(f->name);
    free(f);
    return 0;
}

static void flist_renumber(void)
{
    struct list_head *item;
    struct flist *f;
    int i = 0;

    list_for_each(item,&flist) {
	f = list_entry(item, struct flist, list);
	f->nr = ++i;
    }
    fcount = i;
}

static int flist_islast(struct flist *f)
{
    return (&flist == f->list.next) ? 1 : 0;
}

static int flist_isfirst(struct flist *f)
{
    return (&flist == f->list.prev) ? 1 : 0;
}

static struct flist* flist_first(void)
{
    return list_entry(flist.next, struct flist, list);
}

static struct flist* flist_last(void)
{
    return list_entry(flist.prev, struct flist, list);
}

static struct flist* flist_next(struct flist *f, int eof, int loop)
{
    if (flist_islast(f)) {
	if (eof)
	    return NULL;
	if (loop)
	    return flist_first();
	return f;
    }
    return list_entry(f->list.next, struct flist, list);
}

static struct flist* flist_prev(struct flist *f, int loop)
{
    if (flist_isfirst(f)) {
	if (loop)
	    return flist_last();
	return f;
    }
    return list_entry(f->list.prev, struct flist, list);
}

static struct flist* flist_goto(int dest)
{
    struct list_head *item;
    struct flist *f;

    list_for_each(item,&flist) {
	f = list_entry(item, struct flist, list);
	if (f->nr == dest)
	    return f;
    }
    return NULL;
}

static void flist_randomize(void)
{
    struct flist *f;
    int count;

    srand((unsigned)time(NULL));
    flist_renumber();
    for (count = fcount; count > 0; count--) {
	f = flist_goto((rand() % count)+1);
	list_del(&f->list);
	list_add_tail(&f->list,&flist);
	flist_renumber();
    }
}

static void flist_print_tagged(FILE *fp)
{
    struct list_head *item;
    struct flist *f;

    list_for_each(item,&flist) {
	f = list_entry(item, struct flist, list);
	if (f->tag)
	    fprintf(fp,"%s\n",f->name);
    }
}

/* ---------------------------------------------------------------------- */

static void
shadow_draw_image(struct ida_image *img, int xoff, int yoff,
		  unsigned int first, unsigned int last, int weight)
{
    unsigned int     dwidth  = MIN(img->i.width,  fb_var.xres);
    unsigned int     dheight = MIN(img->i.height, fb_var.yres);
    unsigned int     data, offset, y, xs, ys;

    if (100 == weight)
	shadow_clear_lines(first, last);
    else
	shadow_darkify(0, fb_var.xres-1, first, last, 100 - weight);
    
    /* offset for image data (image > screen, select visible area) */
    offset = (yoff * img->i.width + xoff) * 3;

    /* offset for video memory (image < screen, center image) */
    xs = 0, ys = 0;
    if (img->i.width < fb_var.xres)
	xs += (fb_var.xres - img->i.width) / 2;
    if (img->i.height < fb_var.yres)
	ys += (fb_var.yres - img->i.height) / 2;

    /* go ! */
    for (data = 0, y = 0;
	 data < img->i.width * img->i.height * 3
	     && data / img->i.width / 3 < dheight;
	 data += img->i.width * 3, y++) {
	if (ys+y < first)
	    continue;
	if (ys+y > last)
	    continue;
	if (100 == weight)
	  shadow_draw_rgbdata(xs, ys+y, dwidth,
			      img->data + data + offset);
	else
	  shadow_merge_rgbdata(xs, ys+y, dwidth, weight,
			       img->data + data + offset);
    }
}

static void status_prepare(void)
{
    struct ida_image *img = flist_img_get(fcurrent);
    int y1 = fb_var.yres - (face->size->metrics.height >> 6);
    int y2 = fb_var.yres - 1;

    if (img) {
	shadow_draw_image(img, fcurrent->left, fcurrent->top, y1, y2, 100);
	shadow_darkify(0, fb_var.xres-1, y1, y2, transparency);
    } else {
	shadow_clear_lines(y1, y2);
    }
    shadow_draw_line(0, fb_var.xres-1, y1-1, y1-1);
}

static void status_update(unsigned char *desc, char *info)
{
    int yt = fb_var.yres + (face->size->metrics.descender >> 6);
    wchar_t str[128];
    
    if (!statusline)
	return;
    status_prepare();

    swprintf(str,ARRAY_SIZE(str),L"%s",desc);
    shadow_draw_string(face, 0, yt, str, -1);
    if (info) {
	swprintf(str,ARRAY_SIZE(str), L"[ %s ] H - Help", info);
    } else {
	swprintf(str,ARRAY_SIZE(str), L"| H - Help");
    }
    shadow_draw_string(face, fb_var.xres, yt, str, 1);

    shadow_render();
}

static void status_error(unsigned char *msg)
{
    int yt = fb_var.yres + (face->size->metrics.descender >> 6);
    wchar_t str[128];

    status_prepare();

    swprintf(str,ARRAY_SIZE(str), L"%s", msg);
    shadow_draw_string(face, 0, yt, str, -1);

    shadow_render();
    sleep(2);
}

static void status_edit(unsigned char *msg, int pos)
{
    int yt = fb_var.yres + (face->size->metrics.descender >> 6);
    wchar_t str[128];

    status_prepare();

    swprintf(str,ARRAY_SIZE(str), L"%s", msg);
    shadow_draw_string_cursor(face, 0, yt, str, pos);

    shadow_render();
}

static void show_exif(struct flist *f)
{
    static unsigned int tags[] = {
	0x010f, // Manufacturer
	0x0110, // Model

	0x0112, // Orientation
	0x0132, // Date and Time

	0x01e3, // White Point
	0x829a, // Exposure Time
	0x829d, // FNumber
	0x9206, // Subject Distance
	0xa40c, // Subject Distance Range
	0xa405, // Focal Length In 35mm Film
	0x9209, // Flash
    };
    ExifData   *ed;
    ExifEntry  *ee;
    unsigned int tag,l1,l2,len,count,i;
    const char *title[ARRAY_SIZE(tags)];
    char *value[ARRAY_SIZE(tags)];
    wchar_t *linebuffer[ARRAY_SIZE(tags)];

    if (!visible)
	return;

    ed = exif_data_new_from_file(f->name);
    if (NULL == ed) {
	status_error("image has no EXIF data");
	return;
    }

    /* pass one -- get data + calc size */
    l1 = 0;
    l2 = 0;
    for (tag = 0; tag < ARRAY_SIZE(tags); tag++) {
	ee = exif_content_get_entry (ed->ifd[EXIF_IFD_0], tags[tag]);
	if (NULL == ee)
	    ee = exif_content_get_entry (ed->ifd[EXIF_IFD_EXIF], tags[tag]);
	if (NULL == ee) {
	    title[tag] = NULL;
	    value[tag] = NULL;
	    continue;
	}
	title[tag] = exif_tag_get_title(tags[tag]);
#ifdef HAVE_NEW_EXIF
	value[tag] = malloc(128);
	exif_entry_get_value(ee, value[tag], 128);
#else
	value[tag] = strdup(exif_entry_get_value(ee));
#endif
	len = strlen(title[tag]);
	if (l1 < len)
	    l1 = len;
	len = strlen(value[tag]);
	if (l2 < len)
	    l2 = len;
    }

    /* pass two -- print stuff */
    count = 0;
    for (tag = 0; tag < ARRAY_SIZE(tags); tag++) {
	if (NULL == title[tag])
	    continue;
	linebuffer[count] = malloc(sizeof(wchar_t)*(l1+l2+8));
	swprintf(linebuffer[count], l1+l2+8,
		 L"%-*.*s : %-*.*s",
		 l1, l1, title[tag],
		 l2, l2, value[tag]);
	count++;
    }
    shadow_draw_text_box(face, 24, 16, transparency,
			 linebuffer, count);
    shadow_render();

    /* pass three -- free data */
    for (tag = 0; tag < ARRAY_SIZE(tags); tag++)
	if (NULL != value[tag])
	    free(value[tag]);
    exif_data_unref (ed);
    for (i = 0; i < count; i++)
	free(linebuffer[i]);
}

static void show_help(void)
{
    static wchar_t *help[] = {
	L"keyboard commands",
	L"~~~~~~~~~~~~~~~~~",
	L"  ESC, Q      - quit",
	L"  pgdn, space - next image",
	L"  pgup        - previous image",
	L"  +/-         - zoom in/out",
	L"  A           - autozoom image",
	L"  cursor keys - scroll image",
	L"",
	L"  H           - show this help text",
	L"  I           - show EXIF info",
	L"  P           - pause slideshow",
	L"  V           - toggle statusline",
	L"",
	L"available if started with --edit switch,",
	L"rotation works for jpeg images only:",
	L"  shift+D     - delete image",
	L"  R           - rotate clockwise",
	L"  L           - rotate counter-clockwise",
    };

    shadow_draw_text_box(face, 24, 16, transparency,
			 help, ARRAY_SIZE(help));
    shadow_render();
}

/* ---------------------------------------------------------------------- */

struct termios  saved_attributes;
int             saved_fl;

static void
tty_raw(void)
{
    struct termios tattr;
    
    fcntl(0,F_GETFL,&saved_fl);
    tcgetattr (0, &saved_attributes);
    
    fcntl(0,F_SETFL,O_NONBLOCK);
    memcpy(&tattr,&saved_attributes,sizeof(struct termios));
    tattr.c_lflag &= ~(ICANON|ECHO);
    tattr.c_cc[VMIN] = 1;
    tattr.c_cc[VTIME] = 0;
    tcsetattr (0, TCSAFLUSH, &tattr);
}

static void
tty_restore(void)
{
    fcntl(0,F_SETFL,saved_fl);
    tcsetattr (0, TCSANOW, &saved_attributes);
}

/* testing: find key codes */
static void debug_key(char *key)
{
    char linebuffer[128];
    int i,len;

    len = sprintf(linebuffer,"key: ");
    for (i = 0; key[i] != '\0'; i++)
	len += snprintf(linebuffer+len, sizeof(linebuffer)-len,
			"%s%c",
			key[i] < 0x20 ? "^" : "",
			key[i] < 0x20 ? key[i] + 0x40 : key[i]);
    status_update(linebuffer, NULL);
}

static void
console_switch(void)
{
    switch (fb_switch_state) {
    case FB_REL_REQ:
	fb_switch_release();
    case FB_INACTIVE:
	visible = 0;
	break;
    case FB_ACQ_REQ:
	fb_switch_acquire();
    case FB_ACTIVE:
	visible = 1;
	ioctl(fd,FBIOPAN_DISPLAY,&fb_var);
	shadow_set_palette(fd);
	shadow_set_dirty();
	shadow_render();
	break;
    default:
	break;
    }
    switch_last = fb_switch_state;
    return;
}

/* ---------------------------------------------------------------------- */

static void free_image(struct ida_image *img)
{
    if (img) {
	if (img->data) {
	    img_mem -= img->i.width * img->i.height * 3;
	    free(img->data);
	}
	free(img);
    }
}

static struct ida_image*
read_image(char *filename)
{
    struct ida_loader *loader = NULL;
    struct ida_image *img;
    struct list_head *item;
    char blk[512];
    FILE *fp, *logfp;
    unsigned int y;
    void *data;
    
    /* open file */
    /* open file */
    if (NULL == (fp = fopen(filename, "r"))) 
    {
	fprintf(stderr,"open %s: %s\n",filename,strerror(errno));
	return NULL;
    }
    memset(blk,0,sizeof(blk));
    fread(blk,1,sizeof(blk),fp);
    rewind(fp);

    char slice1[44] = "/home/pi/fbi/fbi-2.07/read_image.txt";
    if (NULL == (logfp = fopen(slice1, "w"))) {
        //fprintf(stderr,"open %s: %s\n",filename,strerror(errno));
        return NULL;
    }

    fprintf(logfp, "testing write file\n");


    /* pick loader */
    list_for_each(item,&loaders) {
        loader = list_entry(item, struct ida_loader, list);
	if (NULL == loader->magic)
	    break;
	if (0 == memcmp(blk+loader->moff,loader->magic,loader->mlen))
	    break;
	loader = NULL;
    }

    if (NULL == loader) {
	/* no loader found, try to use ImageMagick's convert */
	int p[2];

	if (0 != pipe(p))
	    return NULL;
	switch (fork()) {
	case -1: /* error */
	    perror("fork");
	    close(p[0]);
	    close(p[1]);
	    return NULL;
	case 0: /* child */
	    dup2(p[1], 1 /* stdout */);
	    close(p[0]);
	    close(p[1]);
	    execlp("convert", "convert", "-depth", "8", filename, "ppm:-", NULL);
	    exit(1);
	default: /* parent */
	    close(p[1]);
	    fp = fdopen(p[0], "r");
	    if (NULL == fp)
		return NULL;
	    loader = &ppm_loader;
	}
    }

    /* load image */
    img = malloc(sizeof(*img));
    memset(img,0,sizeof(*img));
    fprintf(logfp, "loader->init loders is %s\n", loader->name);
    data = loader->init(fp,filename,0,&img->i,0);
    if (NULL == data) {
	fprintf(stderr,"loading %s [%s] FAILED\n",filename,loader->name);
	free_image(img);
	return NULL;
    }
    img->data = flist_malloc(img->i.width * img->i.height * 3);
    img_mem += img->i.width * img->i.height * 3;
    for (y = 0; y < img->i.height; y++) {
        if (switch_last != fb_switch_state)
	    console_switch();
	loader->read(img->data + img->i.width * 3 * y, y, data);
    }
    loader->done(data);
    fclose(logfp);
    return img;
}

static struct ida_image*
scale_image(struct ida_image *src, float scale)
{
    struct op_resize_parm p;
    struct ida_rect  rect;
    struct ida_image *dest;
    void *data;
    unsigned int y;

    dest = malloc(sizeof(*dest));
    memset(dest,0,sizeof(*dest));
    memset(&rect,0,sizeof(rect));
    memset(&p,0,sizeof(p));
    
    p.width  = src->i.width  * scale;
    p.height = src->i.height * scale;
    p.dpi    = src->i.dpi;
    if (0 == p.width)
	p.width = 1;
    if (0 == p.height)
	p.height = 1;
    
    data = desc_resize.init(src,&rect,&dest->i,&p);
    dest->data = flist_malloc(dest->i.width * dest->i.height * 3);
    img_mem += dest->i.width * dest->i.height * 3;
    for (y = 0; y < dest->i.height; y++) {
	if (switch_last != fb_switch_state)
	    console_switch();
	desc_resize.work(src,&rect,
			 dest->data + 3 * dest->i.width * y,
			 y, data);
    }
    desc_resize.done(data);
    return dest;
}

static float auto_scale(struct ida_image *img)
{
    float xs,ys,scale;
    
    xs = (float)fb_var.xres / img->i.width;
    if (fitwidth)
	return xs;
    ys = (float)fb_var.yres / img->i.height;
    scale = (xs < ys) ? xs : ys;
    return scale;
}

/* ---------------------------------------------------------------------- */

static void effect_blend(struct flist *f, struct flist *t)
{
    struct timeval start, now;
    int msecs, weight = 0;
    char linebuffer[80];
    int pos = 0;
    int count = 0;

    gettimeofday(&start, NULL);
    do {
	gettimeofday(&now, NULL);
	msecs  = (now.tv_sec  - start.tv_sec)  * 1000;
	msecs += (now.tv_usec - start.tv_usec) / 1000;
	weight = msecs * 100 / blend_msecs;
	if (weight > 100)
	    weight = 100;
	shadow_draw_image(flist_img_get(f), f->left, f->top,
			  0, fb_var.yres-1, 100);
	shadow_draw_image(flist_img_get(t), t->left, t->top,
			  0, fb_var.yres-1, weight);

	if (perfmon) {
	    pos += snprintf(linebuffer+pos, sizeof(linebuffer)-pos,
			    " %d%%", weight);
	    status_update(linebuffer, NULL);
	    count++;
	}

	shadow_render();
    } while (weight < 100);

    if (perfmon) {
	gettimeofday(&now, NULL);
	msecs  = (now.tv_sec  - start.tv_sec)  * 1000;
	msecs += (now.tv_usec - start.tv_usec) / 1000;
	pos += snprintf(linebuffer+pos, sizeof(linebuffer)-pos,
			" | %d/%d -> %d msec",
			msecs, count, msecs/count);
	status_update(linebuffer, NULL);
	shadow_render();
	sleep(2);
    }
}

static int
svga_show(struct flist *f, struct flist *prev,
	  int timeout, char *desc, char *info, int *nr)
{
    static int        paused = 0, skip = KEY_SPACE;

    struct ida_image  *img = flist_img_get(f);
    int               exif = 0, help = 0;
    int               rc;
    char              key[11];
    fd_set            set;
    fd_set            readfds;
    struct timeval    limit;
    char              linebuffer[80];
    int               fdmax;
    int               max_sd;
    int               sd;
    int               i;
    int               activity;
    int               max_clients=30;
    int               new_socket;
    int               valread;
    char              buffer[1025];  //data buffer of 1K
    int               isMasterSocket=0;
    int               socket_key=0;//key do nothing

    *nr = 0;
    if (NULL == img)
	return skip;
    
    redraw = 1;
    for (;;) {
	if (redraw) {
	    redraw = 0;
	    if (img->i.height <= fb_var.yres) {
		f->top = 0;
	    } else {
		if (f->top < 0)
		    f->top = 0;
		if (f->top + fb_var.yres > img->i.height)
		    f->top = img->i.height - fb_var.yres;
	    }
	    if (img->i.width <= fb_var.xres) {
		f->left = 0;
	    } else {
		if (f->left < 0)
		    f->left = 0;
		if (f->left + fb_var.xres > img->i.width)
		    f->left = img->i.width - fb_var.xres;
	    }
	    if (blend_msecs && prev && prev != f &&
		flist_img_get(prev) && flist_img_get(f)) {
		effect_blend(prev, f);
		prev = NULL;
	    } else {
		shadow_draw_image(img, f->left, f->top, 0, fb_var.yres-1, 100);
	    }
	    status_update(desc, info);
	    shadow_render();

	    if (read_ahead) {
		struct flist *f = flist_next(fcurrent,1,0);
		if (f && !f->fimg)
		    flist_img_load(f,1);
		status_update(desc, info);
		shadow_render();
	    }
	}
        if (switch_last != fb_switch_state) {
	    console_switch();
	    continue;
	}
	FD_ZERO(&set);
        FD_ZERO(&readfds);
	FD_SET(0, &set);
        //add master socket to set
        FD_SET(master_socket, &readfds);
        max_sd = master_socket;

        //add child sockets to set
        for ( i = 0 ; i < max_clients ; i++) 
        {
            //socket descriptor
	    sd = client_socket[i];
            
	    //if valid socket descriptor then add to read list
	    if(sd > 0)
            {
	        FD_SET( sd , &readfds);
            }
            
            //highest file descriptor number, need it for the select function
            if(sd > max_sd)
            {
	        max_sd = sd;
            }
        }
	fdmax = 1;
	limit.tv_sec = timeout;
	limit.tv_usec = 0;
        //wait for an activity on one of the sockets , timeout is NULL , so wait indefinitely
        activity = select( max_sd + 1 , &readfds , NULL , NULL , 
                                           ( 0 != timeout) ? &limit : NULL);

        if(activity == 0)
        {
            return KEY_TIMEOUT;
        }


        if (FD_ISSET(master_socket, &readfds)) 
        {
            isMasterSocket=1;
            fprintf(stderr,"master_socket connection found\n");
            if ((new_socket = accept(master_socket, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)
            {
                perror("accept");
                exit(EXIT_FAILURE);
            }
            //add new socket to array of sockets
            for (i = 0; i < max_clients; i++) 
            {
                //if position is empty
	        if( client_socket[i] == 0 )
                {
                    client_socket[i] = new_socket;
                    //printf("Adding to list of sockets as %d\n" , i);
	            break;
                }
            }  
        } else
        {
                isMasterSocket = 0;
        } 

        //else its some IO operation on some other socket :)
        for (i = 0; i < max_clients; i++) 
        {
            sd = client_socket[i];

            if (FD_ISSET( sd , &readfds)) 
            {
                //Check if it was for closing , and also read the incoming message
                if ((valread = read( sd , buffer, 1024)) == 0)
                {
                    //Close the socket and mark as 0 in list for reuse
                    close( sd );
                    client_socket[i] = 0;
                }

                //Echo back the message that came in
                else
                {
                    //set the string terminating NULL byte on the end of the data read
                    buffer[valread] = '\0';
                    send(sd , buffer , strlen(buffer) , 0 );
                    if(*buffer == 'q')
                        socket_key = KEY_Q;
                    if(*buffer == 'b')
                        socket_key = KEY_B;
                    if(*buffer == 'r')
                        socket_key = KEY_R;
                    if(*buffer == 'n')
                        socket_key = KEY_PGDN;
                    if(*buffer == 'c')
                        socket_key = KEY_C;
                    //send(sd , buffer , strlen(buffer) , 0 );
                }
            }

        }
        //if the message doesn't come from the mastersocket but from the client
        //return with key press
        if(activity == 1 && isMasterSocket == 0)
        {
            //its a client connection
            return socket_key;
        } else
        {
            // we are a master socket connection do nothing
            return 0;
        }
        
        if (switch_last != fb_switch_state) {
	    console_switch();
	    continue;
	}
   }
}

static void scale_fix_top_left(struct flist *f, float old, float new)
{
    struct ida_image *img = flist_img_get(f);
    unsigned int width, height;
    float cx,cy;

    cx = (float)(f->left + fb_var.xres/2) / (img->i.width  * old);
    cy = (float)(f->top  + fb_var.yres/2) / (img->i.height * old);

    width   = img->i.width  * new;
    height  = img->i.height * new;
    f->left = cx * width  - fb_var.xres/2;
    f->top  = cy * height - fb_var.yres/2;
}

/* ---------------------------------------------------------------------- */

static char *my_basename(char *filename)
{
    char *h;
    
    h = strrchr(filename,'/');
    if (h)
	return h+1;
    return filename;
}

static char *file_desktop(char *filename)
{
    static char desc[128];
    char *h;

    strncpy(desc,filename,sizeof(desc)-1);
    if (NULL != (h = strrchr(filename,'/'))) {
	snprintf(desc,sizeof(desc),"%.*s/%s", 
		 (int)(h - filename), filename,
		 ".directory");
    } else {
	strcpy(desc,".directory");
    }
    return desc;
}

static char *make_desc(struct ida_image_info *img, char *filename)
{
    static char linebuffer[128];
    struct ida_extra *extra;
    char *desc;
    int len;

    memset(linebuffer,0,sizeof(linebuffer));
    strncpy(linebuffer,filename,sizeof(linebuffer)-1);

    if (comments) {
	extra = load_find_extra(img, EXTRA_COMMENT);
	if (extra)
	    snprintf(linebuffer,sizeof(linebuffer),"%.*s",
		     extra->size,extra->data);
    } else {
	desc = file_desktop(filename);
	len = desktop_read_entry(desc, "Comment=", linebuffer, sizeof(linebuffer));
	if (0 != len)
	    snprintf(linebuffer+len,sizeof(linebuffer)-len,
		     " (%s)", my_basename(filename));
    }
    return linebuffer;
}

static char *make_info(struct ida_image *img, float scale)
{
    static char linebuffer[128];
    
    snprintf(linebuffer, sizeof(linebuffer),
	     "%s%.0f%% %dx%d %d/%d",
	     fcurrent->tag ? "* " : "",
	     scale*100,
	     img->i.width, img->i.height,
	     fcurrent->nr, fcount);
    return linebuffer;
}

static char edit_line(struct ida_image *img, char *line, int max)
{
    int      len = strlen(line);
    int      pos = len;
    int      rc;
    char     key[11];
    fd_set  set;

    do {
	status_edit(line,pos);
	
	FD_SET(0, &set);
	rc = select(1, &set, NULL, NULL, NULL);
        if (switch_last != fb_switch_state) {
	    console_switch();
	    continue;
	}
	rc = read(0, key, sizeof(key)-1);
	if (rc < 1) {
	    /* EOF */
	    return KEY_EOF;
	}
	key[rc] = 0;

	if (0 == strcmp(key,"\x0a")) {
	    /* Enter */
	    return 0;
	    
	} else if (0 == strcmp(key,"\x1b")) {
	    /* ESC */
	    return KEY_ESC;
	    
	} else if (0 == strcmp(key,"\x1b[C")) {
	    /* cursor right */
	    if (pos < len)
		pos++;

	} else if (0 == strcmp(key,"\x1b[D")) {
	    /* cursor left */
	    if (pos > 0)
		pos--;

	} else if (0 == strcmp(key,"\x1b[1~")) {
	    /* home */
	    pos = 0;
	    
	} else if (0 == strcmp(key,"\x1b[4~")) {
	    /* end */
	    pos = len;
	    
	} else if (0 == strcmp(key,"\x7f")) {
	    /* backspace */
	    if (pos > 0) {
		memmove(line+pos-1,line+pos,len-pos+1);
		pos--;
		len--;
	    }

	} else if (0 == strcmp(key,"\x1b[3~")) {
	    /* delete */
	    if (pos < len) {
		memmove(line+pos,line+pos+1,len-pos);
		len--;
	    }

	} else if (1 == rc && isprint(key[0]) && len < max) {
	    /* new key */
	    if (pos < len)
		memmove(line+pos+1,line+pos,len-pos+1);
	    line[pos] = key[0];
	    pos++;
	    len++;
	    line[len] = 0;

	} else if (0 /* debug */) {
	    debug_key(key);
	    sleep(1);
	}
    } while (1);
}

static void edit_desc(struct ida_image *img, char *filename)
{
    static char linebuffer[128];
    char *desc;
    int len, rc;

    desc = file_desktop(filename);
    len = desktop_read_entry(desc, "Comment=", linebuffer, sizeof(linebuffer));
    if (0 == len) {
	linebuffer[0] = 0;
	len = 0;
    }
    rc = edit_line(img, linebuffer, sizeof(linebuffer)-1);
    if (0 != rc)
	return;
    desktop_write_entry(desc, "Directory", "Comment=", linebuffer);
}

/* ---------------------------------------------------------------------- */

static struct ida_image *flist_img_get(struct flist *f)
{
    if (1 != f->scale)
	return f->simg;
    else
	return f->fimg;
}

static void flist_img_free(struct flist *f)
{
    if (!f->fimg)
	return;

    free_image(f->fimg);
    if (f->simg)
	free_image(f->simg);
    f->fimg = NULL;
    f->simg = NULL;
    list_del(&f->lru);
    img_cnt--;
}

static int flist_img_free_lru(void)
{
    struct flist *f;

    if (img_cnt <= min_cnt)
	return -1;
    f = list_entry(flru.next, struct flist, lru);
    flist_img_free(f);
    return 0;
}

static void flist_img_release_memory(void)
{
    int try_release;

    for (;;) {
	try_release = 0;
	if (img_cnt > max_cnt)
	    try_release = 1;
	if (img_mem > max_mem_mb * 1024 * 1024)
	    try_release = 1;
	if (!try_release)
	    break;
	if (0 != flist_img_free_lru())
	    break;
    }
    return;
}

static void *flist_malloc(size_t size)
{
    void *ptr;

    for (;;) {
	ptr = malloc(size);
	if (ptr)
	    return ptr;
	if (0 != flist_img_free_lru()) {
	    status_error("Oops: out of memory, exiting");
	    exit(1);
	}
    }
}

static void flist_img_scale(struct flist *f, float scale, int prefetch)
{
    char linebuffer[128];

    if (!f->fimg)
	return;
    if (f->simg && f->scale == scale)
	return;

    if (f->simg) {
	free_image(f->simg);
	f->simg = NULL;
    }
    if (scale != 1) {
	if (!prefetch) {
	    snprintf(linebuffer, sizeof(linebuffer),
		     "scaling (%.0f%%) %s ...",
		     scale*100, f->name);
	    status_update(linebuffer, NULL);
	}
	f->simg = scale_image(f->fimg,scale);
	if (!f->simg) {
	    snprintf(linebuffer,sizeof(linebuffer),
		     "%s: scaling FAILED",f->name);
	    status_error(linebuffer);
	}
    }
    f->scale = scale;
}

static void flist_img_load(struct flist *f, int prefetch)
{
    char linebuffer[128];
    float scale = 1;
    FILE *fp;

    if (f->fimg) {
	/* touch */
	list_del(&f->lru);
	list_add_tail(&f->lru, &flru);
	return;
    }

    snprintf(linebuffer,sizeof(linebuffer),"%s %s ...",
	     prefetch ? "prefetch" : "loading", f->name);
    status_update(linebuffer, NULL);
    //fprintf(stderr,"timeout test is :\n");
    f->fimg = read_image(f->name);
    if (!f->fimg) {
	snprintf(linebuffer,sizeof(linebuffer),
		 "%s: loading FAILED",f->name);
	status_error(linebuffer);
	return;
    }

    if (!f->seen) {
	scale = 1;
	if (autoup || autodown) {
	    scale = auto_scale(f->fimg);
	    if (scale < 1 && !autodown)
		scale = 1;
	    if (scale > 1 && !autoup)
		scale = 1;
	}
    } else {
	scale = f->scale;
    }
    flist_img_scale(f, scale, prefetch);

    if (!f->seen) {
 	struct ida_image *img = flist_img_get(f);
	if (img->i.width > fb_var.xres)
	    f->left = (img->i.width - fb_var.xres) / 2;
	if (img->i.height > fb_var.yres) {
	    f->top = (img->i.height - fb_var.yres) / 2;
	    if (textreading) {
		int pages = ceil((float)img->i.height / fb_var.yres);
		f->text_steps = ceil((float)img->i.height / pages);
		f->top = 0;
	    }
	}
    }

    list_add_tail(&f->lru, &flru);
    f->seen = 1;
    img_cnt++;
}

static void load_files_delays()
{
    FILE *fp;
    FILE *pngfp;
    char str[60];
    char **delaynumbers;
    char **pngfiles;
    int variableNumberOfElements=0;
    int i =0;
    int delayvalue;
    char ch;

    fp = fopen("/home/pi/fbi/fbi-2.07/print/delays.txt" , "r");
    while(!feof(fp))
    {
        ch = fgetc(fp);
        if(ch == '\n')
        {
            variableNumberOfElements++;
        }
    }


    fclose(fp);

    delaynumbers = malloc(variableNumberOfElements * sizeof(char*));
    pngfiles     = malloc(variableNumberOfElements * sizeof(char*));
    for (i=0; i < variableNumberOfElements; i++)
    {
        delaynumbers[i] = malloc((60+1) * sizeof(char));
        pngfiles[i]      = malloc((60+1) * sizeof(char));
    }

    fp    = fopen("/home/pi/fbi/fbi-2.07/print/delays.txt" , "r");
    pngfp = fopen("/home/pi/fbi/fbi-2.07/print/pngfiles.txt" , "r");
    if(fp == NULL || pngfp == NULL) 
    {
       perror("Error opening file");
    //   return(-1);
    }
    for(i=0;i<variableNumberOfElements;i++)
    {
        fgets(delaynumbers[i],60,fp);
        delayvalue = atoi(delaynumbers[i]);
        fgets(pngfiles[i],60,pngfp);
        //fprintf(stderr,"delay value found %d\n", delayvalue);
        //fprintf(stderr,"png value found %s\n", pngfiles[i]);
        flist_timeout_add(pngfiles[i], delayvalue);
    }
    fclose(fp);
    fclose(pngfp);

}

/* ---------------------------------------------------------------------- */

static void cleanup_and_exit(int code)
{
    shadow_fini();
    fb_clear_screen();
    tty_restore();
    fb_cleanup();
    flist_print_tagged(stdout);
    exit(code);
}

int
main(int argc, char *argv[])
{
    int              once;
    int              i, arg, key;
    char             *info, *desc, *filelist;
    char             linebuffer[128];
    struct flist     *fprev = NULL;

//socket connection
    int opt = 1;
    int client_nr;

    //char buffer[1025];  //data buffer of 1K

    //set of socket descriptors
    //fd_set readfds;
    //loading some files
    //load_files_delays();


   //initialise all client_socket[] to 0 so not checked
    for (client_nr = 0; client_nr < max_clients; client_nr++) 
    {
        client_socket[client_nr] = 0;
    }

    //create a master socket
    if( (master_socket = socket(AF_INET , SOCK_STREAM , 0)) == 0) 
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    //set master socket to allow multiple connections , this is just a good habit, it will work without this
    if( setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 )
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    //type of socket created
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( PORT );

    //bind the socket to localhost port 8888
    if (bind(master_socket, (struct sockaddr *)&address, sizeof(address))<0) 
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    //try to specify maximum of 3 pending connections for the master socket
    if (listen(master_socket, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    //accept the incoming connection
    addrlen = sizeof(address);

#if 0
    /* debug aid, to attach gdb ... */ 
    fprintf(stderr,"pid %d\n",getpid());
    sleep(10);
#endif

    setlocale(LC_ALL,"");
#ifdef HAVE_LIBLIRC
    lirc = lirc_fbi_init();
#endif
    fbi_read_config();
    cfg_parse_cmdline(&argc,argv,fbi_cmd);
    cfg_parse_cmdline(&argc,argv,fbi_cfg);

    if (GET_AUTO_ZOOM()) {
	cfg_set_bool(O_AUTO_UP,   1);
	cfg_set_bool(O_AUTO_DOWN, 1);
    }

    if (GET_HELP()) {
	usage(argv[0]);
	exit(0);
    }
    if (GET_VERSION()) {
	version();
	exit(0);
    }
    if (GET_WRITECONF())
	fbi_write_config();

    once        = GET_ONCE();
    autoup      = GET_AUTO_UP();
    autodown    = GET_AUTO_DOWN();
    fitwidth    = GET_FIT_WIDTH();
    statusline  = GET_VERBOSE();
    textreading = GET_TEXT_MODE();
    editable    = GET_EDIT();
    backup      = GET_BACKUP();
    preserve    = GET_PRESERVE();
    read_ahead  = GET_READ_AHEAD();

    max_mem_mb  = GET_CACHE_MEM();
    blend_msecs = GET_BLEND_MSECS();
    v_steps     = GET_SCROLL();
    h_steps     = GET_SCROLL();
    timeout     = GET_TIMEOUT();
    pcd_res     = GET_PCD_RES();

    fprintf(stderr,"timeout is :%d\n",timeout);

    fbgamma     = GET_GAMMA();

    fontname    = cfg_get_str(O_FONT);
    filelist    = cfg_get_str(O_FILE_LIST);
    
    if (filelist)
	flist_add_list(filelist);
    for (i = optind; i < argc; i++)
    {
	flist_add(argv[i]);
        blank_add(argv[i]);
    }
    flist_renumber();

    if (0 == fcount) {
	usage(argv[0]);
	exit(1);
    }

    if (GET_RANDOM())
	flist_randomize();
    //fcurrent = flist_first();
    fcurrent = get_blank();

    font_init();
    if (NULL == fontname)
	fontname = "monospace:size=16";
    face = font_open(fontname);
    if (NULL == face) {
	fprintf(stderr,"can't open font: %s\n",fontname);
	exit(1);
    }
    //fprintf(stderr,"device %s\n", cfg_get_str(O_DEVICE));
    //fprintf(stderr, "video mode %s\n", cfg_get_str(O_VIDEO_MODE));
    //fprintf(stderr, "VT %s\n", GET_VT());
    fd = fb_init(cfg_get_str(O_DEVICE),
		 cfg_get_str(O_VIDEO_MODE),
		 GET_VT());
    fb_catch_exit_signals();
    fb_switch_init();
    shadow_init();
    shadow_set_palette(fd);
    signal(SIGTSTP,SIG_IGN);
    
    /* svga main loop */
    tty_raw();
    desc = NULL;
    info = NULL;
    for (;;) {
	flist_img_load(fcurrent, 0);
	flist_img_release_memory();
	img = flist_img_get(fcurrent);
	if (img) {
	    desc = make_desc(&fcurrent->fimg->i, fcurrent->name);
	    info = make_info(fcurrent->fimg, fcurrent->scale);
	}

	key = svga_show(fcurrent, fprev, timeout, desc, info, &arg);
	fprev = fcurrent;
	switch (key) {
	case KEY_DELETE:
	    if (editable) {
		struct flist *fdel = fcurrent;
		if (flist_islast(fcurrent))
		    fcurrent = flist_prev(fcurrent,0);
		else
		    fcurrent = flist_next(fcurrent,0,0);
		unlink(fdel->name);
		flist_img_free(fdel);
		flist_del(fdel);
		flist_renumber();
		if (list_empty(&flist)) {
		    /* deleted last one */
		    cleanup_and_exit(0);
		}
	    } else {
		status_error("readonly mode, sorry [start with --edit?]");
	    }
	    break;
	case KEY_ROT_CW:
	case KEY_ROT_CCW:
	{
	    if (editable) {
		snprintf(linebuffer,sizeof(linebuffer),
			 "rotating %s ...",fcurrent->name);
		status_update(linebuffer, NULL);
		jpeg_transform_inplace
		    (fcurrent->name,
		     (key == KEY_ROT_CW) ? JXFORM_ROT_90 : JXFORM_ROT_270,
		     NULL,
		     NULL,0,
		     (backup   ? JFLAG_FILE_BACKUP    : 0) | 
		     (preserve ? JFLAG_FILE_KEEP_TIME : 0) | 
		     JFLAG_TRANSFORM_IMAGE     |
		     JFLAG_TRANSFORM_THUMBNAIL |
		     JFLAG_UPDATE_ORIENTATION);
		flist_img_free(fcurrent);
	    } else {
		status_error("readonly mode, sorry [start with --edit?]");
	    }
	    break;
	}
	case KEY_TAGFILE:
	    fcurrent->tag = !fcurrent->tag;
	    /* fall throuth */
	case KEY_SPACE:
	    fcurrent = flist_next(fcurrent,1,0);
	    if (NULL != fcurrent)
		break;
	    /* else fall */
	case KEY_ESC:
	case KEY_Q:
	case KEY_EOF:
	    cleanup_and_exit(0);
	    break;
	case KEY_PGDN:
	    //fcurrent = flist_next(fcurrent,0,1);
            fmain_list = flist_next(fmain_list,0,1);
            fcurrent = fmain_list;
	    break;
	case KEY_PGUP:
	    //fcurrent = flist_prev(fcurrent,1);
            fcurrent = flist_prev(fmain_list,1);
	    break;
	case KEY_TIMEOUT:
	    fcurrent = flist_next(fcurrent,once,1);
	    if (NULL == fcurrent) {
                //set blank image
                fcurrent = flist_first();
                //set timeout to 0
                timeout = 0;
		//cleanup_and_exit(0);
	    }
	    /* FIXME: wrap around */
	    break;
	case KEY_PLUS:
	case KEY_MINUS:
	case KEY_ASCALE:
	case KEY_SCALE:
	    {
		float newscale, oldscale = fcurrent->scale;

		if (key == KEY_PLUS) {
		    newscale = fcurrent->scale * 1.6;
		} else if (key == KEY_MINUS) {
		    newscale = fcurrent->scale / 1.6;
		} else if (key == KEY_ASCALE) {
		    newscale = auto_scale(fcurrent->fimg);
		} else {
		    newscale = arg / 100.0;
		}
		if (newscale < 0.1)
		    newscale = 0.1;
		if (newscale > 10)
		    newscale = 10;
		flist_img_scale(fcurrent, newscale, 0);
		scale_fix_top_left(fcurrent, oldscale, newscale);
		break;
	    }
	case KEY_GOTO:
	    if (arg > 0 && arg <= fcount)
		fcurrent = flist_goto(arg);
	    break;
	case KEY_DELAY:
	    timeout = arg;
	    break;
	case KEY_VERBOSE:
#if 0 /* fbdev testing/debugging hack */
	    {
		ioctl(fd,FBIOBLANK,1);
		sleep(1);
		ioctl(fd,FBIOBLANK,0);
	    }
#endif
	    statusline = !statusline;
	    break;
	case KEY_DESC:
	    if (!comments) {
		edit_desc(img, fcurrent->name);
		desc = make_desc(&fcurrent->fimg->i,fcurrent->name);
	    }
	    break;
        case KEY_B:
           {
            char slice1[44] = "/home/pi/fbi/fbi-2.07/slice/triado_2067.png";
            char slice2[44] = "/home/pi/fbi/fbi-2.07/slice/triado_2068.png";
            char slice3[44] = "/home/pi/fbi/fbi-2.07/slice/triado_2069.png";
            flist_add(slice1);
            flist_add(slice2);
            flist_add(slice3);
            flist_renumber();
            fmain_list = flist_first();
            //fcurrent = flist_first();
            //timeout=2;
            break;
           } 
        case KEY_R:
           {
            //fcurrent = flist_first();
            fmain_list = flist_first();
            //timeout=2;
           }
        case KEY_C:
           {
            fcurrent = get_blank();
           }
	}
    }
}
