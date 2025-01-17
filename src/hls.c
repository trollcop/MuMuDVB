/** @file
 * @brief HLS output support
 */

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#include <linux/limits.h>
#else
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
/* MAX_PATH on Win32 */
#define PATH_MAX (260)
#include <io.h>
#define ftruncate _chsize
#include "win32.h"
#endif
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include "mumudvb.h"
#include "errors.h"
#include "log.h"
#include "unicast_http.h"
#include "hls.h"
#include "ts.h"

static char *log_module="HLS : ";

pthread_mutex_t hls_periodic_lock;
hls_open_fds_t hls_fds[MAX_CHANNELS];
extern int dont_send_scrambled;

#ifndef _WIN32
int mkpath(char* file_path, mode_t mode) {
    if (!file_path || !*file_path) return -1;

    for (char* p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
	log_message( log_module, MSG_FLOOD,"Create dir \"%s\"\n", file_path);
        if (mkdir(file_path, mode)) {
            if (errno != EEXIST) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }
    log_message( log_module, MSG_FLOOD,"Create dir \"%s\"\n", file_path);
    if (mkdir(file_path, mode)) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}
#endif

int hls_sid_entry_compare(const void *pa, const void *pb) {
	const int *a = pa;
	const int *b = pb;
	return a[0] - b[0];
}

int hls_find_iframe(unsigned char *buf, unsigned int size) {
	ts_header_t *header;
	af_header_t *af;
	unsigned char *h264_packet;

	unsigned int pos = 0;

	// TODO: detection code needs futher testing on various live traffic
	for (; pos < size; pos += TS_PACKET_SIZE) {
		header=(ts_header_t *)(buf + pos);

		// MPEG-TS encoder signalling
		// match 2 (af + no payload) or 3 (af + payload) + RAI = 1
		af=(af_header_t *)(buf + pos + TS_HEADER_LEN);
    		if (header->adaptation_field_control > 1 && af->random_access_indicator)
    		{
		    log_message( log_module, MSG_FLOOD,"MPEG-TS RAI found at %d\n", pos);
        	    goto exit;
    		}

		// h264 stream
		// match 1 (payload only)
    		if (header->adaptation_field_control == 1) {
		    h264_packet = buf + pos + TS_HEADER_LEN;
    		    int fragment_type = h264_packet[0] & 0x1F;
    		    int nal_type = h264_packet[1] & 0x1F;
    		    int start_bit = h264_packet[1] & 0x80;

    		    if (((fragment_type == 28 || fragment_type == 29) && nal_type == 5 && start_bit == 128) || fragment_type == 5)
    		    {
    		        log_message( log_module, MSG_FLOOD,"H264 I-Frame found at %d\n", pos);
        		goto exit;
    		    }
    		}
        }
        return -1; // not found

        exit:
        return pos;
}

int hls_entry_find(int service_id, hls_open_fds_t *hls_fds)
{
	int entry;
	// search for used entry
	for (entry = 0; entry < MAX_CHANNELS; entry++){
	    if (hls_fds[entry].initialized && hls_fds[entry].service_id == service_id) goto entry_found;
	}
	// if not, search for first uninitialized entry
	for (entry = 0; entry < MAX_CHANNELS; entry++){
	    if (!hls_fds[entry].initialized) {
		    log_message( log_module, MSG_FLOOD,"Found unused entry %d for service id %d.\n", entry, service_id);
		    goto entry_found;
	    }
	}
	// cannot find free entry (overload?)
	log_message( log_module, MSG_ERROR,"Unable to find free entry, output may be trashed.\n");
	exit(1);

	entry_found:
        return entry;
};

int hls_entry_initialize(mumudvb_channel_t *actual_channel, hls_open_fds_t *hls_entry, unicast_parameters_t *unicast_vars, unsigned int access_time)
{
	memset(hls_entry, 0, sizeof(*hls_entry));
	hls_entry->filenames_num = unicast_vars->hls_rotate_count + 2; // names count for all chunks plus stream and delete names
	hls_entry->filenames = calloc(1, hls_entry->filenames_num * sizeof(hls_file_t)); // allocate and clear

	strncpy(hls_entry->path, unicast_vars->hls_storage_dir, LEN_MAX);
	sprintf(hls_entry->name_playlist, "%d.m3u8", actual_channel->service_id);

	sprintf(hls_entry->filenames[0].name, "%d_%u.ts", actual_channel->service_id, access_time);	// construct uniq filename here

	if (strlen(actual_channel->user_name)) {
	    strncpy(hls_entry->name, actual_channel->user_name, LEN_MAX);
	} else {
	    strncpy(hls_entry->name, actual_channel->name, LEN_MAX);
	}

	char path_stream[PATH_MAX];
	snprintf(path_stream, sizeof(path_stream), "%s/%s", hls_entry->path, hls_entry->filenames[0].name);

	hls_entry->stream = fopen(path_stream, "wb");
	if (hls_entry->stream == 0) {
	    perror("Cannot open output file for write");
	    return -1;
	}

	hls_entry->service_id = actual_channel->service_id;
	hls_entry->access_time = access_time;
	hls_entry->rotate_time = access_time;
	hls_entry->initialized = 1;

	return 0;
}

int hls_entry_rotate(hls_open_fds_t *hls_entry, unicast_parameters_t *unicast_vars, unsigned int access_time)
{
	if (hls_entry->initialized != 1) {
		log_message( log_module, MSG_ERROR,"BUG: rotate with uninitialized entry!\n");
		return -1;
	}

	if (hls_entry->stream) fclose(hls_entry->stream);

	// shift names in array
	memmove(&hls_entry->filenames[1], &hls_entry->filenames[0], sizeof(hls_entry->filenames[0]) * (hls_entry->filenames_num - 1));
	snprintf(hls_entry->filenames[0].name, sizeof(hls_entry->filenames[0].name), "%d_%u.ts", hls_entry->service_id, access_time);	// construct uniq filename here

	if (hls_entry->sequence >= hls_entry->filenames_num - 3) { // generate playlist only if we have all chunks written
	    unsigned int playlist_size;
	    char path_playlist[PATH_MAX];
	    snprintf(path_playlist, sizeof(path_playlist), "%s/%s", hls_entry->path, hls_entry->name_playlist);

	    // allocate memory for all filenames and playlist text structure
	    char *outbuf = malloc(1024 + hls_entry->filenames_num * (sizeof(hls_entry->filenames[0]) + 64));

	    playlist_size = sprintf(
		outbuf,
		"#EXTM3U\n"
		"#EXT-X-TARGETDURATION:%d\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-MEDIA-SEQUENCE:%u\n",
		unicast_vars->hls_rotate_time, hls_entry->sequence
	    );

	    // cycle via all file entries excluding delete and stream
	    unsigned int x;
	    for (x = hls_entry->filenames_num - 2; x > 0; x--) {
		if(*hls_entry->filenames[x].name) {
		    playlist_size += sprintf(
			outbuf + playlist_size,
			"#EXTINF:%d.0,\n"
			"%s\n",
			unicast_vars->hls_rotate_time, hls_entry->filenames[x].name
		    );
		}
	    }

	    FILE *playlist = fopen(path_playlist, "wb");
	    if (playlist == 0) {
		perror("Cannot open output file for write");
		free(outbuf);
		return -1;

	    }

	    fwrite(outbuf, playlist_size, 1, playlist);

	    ftruncate(fileno(playlist), playlist_size);
	    fclose(playlist);
	    free(outbuf);
	}

	log_message( log_module, MSG_FLOOD,"Rotate event for service_id %d, writing stream to \"%s\"\n", hls_entry->service_id, hls_entry->filenames[0].name);

	char path_stream[PATH_MAX];
	snprintf(path_stream, sizeof(path_stream), "%s/%s", hls_entry->path, hls_entry->filenames[0].name);
	hls_entry->stream = fopen(path_stream, "wb");

	if (hls_entry->stream == 0) {
	    perror("Cannot open output file for write");
	    return -1;
	}

	int delete_pos = hls_entry->filenames_num - 1; //delete file at last array position
	if (strlen(hls_entry->filenames[delete_pos].name)) {

		char path_delete[PATH_MAX];
		snprintf(path_delete, sizeof(path_delete), "%s/%s", hls_entry->path, hls_entry->filenames[delete_pos].name);

		log_message( log_module, MSG_FLOOD,"Removing file \"%s\"\n", path_delete);
	 	remove(path_delete);
	 	*hls_entry->filenames[delete_pos].name = 0;
	}

	hls_entry->rotate_time = access_time;
	hls_entry->need_rotate = 0;
	hls_entry->sequence++;

	return 0;
}

int hls_playlist_master(hls_open_fds_t *hls_fds, unicast_parameters_t *unicast_vars, unsigned int *master_playlist_checksum)
{
	int entry, active_entry;
	int num_active = 0;
	int sids[MAX_CHANNELS][2]; // array for playlist sorting
	unsigned int master_playlist_calc_checksum;

	// generate new playlist crc and active sids database
	master_playlist_calc_checksum = 0;
	for (entry = 0; entry < MAX_CHANNELS; entry++) {
	    if (hls_fds[entry].initialized && hls_fds[entry].sequence > 0 && strlen(hls_fds[entry].name_playlist)
	        && (!unicast_vars->playlist_ignore_dead || hls_fds[entry].has_traffic) ) {
		master_playlist_calc_checksum += entry + hls_fds[entry].service_id; // TODO: check for CRC collisions!
		sids[num_active][0] = hls_fds[entry].service_id;
		sids[num_active][1] = entry;
		num_active++;
	    }
	}

	if (*master_playlist_checksum != master_playlist_calc_checksum) {

	    *master_playlist_checksum = master_playlist_calc_checksum;	// store new checksum for next cycle

	    log_message( log_module, MSG_FLOOD,"Refresh master playlist, crc: %u -> %u\n", *master_playlist_checksum, master_playlist_calc_checksum);

	    int playlist_size;
	    char path_playlist[PATH_MAX];

	    snprintf(path_playlist, sizeof(path_playlist), "%s/%s", unicast_vars->hls_storage_dir, unicast_vars->hls_playlist_name);

	    char *outbuf = malloc((num_active + 1) * (LEN_MAX + 16)); // allocate memory for playlist

	    // packets arrive in chaotic order, so sort playlist by service_id here
	    qsort(sids, num_active, sizeof(sids[0]), hls_sid_entry_compare);

	    playlist_size = sprintf(outbuf, "#EXTM3U\n");

	    for (active_entry = 0; active_entry < num_active; active_entry++) {
		    playlist_size += sprintf(
			outbuf + playlist_size,
		        "#EXTINF:0,%s\n"
			"%s\n",
			hls_fds[sids[active_entry][1]].name,
			hls_fds[sids[active_entry][1]].name_playlist
		    );
	    }

	    FILE *playlist = fopen(path_playlist, "wb");
	    if (playlist == 0) {
		perror("Cannot open output file for write");
		free(outbuf);
		return -1;

	    }

	    fwrite(outbuf, playlist_size, 1, playlist);

	    ftruncate(fileno(playlist), playlist_size);
	    fclose(playlist);
	    free(outbuf);
	}

        return 0;
};

void hls_cleanup_files(hls_open_fds_t *hls_entry)
{
	log_message( log_module, MSG_FLOOD,"Closing FD and removing files for \"%s\"\n", hls_entry->name);

	if (hls_entry->stream) fclose(hls_entry->stream);

	char path_delete[PATH_MAX];

	unsigned int i;
	for(i = 0; i < hls_entry->filenames_num; i++)
	{
	    if (*hls_entry->filenames[i].name) {
		snprintf(path_delete, sizeof(path_delete), "%s/%s", hls_entry->path, hls_entry->filenames[i].name);
		log_message( log_module, MSG_FLOOD,"Removing file \"%s\"\n", path_delete);
		remove(path_delete);
	    }
	}
	if (*hls_entry->name_playlist) {
	    snprintf(path_delete, sizeof(path_delete), "%s/%s", hls_entry->path, hls_entry->name_playlist);
	    log_message( log_module, MSG_FLOOD,"Removing file \"%s\"\n", path_delete);
	    remove(path_delete);
	}
}


void hls_entry_destroy(hls_open_fds_t *hls_entry)
{
	hls_cleanup_files(hls_entry);
	if (hls_entry->filenames) free(hls_entry->filenames);
	memset(hls_entry, 0, sizeof(*hls_entry));
}

void hls_array_cleanup(hls_open_fds_t *hls_fds, unsigned int hls_rotate_time, unsigned int cur_time)
{
	int entry;
	for (entry = 0; entry < MAX_CHANNELS; entry++) {
	    if (hls_fds[entry].initialized && hls_fds[entry].access_time < cur_time - (hls_rotate_time * 3)) {
	    	log_message( log_module, MSG_DEBUG,"Entry for \"%s\" is too old, removing it\n", hls_fds[entry].name);
	    	hls_entry_destroy(&hls_fds[entry]);
	    }
	}
}

void *hls_periodic_task(void* arg)
{
	unsigned int cur_time, entry;
	unsigned int master_playlist_checksum = 0;
	unsigned int master_playlist_timestamp = 0;

        hls_thread_parameters_t *params = (hls_thread_parameters_t *) arg;
	unicast_parameters_t *unicast_vars = (unicast_parameters_t *) params->unicast_vars;

	while(!params->threadshutdown) {
	    log_message( log_module, MSG_FLOOD,"Run periodic task...\n");

	    cur_time = (unsigned int)(get_time() / 1000000ULL);

	    // try to rotate all channels
	    for (entry = 0; entry < MAX_CHANNELS; entry++){
		if (hls_fds[entry].initialized && (hls_fds[entry].rotate_time < cur_time - unicast_vars->hls_rotate_time)) {

		    pthread_mutex_lock(&hls_periodic_lock);

		    // if we waiting for I-frame, we can skip first rotate event, second will be run regardless of need_rotate state
		    if (!hls_fds[entry].need_rotate && unicast_vars->hls_rotate_iframe) {
			    hls_fds[entry].need_rotate = 1;
		    } else {
			    hls_entry_rotate(&hls_fds[entry], unicast_vars, cur_time);
		    }

		    pthread_mutex_unlock(&hls_periodic_lock);

		}
	    }
	    // try to rotate master playlist if it's too old
	    if(master_playlist_timestamp < cur_time - unicast_vars->hls_rotate_time) {
		master_playlist_timestamp = cur_time;

		pthread_mutex_lock(&hls_periodic_lock);

		hls_array_cleanup(&hls_fds[0], unicast_vars->hls_rotate_time, cur_time);
		hls_playlist_master(&hls_fds[0], unicast_vars, &master_playlist_checksum);

		pthread_mutex_unlock(&hls_periodic_lock);
	    }

	    sleep(unicast_vars->hls_rotate_time);
	}
	log_message( log_module,  MSG_DEBUG,  "Periodic task stopped.\n");
	return 0;
}

int hls_worker_main(mumudvb_channel_t *actual_channel, unicast_parameters_t *unicast_vars, unsigned int cur_time)
{
	int written = 0;
	int entry;

	int service_id = actual_channel->service_id;
	unsigned char *outbuf = actual_channel->buf;
	int output_size = actual_channel->nb_bytes;

	// do not store scrambled traffic and exclude it from playlist if specified
	if ((dont_send_scrambled && actual_channel->scrambled_channel == HIGHLY_SCRAMBLED) ||
	    (unicast_vars->playlist_ignore_scrambled_ratio && unicast_vars->playlist_ignore_scrambled_ratio <= actual_channel->ratio_scrambled))
	{
		return output_size; // return as successfully processed
	}

	pthread_mutex_lock(&hls_periodic_lock);

	entry = hls_entry_find(service_id, &hls_fds[0]);
	if (!hls_fds[entry].initialized) {
	    hls_entry_initialize(actual_channel, &hls_fds[entry], unicast_vars, cur_time);
	}

	hls_fds[entry].access_time = cur_time;
	hls_fds[entry].has_traffic = actual_channel->has_traffic;

	// in case of I-frame rotate we intercept part of write and split it by I-frame
	if (hls_fds[entry].need_rotate) {
		int iframe_pos = hls_find_iframe(outbuf, output_size);
		if (iframe_pos >= 0) {
			written += fwrite(outbuf, iframe_pos, 1, hls_fds[entry].stream);
			outbuf += iframe_pos;
			output_size -= iframe_pos;
			hls_entry_rotate(&hls_fds[entry], unicast_vars, cur_time);
			hls_fds[entry].need_rotate = 0;
		}
	}
	written += fwrite(outbuf, output_size, 1, hls_fds[entry].stream);

	pthread_mutex_unlock(&hls_periodic_lock);

	return written;
}

void hls_data_send(mumudvb_channel_t *actual_channel, unicast_parameters_t *unicast_vars, uint64_t now_time)
{
	hls_worker_main(actual_channel, unicast_vars, (unsigned int)(now_time / 1000000));
}

int hls_start(unicast_parameters_t *unicast_vars)
{
	if (mkpath(unicast_vars->hls_storage_dir, 0755)) {
	    log_message( log_module, MSG_ERROR,"Cannot create hls storage dir \"%s\".\n", unicast_vars->hls_storage_dir);
	    return -1;
	}

	pthread_mutex_init(&hls_periodic_lock, NULL);

	return 0;
}

void hls_stop(unicast_parameters_t *unicast_vars)
{
	char path_delete[PATH_MAX];

	pthread_mutex_lock(&hls_periodic_lock);
	hls_array_cleanup(&hls_fds[0], 0, ~0);		// pass maximum time value to ensure cleanup

	snprintf(path_delete, sizeof(path_delete), "%s/%s", unicast_vars->hls_storage_dir, unicast_vars->hls_playlist_name);
	log_message( log_module, MSG_FLOOD,"Removing file \"%s\"\n", path_delete);
	remove(path_delete);

	pthread_mutex_unlock(&hls_periodic_lock);
	pthread_mutex_destroy(&hls_periodic_lock);
}
