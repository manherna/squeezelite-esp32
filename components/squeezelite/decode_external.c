/* 
 *  Squeezelite for esp32
 *
 *  (c) Sebastien 2019
 *      Philippe G. 2019, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
 
#include "squeezelite.h"
#include "bt_app_sink.h"
#include "raop_sink.h"
#include <math.h>

#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#define LOCK_D   mutex_lock(decode.mutex);
#define UNLOCK_D mutex_unlock(decode.mutex);

enum { DECODE_BT = 1, DECODE_AIRPLAY };

extern struct outputstate output;
extern struct decodestate decode;
extern struct buffer *outputbuf;
// this is the only system-wide loglevel variable
extern log_level loglevel;

// not great to have these here, but they should not be in embedded.h
bool enable_bt_sink = false;
bool enable_airplay = false;

#define RAOP_OUTPUT_SIZE 	(RAOP_SAMPLE_RATE * 2 * 2 * 2 * 1.2)
#define SYNC_NB				5

static raop_event_t	raop_state;
static bool raop_expect_stop = false;
static struct {
	bool enabled, start;
	s32_t error[SYNC_NB];
	u32_t idx, len;
	u32_t start_time, playtime;
} raop_sync;

/****************************************************************************************
 * Common sink data handler
 */
static void sink_data_handler(const uint8_t *data, uint32_t len)
{
    size_t bytes, space;
		
	// would be better to lock decoder, but really, it does not matter
	if (decode.state != DECODE_STOPPED) {
		LOG_SDEBUG("Cannot use external sink while LMS is controlling player");
		return;
	} 
	
	// there will always be room at some point
	while (len) {
		LOCK_O;

		bytes = min(_buf_space(outputbuf), _buf_cont_write(outputbuf));
		bytes = min(len, bytes);
#if BYTES_PER_FRAME == 4
		memcpy(outputbuf->writep, data, bytes);
#else
		{
			s16_t *iptr = (s16_t*) data;
			ISAMPLE_T *optr = (ISAMPLE_T*) outputbuf->writep;
			size_t n = bytes / BYTES_PER_FRAME * 2;
			while (n--) *optr++ = *iptr++ << 16;
		}
#endif	
		_buf_inc_writep(outputbuf, bytes);
		space = _buf_space(outputbuf);
		
		len -= bytes;
		data += bytes;
				
		UNLOCK_O;
		
		// allow i2s to empty the buffer if needed
		if (len && !space) usleep(50000);
	}	
}

/****************************************************************************************
 * BT sink command handler
 */

static void bt_sink_cmd_handler(bt_sink_cmd_t cmd, ...) 
{
	va_list args;
	
	LOCK_D;
	
	if (decode.state != DECODE_STOPPED) {
		LOG_WARN("Cannot use BT sink while LMS is controlling player");
		UNLOCK_D;
		bt_sink_cmd(BT_SINK_DISCONNECTED);
		return;
	} 	
	
	va_start(args, cmd);
	
	if (cmd != BT_SINK_VOLUME) LOCK_O;
		
	switch(cmd) {
	case BT_SINK_CONNECTED:
		output.external = DECODE_BT;
		output.state = OUTPUT_STOPPED;
		LOG_INFO("BT sink started");
		break;
	case BT_SINK_DISCONNECTED:	
		if (output.external == DECODE_BT) {
			output.external = 0;
			output.state = OUTPUT_OFF;
			LOG_INFO("BT sink stopped");
		}	
		break;
	case BT_SINK_PLAY:
		output.state = OUTPUT_RUNNING;
		LOG_INFO("BT sink playing");
		break;
	case BT_SINK_STOP:		
		_buf_flush(outputbuf);
	case BT_SINK_PAUSE:		
		output.state = OUTPUT_STOPPED;
		LOG_INFO("BT sink stopped");
		break;
	case BT_SINK_RATE:
		output.next_sample_rate = output.current_sample_rate = va_arg(args, u32_t);
		LOG_INFO("Setting BT sample rate %u", output.next_sample_rate);
		break;
	case BT_SINK_VOLUME: {
		u16_t volume = (u16_t) va_arg(args, u32_t);
		volume = 65536 * powf(volume / 128.0f, 3);
		set_volume(volume, volume);
		break;
	}
	}
	
	if (cmd != BT_SINK_VOLUME) UNLOCK_O;
	UNLOCK_D;

	va_end(args);
}

/****************************************************************************************
 * raop sink data handler
 */
static void raop_sink_data_handler(const uint8_t *data, uint32_t len, u32_t playtime) {
	
	raop_sync.playtime = playtime;
	raop_sync.len = len;

	sink_data_handler(data, len);
}	

/****************************************************************************************
 * AirPlay sink command handler
 */
void raop_sink_cmd_handler(raop_event_t event, void *param)
{
	LOCK_D;
	
	if (decode.state != DECODE_STOPPED) {
		LOG_WARN("Cannot use Airplay sink while LMS is controlling player");
		UNLOCK_D;
		return;
	} 	
	
	if (event != RAOP_VOLUME) LOCK_O;
	
	// this is async, so player might have been deleted
	switch (event) {
		case RAOP_TIMING: {
			u32_t ms, now = gettime_ms();
			s32_t sync_nb, error = 0;
			
			if (!raop_sync.enabled || output.state < OUTPUT_RUNNING || output.frames_played_dmp < output.device_frames) break;
			
			// first must make sure we started on time
			if (raop_sync.start) {
				// how many ms have we really played
				ms = now - output.updated + ((u64_t) (output.frames_played_dmp - output.device_frames) * 1000) / RAOP_SAMPLE_RATE;
				raop_sync.error[raop_sync.idx] = ms - (now - raop_sync.start_time); 
				sync_nb = 2;
				LOG_INFO("backend played %u, desired %u, (delta:%d)", ms, now - raop_sync.start_time, raop_sync.error[raop_sync.idx]);
			} else {	
				// in how many ms will the most recent block play 
				ms = ((u64_t) ((_buf_used(outputbuf) - raop_sync.len) / BYTES_PER_FRAME + output.device_frames + output.frames_in_process) * 1000) / RAOP_SAMPLE_RATE - (now - output.updated);
				raop_sync.error[raop_sync.idx] = (raop_sync.playtime - now) - ms;
				sync_nb = SYNC_NB;
				LOG_INFO("head local:%u, remote:%u (delta:%d)", ms, raop_sync.playtime - now, raop_sync.error[raop_sync.idx]);
				LOG_DEBUG("obuf:%u, sync_len:%u, devframes:%u, inproc:%u", _buf_used(outputbuf), raop_sync.len, output.device_frames, output.frames_in_process);
			}	
			
			// calculate the average error
			for (int i = 0; i < sync_nb; i++) error += raop_sync.error[i];
			error /= sync_nb;
			raop_sync.idx = (raop_sync.idx + 1) % sync_nb;
			
			// need at least nb_sync measures done to exit quick mode
			if (raop_sync.start && !raop_sync.idx && abs(error) < 10) raop_sync.start = false;
			
			// correct if needed
			if (error < -10) {
				output.skip_frames = (abs(error) * RAOP_SAMPLE_RATE) / 1000;
				output.state = OUTPUT_SKIP_FRAMES;					
				memset(raop_sync.error, 0, sizeof(raop_sync.error));
				LOG_INFO("skipping %u frames", output.skip_frames);
			} else if (error > 10) {
				output.pause_frames = (abs(error) * RAOP_SAMPLE_RATE) / 1000;
				output.state = OUTPUT_PAUSE_FRAMES;
				memset(raop_sync.error, 0, sizeof(raop_sync.error));
				LOG_INFO("pausing for %u frames", output.pause_frames);
			}
				
			break;
		}
		case RAOP_SETUP:
			// we need a fair bit of space for RTP process
			_buf_resize(outputbuf, RAOP_OUTPUT_SIZE);
			LOG_INFO("resizing buffer %u", outputbuf->size);
			break;
		case RAOP_STREAM:
			LOG_INFO("Stream", NULL);
			raop_state = event;
			memset(raop_sync.error, 0, sizeof(raop_sync.error));
			raop_sync.idx = 0;
			raop_sync.start = true;		
			raop_sync.enabled = !strcasestr(output.device, "BT");
			output.external = DECODE_AIRPLAY;
			output.next_sample_rate = output.current_sample_rate = RAOP_SAMPLE_RATE;
			output.state = OUTPUT_STOPPED;
			break;
		case RAOP_STOP:
			LOG_INFO("Stop", NULL);
			output.external = 0;
			output.state = OUTPUT_OFF;
			output.frames_played = 0;
			raop_state = event;
			break;
		case RAOP_FLUSH:
			LOG_INFO("Flush", NULL);
			raop_expect_stop = true;
			raop_state = event;
			_buf_flush(outputbuf);		
			output.state = OUTPUT_STOPPED;
			output.frames_played = 0;
			break;
		case RAOP_PLAY: {
			LOG_INFO("Play", NULL);
			if (raop_state != RAOP_PLAY) {
				output.state = OUTPUT_START_AT;
				output.start_at = *(u32_t*) param;
				raop_sync.start_time = output.start_at;
				LOG_INFO("Starting at %u (in %d ms)", output.start_at, output.start_at - gettime_ms());
			}
			raop_state = event;
			break;
		}
		case RAOP_VOLUME: {
			float volume = *((float*) param);
			LOG_INFO("Volume[0..1] %0.4f", volume);
			volume = 65536 * powf(volume, 3);
			set_volume((u16_t) volume, (u16_t) volume);
			break;
		}
		default:
			break;
	}
	
	if (event != RAOP_VOLUME) UNLOCK_O;
	
	UNLOCK_D;
}

/****************************************************************************************
 * We provide the generic codec register option
 */
void register_external(void) {
	if (!strcasestr(output.device, "BT ") ) {
		if(enable_bt_sink){
			bt_sink_init(bt_sink_cmd_handler, sink_data_handler);
			LOG_INFO("Initializing BT sink");
		}
	} else {
		LOG_WARN("Cannot be a BT sink and source");
	}	
	if(enable_airplay){
		raop_sink_init(raop_sink_cmd_handler, raop_sink_data_handler);
		LOG_INFO("Initializing AirPlay sink");
	}
}

void deregister_external(void) {
	if (!strcasestr(output.device, "BT ") && enable_bt_sink) {
		bt_sink_deinit();
		LOG_INFO("Stopping BT sink");
	}
	if(enable_airplay){
		raop_sink_deinit();
		LOG_INFO("Stopping AirPlay sink");
	}
}
