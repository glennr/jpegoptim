/*******************************************************************
 * JPEGoptim
 * Copyright (c) Timo Kokkonen, 1996,2002,2004.
 *
 * requires libjpeg.a (from JPEG Group's JPEG software 
 *                     release 6a or later...)
 *
 * $Id: jpegoptim.c,v 1.14 2009/09/30 07:16:58 tjko Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <utime.h>
#include <dirent.h>
#if HAVE_GETOPT_H && HAVE_GETOPT_LONG
#include <getopt.h>
#else
#include "getopt.h"
#endif
#include <signal.h>
#include <string.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <time.h>
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif

#define VERSIO "1.2.3"

#ifdef BROKEN_METHODDEF
#undef METHODDEF
#define METHODDEF(x) static x
#endif

#define EXIF_JPEG_MARKER   JPEG_APP0+1
#define EXIF_IDENT_STRING  "Exif\000\000"
#define EXIF_IDENT_STRING_SIZE 6

#define IPTC_JPEG_MARKER   JPEG_APP0+13

#define ICC_JPEG_MARKER   JPEG_APP0+2
#define ICC_IDENT_STRING  "ICC_PROFILE\0"
#define ICC_IDENT_STRING_SIZE 12

void fatal(const char *msg);

struct my_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;   
};

typedef struct my_error_mgr * my_error_ptr;

struct jpeg_decompress_struct dinfo;
struct jpeg_compress_struct cinfo;
struct my_error_mgr jcerr,jderr;

const char *rcsid = "$Id: jpegoptim.c,v 1.14 2009/09/30 07:16:58 tjko Exp $";

struct option long_options[] = {
  {"verbose",0,0,'v'},
  {"help",0,0,'h'},
  {"quiet",0,0,'q'},
  {"max",1,0,'m'},
  {"totals",0,0,'t'},
  {"noaction",0,0,'n'},
  {"dest",1,0,'d'},
  {"force",0,0,'f'},
  {"version",0,0,'V'},
  {"preserve",0,0,'p'},
  {"strip-all",0,0,'s'},
  {"strip-com",0,0,'c'},
  {"strip-exif",0,0,'e'},
  {"strip-iptc",0,0,'i'},
  {"strip-icc",0,0,'P'},
  {0,0,0,0}
};

int verbose_mode = 0;
int quiet_mode = 0;
int global_error_counter = 0;
int preserve_mode = 0;
int overwrite_mode = 0;
int totals_mode = 0;
int noaction = 0;
int quality = -1;
int retry = 0;
int dest = 0;
int force = 0;
int save_exif = 1;
int save_iptc = 1;
int save_com = 1;
int save_icc = 1;
char *outfname = NULL;
FILE *infile = NULL, *outfile = NULL;

JSAMPARRAY buf = NULL;
jvirt_barray_ptr *coef_arrays = NULL;
jpeg_saved_marker_ptr exif_marker = NULL;
jpeg_saved_marker_ptr iptc_marker = NULL;
jpeg_saved_marker_ptr icc_marker = NULL;
long average_count = 0;
double average_rate = 0.0, total_save = 0.0;

/*****************************************************************/

METHODDEF(void) 
my_error_exit (j_common_ptr cinfo)
{
  my_error_ptr myerr = (my_error_ptr)cinfo->err;
  (*cinfo->err->output_message) (cinfo);
  longjmp(myerr->setjmp_buffer,1);
}

METHODDEF(void)
my_output_message (j_common_ptr cinfo)
{
  char buffer[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message) (cinfo, buffer); 
  if (verbose_mode) printf(" (%s) ",buffer);
  global_error_counter++;
}


void p_usage(void) 
{
 if (!quiet_mode) {
  fprintf(stderr,"jpegoptim v" VERSIO 
	  "  Copyright (c) Timo Kokkonen, 1996-2009.\n"); 

  fprintf(stderr,
       "Usage: jpegoptim [options] <filenames> \n\n"
    "  -d<path>, --dest=<path>\n"
    "                  specify alternative destination directory for \n"
    "                  optimized files (default is to overwrite originals)\n"
    "  -f, --force     force optimization\n"
    "  -h, --help      display this help and exit\n"
    "  -m[0..100], --max=[0..100] \n"
    "                  set maximum image quality factor (disables lossless\n"
    "                  optimization mode, which is by default on)\n"
    "  -n, --noaction  don't really optimize files, just print results\n"
    "  -o, --overwrite overwrite target file even if it exists\n"
    "  -p, --preserve  preserve file timestamps\n"
    "  -q, --quiet     quiet mode\n"
    "  -t, --totals    print totals after processing all files\n"
    "  -v, --verbose   enable verbose mode (positively chatty)\n"
    "  -V, --version   print program version\n\n"
    "  --strip-all     strip all (Comment & Exif) markers from output file\n"
    "  --strip-com     strip Comment markers from output file\n"
    "  --strip-exif    strip Exif markers from output file\n"
    "  --strip-iptc    strip IPTC markers from output file\n"
    "  --strip-icc     strip ICC profile markers from output file\n"
    "\n\n");
 }

 exit(1);
}

int delete_file(char *name)
{
  int retval;

  if (!name) return -1;
  if (verbose_mode > 1 && !quiet_mode) fprintf(stderr,"deleting: %s\n",name);
  if ((retval=unlink(name)) && !quiet_mode) 
    fprintf(stderr,"jpegoptim: error removing file: %s\n",name);

  return retval;
}

long file_size(FILE *fp)
{
  struct stat buf;

  if (!fp) return -1;
  if (fstat(fileno(fp),&buf)) return -2;
  return (long)buf.st_size;
}

int is_directory(const char *path)
{
  DIR *dir;

  if (!path) return 0;
  if (!(dir = opendir(path))) return 0;
  closedir(dir);
  return 1;
}


int is_dir(FILE *fp, time_t *atime, time_t *mtime)
{
 struct stat buf;

 if (!fp) return 0;
 if (fstat(fileno(fp),&buf)) return 0;
 if (atime) *atime=buf.st_atime;
 if (mtime) *mtime=buf.st_mtime;
 if (S_ISDIR(buf.st_mode)) return 1;
 return 0;
}


int file_exists(const char *pathname)
{
  FILE *file;

  if (!pathname) return 0;
  if (!(file=fopen(pathname,"r"))) return 0;
  fclose(file);
  return 1;
}


char *splitdir(const char *pathname, char *buf, int buflen)
{
  char *s = NULL;
  int size = 0;

  if (!pathname || !buf || buflen < 2) return NULL;

  if ((s = strrchr(pathname,'/'))) size = (s-pathname)+1;
  if (size >= buflen) return NULL;
  if (size > 0) memcpy(buf,pathname,size);
  buf[size]=0;

  return buf;
}


void own_signal_handler(int a)
{
  if (verbose_mode > 1) printf("jpegoptim: signal: %d\n",a);
  if (outfile) fclose(outfile);
  if (outfname) if (file_exists(outfname)) delete_file(outfname);
  exit(1);
}


void fatal(const char *msg)
{
  if (!msg) msg="(NULL)";
  fprintf(stderr,"jpegoptim: %s.\n",msg);

  if (outfile) fclose(outfile);
  if (outfname) if (file_exists(outfname)) delete_file(outfname);
  exit(3);
}

void write_markers(struct jpeg_decompress_struct *dinfo,
		   struct jpeg_compress_struct *cinfo)
{
  jpeg_saved_marker_ptr mrk;

  if (!cinfo || !dinfo) return;

  mrk=dinfo->marker_list;
  while (mrk) {
    if (save_com && mrk->marker == JPEG_COM) 
      jpeg_write_marker(cinfo,JPEG_COM,mrk->data,mrk->data_length);

    if (save_iptc && mrk->marker == IPTC_JPEG_MARKER) 
      jpeg_write_marker(cinfo,IPTC_JPEG_MARKER,mrk->data,mrk->data_length);

    if (save_exif && mrk->marker == EXIF_JPEG_MARKER) {
      if (!memcmp(mrk->data,EXIF_IDENT_STRING,EXIF_IDENT_STRING_SIZE)) {
	jpeg_write_marker(cinfo,EXIF_JPEG_MARKER,mrk->data,mrk->data_length);
      }
    }
     
    if (save_icc && mrk->marker == ICC_JPEG_MARKER) {
      if (!memcmp(mrk->data,ICC_IDENT_STRING,ICC_IDENT_STRING_SIZE)) {
	jpeg_write_marker(cinfo,ICC_JPEG_MARKER,mrk->data,mrk->data_length);
      }
    }
     
    mrk=mrk->next;
  }
}

/*****************************************************************/
int main(int argc, char **argv) 
{
  char tmpfilename[MAXPATHLEN],tmpdir[MAXPATHLEN];
  char newname[MAXPATHLEN], dest_path[MAXPATHLEN];
  volatile int i;
  int c,j, err_count;
  int opt_index = 0;
  long insize,outsize;
  double ratio;
  struct utimbuf time_save;
  jpeg_saved_marker_ptr cmarker; 


  if (rcsid); /* so compiler won't complain about "unused" rcsid string */

  signal(SIGINT,own_signal_handler);
  signal(SIGTERM,own_signal_handler);

  /* initialize decompression object */
  dinfo.err = jpeg_std_error(&jderr.pub);
  jpeg_create_decompress(&dinfo);
  jderr.pub.error_exit=my_error_exit;
  jderr.pub.output_message=my_output_message;

  /* initialize compression object */
  cinfo.err = jpeg_std_error(&jcerr.pub);
  jpeg_create_compress(&cinfo);
  jcerr.pub.error_exit=my_error_exit;
  jcerr.pub.output_message=my_output_message;


  if (argc<2) {
    if (!quiet_mode) fprintf(stderr,"jpegoptim: file arguments missing\n"
			     "Try 'jpegoptim --help' for more information.\n");
    exit(1);
  }
 
  /* parse command line parameters */
  while(1) {
    opt_index=0;
    if ((c=getopt_long(argc,argv,"d:hm:ntqvfVpo",long_options,&opt_index))
	      == -1) 
      break;

    switch (c) {
    case 'm':
      {
        int tmpvar;

        if (sscanf(optarg,"%d",&tmpvar) == 1) {
	  quality=tmpvar;
	  if (quality < 0) quality=0;
	  if (quality > 100) quality=100;
	}
	else if (!quiet_mode) {
	  fatal("invalid argument for -m, --max");
	}
      }
      break;
    case 'd':
      if (realpath(optarg,dest_path)==NULL || !is_directory(dest_path)) {
	fatal("invalid argument for option -d, --dest");
      }
      if (verbose_mode) 
	fprintf(stderr,"Destination directory: %s\n",dest_path);
      dest=1;
      break;
    case 'v':
      verbose_mode++;
      break;
    case 'h':
      p_usage();
      break;
    case 'q':
      quiet_mode=1;
      break;
    case 't':
      totals_mode=1;
      break;
    case 'n':
      noaction=1;
      break;
    case 'f':
      force=1;
      break;
    case '?':
      break;
    case 'V':
      printf("jpegoptim v%s  %s\n",VERSIO,HOST_TYPE);
      printf("Copyright (c) Timo Kokkonen, 1996-2009.\n");
      exit(0);
      break;
    case 'o':
      overwrite_mode=1;
      break;
    case 'p':
      preserve_mode=1;
      break;
    case 's':
      save_exif=0;
      save_iptc=0;
      save_com=0;
      save_icc=0;
      break;
    case 'c':
      save_com=0;
      break;
    case 'e':
      save_exif=0;
      break;
    case 'i':
      save_iptc=0;
      break;
    case 'P':
      save_icc=0;
      break;

    default:
      if (!quiet_mode) 
	fprintf(stderr,"jpegoptim: error parsing parameters.\n");
    }
  }


  if (verbose_mode && (quality>0)) 
    fprintf(stderr,"Image quality limit set to: %d\n",quality);


  /* loop to process the input files */
  i=1;  
  do {
   if (!argv[i][0]) continue;
   if (argv[i][0]=='-') continue;

   if (!noaction) {
     /* generate temp (& new) filename */
     if (dest) {
       strncpy(tmpdir,dest_path,sizeof(tmpdir));
       strncpy(newname,dest_path,sizeof(newname));
       if (tmpdir[strlen(tmpdir)-1] != '/') {
	 strncat(tmpdir,"/",sizeof(tmpdir)-strlen(tmpdir));
	 strncat(newname,"/",sizeof(newname)-strlen(newname));
       }
       strncat(newname,(char*)basename(argv[i]),
	       sizeof(newname)-strlen(newname));
     } else {
       if (!splitdir(argv[i],tmpdir,sizeof(tmpdir))) 
	 fatal("splitdir() failed!");
       strncpy(newname,argv[i],sizeof(newname));
     }
     snprintf(tmpfilename,sizeof(tmpfilename),
	      "%sjpegoptim-%d-%d.tmp", tmpdir, (int)getuid(), (int)getpid());
   }

  retry_point:
   if ((infile=fopen(argv[i],"r"))==NULL) {
     if (!quiet_mode) fprintf(stderr, "jpegoptim: can't open %s\n", argv[i]);
     continue;
   }
   if (is_dir(infile,&time_save.actime,&time_save.modtime)) {
     fclose(infile);
     if (verbose_mode) printf("directory: %s  skipped\n",argv[i]); 
     continue;
   }

   /* setup error handling for decompress */
   if (setjmp(jderr.setjmp_buffer)) {
      jpeg_abort_decompress(&dinfo);
      fclose(infile);
      printf(" [ERROR]\n");
      continue;
   }

   if (!retry) { printf("%s ",argv[i]); fflush(stdout); }

   /* prepare to decompress */
   global_error_counter=0;
   err_count=jderr.pub.num_warnings;
   if (save_com) jpeg_save_markers(&dinfo, JPEG_COM, 0xffff);
   if (save_iptc) jpeg_save_markers(&dinfo, IPTC_JPEG_MARKER, 0xffff);
   if (save_exif) jpeg_save_markers(&dinfo, EXIF_JPEG_MARKER, 0xffff);
   if (save_icc) jpeg_save_markers(&dinfo, ICC_JPEG_MARKER, 0xffff);

   jpeg_stdio_src(&dinfo, infile);
   jpeg_read_header(&dinfo, TRUE); 

   /* check for Exif/IPTC markers */
   exif_marker=NULL;
   iptc_marker=NULL;
   icc_marker=NULL;
   cmarker=dinfo.marker_list;
   while (cmarker) {
     if (cmarker->marker == EXIF_JPEG_MARKER) {
       if (!memcmp(cmarker->data,EXIF_IDENT_STRING,EXIF_IDENT_STRING_SIZE)) 
	 exif_marker=cmarker;
     }
     if (cmarker->marker == IPTC_JPEG_MARKER) {
       iptc_marker=cmarker;
     }
     if (cmarker->marker == ICC_JPEG_MARKER) {
       if (!memcmp(cmarker->data,ICC_IDENT_STRING,ICC_IDENT_STRING_SIZE)) 
	 icc_marker=cmarker;
     }
     cmarker=cmarker->next;
   }


   if (!retry) {
     printf("%dx%d %dbit ",(int)dinfo.image_width,
	    (int)dinfo.image_height,(int)dinfo.num_components*8);

     if (exif_marker) printf("Exif ");
     if (iptc_marker) printf("IPTC ");
     if (icc_marker) printf("ICC ");
     if (dinfo.saw_Adobe_marker) printf("Adobe ");
     if (dinfo.saw_JFIF_marker) printf("JFIF ");
     fflush(stdout);
   }

   insize=file_size(infile);

  /* decompress the file */
   if (quality>=0 && !retry) {
     jpeg_start_decompress(&dinfo);

     buf = malloc(sizeof(JSAMPROW)*dinfo.output_height);
     if (!buf) fatal("not enough memory");
     for (j=0;j<dinfo.output_height;j++) {
       buf[j]=malloc(sizeof(JSAMPLE)*dinfo.output_width*
		     dinfo.out_color_components);
       if (!buf[j]) fatal("not enough memory");
     }

     while (dinfo.output_scanline < dinfo.output_height) {
       jpeg_read_scanlines(&dinfo,&buf[dinfo.output_scanline],
			   dinfo.output_height-dinfo.output_scanline);
     }
   } else {
     coef_arrays = jpeg_read_coefficients(&dinfo);
   }

   if (!retry) {
     if (!global_error_counter) printf(" [OK] ");
     else printf(" [WARNING] ");
     fflush(stdout);
   }


   if (dest && !noaction) {
     if (file_exists(newname) && !overwrite_mode) {
       fprintf(stderr,"target file already exists!\n");
       jpeg_abort_decompress(&dinfo);
       fclose(infile);
       for (j=0;j<dinfo.output_height;j++) free(buf[j]);
       free(buf); buf=NULL;
       continue;
     }
   }

   if (noaction) {
     outfname=NULL;
     if ((outfile=tmpfile())==NULL) fatal("error opening temp file");
   } else {
     outfname=tmpfilename;
     if ((outfile=fopen(outfname,"w"))==NULL) 
       fatal("error opening target file");
   }

   if (setjmp(jcerr.setjmp_buffer)) {
      jpeg_abort_compress(&cinfo);
      jpeg_abort_decompress(&dinfo);
      fclose(outfile);
      if (infile) fclose(infile);
      printf(" [Compress ERROR]\n");
      for (j=0;j<dinfo.output_height;j++) free(buf[j]);
      free(buf); buf=NULL;
      if (file_exists(outfname)) delete_file(outfname);
      outfname=NULL;
      continue;
   }


   jpeg_stdio_dest(&cinfo, outfile);

   if (quality>=0 && !retry) {
     /* lossy "optimization" ... */

     cinfo.in_color_space=dinfo.out_color_space;
     cinfo.input_components=dinfo.output_components;
     cinfo.image_width=dinfo.image_width;
     cinfo.image_height=dinfo.image_height;
     jpeg_set_defaults(&cinfo); 
     jpeg_set_quality(&cinfo,quality,TRUE);
     cinfo.optimize_coding = TRUE;
     
     j=0;
     jpeg_start_compress(&cinfo,TRUE);
     
     /* write markers */
     write_markers(&dinfo,&cinfo);

     while (cinfo.next_scanline < cinfo.image_height) {
       jpeg_write_scanlines(&cinfo,&buf[cinfo.next_scanline],
			    dinfo.output_height);
     }
     
     for (j=0;j<dinfo.output_height;j++) free(buf[j]);
     free(buf); buf=NULL;
   } else {
     /* lossless "optimization" ... */

     jpeg_copy_critical_parameters(&dinfo, &cinfo);
     cinfo.optimize_coding = TRUE;

     jpeg_write_coefficients(&cinfo, coef_arrays);

     /* write markers */
     write_markers(&dinfo,&cinfo);
   }



   jpeg_finish_compress(&cinfo);
   jpeg_finish_decompress(&dinfo);
   fclose(infile);
   outsize=file_size(outfile);
   fclose(outfile);

   if (preserve_mode && !noaction) {
     if (utime(outfname,&time_save) != 0) {
       fprintf(stderr,"jpegoptim: failed to reset output file time/date\n");
     }
   }

   if (quality>=0 && outsize>=insize && !retry) {
     if (verbose_mode) printf("(retry w/lossless) ");
     retry=1;
     goto retry_point; 
   }

   retry=0;
   ratio=(insize-outsize)*100.0/insize;
   printf("%ld --> %ld bytes (%0.2f%%), ",insize,outsize,ratio);
   average_count++;
   average_rate+=(ratio<0 ? 0.0 : ratio);

   if (outsize<insize || force) {
        total_save+=(insize-outsize)/1024.0;
	printf("optimized.\n");
        if (noaction) continue;
	if (verbose_mode > 1 && !quiet_mode) 
	  fprintf(stderr,"renaming: %s to %s\n",outfname,newname);
	if (rename(outfname,newname)) fatal("cannot rename temp file");
   } else {
	printf("skipped.\n");
	if (!noaction) delete_file(outfname);
   }
   

  } while (++i<argc);


  if (totals_mode&&!quiet_mode)
    printf("Average ""compression"" (%ld files): %0.2f%% (%0.0fk)\n",
	   average_count, average_rate/average_count, total_save);
  jpeg_destroy_decompress(&dinfo);
  jpeg_destroy_compress(&cinfo);

  return 0;
}

/* :-) */