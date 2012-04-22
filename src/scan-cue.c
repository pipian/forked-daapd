/*
 * Cuesheet parsing
 *
 * Copyright (C) 2010 Ian Jacobi (pipian@pipian.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.h"
#include "db.h"
#include "misc.h"

#ifdef FLAC
#include <FLAC/metadata.h>
#endif

typedef struct media_file_info MP3FILE;

/* Mapping between the metadata name(s) and the offset
 * of the equivalent metadata field in struct media_file_info */
struct metadata_map {
  char *key;
  int as_int;
  size_t offset;
  int (*handler_function)(struct media_file_info *, char *);
};

extern const struct metadata_map md_map_generic[];
extern const struct metadata_map md_map_vorbis[];

char *read_token(char **str) {
    char *token;
    
    if (str == NULL || *str == NULL)
	return NULL;
    
    token = *str;
    
    while (*token == ' ' || *token == '\t' || *token == '\r' || *token == '\n')
	token++;
    
    if (*token == '\0')
	return NULL;
    
    *str = token + 1;
    
    if (*token == '"') {
	while (**str != '"' && **str != '\0') {
	    if (**str == '\\')
		(*str)++;
	    
	    if (**str != '\0')
		(*str)++;
	}
	if (**str == '"')
	    (*str)++;
    } else {
	while (**str != ' ' && **str != '\t' && **str != '\r' && **str != '\n' && **str != '\0')
	    (*str)++;
    }
    
    if (**str != '\0') {
	**str = '\0';
	(*str)++;
	
	while (**str == ' ' || **str == '\t' || **str == '\r' || **str == '\n')
	    (*str)++;
    }
    
    return token;
}

char *unquote(char *quoted) {
    char *unquoted = quoted, *unquotedPtr = unquoted;
    
    if (quoted == NULL)
	return NULL;
    
    if (*quoted == '"')
	quoted++;
    
    while (*quoted != '"' && *quoted != '\0') {
	if (*quoted == '\\') {
	    quoted++;
	}
	
	if (*quoted != '\0') {
	    *unquotedPtr = *quoted;
	    unquotedPtr++;
	    quoted++;
	}
    }
    
    *unquotedPtr = '\0';
    
    return unquoted;
}

void msf_to_sample_offset(char *msf, MP3FILE *pmp3, MP3FILE *cuesheet_track) {
    char *start = msf, *ptr;
    int minute, second, frame;
    
    ptr = strchr(start, ':');
    if (ptr == NULL) {
	return;
    }
    
    *ptr = '\0';
    minute = atoi(start);
    
    start = ptr + 1;
    ptr = strchr(start, ':');
    if (ptr == NULL) {
	return;
    }
    
    *ptr = '\0';
    second = atoi(start);
    
    start = ptr + 1;
    frame = atoi(start);
    
    frame = ((minute * 60) + second) * 75 + frame;
    
    if (pmp3->samplerate > 0) {
	cuesheet_track->sample_offset = (int64_t)pmp3->samplerate * frame / 75;
    } else {
	cuesheet_track->sample_offset = frame;
    }
}

/**
 * parse a cuesheet into the given MP3FILE.
 *
 * @param cuesheet cuesheet to parse
 * @param tracks hint of number of tracks in the cuesheet
 * @param cuesheet_tracks array of MP3FILEs corresponding to the cuesheet tracks
 * @param pmp3 MP3FILE struct to be filled with metainfo
 * @return The number of tracks read from the cuesheet
 */
int parse_cuesheet(char *cuesheet, int tracks, MP3FILE *cuesheet_tracks, MP3FILE *pmp3) {
    int track, index, i;
    char *line = cuesheet, *end, *directive;
    char *val;
    char **strval;
    uint32_t *intval;
    
    track = 0;
    index = 0;
    while ((end = strchr(line, '\n')) != NULL) {
	/* Read each line of the cuesheet. */
	*end = '\0';
	
	/* Read the first token of the file. */
	directive = read_token(&line);
	
	/* Try different commands. */
	if (directive == NULL) {
	    /* Empty line. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "catalog") == 0) {
	    /* We don't care about Media Catalog Numbers. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "cdtextfile") == 0) {
	    /* We don't care about CDTEXTFILE. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "file") == 0) {
	    /* Don't really care about FILE, for now. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "flags") == 0) {
	    /* Don't really care about FLAGS (other than DATA), for now. */
	    while ((val = read_token(&line)) != NULL) {
		if (strcasecmp(val, "data") == 0 && track) {
		    cuesheet_tracks[track - 1].disabled = 1;
		    break;
		}
	    }
	} else if (strcasecmp(directive, "index") == 0) {
	    /* Ah, an index! */
	    
	    /* Is this the first index? */
	    if (track && !index) {
		/* Like iTunes and normal CDDA, we skip INDEX 00. */
		/* But what about hidden early tracks? */
		val = read_token(&line);
		
		if (val != NULL) {
		    val = read_token(&line);
		    
		    if (atoi(val) == 1)
		    {
			index = 1;
			val = read_token(&line);

			/* And set the previous sample_count. */
			if (val != NULL) {
			    cuesheet_tracks[track - 1].subtrack = 1;
			    msf_to_sample_offset(val, pmp3, &(cuesheet_tracks[track - 1]));

			    /* And set the previous sample_count. */
			    if (track > 1) {
				cuesheet_tracks[track - 2].sample_count = cuesheet_tracks[track - 1].sample_offset - cuesheet_tracks[track - 2].sample_offset;
				if (pmp3->samplerate > 0) {
				    cuesheet_tracks[track - 2].song_length = cuesheet_tracks[track - 2].sample_count * 1000 / pmp3->samplerate;
				}
			    }
			}
		    }
		}
	    }
	} else if (strcasecmp(directive, "isrc") == 0) {
	    /* Don't really care about ISRC. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "performer") == 0) {
	    /* Ah, a performer! */
	    val = unquote(read_token(&line));
	    
	    if (val != NULL) {
		if (track) {
		    /* Add it to the appropriate track. */
		    strval = (char **)&(cuesheet_tracks[track - 1].artist);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		} else {
		    /* Add it to the full album. */
		    strval = (char **)&(pmp3->artist);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		    
		    strval = (char **)&(pmp3->album_artist);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		}
	    }
	} else if (strcasecmp(directive, "postgap") == 0) {
	    /* Don't really care about POSTGAP. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "pregap") == 0) {
	    /* Don't really care about PREGAP. */
	    line = end + 1;
	    continue;
	} else if (strcasecmp(directive, "rem") == 0) {
	    /* Ah, a REM! */
	    val = read_token(&line);
	    
	    if (val != NULL) {
		char *c;
		
		/*
		 * If the val is all-caps, we're going to assume it's a
		 * supplementary directive.
		 */
		for (c = val; *c != '\0'; c++) {
		    if (*c < 'A' || *c > 'Z') {
			break;
		    }
		}
		
		if (*c == '\0') {
		    if (strcasecmp(val, "comment") == 0) {
			/* Special case COMMENT */
			if (track) {
			    /* Add it to the appropriate track. */
			    strval = (char **)&(cuesheet_tracks[track - 1].comment);
			    
			    if (*strval == NULL)
				*strval = strdup(line);
			} else {
			    /* Add it to the full album. */
			    strval = (char **)&(pmp3->comment);
			    
			    if (*strval == NULL)
				*strval = strdup(line);
			}
		    } else if (strcasecmp(val, "date") == 0) {
			/* Special case DATE */
			if (track) {
			    /* Add it to the appropriate track. */
			    intval = (uint32_t *)&(cuesheet_tracks[track - 1].year);
			    
			    if (*intval == 0)
			    {
				safe_atou32(line, intval);
			    }
			} else {
			    /* Add it to the full album. */
			    intval = (uint32_t *)&(pmp3->year);
			    
			    if (*intval == 0)
			    {
				safe_atou32(line, intval);
			    }
			}
		    } else if (strcasecmp(val, "discid") == 0) {
			/* Special case DISCID (ignored) */
			line = end + 1;
			continue;
		    } else if (strcasecmp(val, "genre") == 0) {
			/* Special case GENRE */
			if (track) {
			    /* Add it to the appropriate track. */
			    strval = (char **)&(cuesheet_tracks[track - 1].genre);
			    
			    if (*strval == NULL)
				*strval = strdup(line);
			} else {
			    /* Add it to the full album. */
			    strval = (char **)&(pmp3->genre);
			    
			    if (*strval == NULL)
				*strval = strdup(line);
			}
		    } else {
			/*
			 * Try to parse as a Vorbis Comment (Which may
			 * be missing '=').  We guess the key by
			 * capitalization (and also flatten whitespace).
			 */
			char *key = strdup(val);
			int len = strlen(key);
			
			while ((val = read_token(&line)) != NULL) {
			    for (c = val; *c != '\0'; c++) {
				if (*c < 'A' || *c > 'Z') {
				    break;
				}
			    }
			    
			    if (*c == '\0') {
				len += strlen(val) + 1;
				key = realloc(key, len + 1);
				strcat(key, " ");
				strcat(key, val);
			    } else {
				break;
			    }
			}
			
			if (val != NULL) {
			    /* Eeeh... We wiped the old value... */
			    val[strlen(val)] = ' ';
			}
			
			/* Let's now search for matching keys. */
			for (i = 0; md_map_generic[i].key != NULL; i++) {
			    if (strcasecmp(md_map_generic[i].key, key) == 0) {
				/* Set the appropriate value corresponding to key. */
				if (md_map_generic[i].handler_function)
				{
				    md_map_generic[i].handler_function(&(cuesheet_tracks[track - 1]), val);
				    break;
				}
				
				if (!md_map_generic[i].as_int)
				{
				    strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[i].offset);
				    
				    if (*strval == NULL)
					*strval = strdup(val);
				}
				else
				{
				    intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[i].offset);
				    
				    if (*intval == 0)
				    {
					safe_atou32(val, intval);
				    }
				}
				break;
			    }
			}
			for (i = 0; md_map_vorbis[i].key != NULL; i++) {
			    if (strcasecmp(md_map_vorbis[i].key, key) == 0) {
				/* Set the appropriate value corresponding to key. */
				if (md_map_vorbis[i].handler_function)
				{
				    md_map_vorbis[i].handler_function(&(cuesheet_tracks[track - 1]), val);
				    break;
				}
				
				if (!md_map_vorbis[i].as_int)
				{
				    strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[i].offset);
				    
				    if (*strval == NULL)
					*strval = strdup(val);
				}
				else
				{
				    intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[i].offset);
				    
				    if (*intval == 0)
				    {
					safe_atou32(val, intval);
				    }
				}
				break;
			    }
			}
		    }
		} else if (c == val && *c == '"') {
		    /*
		     * May be doing a quoted Vorbis Comment:
		     * "KEY"=VALUE or "KEY" VALUE.
		     */
		    for (c = val; *c != '\0'; c++) {
			if (*c == '\\')
			    c++;
			else if (*c == '"')
			    break;
		    }
		    
		    if (*c == '"') {
			c++;
			if (*c == ' ' || *c == '=') {
			    *c = '\0';
			    line = c + 1;
			    
			    val = unquote(val);
			    
			    /* Let's now search for matching keys. */
			    for (i = 0; md_map_generic[i].key != NULL; i++) {
				if (strcasecmp(md_map_generic[i].key, val) == 0) {
				    /* Set the appropriate value corresponding to val. */
				    if (md_map_generic[i].handler_function)
				    {
					md_map_generic[i].handler_function(&(cuesheet_tracks[track - 1]), line);
					break;
				    }
				    
				    if (!md_map_generic[i].as_int)
				    {
					strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[i].offset);
					
					if (*strval == NULL)
					    *strval = strdup(line);
				    }
				    else
				    {
					intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[i].offset);
					
					if (*intval == 0)
					{
					    safe_atou32(line, intval);
					}
				    }
				    break;
				}
			    }
			    for (i = 0; md_map_vorbis[i].key != NULL; i++) {
				if (strcasecmp(md_map_vorbis[i].key, val) == 0) {
				    /* Set the appropriate value corresponding to val. */
				    if (md_map_vorbis[i].handler_function)
				    {
					md_map_vorbis[i].handler_function(&(cuesheet_tracks[track - 1]), line);
					break;
				    }
				    
				    if (!md_map_vorbis[i].as_int)
				    {
					strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[i].offset);
					
					if (*strval == NULL)
					    *strval = strdup(line);
				    }
				    else
				    {
					intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[i].offset);
					
					if (*intval == 0)
					{
					    safe_atou32(val, intval);
					}
				    }
				    break;
				}
			    }
			}
		    }
		}
	    }
	} else if (strcasecmp(directive, "songwriter") == 0) {
	    /* Ah, a songwriter! */
	    val = unquote(read_token(&line));
	    
	    if (val != NULL) {
		if (track) {
		    /* Add it to the appropriate track. */
		    strval = (char **)&(cuesheet_tracks[track - 1].composer);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		} else {
		    /* Add it to the full album. */
		    strval = (char **)&(pmp3->composer);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		}
	    }
	} else if (strcasecmp(directive, "title") == 0) {
	    /* Ah, a title! */
	    val = unquote(read_token(&line));
	    
	    if (val != NULL) {
		if (track) {
		    /* Add it to the appropriate track. */
		    strval = (char **)&(cuesheet_tracks[track - 1].title);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		} else {
		    /* Add it to the full album. */
		    strval = (char **)&(pmp3->album);
		    
		    if (*strval == NULL)
			*strval = strdup(val);
		}
	    }
	} else if (strcasecmp(directive, "track") == 0) {
	    /* Ah, a track! */
	    val = read_token(&line);
	    
	    if (val != NULL) {
		/* The val had best be all digits. */
		track = atoi(val);
		index = 0;
		
		if (track > tracks) {
		    cuesheet_tracks = realloc(cuesheet_tracks, sizeof(MP3FILE) * track);
		    for (; tracks < track; tracks++) {
			memset(&(cuesheet_tracks[tracks]), 0, sizeof(MP3FILE));
		    }
		}

		if (cuesheet_tracks[track - 1].track == 0)
		    cuesheet_tracks[track - 1].track = track;
	    }
	} else {
	    /* Don't know this! */
	    DPRINTF(E_WARN,L_SCAN,"Don't recognize cuesheet directive %s\n", directive);
	}
	
	line = end + 1;
    }
    
    pmp3->cuesheet_tracks = cuesheet_tracks;
    pmp3->total_tracks = track;
    
    /* And fix the final sample_count. */
    if (pmp3->sample_count > 0) {
	cuesheet_tracks[track - 1].sample_count = pmp3->sample_count - cuesheet_tracks[track - 1].sample_offset;
	if (pmp3->samplerate > 0) {
	    cuesheet_tracks[track - 1].song_length = cuesheet_tracks[track - 1].sample_count * 1000 / pmp3->samplerate;
	}
    }

    return track;
}

/**
 * get metainfo from a cuesheet
 *
 * @param filename full path to file being scanned
 * @param pmp3 MP3FILE struct to be filled with with metainfo
 * @return The number of tracks read from the cuesheet
 */
int scan_get_cuesheet(char *filename, MP3FILE *pmp3) {
    struct stat sb;
    MP3FILE *cuesheet_tracks = NULL;
    FILE *cuesheet_file = NULL;
    char *cuesheet = NULL, *cue_filename, *ext;
    char **strval;
    uint32_t *intval;
    int tracks = 0, len;
    
#ifdef FLAC
    /* Try scanning embedded cuesheet if the file is a FLAC file. */
    if (strcmp(pmp3->codectype, "flac") == 0) {
	FLAC__Metadata_Chain *chain;
	FLAC__Metadata_Iterator *iterator;
	FLAC__StreamMetadata *block;
	int i, j;
	int found = 0;
	
	chain = FLAC__metadata_chain_new();
	if (! chain) {
	    DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata chain\n");
	    goto skip_flac;
	}
	if (! FLAC__metadata_chain_read(chain, filename)) {
	    DPRINTF(E_WARN,L_SCAN,"Cannot read FLAC metadata from %s\n", filename);
	    FLAC__metadata_chain_delete(chain);
	    goto skip_flac;
	}
	
	iterator = FLAC__metadata_iterator_new();
	if (! iterator) {
	    DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata iterator\n");
	    FLAC__metadata_chain_delete(chain);
	    goto skip_flac;
	}
	
	FLAC__metadata_iterator_init(iterator, chain);
	do {
	    block = FLAC__metadata_iterator_get_block(iterator);
	    
	    if (block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
		/* Scan track-specific data. */
		for (i = 0; i < (int)block->data.vorbis_comment.num_comments; i++) {
		    char *entry = strdup((char*)block->data.vorbis_comment.comments[i].entry);
		    char *name, *trackStr, *val;
		    int track = 0;
		    
		    /*
		     * CUE_TRACK##_FIELD format from foobar2000:
		     * http://www.hydrogenaudio.org/forums/index.php?showtopic=47532)
		     */
		    if (strncasecmp("cue_track", entry, 9) == 0) {
			trackStr = entry + 9;
			/* Check that the next characters are digits. */
			for (name = trackStr; *name >= '0' && *name <= '9'; name++);
			/* Followed by a '_'. */
			if (*name != '_' || name == trackStr) {
			    free(entry);
			    continue;
			}
			*name = '\0';
			track = atoi(trackStr);
			name++;
			
			/* Extract the actual tagname. */
			for (val = name; *val != '=' && *val != '\0'; val++);
			if (*val != '=' || val == name) {
			    free(entry);
			    continue;
			}
			*val = '\0';
			val++;
			
			for (j = 0; md_map_generic[j].key != NULL; j++) {
			    if (strcasecmp(md_map_generic[j].key, name) == 0) {
				/* Realloc cuesheet_tracks if needed. */
				if (track > tracks) {
				    cuesheet_tracks = realloc(cuesheet_tracks, sizeof(MP3FILE) * track);
				    for (; tracks < track; tracks++) {
					memset(&(cuesheet_tracks[tracks]), 0, sizeof(MP3FILE));
				    }
				}
				
				/* Set the appropriate value corresponding to name. */
				if (md_map_generic[j].handler_function)
				{
				    md_map_generic[j].handler_function(&(cuesheet_tracks[track - 1]), val);
				    break;
				}
				
				if (!md_map_generic[j].as_int)
				{
				    strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[j].offset);
				    
				    if (*strval == NULL)
					*strval = strdup(val);
				}
				else
				{
				    intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_generic[j].offset);
				    
				    if (*intval == 0)
				    {
					safe_atou32(val, intval);
				    }
				}
				break;
			    }
			}
			for (j = 0; md_map_vorbis[j].key != NULL; j++) {
			    if (strcasecmp(md_map_vorbis[j].key, name) == 0) {
				/* Realloc cuesheet_tracks if needed. */
				if (track > tracks) {
				    cuesheet_tracks = realloc(cuesheet_tracks, sizeof(MP3FILE) * track);
				    for (; tracks < track; tracks++) {
					memset(&(cuesheet_tracks[tracks]), 0, sizeof(MP3FILE));
				    }
				}
				
				/* Set the appropriate value corresponding to name. */
				if (md_map_vorbis[j].handler_function)
				{
				    md_map_vorbis[j].handler_function(&(cuesheet_tracks[track - 1]), val);
				    break;
				}
				
				if (!md_map_vorbis[j].as_int)
				{
				    strval = (char **) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[j].offset);
				    
				    if (*strval == NULL)
					*strval = strdup(val);
				}
				else
				{
				    intval = (uint32_t *) ((char *)&(cuesheet_tracks[track - 1]) + md_map_vorbis[j].offset);
				    
				    if (*intval == 0)
				    {
					safe_atou32(val, intval);
				    }
				}
				break;
			    }
			}
		    } else if (strncasecmp("cuesheet", entry, 8) == 0) {
			/* Read from a CUESHEET tag. */
			if (*(entry + 8) == '=') {
			    cuesheet = strdup(entry + 9);
			}
		    }
		    
		    free(entry);
		}
		
		found |= 1;
	    } else if (block->type == FLAC__METADATA_TYPE_CUESHEET) {
		/*
		 * Actually allocate the true number of tracks and set
		 * the offsets.
		 */
		uint64_t prev_offset = 0;
		
		/* Realloc cuesheet_tracks if needed. */
		if (block->data.cue_sheet.num_tracks - 1> tracks) {
		    cuesheet_tracks = realloc(cuesheet_tracks, sizeof(MP3FILE) * block->data.cue_sheet.num_tracks - 1);
		    for (; tracks < block->data.cue_sheet.num_tracks - 1; tracks++) {
			memset(&(cuesheet_tracks[tracks]), 0, sizeof(MP3FILE));
		    }
		}
		
		/* We look for INDEX 01, like the CDDA standard. */
		/* What about hidden tracks? */
		/* This assumes tracks are encoded in order so uh... */
		for (i = 0; i < block->data.cue_sheet.num_tracks; i++) {
		    FLAC__StreamMetadata_CueSheet_Track *cue_track =
			&(block->data.cue_sheet.tracks[i]);
		    
		    if (i != block->data.cue_sheet.num_tracks - 1) {
			if (cuesheet_tracks[i].track == 0)
			    cuesheet_tracks[i].track = i + 1;
			cuesheet_tracks[i].subtrack = 1;
			cuesheet_tracks[i].sample_offset = cue_track->offset;
			for (j = 0; j < cue_track->num_indices; j++)
			{
			    if (cue_track->indices[j].number == 1)
			    {
				cuesheet_tracks[i].sample_offset += cue_track->indices[j].offset;
				break;
			    }
			}
			if (i != 0) {
			    cuesheet_tracks[i - 1].sample_count = cuesheet_tracks[i].sample_offset - prev_offset;
			    if (pmp3->samplerate > 0) {
				cuesheet_tracks[i - 1].song_length = cuesheet_tracks[i - 1].sample_count * 1000 / pmp3->samplerate;
			    }
			}
			if (cue_track->type == 1) {
			    /* Need to mark this track as a track to ignore. */
			    cuesheet_tracks[i].disabled = 1;
			} else {
			    prev_offset = cuesheet_tracks[i].sample_offset;
			}
		    } else {
			/* Final track. */
			cuesheet_tracks[i - 1].sample_count = cue_track->offset - prev_offset;
		    }
		}
		
		found |= 2;
	    }
	    
	    if (found == 3) {
		break;
	    }
	} while (FLAC__metadata_iterator_next(iterator));
	
	FLAC__metadata_iterator_delete(iterator);
	FLAC__metadata_chain_delete(chain);
    }
skip_flac:
#endif
    
    /* Next, try reading from filename.cue. */
    if (cuesheet == NULL) {
	len = strlen(filename);
	cue_filename = calloc(len + 5, 1);
	strncpy(cue_filename, filename, len + 1);
	ext = strrchr(cue_filename, '.');
	if (ext != NULL) {
	    strncpy(ext, ".cue", 5);
	} else {
	    strncat(cue_filename, ".cue", 4);
	}
	
	if (lstat(cue_filename, &sb) == 0) {
	    /* Read file. */
	    cuesheet = calloc(sb.st_size + 1, 1);
	    cuesheet_file = fopen(cue_filename, "rb");
	    fread(cuesheet, 1, sb.st_size, cuesheet_file);
	    fclose(cuesheet_file);
	}
    }
    
    if (cuesheet == NULL) {
	strncpy(cue_filename, filename, len + 1);
	strncat(cue_filename, ".cue", 4);
	
	/* Finally, try reading from filename.[extension].cue. */
	if (lstat(cue_filename, &sb) == 0) {
	    /* Read file. */
	    cuesheet = calloc(sb.st_size + 1, 1);
	    cuesheet_file = fopen(cue_filename, "rb");
	    fread(cuesheet, 1, sb.st_size, cuesheet_file);
	    fclose(cuesheet_file);
	}
    }
    
    if (cuesheet == NULL) {
	/* Guess we didn't find anything. */
	return tracks;
    }
    
    tracks = parse_cuesheet(cuesheet, tracks, cuesheet_tracks, pmp3);
    free(cuesheet);
    
    return tracks;
}
