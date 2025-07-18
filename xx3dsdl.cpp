/*
* This software is provided as is, without any warranty, express or implied.
* This software is licensed under a Creative Commons (CC BY-NC-SA) license.
* This software is authored by Catwashere (2025).
*/

#include <ftd3xx/ftd3xx.h>

 // using lodepng to load a png image instead of the default SDL2_image to avoid the need to add another dependency
#include "lodepng.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>
#include <queue>
#include <algorithm>
#include <map>

#include "execpath.h"

#define NAME "xx3dsdl"

#define PRODUCT_1 "N3DSXL"
#define PRODUCT_2 "N3DSXL.2"

#define BULK_OUT 0x02
#define BULK_IN 0x82

#define FIFO_CHANNEL 0

#define CAP_WIDTH 240
#define CAP_HEIGHT (400 + 320)

#define CAP_RES (CAP_WIDTH * CAP_HEIGHT)

#define TOP_RES (400 * 240)
#define DELTA_RES ((400 - 320) * 240)

#define FRAME_SIZE_RGB (CAP_RES * 3)
#define FRAME_SIZE_RGBA (CAP_RES * 4)

#define AUDIO_CHANNELS 2
#define SAMPLE_RATE 32734

#define SAMPLE_SIZE_8 2192
#define SAMPLE_SIZE_16 (SAMPLE_SIZE_8 / 2)

#define BUF_COUNT 8
#define BUF_SIZE (FRAME_SIZE_RGB + SAMPLE_SIZE_8)

#define FRAMERATE_LIMIT 60

#define SAMPLE_LIMIT 3
#define DROP_LIMIT 3

#define TRANSFER_ABORT -1

const std::string CONF_DIR = std::string(std::getenv("HOME")) + "/.config/" + std::string(NAME) + "/";

bool g_running = true;
bool g_finished = false;

bool g_safe_mode = false;

const char* driver = "UNKNOWN";
bool g_kmsdrm = false;
SDL_Rect g_display_bounds[2] = {};
int g_numdisplays = 0;

class Capture {
public:
	static inline UCHAR buf[BUF_COUNT][BUF_SIZE];
	static inline ULONG read[BUF_COUNT];

	static inline bool starting = true;

	static inline bool connected = false;
	static inline bool disconnecting = false;

	static inline bool auto_connect = false;

	static inline bool connect() {
		if (Capture::connected) {
			return true;
		}

		if (FT_Create(const_cast<char*>(PRODUCT_1), FT_OPEN_BY_DESCRIPTION, &Capture::handle) && FT_Create(const_cast<char*>(PRODUCT_2), FT_OPEN_BY_DESCRIPTION, &Capture::handle)) {
			printf("[%s] Create failed.\n", NAME);
			return false;
		}

		UCHAR buf[4] = {0x40, 0x80, 0x00, 0x00};
		ULONG written = 0;

		FT_AbortPipe(Capture::handle, BULK_OUT);
		FT_AbortPipe(Capture::handle, BULK_IN);
		FT_FlushPipe(Capture::handle, BULK_OUT);
		FT_FlushPipe(Capture::handle, BULK_IN);
		FT_ClearStreamPipe(Capture::handle, false, false, BULK_IN);
		FT_ClearStreamPipe(Capture::handle, false, false, BULK_OUT);

		if (FT_WritePipe(Capture::handle, BULK_OUT, buf, 4, &written, 0)) {
			printf("[%s] Write failed.\n", NAME);
			return false;
		}
		
		UCHAR buf2[16] = {0x98, 0x05, 0x9f, 0x0};
		ULONG returned = 0;
		
		if (FT_WritePipe(handle, BULK_OUT, buf2, 4, &returned, 0)) {
			printf("[%s] Write bsId failed.\n", NAME);
			return false;
		}

		if (FT_ReadPipe(handle, BULK_IN, buf2, 16, &returned, 0)) {
			printf("[%s] Read bsId failed.\n", NAME);
			return false;
		}

		uint32_t bsId = (((((buf2[4] << 8) | buf2[3]) << 8) | buf2[2]) << 8) | buf2[1];
		if((bsId & 0xf0f0ff) != 0xc0b0a1){
			printf("[%s] bsId validation failed\n", NAME);
			return false;
		}

		buf[1] = 0x00;

		if (FT_WritePipe(Capture::handle, BULK_OUT, buf, 4, &written, 0)) {
			printf("[%s] Write failed.\n", NAME);
			return false;
		}

		if (FT_SetStreamPipe(Capture::handle, false, false, BULK_IN, BUF_SIZE)) {
			printf("[%s] Stream failed.\n", NAME);
			return false;
		}

		for (int i = 0; i < BUF_COUNT; ++i) {
			if (FT_InitializeOverlapped(Capture::handle, &Capture::overlap[i])) {
				printf("[%s] Initialize failed.\n", NAME);
				return false;
			}
		}

		for (int i = 0; i < BUF_COUNT; ++i) {
			if (FT_ReadPipeAsync(Capture::handle, FIFO_CHANNEL, Capture::buf[i], BUF_SIZE, &Capture::read[i], &Capture::overlap[i]) != FT_IO_PENDING) {
				printf("[%s] Read failed.\n", NAME);
				return false;
			}
		}

		return true;
	}

	static inline void stream(std::promise<int> *p_audio_promise, std::promise<int> *p_video_promise, bool *p_audio_waiting, bool *p_video_waiting) {
		while (g_running) {
			if (!Capture::connected) {
				if (Capture::auto_connect) {
					if (!(Capture::connected = Capture::connect())) {
						SDL_Delay(5000);
					}
				}

				else {
					SDL_Delay(5);
				}

				continue;
			}

			if (Capture::disconnecting || !Capture::transfer()) {
				Capture::disconnecting = Capture::connected = Capture::disconnect();
				Capture::signal(p_video_promise, p_video_waiting, TRANSFER_ABORT);

				Capture::starting = true;
				Capture::index = 0;

				continue;
			}

			Capture::signal(p_audio_promise, p_audio_waiting, Capture::index);
			Capture::signal(p_video_promise, p_video_waiting, Capture::index);

			Capture::index = (Capture::index + 1) % BUF_COUNT;

			if (Capture::starting) {
				Capture::starting = Capture::index;
			}
		}

		Capture::disconnecting = Capture::connected = Capture::disconnect();

		while (!g_finished) {
			Capture::signal(p_audio_promise, p_audio_waiting, TRANSFER_ABORT);
			Capture::signal(p_video_promise, p_video_waiting, TRANSFER_ABORT);

			SDL_Delay(5);
		}
	}

private:
	static inline FT_HANDLE handle;
	static inline OVERLAPPED overlap[BUF_COUNT];

	static inline int index = 0;

	static inline bool disconnect() {
		if (!Capture::connected) {
			return false;
		}

		if (Capture::handle == nullptr) {
			printf("[%s] Handle is null, skipping disconnect.\n", NAME);
			return false;
		}
		SDL_Delay(100);

		for (int i = 0; i < BUF_COUNT; ++i) {
			if (FT_ReleaseOverlapped(Capture::handle, &Capture::overlap[i])) {
				printf("[%s] Release failed.\n", NAME);
			}
		}

		SDL_Delay(50);
		
		if (FT_Close(Capture::handle)) {
			printf("[%s] Close failed.\n", NAME);
		}
		
		return false;
	}

	static inline bool transfer() {
		if (FT_GetOverlappedResult(Capture::handle, &Capture::overlap[Capture::index], &Capture::read[Capture::index], true) == FT_IO_INCOMPLETE && FT_AbortPipe(Capture::handle, BULK_IN)) {
			printf("[%s] Abort failed.\n", NAME);
			return false;
		}

		if (FT_ReadPipeAsync(Capture::handle, FIFO_CHANNEL, Capture::buf[Capture::index], BUF_SIZE, &Capture::read[Capture::index], &Capture::overlap[Capture::index]) != FT_IO_PENDING) {
			printf("[%s] Read failed.\n", NAME);
			return false;
		}

		return true;
	}

	static inline void signal(std::promise<int> *p_promise, bool *p_waiting, int value) {
		if (*p_waiting) {
			*p_waiting = false;
			p_promise->set_value(value);
		}
	}
};

class Audio {
public:
	static inline Audio *p_audio;

	static inline int volume = 100;
	static inline bool mute = false;

	static inline std::promise<int> promise;
	static inline bool waiting = false;

	static inline SDL_AudioDeviceID device_id;
	static inline SDL_AudioSpec audio_spec;

	Audio() {
		SDL_AudioSpec wanted_spec;
		SDL_memset(&wanted_spec, 0, sizeof(wanted_spec));
		wanted_spec.freq = SAMPLE_RATE;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = AUDIO_CHANNELS;
		wanted_spec.samples = 1024;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = this;

		device_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &audio_spec, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
		if (device_id == 0) {
			printf("[%s] SDL_OpenAudioDevice failed: %s\n", NAME, SDL_GetError());
			return;
		}

		SDL_PauseAudioDevice(device_id, 0);
		SDL_Delay(100);  // Give time for callback to be called
	}

	static inline void playback() {
		while (g_running) {
			Audio::promise = std::promise<int>();
			Audio::waiting = true;

			int ready = Audio::promise.get_future().get();

			if (ready == TRANSFER_ABORT) {
				continue;
			}

			if (Capture::starting) {
				Audio::reset();
				continue;
			}

			if (!Audio::load(&Capture::buf[ready][FRAME_SIZE_RGB], &Capture::read[ready])) {
				continue;
			}

			Audio::index = (Audio::index + 1) % BUF_COUNT;

			if (Audio::starting) {
				Audio::starting = false; 
			}

			Audio::unblock();
		}

		Audio::unblock();
		delete Audio::p_audio;
	}

	static inline void audio_callback(void *userdata, Uint8 *stream, int len) {
		Audio *audio = static_cast<Audio*>(userdata);

		int samples_needed = len / sizeof(Sint16);
		Sint16 *output = reinterpret_cast<Sint16*>(stream);
		int samples_written = 0;

		float volumeLevel = Audio::volume / 100.0f;
		if (Audio::mute) {
			// not droping audio when muted to avoid audio noise when unmuting
			volumeLevel = 0;
		}else{
			// non linear volume
			volumeLevel *= volumeLevel;
		}

		while (samples_written < samples_needed && !Audio::samples.empty()) {
			Audio::Sample& sample = Audio::samples.front();
			int samples_to_copy = std::min(samples_needed - samples_written, 
										static_cast<int>(sample.size - sample.offset));
			
			for (int i = 0; i < samples_to_copy; ++i) {
				output[samples_written + i] = static_cast<Sint16>(sample.bytes[sample.offset + i] * volumeLevel);
			}
			
			sample.offset += samples_to_copy;
			samples_written += samples_to_copy;
			
			if (sample.offset >= sample.size) {
				Audio::samples.pop();
			}
		}

		// Fill remaining with silence if needed
		if (samples_written < samples_needed) {
			SDL_memset(output + samples_written, 0, (samples_needed - samples_written) * sizeof(Sint16));
		}
		
		// Don't call unblock here - it's causing issues
		// Audio::unblock();
	}

private:
	struct Sample {
		Sample(Sint16 *bytes, std::size_t size) : bytes(bytes), size(size), offset(0) {}

		Sint16 *bytes;
		std::size_t size;
		std::size_t offset;
	};

	static inline Sint16 buf[BUF_COUNT][SAMPLE_SIZE_16];
	static inline std::queue<Audio::Sample> samples;

	static inline bool starting = true;

	static inline int index = 0;
	static inline int drops = 0;

	static inline std::promise<void> barrier;
	static inline bool blocked = false;

	static inline void reset() {
		Audio::unblock();
		delete Audio::p_audio;

		Audio::samples = {};
		Audio::p_audio = new Audio();

		Audio::starting = true;

		Audio::index = 0;
		Audio::drops = 0;
	}

	static inline bool load(UCHAR *p_buf, ULONG *p_read) {
		if (*p_read <= FRAME_SIZE_RGB) {
			return false;
		}

		if (Audio::samples.size() > SAMPLE_LIMIT) {
			if (++Audio::drops > DROP_LIMIT) {
				Audio::reset();
			}

			else {
				return false;
			}
		}

		Audio::drops = 0;

		Audio::map(p_buf, Audio::buf[Audio::index]);
		Audio::samples.emplace(Audio::buf[Audio::index], (*p_read - FRAME_SIZE_RGB) / 2);

		return true;
	}

	static inline void map(UCHAR *p_in, Sint16 *p_out) {
		for (int i = 0; i < SAMPLE_SIZE_16; ++i) {
			p_out[i] = p_in[i * 2 + 1] << 8 | p_in[i * 2];
		}
	}

	static inline void unblock() {
		if (Audio::blocked) {
			Audio::blocked = false;
			Audio::barrier.set_value();
		}
	}

	~Audio() {
		if (device_id != 0) {
			SDL_CloseAudioDevice(device_id);
		}
	}
};

class Video {
public:
	class Screen {
	public:
		enum Type { TOP, BOT, JOINT, SIZE };
		enum Crop { DEFAULT_3DS, SCALED_DS, NATIVE_DS, COUNT };
		enum Fulltype { ONLY_TOP, ONLY_BOT, TOP_BOT_LT, TOP_BOT_RT, TOP_BOT_RB, TOP_BOT_LB, BOT_TOP_LT, BOT_TOP_RT, BOT_TOP_RB, BOT_TOP_LB, MODS };

		static inline const int widths[Video::Screen::Crop::COUNT] = { 400, 320, 256 };
		static inline const int heights[Video::Screen::Crop::COUNT] = { 240, 240, 192 };

		SDL_Window *m_window;
		SDL_Renderer *m_renderer;
		SDL_Texture *m_in_texture;
		SDL_Texture *m_out_texture;
		SDL_Rect m_in_rect;
		SDL_Rect m_out_rect;

		Fulltype m_fulltype = Video::Screen::Fulltype::ONLY_TOP;

		bool m_blur = false;
		Crop m_crop = Video::Screen::Crop::DEFAULT_3DS;
		int m_rotation = 0;
		double m_scale = 1.0;
		int zindex = 0;

		Screen() : m_window(nullptr), m_renderer(nullptr), m_in_texture(nullptr), m_out_texture(nullptr) {}

		std::string key() {
			switch (this->m_type) {
			case Video::Screen::Type::TOP:
				return "top";

			case Video::Screen::Type::BOT:
				return "bot";

			case Video::Screen::Type::JOINT:
				return "joint";

			default:
				return "";
			}
		}

		void build(Video::Screen::Type type, int u, int width, bool visible) {
			this->m_type = type;

			this->resize(this->width(Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS]), this->height(Video::Screen::heights[Video::Screen::Crop::DEFAULT_3DS]));

			// Set up input rectangle (source texture coordinates)
			this->m_in_rect.x = 0;
			this->m_in_rect.y = u;
			this->m_in_rect.w = this->m_height;
			this->m_in_rect.h = width;

			// Set up output rectangle (destination on screen)
			this->m_out_rect.x = (width - this->m_height) / 2;
			this->m_out_rect.y = (this->m_height - width) / 2;
			this->m_out_rect.w = this->m_height;
			this->m_out_rect.h = width;
			
			if (visible) {
				this->open();
			}
		}

		void reset() {
			if(g_kmsdrm){
				if(this->m_type == Video::Screen::Type::TOP || this->m_type == Video::Screen::Type::JOINT){
					this->resize(g_display_bounds[0].w, g_display_bounds[0].h);
				}else{
					this->resize(g_display_bounds[1].w, g_display_bounds[1].h);	
				}
			}else{
				this->resize(this->width(Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS]), this->height(Video::Screen::heights[Video::Screen::Crop::DEFAULT_3DS]));
			}

			if (this->m_in_texture) {
				SDL_DestroyTexture(this->m_in_texture);
			}
			
			if (this->m_renderer) {
				this->m_in_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
														SDL_TEXTUREACCESS_STREAMING, CAP_WIDTH, CAP_HEIGHT);
			}

			if (this->m_out_texture) {
				SDL_DestroyTexture(this->m_out_texture);
			}
			
			if (this->m_renderer) {
				this->m_out_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
														SDL_TEXTUREACCESS_TARGET, this->m_width, this->m_height);
			}

			if (this->m_rotation) {
				if (this->horizontal()) {
					std::swap(this->m_width, this->m_height);
				}

				this->rotate();
			}

			if (this->m_crop) {
				this->crop();
			}

			this->move();
		}

		void blur() {
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, (this->m_blur)?"1":"0");
		}

		void move() {
			// Reset position first
			this->m_in_rect.x = 0;
			this->m_in_rect.y = 0;

			switch (this->m_type) {
			case Video::Screen::Type::TOP:
			{
				int crop = Video::screens[Video::Screen::Type::JOINT].m_crop;
				bool horizontal = Video::screens[Video::Screen::Type::JOINT].horizontal();
				if (Video::split) {
					crop = this->m_crop;
					horizontal = this->horizontal();
				} 

				this->m_in_rect.y += ((Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS] - Video::Screen::widths[crop]) > 0 ? (Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS] - Video::Screen::widths[Video::Screen::Crop::SCALED_DS]) / 2:0) ;
				int crop_width = Video::Screen::widths[Video::Screen::Crop::SCALED_DS];
				if(this->m_in_rect.y == 0) {
					crop_width = Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS];
				}

				this->m_in_rect.h = crop_width;

				if(!g_kmsdrm) {
					this->m_out_rect.w = Video::Screen::heights[crop];
					this->m_out_rect.h = Video::Screen::widths[crop];

					if (!horizontal) {
						this->m_out_rect.x = (this->m_out_rect.h - this->m_out_rect.w) / 2;
						this->m_out_rect.y = (this->m_out_rect.w - this->m_out_rect.h) / 2;
					}
					else {
						this->m_out_rect.x = 0;
						this->m_out_rect.y = 0;
					}
				}
				else{
					this->m_out_rect.x = (this->m_width - this->m_height) / 2;
					this->m_out_rect.y = (this->m_height - this->m_width) / 2;
					this->m_out_rect.w = this->m_height;
					this->m_out_rect.h = this->m_width;
				}

				return;
			}

			case Video::Screen::Type::BOT:
			{
				int crop = Video::screens[Video::Screen::Type::JOINT].m_crop;
				bool horizontal = Video::screens[Video::Screen::Type::JOINT].horizontal();
				if (Video::split) {
					crop = this->m_crop;
					horizontal = this->horizontal();
				}

				else {
					this->m_in_rect.x += (Video::Screen::heights[Video::Screen::Crop::DEFAULT_3DS] - Video::Screen::heights[Video::Screen::Crop::SCALED_DS]) / 2;
				}

				this->m_in_rect.y += Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS];
					
				if(!g_kmsdrm) {
					this->m_out_rect.w = Video::Screen::heights[crop];
					this->m_out_rect.h = width(Video::Screen::widths[crop]);

					if (!horizontal) {
						this->m_out_rect.x = (this->m_out_rect.h - this->m_out_rect.w) / 2;
						this->m_out_rect.y = (this->m_out_rect.w - this->m_out_rect.h) / 2;
					}
					else {
						this->m_out_rect.x = 0;
						this->m_out_rect.y = 0;
					}	
				}
				else{
					this->m_out_rect.x = (this->m_width - this->m_height) / 2;
					this->m_out_rect.y = (this->m_height - this->m_width) / 2;
					this->m_out_rect.w = this->m_height;
					this->m_out_rect.h = this->m_width;
				}

				return;
			}

			case Video::Screen::Type::JOINT:
			{
				Video::screens[Video::Screen::Type::TOP].move();
				Video::screens[Video::Screen::Type::BOT].move();

				SDL_Rect *top_screen = &Video::screens[Video::Screen::Type::TOP].m_out_rect;
				SDL_Rect *bottom_screen = &Video::screens[Video::Screen::Type::BOT].m_out_rect;

				if (g_kmsdrm) {
					top_screen->x = (this->m_width - this->m_height) / 2;
					top_screen->y = (this->m_height - this->m_width) / 2;
					top_screen->w = this->m_height;
					top_screen->h = this->m_width;

					bottom_screen->x = (this->m_width - this->m_height) / 2,
					bottom_screen->y = (this->m_height - this->m_width) / 2,
					bottom_screen->w = this->m_height;
					bottom_screen->h = this->m_width;

					switch(this->m_fulltype) {
						case Video::Screen::Fulltype::ONLY_TOP:
						{
							bottom_screen->w = 0;
							bottom_screen->h = 0;
							break;
						}
						case Video::Screen::Fulltype::ONLY_BOT:
						{
							top_screen->w = 0;
							top_screen->h = 0;
							break;
						}
						case Video::Screen::Fulltype::MODS:
							// This shouldn't happen in normal operation
							break;
						default: {
							auto setRect = [](SDL_Rect* rect, int x, int y, int w, int h) {
								rect->x = x;
								rect->y = y;
								rect->w = w;
								rect->h = h;
							};

							struct Layout {
								bool bot_on_top;
								int z_bot, z_top;
								int x_off, y_off;
							};

							static const std::map<Video::Screen::Fulltype, Layout> layouts = {
								{ Video::Screen::Fulltype::TOP_BOT_LT, {true, 1, 0, (this->m_width/4 - this->m_height/4) / 2, (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::TOP_BOT_RT, {true, 1, 0, this->m_width - this->m_height/4 - (this->m_width/4 - this->m_height/4) / 2, (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::TOP_BOT_LB, {true, 1, 0, (this->m_width/4 - this->m_height/4) / 2, this->m_height - this->m_width/4 - (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::TOP_BOT_RB, {true, 1, 0, this->m_width - this->m_height/4 - (this->m_width/4 - this->m_height/4) / 2, this->m_height - this->m_width/4 - (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::BOT_TOP_LT, {false, 0, 1, (this->m_width/4 - this->m_height/4) / 2, (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::BOT_TOP_RT, {false, 0, 1, this->m_width - this->m_height/4 - (this->m_width/4 - this->m_height/4) / 2, (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::BOT_TOP_LB, {false, 0, 1, (this->m_width/4 - this->m_height/4) / 2, this->m_height - this->m_width/4 - (this->m_height/4 - this->m_width/4) / 2} },
								{ Video::Screen::Fulltype::BOT_TOP_RB, {false, 0, 1, this->m_width - this->m_height/4 - (this->m_width/4 - this->m_height/4) / 2, this->m_height - this->m_width/4 - (this->m_height/4 - this->m_width/4) / 2} },
							};

							auto it = layouts.find(this->m_fulltype);
							if (it != layouts.end()) {
								const auto& l = it->second;
								Video::screens[Video::Screen::Type::BOT].zindex = l.z_bot;
								Video::screens[Video::Screen::Type::TOP].zindex = l.z_top;
								if (l.bot_on_top) {
									setRect(bottom_screen, l.x_off, l.y_off, this->m_height/4, this->m_width/4);
								} else {
									setRect(top_screen, l.x_off, l.y_off, this->m_height/4, this->m_width/4);
								}
							}
							break;
						}
					}
				}
				else {
					switch(this->m_rotation) {
						case 0:
						{
							int bot_diff = (bottom_screen->h - bottom_screen->w) / 2;
							bottom_screen->x = (top_screen->h - bottom_screen->h) / 2 + bot_diff;
							bottom_screen->y += top_screen->w;
							break;
						}
						case 90:
						{
							bottom_screen->y = (top_screen->h - bottom_screen->h) / 2;
							top_screen->x += bottom_screen->w;
							break;
						}
						case 180:
						{
							int bot_diff = (bottom_screen->h - bottom_screen->w) / 2;
							bottom_screen->x = (top_screen->h - bottom_screen->h) / 2 + bot_diff;
							top_screen->y += bottom_screen->w;
							break;
						}
						case 270:
						{
							bottom_screen->y = (top_screen->h - bottom_screen->h) / 2;
							bottom_screen->x += top_screen->w;
							break;
						}
					}
				}
			}

			case Video::Screen::Type::SIZE:
				// This shouldn't happen in normal operation
				return;
			}
		}

		void toggle() {
			this->m_window ? this->close() : this->open();
		}

		void close() {
			if (this->m_in_texture) {
				SDL_DestroyTexture(this->m_in_texture);
				this->m_in_texture = nullptr;
			}
			if (this->m_out_texture) {
				SDL_DestroyTexture(this->m_out_texture);
				this->m_out_texture = nullptr;
			}
			if (this->m_renderer) {
				SDL_DestroyRenderer(this->m_renderer);
				this->m_renderer = nullptr;
			}
			if (this->m_window) {
				SDL_DestroyWindow(this->m_window);
				this->m_window = nullptr;
			}
		}

		void draw() {
			if (!this->m_window || !this->m_renderer) return;
			if (!g_kmsdrm) {
				SDL_SetWindowSize(this->m_window, this->m_width * this->m_scale, this->m_height * this->m_scale);
			}
			
			// Set render target to output texture
			SDL_SetRenderTarget(this->m_renderer, this->m_out_texture);
			SDL_SetRenderDrawColor(this->m_renderer, 0, 0, 0, 255);
			SDL_RenderClear(this->m_renderer);

			// Copy input texture to output texture with rotation
			if (this->m_in_texture) {
				SDL_RenderCopyEx(this->m_renderer, this->m_in_texture, &this->m_in_rect, &this->m_out_rect, 
								this->m_rotation - 90, NULL, SDL_FLIP_NONE);
			}

			// Reset render target to window
			SDL_SetRenderTarget(this->m_renderer, nullptr);
			SDL_SetRenderDrawColor(this->m_renderer, 0, 0, 0, 255);
			SDL_RenderClear(this->m_renderer);

			// Apply brightness by modulating texture color
			Uint8 brightness = static_cast<Uint8>(Video::brightness * 2.55f);
			SDL_SetTextureColorMod(this->m_out_texture, brightness, brightness, brightness);

			// Render output texture to window with rotation
			SDL_RenderCopy(this->m_renderer, this->m_out_texture, nullptr, nullptr);
			SDL_RenderPresent(this->m_renderer);
		}

		void draw(SDL_Rect *p_top_rect, SDL_Rect *p_top_out_rect, SDL_Rect *p_bot_rect, SDL_Rect *p_bot_out_rect) {
			if (!this->m_window || !this->m_renderer) return;
			if (!g_kmsdrm) {
				SDL_SetWindowSize(this->m_window, this->m_width * this->m_scale, this->m_height * this->m_scale);
			}

			// Set render target to output texture
			SDL_SetRenderTarget(this->m_renderer, this->m_out_texture);
			SDL_SetRenderDrawColor(this->m_renderer, 0, 0, 0, 255);
			SDL_RenderClear(this->m_renderer);

			// Copy both screen textures to output texture with rotation
			if (this->m_in_texture) {
				if(Video::screens[Video::Screen::Type::BOT].zindex > Video::screens[Video::Screen::Type::TOP].zindex) {
					SDL_RenderCopyEx(this->m_renderer, this->m_in_texture, p_top_rect, p_top_out_rect, 
						this->m_rotation - 90, NULL, SDL_FLIP_NONE);
					SDL_RenderCopyEx(this->m_renderer, this->m_in_texture, p_bot_rect, p_bot_out_rect, 
						this->m_rotation - 90, NULL, SDL_FLIP_NONE);
				}else{
					SDL_RenderCopyEx(this->m_renderer, this->m_in_texture, p_bot_rect, p_bot_out_rect, 
						this->m_rotation - 90, NULL, SDL_FLIP_NONE);
					SDL_RenderCopyEx(this->m_renderer, this->m_in_texture, p_top_rect, p_top_out_rect, 
						this->m_rotation - 90, NULL, SDL_FLIP_NONE);
				}
				
			}

			// Reset render target to window
			SDL_SetRenderTarget(this->m_renderer, nullptr);
			SDL_SetRenderDrawColor(this->m_renderer, 0, 0, 0, 255);
			SDL_RenderClear(this->m_renderer);

			// Apply brightness by modulating texture color
			Uint8 brightness = static_cast<Uint8>(Video::brightness * 2.55f);
			SDL_SetTextureColorMod(this->m_out_texture, brightness, brightness, brightness);

			// Render output texture to window with rotation
			SDL_RenderCopy(this->m_renderer, this->m_out_texture, nullptr, nullptr);
			
			SDL_RenderPresent(this->m_renderer);
		}

		int m_width = 0;
		int m_height = 0;

		void rotateLeft() {
			if (g_kmsdrm) {
				return;
			}

			this->m_rotation = ((this->m_rotation / 90 * 90 - 90) % 360 + 360) % 360;
			this->rotate();
		}

		void rotateRight() {
			if (g_kmsdrm) {
				return;
			}

			this->m_rotation = ((this->m_rotation / 90 * 90 - 90) % 360 + 360) % 360;
			this->rotate();
		}

		void rotate() {
			if (this->m_window) {
				SDL_SetWindowSize(this->m_window, this->m_width * this->m_scale, this->m_height * this->m_scale);
			}

			if (this->m_out_texture) {
				SDL_DestroyTexture(this->m_out_texture);
			}
			
			if (this->m_renderer) {
				this->m_out_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
														SDL_TEXTUREACCESS_TARGET, this->m_width, this->m_height);
			}
		}

		void crop() {
			if (g_kmsdrm) {
				return;
			}

			this->horizontal() ? this->resize(this->height(Video::Screen::heights[this->m_crop]), this->width(Video::Screen::widths[this->m_crop])) : this->resize(this->width(Video::Screen::widths[this->m_crop]), this->height(Video::Screen::heights[this->m_crop]));

			if (this->m_window) {
				SDL_SetWindowSize(this->m_window, this->m_width * this->m_scale, this->m_height * this->m_scale);
			}

			if (this->m_out_texture) {
				SDL_DestroyTexture(this->m_out_texture);
			}
			
			if (this->m_renderer) {
				this->m_out_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
														SDL_TEXTUREACCESS_TARGET, this->m_width, this->m_height);
			}
		}

	private:
		Screen::Type m_type;

		bool horizontal() {
			return this->m_rotation / 10 % 2;
		}

		int height(int height) {
			return height * (this->m_type == Video::Screen::Type::JOINT ? 2 : 1);
		}

		int width(int width) {
			return (this->m_type == Video::Screen::Type::BOT && width == this->widths[Video::Screen::Crop::DEFAULT_3DS]) ? this->widths[Video::Screen::Crop::SCALED_DS] : width;
		}

		std::string title() {
			switch (this->m_type) {
			case Video::Screen::Type::TOP:
				return std::string(NAME) + "-top";

			case Video::Screen::Type::BOT:
				return std::string(NAME) + "-bot";

			default:
				return NAME;
			}
		}

		void resize(int width, int height) {
			this->m_width = width;
			this->m_height = height;
		}

		void open() {
			this->blur();
			if(g_kmsdrm){
				int numScreen = (this->m_type == Video::Screen::Type::JOINT)?0:this->m_type;

				this->m_window = SDL_CreateWindow(this->title().c_str(), 
											g_display_bounds[numScreen].x, g_display_bounds[numScreen].y, 
											g_display_bounds[numScreen].w, g_display_bounds[numScreen].h, 
											SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
			}else{
				this->m_window = SDL_CreateWindow(this->title().c_str(), 
											SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
											this->m_width * this->m_scale, this->m_height * this->m_scale, 
											SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
			}

			if (!this->m_window) {
				printf("[%s] SDL_CreateWindow failed: %s\n", NAME, SDL_GetError());
				return;
			}

			this->m_renderer = SDL_CreateRenderer(this->m_window, -1, 
				Video::vsync ? SDL_RENDERER_PRESENTVSYNC : SDL_RENDERER_ACCELERATED);

			if (!this->m_renderer) {
				printf("[%s] SDL_CreateRenderer failed: %s\n", NAME, SDL_GetError());
				SDL_DestroyWindow(this->m_window);
				this->m_window = nullptr;
				return;
			}

			// Create input texture for capture data
			this->m_in_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
													SDL_TEXTUREACCESS_STREAMING, CAP_WIDTH, CAP_HEIGHT);
			if (!this->m_in_texture) {
				printf("[%s] SDL_CreateTexture (input) failed: %s\n", NAME, SDL_GetError());
			}

			// Create output texture
			this->m_out_texture = SDL_CreateTexture(this->m_renderer, SDL_PIXELFORMAT_RGBA32, 
													SDL_TEXTUREACCESS_TARGET, this->m_width, this->m_height);
			if (!this->m_out_texture) {
				printf("[%s] SDL_CreateTexture (output) failed: %s\n", NAME, SDL_GetError());
			}
		}


	};

	static inline Screen screens[Video::Screen::Type::SIZE];

	static inline int brightness = 100;

	static inline bool split = false;
	static inline bool vsync = false;

	static inline std::promise<int> promise;
	static inline bool waiting = false;

	static inline void (*p_load) (std::string path, std::string name);
	static inline void (*p_save) (std::string path, std::string name);

	static inline Screen *screen(std::string key) {
		if (key == "top") {
			return &Video::screens[Video::Screen::Type::TOP];
		}

		if (key == "bot") {
			return &Video::screens[Video::Screen::Type::BOT];
		}

		if (key == "joint") {
			return &Video::screens[Video::Screen::Type::JOINT];
		}

		return nullptr;
	}

	static inline void init() {
		Video::screens[Video::Screen::Type::TOP].reset();
		Video::screens[Video::Screen::Type::BOT].reset();
		Video::screens[Video::Screen::Type::JOINT].reset();

		if (!((Video::screens[Video::Screen::Type::JOINT].m_window != nullptr) ^ Video::split)) {
			Video::screens[Video::Screen::Type::TOP].toggle();
			Video::screens[Video::Screen::Type::BOT].toggle();
			Video::screens[Video::Screen::Type::JOINT].toggle();
		}

		if (Video::split) {
			Video::screens[Video::Screen::Type::TOP].move();
			Video::screens[Video::Screen::Type::BOT].move();
		}
		else {
			Video::screens[Video::Screen::Type::JOINT].move();
		}
	}

	static inline void blank() {
		memset(Video::buf, 0x00, FRAME_SIZE_RGBA);

		unsigned char* image = nullptr;
		unsigned width, height;

		unsigned error = lodepng_decode32_file(&image, &width, &height, (getExecutionPath() + "blank.png").c_str());
		if (error) {
			printf("Error %u: %s\n", error, lodepng_error_text(error));
		}else{
			memcpy(Video::buf, image, FRAME_SIZE_RGBA);
			free(image);
		}
		
		// Update all screen textures
		for (int i = 0; i < Video::Screen::Type::SIZE; ++i) {
			if (Video::screens[i].m_in_texture) {
				SDL_UpdateTexture(Video::screens[i].m_in_texture, nullptr, Video::buf, CAP_WIDTH * 4);
			}
		}

		Video::draw();
	}

	static inline void render() {
		while (g_running) {
			Video::poll();

			if (!Capture::connected) {
				Video::blank();
				SDL_Delay(5);

				continue;
			}

			Video::promise = std::promise<int>();
			Video::waiting = true;

			int ready = Video::promise.get_future().get();

			if (ready == TRANSFER_ABORT) {
				continue;
			}

			if (Capture::starting) {
				Video::blank();
				continue;
			}
			
			if (!Video::load(Capture::buf[ready], &Capture::read[ready])) {
				continue;
			}

			Video::draw();
		}
	}



private:
	static inline UCHAR buf[FRAME_SIZE_RGBA];

	static inline void toggleSplit() {
		if (g_kmsdrm) {
			return;
		} 

		Video::split ^= true;
		Video::swap();
	}

	static inline void swap() {
		Video::screens[Video::Screen::Type::TOP].toggle();
		Video::screens[Video::Screen::Type::BOT].toggle();
		Video::screens[Video::Screen::Type::JOINT].toggle();

		if (Video::split) {
			Video::screens[Video::Screen::Type::TOP].move();
			Video::screens[Video::Screen::Type::BOT].move();
		}
		else {
			Video::screens[Video::Screen::Type::JOINT].move();
		}
	}

	static inline void poll() {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT:
				g_running = false;
				break;

			case SDL_WINDOWEVENT:
				if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
					g_running = false;
				}
				break;

			case SDL_KEYDOWN:
				handleKeyDown(event);
				break;

			case SDL_KEYUP:
				handleKeyUp(event);
				break;
			}
		}
	}

	static inline void handleKeyDown(const SDL_Event& event) {
		// Get the focused window
		Screen* focusedScreen = getFocusedScreen();
		
		switch (event.key.keysym.sym) {
		// Global controls (affect all windows)
		case SDLK_MINUS:
			Video::brightness = Video::brightness > 5 ? Video::brightness / 5 * 5 - 5 : 0;
			break;

		case SDLK_EQUALS:
		case SDLK_PLUS:
			Video::brightness = Video::brightness < 95 ? Video::brightness / 5 * 5 + 5 : 100;
			break;

		case SDLK_COMMA:
			Audio::volume = Audio::volume > 5 ? Audio::volume / 5 * 5 - 5 : 0;
			break;

		case SDLK_PERIOD:
			Audio::volume = Audio::volume < 95 ? Audio::volume / 5 * 5 + 5 : 100;
			break;

		// Window-specific controls
		case SDLK_DOWN:
			if (focusedScreen) {
				focusedScreen->m_scale = focusedScreen->m_scale > 1.5 ? 
					static_cast<int>(focusedScreen->m_scale / 0.5) * 0.5 - 0.5 : 1.0;
			}
			break;

		case SDLK_UP:
			if (focusedScreen) {
				focusedScreen->m_scale = focusedScreen->m_scale < 4.0 ? 
					static_cast<int>(focusedScreen->m_scale / 0.5) * 0.5 + 0.5 : 4.5;
			}
			break;

		case SDLK_LEFT:
			if(g_kmsdrm) {
				if(g_numdisplays == 1){
					Video::screens[Video::Screen::Type::JOINT].m_fulltype = static_cast<Video::Screen::Fulltype>(((Video::screens[Video::Screen::Type::JOINT].m_fulltype - 1) % Video::Screen::Fulltype::MODS + Video::Screen::Fulltype::MODS) % Video::Screen::Fulltype::MODS);
					Video::screens[Video::Screen::Type::JOINT].move();
				}
			}
			else if (focusedScreen) {
				std::swap(focusedScreen->m_width, focusedScreen->m_height);
				focusedScreen->rotateLeft();
				focusedScreen->move();
			}
			break;

		case SDLK_RIGHT:
			if(g_kmsdrm){
				if(g_numdisplays == 1){
					Video::screens[Video::Screen::Type::JOINT].m_fulltype = static_cast<Video::Screen::Fulltype>(((Video::screens[Video::Screen::Type::JOINT].m_fulltype + 1) % Video::Screen::Fulltype::MODS + Video::Screen::Fulltype::MODS) % Video::Screen::Fulltype::MODS);
					Video::screens[Video::Screen::Type::JOINT].move();
				}
			}
			else if (focusedScreen) {
				std::swap(focusedScreen->m_width, focusedScreen->m_height);
				focusedScreen->rotateRight();
				focusedScreen->move();
			}
			break;

		case SDLK_LEFTBRACKET:
		case SDL_SCANCODE_CUT:
			if(g_kmsdrm && g_numdisplays > 1){
				Video::screens[Video::Screen::Type::TOP].m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop - 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				Video::screens[Video::Screen::Type::BOT].m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop - 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				Video::screens[Video::Screen::Type::TOP].crop();
				Video::screens[Video::Screen::Type::BOT].crop();
				Video::screens[Video::Screen::Type::TOP].reset();
				Video::screens[Video::Screen::Type::BOT].reset();
			}
			else if (focusedScreen) {
				focusedScreen->m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop - 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				focusedScreen->crop();
				focusedScreen->move();
			}
			break;

		case SDLK_RIGHTBRACKET:
		case SDL_SCANCODE_PASTE:
			if(g_kmsdrm && g_numdisplays > 1){
				Video::screens[Video::Screen::Type::TOP].m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop + 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				Video::screens[Video::Screen::Type::BOT].m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop + 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				Video::screens[Video::Screen::Type::TOP].crop();
				Video::screens[Video::Screen::Type::BOT].crop();
				Video::screens[Video::Screen::Type::TOP].reset();
				Video::screens[Video::Screen::Type::BOT].reset();
			}
			else if (focusedScreen) {
				focusedScreen->m_crop = static_cast<Video::Screen::Crop>(((focusedScreen->m_crop + 1) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
				focusedScreen->crop();
				focusedScreen->move();
			}
			break;
		}
	}

	static inline void handleKeyUp(const SDL_Event& event) {
		Screen* focusedScreen = getFocusedScreen();
		
		switch (event.key.keysym.sym) {
		// Global controls
		case SDLK_ESCAPE:
			if (!Capture::auto_connect) {
				Capture::connected ? Capture::disconnecting = true : Capture::connected = Capture::connect();
			}
			break;

		case SDLK_0:
			Video::brightness = 100;
			break;

		case SDLK_TAB:
			Video::toggleSplit();
			break;

		case SDLK_m:
			Audio::mute ^= true;
			break;

		// Window-specific controls
		case SDLK_b:
			if(g_kmsdrm && g_numdisplays > 1){
				Video::screens[Video::Screen::Type::TOP].m_blur ^= true;
				Video::screens[Video::Screen::Type::BOT].m_blur ^= true;
				Video::screens[Video::Screen::Type::TOP].blur();
				Video::screens[Video::Screen::Type::BOT].blur();
				Video::screens[Video::Screen::Type::TOP].reset();
				Video::screens[Video::Screen::Type::BOT].reset();
			}
			else if (focusedScreen) {
				focusedScreen->m_blur ^= true;
				focusedScreen->blur();
				focusedScreen->reset();
			}
			break;

		// Layout controls
		case SDLK_F1:
		case SDLK_F2:
		case SDLK_F3:
		case SDLK_F4:
		case SDLK_F5:
		case SDLK_F6:
		case SDLK_F7:
		case SDLK_F8:
		case SDLK_F9:
		case SDLK_F10:
		case SDLK_F11:
		case SDLK_F12:
			if (!g_safe_mode) {
				if (event.key.keysym.mod & KMOD_CTRL) {
					Video::p_save(CONF_DIR + "presets/", "layout" + std::to_string(event.key.keysym.sym - SDLK_F1 + 1) + ".conf");
				}
				else {
					Video::p_load(CONF_DIR + "presets/", "layout" + std::to_string(event.key.keysym.sym - SDLK_F1 + 1) + ".conf");
					Video::init();
				}
			}
			break;
		}
	}

	static inline Screen* getFocusedScreen() {
		// Find the currently focused window
		SDL_Window* focusedWindow = SDL_GetKeyboardFocus();
		if (!focusedWindow) return nullptr;
		
		for (int i = 0; i < Video::Screen::Type::SIZE; ++i) {
			if (Video::screens[i].m_window == focusedWindow) {
				return &Video::screens[i];
			}
		}
		return nullptr;
	}

	static inline bool load(UCHAR *p_buf, ULONG *p_read) {
		if (*p_read < FRAME_SIZE_RGB) {
			return false;
		}

		Video::map(p_buf, Video::buf);
		
		// Update all screen textures
		for (int i = 0; i < Video::Screen::Type::SIZE; ++i) {
			if (Video::screens[i].m_in_texture) {
				SDL_UpdateTexture(Video::screens[i].m_in_texture, nullptr, Video::buf, CAP_WIDTH * 4);
			}
		}

		return true;
	}

	static inline void map(UCHAR *p_in, UCHAR *p_out) {
		for (int i = 0, j = DELTA_RES, k = TOP_RES; i < CAP_RES; ++i) {
			if (i < DELTA_RES) {
				p_out[4 * i + 0] = p_in[3 * i + 0];
				p_out[4 * i + 1] = p_in[3 * i + 1];
				p_out[4 * i + 2] = p_in[3 * i + 2];
				p_out[4 * i + 3] = 0xff;
			}

			else if (i / CAP_WIDTH & 1) {
				p_out[4 * j + 0] = p_in[3 * i + 0];
				p_out[4 * j + 1] = p_in[3 * i + 1];
				p_out[4 * j + 2] = p_in[3 * i + 2];
				p_out[4 * j + 3] = 0xff;

				++j;
			}

			else {
				p_out[4 * k + 0] = p_in[3 * i + 0];
				p_out[4 * k + 1] = p_in[3 * i + 1];
				p_out[4 * k + 2] = p_in[3 * i + 2];
				p_out[4 * k + 3] = 0xff;

				++k;
			}
		}
	}

	static inline void draw() {
		if (Video::split) {
			Video::screens[Video::Screen::Type::TOP].draw();
			Video::screens[Video::Screen::Type::BOT].draw();
		}

		else {
			Video::screens[Video::Screen::Type::JOINT].draw(
				&Video::screens[Video::Screen::Type::TOP].m_in_rect,
				&Video::screens[Video::Screen::Type::TOP].m_out_rect,
				&Video::screens[Video::Screen::Type::BOT].m_in_rect,
				&Video::screens[Video::Screen::Type::BOT].m_out_rect
			);
		}
	}
};

void load(std::string path, std::string name) {
	std::ifstream file(path + name);

	if (!file.good()) {
		printf("[%s] File \"%s\" load failed.\n", NAME, name.c_str());
		return;
	}

	std::string line;

	while (std::getline(file, line)) {
		std::istringstream kvp(line);
		std::string key;

		Video::Screen *p_screen;

		if (std::getline(kvp, key, '_')) {
			p_screen = Video::screen(key);
		}

		if (!p_screen) {
			kvp.str(line);
			kvp.clear();
		}

		if (std::getline(kvp, key, '=')) {
			std::string value;

			if (std::getline(kvp, value)) {
				if (key == "volume") {
					Audio::volume = std::max(0, std::min(100, std::stoi(value) / 5 * 5));
					continue;
				}

				if (key == "mute") {
					Audio::mute = std::stoi(value);
					continue;
				}

				if (key == "brightness") {
					Video::brightness = std::max(0, std::min(100, std::stoi(value) / 5 * 5));
					continue;
				}

				if (key == "split") {
					Video::split = std::stoi(value);
					continue;
				}

				if (key == "blur") {
					p_screen->m_blur = std::stoi(value);
					continue;
				}

				if (key == "crop") {
					p_screen->m_crop = static_cast<Video::Screen::Crop>((std::stoi(value) % Video::Screen::Crop::COUNT + Video::Screen::Crop::COUNT) % Video::Screen::Crop::COUNT);
					continue;
				}

				if (key == "rotation") {
					p_screen->m_rotation = (std::stoi(value) / 90 * 90 % 360 + 360) % 360;
					continue;
				}

				if (key == "scale") {
					p_screen->m_scale = std::max(1.0, std::min(4.5, static_cast<int>(std::stod(value) / 0.5) * 0.5));
					continue;
				}
			}
		}
	}
}

void save(std::string path, std::string name) {
	// Create directory if it doesn't exist (simplified version)
	std::string mkdir_cmd = "mkdir -p " + path;
	system(mkdir_cmd.c_str());
	
	std::ofstream file(path + name);

	if (!file.good()) {
		printf("[%s] File \"%s\" save failed.\n", NAME, name.c_str());
		return;
	}

	file << "volume=" << Audio::volume << std::endl;
	file << "mute=" << Audio::mute << std::endl;
	file << "brightness=" << Video::brightness << std::endl;
	file << "split=" << Video::split << std::endl;

	for (int i = 0; i < Video::Screen::Type::SIZE; ++i) {
		std::string key = Video::screens[i].key();

		file << key << "_blur=" << Video::screens[i].m_blur << std::endl;
		file << key << "_crop=" << Video::screens[i].m_crop << std::endl;
		file << key << "_rotation=" << Video::screens[i].m_rotation << std::endl;
		file << key << "_scale=" << std::to_string(Video::screens[i].m_scale).erase(3, 5) << std::endl;
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--auto") == 0) {
			Capture::auto_connect = true;
			continue;
		}

		if (strcmp(argv[i], "--safe") == 0) {
			g_safe_mode = true;
			continue;
		}

		if (strcmp(argv[i], "--vsync") == 0) {
			Video::vsync = true;
			continue;
		}

		printf("[%s] Invalid argument \"%s\".\n", NAME, argv[i]);
	}

	// Initialize SDL2
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
		printf("[%s] SDL_Init failed: %s\n", NAME, SDL_GetError());
		return -1;
	}

	driver = SDL_GetCurrentVideoDriver();
	g_kmsdrm = strcmp(driver, "KMSDRM") == 0;
	if(g_kmsdrm) {
		g_numdisplays = SDL_GetNumVideoDisplays();
		g_numdisplays = (g_numdisplays > 2) ? 2 : g_numdisplays;
		for (int i = 0; i < g_numdisplays; ++i) {
			SDL_Rect display_bounds;
			if (SDL_GetDisplayBounds(i, &display_bounds) == 0) {
				g_display_bounds[i] = display_bounds;
			}
		}
	}

	if (g_kmsdrm) {
		Uint8 l_data[1] = {0};
		Uint8 l_mask[1] = {0};

		SDL_Cursor *g_cursor = SDL_CreateCursor(l_data, l_mask, 1, 1, 0, 0);
		SDL_SetCursor(g_cursor);

		g_safe_mode = true;
		Capture::auto_connect = true;

		if(g_numdisplays > 1){
			Video::split = true;
		}
	}

	if (!g_safe_mode) {
		load(CONF_DIR, std::string(NAME) + ".conf");
	}

	Capture::connected = Capture::connect();
	Audio::p_audio = new Audio();

	Video::p_load = &load;
	Video::p_save = &save;

	Video::screens[Video::Screen::Type::TOP].build(Video::Screen::Type::TOP, 0, Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS], Video::split);
	Video::screens[Video::Screen::Type::BOT].build(Video::Screen::Type::BOT, Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS], Video::Screen::widths[Video::Screen::Crop::SCALED_DS], Video::split);
	Video::screens[Video::Screen::Type::JOINT].build(Video::Screen::Type::JOINT, 0, Video::Screen::widths[Video::Screen::Crop::DEFAULT_3DS], !Video::split);

	// Input textures are now created in each screen's open() method

	Video::init();
	Video::blank();

	std::thread capture = std::thread(Capture::stream, &Audio::promise, &Video::promise, &Audio::waiting, &Video::waiting);
	std::thread audio = std::thread(Audio::playback);

	Video::render();
	audio.join();

	g_finished = true;
	capture.join();

	Video::screens[Video::Screen::Type::TOP].close();
	Video::screens[Video::Screen::Type::BOT].close();
	Video::screens[Video::Screen::Type::JOINT].close();

	// Input textures are now cleaned up in each screen's close() method

	if (!g_safe_mode) {
		save(CONF_DIR, std::string(NAME) + ".conf");
	}

	SDL_Quit();

	return 0;
}
