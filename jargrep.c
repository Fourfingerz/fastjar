#include "config.h"
#include <stdio.h>
#include <unistd.h>
#include <regex.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "jargrep.h"
#include "jartool.h"
#include "pushback.h"
#include "zipfile.h"

char *Usage = { "Usage: grepjar [-bcinqsvw] <-e regexp | regexp> file(s)\n" };

int options;
extern char *optarg;
char *regexp;

int opt_valid(int options)
{
int retflag;

	if(((options & JG_PRINT_COUNT) && 
		(options & (JG_PRINT_BYTEOFFSET | JG_PRINT_LINE_NUMBER | JG_SUPRESS_OUTPUT))) 
		|| (options & JG_SUPRESS_OUTPUT)  &&
		(options & (JG_PRINT_LINE_NUMBER | JG_PRINT_BYTEOFFSET)))
	{
		retflag = FALSE;
	}
	else retflag = TRUE;

	return retflag;
}

regex_t *create_regexp(char *regstr, int options)
{
regex_t *exp;
int exp_flags = 0, errcode, msgsize;
char *errmsg;

	if(exp = (regex_t *) malloc(sizeof(regex_t)))
	{
		if(errcode = regcomp(exp, regstr, (options & JG_IGNORE_CASE) ? REG_ICASE : 0)) {
			fprintf(stderr, "regcomp of regex failed,\n");
			if(errmsg = (char *) malloc(msgsize = regerror(errcode, exp, NULL, 0) + 1)) {
				regerror(errcode, exp, errmsg, msgsize);
				fprintf(stderr, "Error: %s\n", errmsg);
				free(exp);
				free(errmsg);
				exit(4);
			}
			else {
				fprintf(stderr, "Malloc of errmsg failed.\n");
				fprintf(stderr, "Error: %s\n", strerror(errno));
				free(exp);
				exit(5);
			}
		}
	}
	else {
		fprintf(stderr, "Malloc of regex failed,\n");
		fprintf(stderr, "Error: %s\n", strerror(errno));
		exit(3);
	}

	return exp; 
}

int check_sig(ub4 *signaturep, ub1 *scratch, pb_file *pbfp)
{
int retflag = 0;

	*signaturep = UNPACK_UB4(scratch, 0);

#ifdef DEBUG    
    printf("signature is %x\n", *signaturep);
#endif
    if(*signaturep == 0x08074b50){
#ifdef DEBUG    
      printf("skipping data descriptor\n");
#endif
      pb_read(pbfp, scratch, 12);
      retflag = 2;
    } else if(*signaturep == 0x02014b50){
#ifdef DEBUG    
      printf("Central header reached.. we're all done!\n");
#endif
      retflag = 1;
    }else if(*signaturep != 0x04034b50){
      printf("Ick! %#x\n", *signaturep);
      retflag = 1;
    }
    
	return retflag;
}

void decd_siz(ub4 *csize, ub4 *usize, ub2 *fnlen, ub2 *eflen, ub2 *flags, ub2 *method, ub4 *crc, ub1 *file_header)
{
    *csize = UNPACK_UB4(file_header, LOC_CSIZE);
#ifdef DEBUG    
    printf("Compressed size is %u\n", *csize);
#endif

	*usize = UNPACK_UB4(file_header, LOC_USIZE);
#ifdef DEBUG
	printf("Uncompressed size is %u\n", *usize);
#endif

    *fnlen = UNPACK_UB2(file_header, LOC_FNLEN);
#ifdef DEBUG    
    printf("Filename length is %hu\n", *fnlen);
#endif

    *eflen = UNPACK_UB2(file_header, LOC_EFLEN);
#ifdef DEBUG    
    printf("Extra field length is %hu\n", *eflen);
#endif

    *flags = UNPACK_UB2(file_header, LOC_EXTRA);
#ifdef DEBUG    
    printf("Flags are %#hx\n", *flags);
#endif

    *method = UNPACK_UB2(file_header, LOC_COMP);
#ifdef DEBUG
    printf("Compression method is %#hx\n", *method);
#endif

    /* if there isn't a data descriptor */
    if(!(*flags & 0x0008)){
      *crc = UNPACK_UB4(file_header, LOC_CRC);
#ifdef DEBUG    
      printf("CRC is %x\n", *crc);
#endif
    }
}

char *new_filename(pb_file *pbf, ub4 len)
{
char *filename;

	if(!(filename = (char *) malloc(len + 1))) {
		fprintf(stderr, "Malloc failed of filename\n");
		fprintf(stderr, "Error: %s\n", strerror(errno));
	}
    pb_read(pbf, filename, len);
    filename[len] = '\0';

#ifdef DEBUG    
    printf("filename is %s\n", filename);
#endif

	return filename;
}

char *read_string(pb_file *pbf, int size)
{
char *page;
	
	if(page = (char *) malloc(size + 1)) {
		pb_read(pbf, page, size);
		page[size] = '\0';
	}
	else {
		fprintf(stderr, "Malloc of page buffer failed.\n");
		fprintf(stderr, "Error: %s\n", strerror(errno));
		exit(16);
	}

	return page;
}

char *extract_line(char *stream, regoff_t begin, regoff_t end, int *b)
{
int e, length;
char *retstr;

	for(*b = begin; *b >= 0 && !iscntrl(stream[*b]); (*b)--);
	(*b)++;
	for(e = end; !iscntrl(stream[e]); e++);
	length = e - *b;
	if(retstr = (char *) malloc(length + 1)) {
		strncpy(retstr, &(stream[*b]), length);
		retstr[length] = '\0';
	}
	else {
		fprintf(stderr, "Malloc failed of output string.\n");
		fprintf(stderr, "Error: %s\n", strerror(errno));
		exit(1);
	}

	return retstr;
}

void prnt_mtchs(char *filename, char *stream, regmatch_t *pmatch, int num)
{
int i, begin, o_begin;
char *str;

	o_begin = -1;
	for(i = 0; i < num; i++) {
		str = extract_line(stream, pmatch[i].rm_so, pmatch[i].rm_eo, &begin);
		if(begin > o_begin)
			printf("%s:%s\n", filename, str);
		o_begin = begin;
		free(str);
	}
}

int cont_grep(regex_t *exp, int fd, char *jarfile, ub4 *signature, ub1 *scratch, pb_file *pbf)
{
int rdamt, retflag = TRUE, regflag, i;
ub4 csize, usize, crc;
ub2 fnlen, eflen, flags, method;
ub1 file_header[30];
char *filename, *str_stream;
regmatch_t match, *match_array, *tmp;

	if((rdamt = pb_read(pbf, (file_header + 4), 26)) != 26) {
		perror("read");
		retflag = FALSE;
   	}
	else {
		decd_siz(&csize, &usize, &fnlen, &eflen, &flags, &method, &crc, file_header);
		filename = new_filename(pbf, fnlen);
		if(filename[fnlen - 1] == '/') {
			lseek(fd, eflen, SEEK_CUR);
		}
		else {
			lseek(fd, eflen, SEEK_CUR);
			str_stream = (method == 8 || (flags & 0x0008)) ? 
				(char *) inflate_string(pbf, csize, usize) : read_string(pbf, csize);
			match_array = NULL;
			for(i = 0, regflag = regexec(exp, str_stream, 1, &match, 0); !regflag; 
				regflag =  regexec(exp, &(str_stream[match.rm_eo]), 1, &match, 0), i++)
			{
				if(tmp = (regmatch_t *) realloc(match_array, sizeof(regmatch_t) * (i + 1))) {
					match_array = tmp;
					if(i) {
						match.rm_so += match_array[i - 1].rm_eo;
						match.rm_eo += match_array[i - 1].rm_eo;
					}
					match_array[i] = match;
				}
				else {
					fprintf(stderr, "Realloc of match_array failed.\n");
					fprintf(stderr, "Error: %s\n", strerror(errno));
					exit(1);
				}
			} 
			if(i) prnt_mtchs(filename, str_stream, match_array, i);
			else printf("%s:no match\n", filename);
			free(str_stream);
			if(match_array) free(match_array);
		}
		free(filename);
		retflag = TRUE;
	}

	return retflag;
}

void jargrep(regex_t *exp, char *jarfile)
{
int fd, floop = TRUE, rdamt;
pb_file pbf;
ub4 signature;
ub1 scratch[16];

	if((fd = open(jarfile, O_RDONLY)) == -1) {
		fprintf(stderr, "Error reading file '%s': %s\n", jarfile, strerror(errno));
	}
	else {
		pb_init(&pbf, fd);	
		
		do {
			if((rdamt = pb_read(&pbf, scratch, 4)) != 4) {
				perror("read");
				floop = FALSE;
			}
			else {
				switch (check_sig(&signature, scratch, &pbf)) {
				case 0:
					floop = cont_grep(exp, fd, jarfile, &signature, scratch, &pbf);
					break;
				case 1:
					floop = FALSE;
					break;
				case 2:
					/* fall through continue */
				}
			}
		} while(floop);
	}
}

int main(int argc, char **argv)
{
int c, retval = 0, fileindex;
regex_t *regexp;
char *regexpstr = NULL, *jarfile;

	while((c = getopt(argc, argv, "bce:inqsvw")) != -1) {
		switch(c) {
			case 'b':
				options |= JG_PRINT_BYTEOFFSET;
				break;
			case 'c':
				options |= JG_PRINT_COUNT;
				break;
			case 'e':
				if(!(regexpstr = (char *) malloc(strlen(optarg) + 1))) {
					fprintf(stderr, "Malloc failure.\n");
					fprintf(stderr, "Error: %s\n", strerror(errno));
					exit(6);
				}
				strcpy(regexpstr, optarg);
				break;
			case 'i':
				options |= JG_IGNORE_CASE;
				break;
			case 'n':
				options |= JG_PRINT_LINE_NUMBER;
				break;
			case 'q':
				options |= JG_SUPRESS_OUTPUT;
				break;
			case 's':
				options |= JG_SUPRESS_ERROR;
				break;
			case 'v':
				options |= JG_INVERT;
				break;
			case 'w':
				options |= JG_WORD_EXPRESSIONS;
				break;
			default:
				fprintf(stderr, "Unknown option -%c\n", c);
				fprintf(stderr, Usage);
				exit(1);
		}
	}
	if(!regexpstr){
		if(((argc - optind) >= 2)) {
			regexpstr = argv[optind];
			fileindex = optind + 1;
		}
		else {
			fprintf(stderr, "Invalid arguments.\n");
			fprintf(stderr, Usage);
			exit(7);
		}
	}
	else if((argc - optind) == 1) {
		fileindex = optind;
	}
	else {
		fprintf(stderr, "Invalid arguments.\n");
		fprintf(stderr, Usage);
		exit(8);
	}

	if(opt_valid(options)) {
		regexp = create_regexp(regexpstr, options);
		init_inflation();
		for(; fileindex < argc; fileindex++)
			jargrep(regexp, argv[fileindex]);
		regfree(regexp);
	}
	else {
		retval = 2;
		fprintf(stderr, "Error: Invalid combination of options.\n");
	}

	return retval;
}
